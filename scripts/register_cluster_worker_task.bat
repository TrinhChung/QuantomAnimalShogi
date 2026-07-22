@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0register_cluster_worker_task.ps1" %*
exit /b %ERRORLEVEL%
