[CmdletBinding()]
param(
    [string]$ListenAddress = '127.0.0.1',

    [ValidateRange(1, 65535)]
    [int]$Port = 5173,

    [ValidateRange(1, 65535)]
    [int]$BridgePort = 8766,

    [switch]$InstallOnly
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$webDirectory = Join-Path $repositoryRoot 'quantum-animal-shogi\web'
$packageFile = Join-Path $webDirectory 'package.json'
$viteCommand = Join-Path $webDirectory 'node_modules\.bin\vite.cmd'
$bridgeScript = Join-Path $webDirectory 'server\native_bot_bridge.mjs'
$logDirectory = Join-Path $repositoryRoot '.cache\web-solo'
$bridgeStandardOutput = Join-Path $logDirectory 'native-bot-bridge.out.log'
$bridgeStandardError = Join-Path $logDirectory 'native-bot-bridge.err.log'

if (-not (Test-Path -LiteralPath $packageFile -PathType Leaf)) {
    throw "Web package was not found at $packageFile"
}
if (-not (Test-Path -LiteralPath $bridgeScript -PathType Leaf)) {
    throw "Native bot bridge was not found at $bridgeScript"
}

Push-Location -LiteralPath $webDirectory
try {
    if (-not (Test-Path -LiteralPath $viteCommand -PathType Leaf)) {
        Write-Host 'Installing web dependencies...'
        & npm install
        if ($LASTEXITCODE -ne 0) {
            throw 'npm install failed'
        }
    }

    if ($InstallOnly) {
        Write-Host 'Web dependencies are ready.'
        return
    }

    New-Item -ItemType Directory -Force -Path $logDirectory | Out-Null
    $nodeCommand = (Get-Command node -ErrorAction Stop).Source
    $bridgeProcess = Start-Process `
        -FilePath $nodeCommand `
        -ArgumentList @($bridgeScript, '--port', $BridgePort) `
        -WorkingDirectory $webDirectory `
        -WindowStyle Hidden `
        -RedirectStandardOutput $bridgeStandardOutput `
        -RedirectStandardError $bridgeStandardError `
        -PassThru

    $bridgeUrl = "http://127.0.0.1:$BridgePort"
    $bridgeReady = $false
    for ($attempt = 0; $attempt -lt 40; ++$attempt) {
        if ($bridgeProcess.HasExited) {
            $bridgeError = Get-Content -LiteralPath $bridgeStandardError -Raw -ErrorAction SilentlyContinue
            throw "Native bot bridge exited during startup. $bridgeError"
        }
        try {
            Invoke-RestMethod -Uri "$bridgeUrl/api/health" -TimeoutSec 1 | Out-Null
            $bridgeReady = $true
            break
        }
        catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if (-not $bridgeReady) {
        throw 'Native bot bridge did not become ready'
    }

    Write-Host "Native C++ bots: connected on port $BridgePort"
    Write-Host "Web: http://${ListenAddress}:$Port/"
    $previousBridgePort = $env:QAS_BOT_BRIDGE_PORT
    $env:QAS_BOT_BRIDGE_PORT = $BridgePort
    try {
        & npm run dev -- --host $ListenAddress --port $Port
        if ($LASTEXITCODE -ne 0) {
            throw 'Vite development server failed'
        }
    }
    finally {
        $env:QAS_BOT_BRIDGE_PORT = $previousBridgePort
    }
}
finally {
    if (Get-Variable bridgeProcess -ErrorAction SilentlyContinue) {
        if (-not $bridgeProcess.HasExited) {
            try {
                Invoke-RestMethod -Method Post -Uri "$bridgeUrl/api/shutdown" -TimeoutSec 2 | Out-Null
                $bridgeProcess.WaitForExit(2000) | Out-Null
            }
            catch {
                # The bridge may already be shutting down after Ctrl+C.
            }
        }
        if (-not $bridgeProcess.HasExited) {
            Stop-Process -Id $bridgeProcess.Id -Force
        }
    }
    Pop-Location
}
