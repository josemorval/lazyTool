@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0decompress_nvdb.ps1" %*
exit /b %ERRORLEVEL%
