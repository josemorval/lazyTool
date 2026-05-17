@echo off
setlocal
cd /d "%~dp0"

set "LT=..\projects\procedural_spheres_pbr_post.lt"
if not "%~1"=="" set "LT=%~1"

del /q build64k.exe build64k.obj out64k.c out64k.obj lt64k.exe lt64k_unpacked.exe 2>nul

cl /nologo /O2 /EHsc /std:c++17 build64k.cpp /Fe:build64k.exe || exit /b 1
build64k.exe "%LT%" out64k.c || exit /b 1

cl /nologo /TC /std:c17 /O1 /Os /Oi- /GS- /Gw /Gy /GF /Zl /Fo:out64k.obj out64k.c /link /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /NODEFAULTLIB /OPT:REF /OPT:ICF /INCREMENTAL:NO user32.lib gdi32.lib kernel32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib /OUT:lt64k.exe || exit /b 1

upx.exe --best --lzma -q lt64k.exe || exit /b 1

for %%F in (lt64k.exe) do echo OK %%F %%~zF bytes
del /q build64k.exe build64k.obj out64k.obj 2>nul
endlocal
