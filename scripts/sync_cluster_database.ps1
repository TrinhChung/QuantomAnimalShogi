[CmdletBinding(SupportsShouldProcess = $true, ConfirmImpact = 'High')]
param(
    [string]$SshHost = 'phuong',

    [string]$LocalHostName = '127.0.0.1',

    [ValidateRange(1, 65535)]
    [int]$LocalPort = 3306,

    [ValidatePattern('^[a-zA-Z0-9_]{1,64}$')]
    [string]$LocalDatabase = 'quantum_animal_shogi_replica',

    [string]$LocalUser = $env:QAS_LOCAL_DB_USER,

    [string]$LocalPassword = $env:QAS_LOCAL_DB_PASSWORD
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not $LocalUser) {
    $LocalUser = [Environment]::GetEnvironmentVariable('QAS_LOCAL_DB_USER', 'User')
}
if ($null -eq $LocalPassword) {
    $LocalPassword = [Environment]::GetEnvironmentVariable('QAS_LOCAL_DB_PASSWORD', 'User')
}
if (-not $LocalUser -or $null -eq $LocalPassword) {
    throw 'Set QAS_LOCAL_DB_USER and QAS_LOCAL_DB_PASSWORD before synchronizing'
}
if (-not $PSCmdlet.ShouldProcess(
        "$LocalHostName`:$LocalPort/$LocalDatabase",
        'Replace the local replica schema and rows from phuong'
    )) {
    return
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$cacheRoot = Join-Path $repositoryRoot '.cache\database-sync'
New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
$dumpPath = Join-Path $cacheRoot ("qas-{0}.sql" -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$remoteScriptPath = Join-Path $cacheRoot ("dump-{0}.sh" -f [DateTime]::UtcNow.ToString('yyyyMMddTHHmmssfffZ'))
$mysqlTool = Get-Command mysql -ErrorAction SilentlyContinue
$mysqlCommand = if ($mysqlTool) { $mysqlTool.Source } else { $null }
if (-not $mysqlCommand) {
    $mysqlCandidates = @(
        (Join-Path $env:ProgramFiles 'MySQL\MySQL Server 8.4\bin\mysql.exe'),
        (Join-Path $env:ProgramFiles 'MySQL\MySQL Server 8.0\bin\mysql.exe')
    )
    $mysqlCommand = $mysqlCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
}
if (-not $mysqlCommand) {
    throw 'mysql.exe was not found in PATH or a standard MySQL Server installation'
}
$remoteDump = @'
set -euo pipefail
source /etc/qas/backup.env
docker exec -e MYSQL_PWD="${QAS_BACKUP_PASSWORD}" "${QAS_MYSQL_CONTAINER}" \
  mysqldump -u"${QAS_BACKUP_USER}" --single-transaction --quick --no-tablespaces \
  --routines --triggers --events --set-gtid-purged=OFF --add-drop-table \
  "${QAS_BACKUP_DATABASE}"
'@
[System.IO.File]::WriteAllText(
    $remoteScriptPath,
    ($remoteDump -replace "`r", ''),
    [System.Text.UTF8Encoding]::new($false)
)

try {
    $sshArguments = @('-o', 'BatchMode=yes', $SshHost, 'bash', '-s')
    $dumpProcess = Start-Process `
        -FilePath (Get-Command ssh -ErrorAction Stop).Source `
        -ArgumentList $sshArguments `
        -RedirectStandardInput $remoteScriptPath `
        -RedirectStandardOutput $dumpPath `
        -WindowStyle Hidden `
        -PassThru
    $dumpProcess.WaitForExit()
    if ($dumpProcess.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $dumpPath) -or (Get-Item $dumpPath).Length -lt 100) {
        throw 'Remote MySQL dump failed or returned an empty file'
    }

    $previousPassword = $env:MYSQL_PWD
    $env:MYSQL_PWD = $LocalPassword
    try {
        & $mysqlCommand --host=$LocalHostName --port=$LocalPort --user=$LocalUser `
            --execute="CREATE DATABASE IF NOT EXISTS ``$LocalDatabase`` CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci"
        if ($LASTEXITCODE -ne 0) {
            throw 'Creating the local replica database failed'
        }
        $importProcess = Start-Process `
            -FilePath $mysqlCommand `
            -ArgumentList @("--host=$LocalHostName", "--port=$LocalPort", "--user=$LocalUser", $LocalDatabase) `
            -RedirectStandardInput $dumpPath `
            -WindowStyle Hidden `
            -Wait `
            -PassThru
        if ($importProcess.ExitCode -ne 0) {
            throw 'Importing the local replica database failed'
        }
    }
    finally {
        $env:MYSQL_PWD = $previousPassword
    }
    Write-Host "Synchronized $LocalDatabase from $SshHost over SSH."
}
finally {
    $resolvedCache = [System.IO.Path]::GetFullPath($cacheRoot)
    $resolvedDump = [System.IO.Path]::GetFullPath($dumpPath)
    if ($resolvedDump.StartsWith($resolvedCache, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedDump -Force -ErrorAction SilentlyContinue
    }
    $resolvedScript = [System.IO.Path]::GetFullPath($remoteScriptPath)
    if ($resolvedScript.StartsWith($resolvedCache, [System.StringComparison]::OrdinalIgnoreCase)) {
        Remove-Item -LiteralPath $resolvedScript -Force -ErrorAction SilentlyContinue
    }
}
