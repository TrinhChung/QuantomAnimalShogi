[CmdletBinding()]
param(
    [string]$SshHost = 'phuong',

    [ValidateRange(1024, 65535)]
    [int]$LocalPort = 18766,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$WorkerArguments
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$token = $env:QAS_CLUSTER_TOKEN
if (-not $token) {
    $token = [Environment]::GetEnvironmentVariable('QAS_CLUSTER_TOKEN', 'User')
}
if (-not $token) {
    throw 'Set the user-level QAS_CLUSTER_TOKEN before starting the worker'
}

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$forward = "127.0.0.1:$LocalPort`:127.0.0.1:8766"
$sshProcess = Start-Process `
    -FilePath (Get-Command ssh -ErrorAction Stop).Source `
    -ArgumentList @(
        '-N', '-T', '-o', 'BatchMode=yes', '-o', 'ExitOnForwardFailure=yes',
        '-o', 'ServerAliveInterval=30', '-o', 'ServerAliveCountMax=3',
        '-L', $forward, $SshHost
    ) `
    -WindowStyle Hidden `
    -PassThru

try {
    $ready = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if ($sshProcess.HasExited) {
            throw "SSH tunnel to $SshHost exited with code $($sshProcess.ExitCode)"
        }
        try {
            $health = Invoke-RestMethod -Uri "http://127.0.0.1:$LocalPort/api/health" -TimeoutSec 2
            if ($health.ok -and $health.mysql -and $health.redis) {
                $ready = $true
                break
            }
        }
        catch {
            Start-Sleep -Milliseconds 500
        }
    }
    if (-not $ready) {
        throw "Cluster master did not become ready through the SSH tunnel to $SshHost"
    }

    $env:QAS_CLUSTER_URL = "http://127.0.0.1:$LocalPort"
    $env:QAS_CLUSTER_TOKEN = $token
    Push-Location $repositoryRoot
    try {
        & python -m evaluation.cluster_worker @WorkerArguments
        exit $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
}
finally {
    if (-not $sshProcess.HasExited) {
        Stop-Process -Id $sshProcess.Id -Force -ErrorAction SilentlyContinue
        $sshProcess.WaitForExit()
    }
    $sshProcess.Dispose()
}
