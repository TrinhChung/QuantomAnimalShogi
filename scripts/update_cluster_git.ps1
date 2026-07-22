[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$RepositoryRoot,

    [ValidatePattern('^[a-zA-Z0-9._-]+$')]
    [string]$Remote = 'origin',

    [ValidatePattern('^[0-9a-f]{40}$')]
    [string]$Commit
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$resolvedRepository = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if (-not (Test-Path -LiteralPath (Join-Path $resolvedRepository '.git'))) {
    throw "Not a Git repository: $resolvedRepository"
}

Push-Location -LiteralPath $resolvedRepository
try {
    $dirtyPaths = @(git status --porcelain)
    if ($LASTEXITCODE -ne 0) {
        throw 'git status failed'
    }
    if ($dirtyPaths.Count -gt 0) {
        throw "Refusing to update a dirty working tree:`n$($dirtyPaths -join "`n")"
    }

    & git fetch --prune $Remote
    if ($LASTEXITCODE -ne 0) {
        throw "git fetch failed for remote $Remote"
    }

    $target = $Commit
    if (-not $target) {
        $branch = (& git branch --show-current).Trim()
        if ($LASTEXITCODE -ne 0 -or -not $branch) {
            throw 'A commit is required while HEAD is detached'
        }
        $target = "$Remote/$branch"
    }

    & git merge --ff-only $target
    if ($LASTEXITCODE -ne 0) {
        throw "Fast-forward update failed for $target"
    }
    $resolvedCommit = (& git rev-parse HEAD).Trim()
    Write-Host "Updated $resolvedRepository to $resolvedCommit"
}
finally {
    Pop-Location
}
