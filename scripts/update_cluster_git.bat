@echo off
setlocal
set "REPO_ROOT=%~dp0.."
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update_cluster_git.ps1" -RepositoryRoot "%REPO_ROOT%" %*
exit /b %ERRORLEVEL%
