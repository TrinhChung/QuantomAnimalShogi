@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0sync_cluster_database.ps1" %*
exit /b %ERRORLEVEL%
