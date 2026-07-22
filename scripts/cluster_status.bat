@echo off
setlocal
set "REPO_ROOT=%~dp0.."
pushd "%REPO_ROOT%" || exit /b 2
python -m evaluation.cluster_cli state %*
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
