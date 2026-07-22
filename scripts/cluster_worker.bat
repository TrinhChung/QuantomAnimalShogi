@echo off
setlocal
set "REPO_ROOT=%~dp0.."
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is required. 1>&2
  exit /b 2
)
pushd "%REPO_ROOT%" || exit /b 2
python -m evaluation.cluster_worker %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
