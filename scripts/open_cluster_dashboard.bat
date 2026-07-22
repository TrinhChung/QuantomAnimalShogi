@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0open_cluster_dashboard.ps1" %*
exit /b %ERRORLEVEL%
