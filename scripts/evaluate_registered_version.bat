@echo off
setlocal
set "REPO_ROOT=%~dp0.."
where python >nul 2>nul
if errorlevel 1 (
  echo Python 3 is required. 1>&2
  exit /b 2
)
if "%~1"=="" (
  echo Usage: %~nx0 VERSION_ID [evaluation options] 1>&2
  exit /b 2
)
pushd "%REPO_ROOT%" || exit /b 2
python -m evaluation.tools.entrypoint registered %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
