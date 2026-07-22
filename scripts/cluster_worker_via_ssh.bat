@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cluster_worker_via_ssh.ps1" %*
exit /b %ERRORLEVEL%
