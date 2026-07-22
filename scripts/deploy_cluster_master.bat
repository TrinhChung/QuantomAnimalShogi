@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy_cluster_master.ps1" %*
exit /b %ERRORLEVEL%
