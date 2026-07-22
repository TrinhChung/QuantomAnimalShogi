[CmdletBinding()]
param(
    [string]$SshHost = 'phuong',

    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$Commit,

    [ValidatePattern('^/[a-zA-Z0-9._/-]+$')]
    [string]$RemoteRepository = '/opt/quantum-animal-shogi',

    [string]$GitUrl = 'git@github.com:TrinhChung/QuantomAnimalShogi.git'
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
if (-not $Commit) {
    $Commit = (& git -C $repositoryRoot rev-parse HEAD).Trim()
    if ($LASTEXITCODE -ne 0 -or $Commit -notmatch '^[0-9a-f]{40}$') {
        throw 'Cannot resolve the local Git commit'
    }
}

& git -C $repositoryRoot fetch origin
if ($LASTEXITCODE -ne 0) {
    throw 'git fetch origin failed'
}
$remoteBranches = @(& git -C $repositoryRoot branch -r --contains $Commit)
if ($LASTEXITCODE -ne 0 -or -not ($remoteBranches -match '^\s*origin/')) {
    throw "Commit $Commit is not published on origin; push it before deployment"
}

$bootstrap = @'
set -euo pipefail
target="$1"
commit="$2"
url="$3"
if [[ ! -d "${target}/.git" ]]; then
  git clone "${url}" "${target}"
fi
cd "${target}"
if [[ -n "$(git status --porcelain)" ]]; then
  echo "Refusing to deploy over a dirty master checkout" >&2
  exit 2
fi
git fetch --prune origin "${commit}"
git checkout --detach "${commit}"
'@

$temporaryScript = Join-Path ([System.IO.Path]::GetTempPath()) ("qas-deploy-{0}.sh" -f [guid]::NewGuid())
$temporaryOutput = "$temporaryScript.out"
$temporaryError = "$temporaryScript.err"
try {
    [System.IO.File]::WriteAllText(
        $temporaryScript,
        ($bootstrap -replace "`r", ''),
        [System.Text.UTF8Encoding]::new($false)
    )
    $checkoutProcess = Start-Process `
        -FilePath (Get-Command ssh -ErrorAction Stop).Source `
        -ArgumentList @('-o', 'BatchMode=yes', $SshHost, 'bash', '-s', '--', $RemoteRepository, $Commit, $GitUrl) `
        -RedirectStandardInput $temporaryScript `
        -RedirectStandardOutput $temporaryOutput `
        -RedirectStandardError $temporaryError `
        -WindowStyle Hidden `
        -Wait `
        -PassThru
    Get-Content -LiteralPath $temporaryOutput -ErrorAction SilentlyContinue
    if ($checkoutProcess.ExitCode -ne 0) {
        $checkoutError = Get-Content -LiteralPath $temporaryError -Raw -ErrorAction SilentlyContinue
        throw "Deployment checkout failed on $SshHost. $checkoutError"
    }
}
finally {
    Remove-Item -LiteralPath $temporaryScript, $temporaryOutput, $temporaryError -Force -ErrorAction SilentlyContinue
}
& ssh -o BatchMode=yes $SshHost bash "$RemoteRepository/evaluation/deploy/bootstrap_master.sh"
if ($LASTEXITCODE -ne 0) {
    throw "Master bootstrap failed on $SshHost"
}
Write-Host "Deployed commit $Commit to $SshHost"
