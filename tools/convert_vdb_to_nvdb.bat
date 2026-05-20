@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0convert_vdb_to_nvdb.ps1" %*
exit /b %ERRORLEVEL%
