@echo off
setlocal
set "REPO_ROOT=%~dp0.."
if "%~1"=="" (
  echo Usage: %~nx0 PROFILE [cluster enqueue options] 1>&2
  echo Example: %~nx0 fixed_depth_quick --candidate-name "Stage 5.1" 1>&2
  exit /b 2
)
pushd "%REPO_ROOT%" || exit /b 2
python -m evaluation.cluster_cli enqueue benchmark --profile %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
