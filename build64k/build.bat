@echo off
setlocal
cd /d "%~dp0"

REM -----------------------------------------------------------------------------
REM build64k helper script
REM -----------------------------------------------------------------------------
REM Usage:
REM   build.bat [project.lt]
REM
REM The script does four things:
REM   1) Builds build64k.exe from build64k.cpp.
REM   2) Uses build64k.exe to generate out64k.c from the .lt project.
REM   3) Compiles out64k.c into lt64k.exe with a tiny /NODEFAULTLIB setup.
REM   4) Compresses lt64k.exe with upx.exe from this same folder.
REM
REM Optional runtime switches are passed to the final C compiler through
REM LT64K_CFLAGS. They do not change out64k.c; they override #ifndef defaults
REM during the final cl.exe step.
REM
REM Default runtime behavior:
REM   - fullscreen borderless window only
REM   - scene render size = fullscreen/backbuffer size
REM   - camera aspect ratio = fullscreen/backbuffer aspect ratio
REM   - fixed-size custom render textures stay fixed
REM   - scene-scaled render textures follow the fullscreen size
REM   - VSync disabled
REM   - FPS overlay disabled
REM
REM Examples from cmd.exe:
REM   set "LT64K_CFLAGS=/DLT_DEBUG_FPS=1"
REM   build.bat ..\projects\procedural_spheres_pbr_post.lt
REM
REM   set "LT64K_SKIP_UPX=1"
REM   build.bat ..\projects\procedural_spheres_pbr_post.lt
REM
REM   set "LT64K_CFLAGS=/DLT_VSYNC=1 /DLT_DEBUG_FPS=1"
REM   build.bat ..\projects\procedural_spheres_pbr_post.lt
REM
REM Examples from PowerShell:
REM   $env:LT64K_CFLAGS="/DLT_DEBUG_FPS=1"
REM   .\build.bat ..\projects\procedural_spheres_pbr_post.lt
REM -----------------------------------------------------------------------------

set LT=..\projects\procedural_spheres_pbr_post.lt
if not "%~1"=="" set LT=%~1

REM Build the exporter itself. This is normal C++ and is not size-critical.
cl /nologo /O2 /EHsc /std:c++17 build64k.cpp /Fe:build64k.exe
if errorlevel 1 exit /b 1

REM Generate the single-file procedural player source.
build64k.exe "%LT%" out64k.c
if errorlevel 1 exit /b 1

REM User-supplied compile-time switches for out64k.c. Empty by default.
if "%LT64K_CFLAGS%"=="" set LT64K_CFLAGS=
echo LT64K_CFLAGS=%LT64K_CFLAGS%

REM Compile the tiny player. /NODEFAULTLIB is deliberate: out64k.c provides only
REM the CRT pieces it needs. Keep optimization moderate; UPX will do the final
REM packing pass better than over-complicating this source.
cl /nologo /TC /std:c17 /O1 /Os /Oi- /GS- /Gw /Gy /GF /Zl %LT64K_CFLAGS% /Fo:out64k.obj out64k.c /link /ENTRY:WinMainCRTStartup /SUBSYSTEM:WINDOWS /NODEFAULTLIB /OPT:REF /OPT:ICF /INCREMENTAL:NO /MAP:lt64k.map user32.lib gdi32.lib kernel32.lib d3d11.lib dxgi.lib dxguid.lib d3dcompiler.lib /OUT:lt64k.exe
if errorlevel 1 exit /b 1

REM Keep an unpacked copy next to the final executable. This is useful when you
REM want to compare sizes, debug crashes, or temporarily test without the UPX
REM decompression stub. The shipped/demo executable remains lt64k.exe.
copy /Y lt64k.exe lt64k_unpacked.exe >nul
if errorlevel 1 exit /b 1

REM Final pack step. UPX is expected to live in the same folder as this batch
REM file. --best --lzma is slower but usually gives the smallest executable,
REM which is what we want for the 64k path.
REM
REM To skip packing for a local test, run:
REM   set "LT64K_SKIP_UPX=1"
REM   build.bat ..\projects\procedural_spheres_pbr_post.lt
if not "%LT64K_SKIP_UPX%"=="1" (
  if not exist "%~dp0upx.exe" (
    echo ERROR: upx.exe was not found next to build.bat.
    echo        Put upx.exe in: %~dp0
    exit /b 1
  )
  "%~dp0upx.exe" --best --lzma -q lt64k.exe
  if errorlevel 1 exit /b 1
)

echo.
echo == 64k link/pack size report ==
for %%F in (out64k.c out64k.obj lt64k_unpacked.exe lt64k.exe lt64k.map) do if exist %%F echo   %%F: %%~zF bytes
if exist lt64k.map (
  echo.
  echo == map sections / largest public symbols hint ==
  powershell -NoProfile -ExecutionPolicy Bypass -Command "Select-String -Path 'lt64k.map' -Pattern '^[ ]*[0-9A-Fa-f]{4}:[0-9A-Fa-f]{8}[ ]+[0-9A-Fa-f]+H[ ]+' | Select-Object -First 24 | ForEach-Object { '  ' + $_.Line.Trim() }"
)

echo.
echo OK: lt64k.exe generado desde %LT%
echo     codigo generado: out64k.c
endlocal

del *.obj *.map
