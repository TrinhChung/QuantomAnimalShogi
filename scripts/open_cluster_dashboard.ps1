[CmdletBinding()]
param(
    [string]$SshHost = 'phuong',

    [ValidateRange(1024, 65535)]
    [int]$LocalPort = 18331,

    [switch]$NoBrowser
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$forward = "127.0.0.1:$LocalPort`:127.0.0.1:8331"
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
    $url = "http://127.0.0.1:$LocalPort/"
    $ready = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++) {
        if ($sshProcess.HasExited) {
            throw "SSH tunnel to $SshHost exited with code $($sshProcess.ExitCode)"
        }
        try {
            $health = Invoke-RestMethod -Uri "${url}api/health" -TimeoutSec 2
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
        throw "Dashboard did not become ready through the SSH tunnel to $SshHost"
    }
    if (-not $NoBrowser) {
        Start-Process $url
    }
    Write-Host "Dashboard: $url"
    Write-Host 'Keep this window open; press Ctrl+C to close the SSH tunnel.'
    $sshProcess.WaitForExit()
}
finally {
    if (-not $sshProcess.HasExited) {
        Stop-Process -Id $sshProcess.Id -Force -ErrorAction SilentlyContinue
        $sshProcess.WaitForExit()
    }
    $sshProcess.Dispose()
}
