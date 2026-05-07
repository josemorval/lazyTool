@echo off
setlocal
cd /d "%~dp0"

set LT=sample_64k.lt
if not "%~1"=="" set LT=%~1

cl /nologo /O2 /EHsc /std:c++17 build64k.cpp /Fe:build64k.exe
if errorlevel 1 exit /b 1

build64k.exe "%LT%" out64k.c
if errorlevel 1 exit /b 1

cl /nologo /TC /std:c17 /O1 /Oi- /GS- /Zl out64k.c /link /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /NODEFAULTLIB /OPT:REF /OPT:ICF user32.lib kernel32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib /OUT:lt64k.exe
if errorlevel 1 exit /b 1

echo.
echo OK: lt64k.exe generado desde %LT%
echo     codigo generado: build64k\out64k.c
endlocal
