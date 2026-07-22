@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0audit_master.ps1" %*
exit /b %ERRORLEVEL%
