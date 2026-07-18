[CmdletBinding()]
param(
    [ValidatePattern('^[a-z0-9][a-z0-9._-]{0,63}$')]
    [string]$Version = 'current',

    [ValidateSet('Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Configuration = 'Release',

    [switch]$SkipTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repositoryRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$buildRoot = Join-Path $repositoryRoot 'build'
$buildDirectory = [IO.Path]::GetFullPath((Join-Path $buildRoot $Version))

if (-not $buildDirectory.StartsWith(
        [IO.Path]::GetFullPath($buildRoot) + [IO.Path]::DirectorySeparatorChar,
        [StringComparison]::OrdinalIgnoreCase)) {
    throw "Build directory must stay inside $buildRoot"
}

& cmake -S $repositoryRoot -B $buildDirectory "-DCMAKE_BUILD_TYPE=$Configuration"
if ($LASTEXITCODE -ne 0) {
    throw "CMake configure failed for build version '$Version'"
}

& cmake --build $buildDirectory --config $Configuration --parallel
if ($LASTEXITCODE -ne 0) {
    throw "CMake build failed for build version '$Version'"
}

if (-not $SkipTests) {
    & ctest --test-dir $buildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) {
        throw "CTest failed for build version '$Version'"
    }
}

Write-Host "Build version '$Version' is ready at $buildDirectory"
