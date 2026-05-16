@echo off
setlocal

rem Usage:
rem   build.bat          profile build, clean first, run editor
rem   build.bat profile  profile build, clean first, run editor
rem   build.bat release  release build, clean first, run editor

set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=profile

if /I "%CONFIG%"=="profile" goto config_ok
if /I "%CONFIG%"=="release" goto config_ok
echo Usage: build.bat [profile^|release]
exit /b 1

:config_ok
echo [CLEAN]
taskkill /IM lazyTool.exe /F >nul 2>nul
taskkill /IM lazyPlayer.exe /F >nul 2>nul
if exist bin rmdir /S /Q bin
mkdir bin
mkdir bin\obj_editor
mkdir bin\obj_player

set INCLUDES=/Isrc /Iexternal /Iexternal\imgui /Iexternal\imgui\backends /Iexternal\cgltf /Iexternal\stb
set DEFINES=/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DLAZYTOOL_UNITY_BUILD
set LIBS=d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib psapi.lib
set COMMON=/nologo /std:c++17 /EHsc /W3 /MT /DNDEBUG

if /I "%CONFIG%"=="release" (
    set EDITOR_FLAGS=/O2 /Ob2 /Gy /Gw /GF /DLAZYTOOL_BUILD_CONFIG=\"release\"
    set PLAYER_FLAGS=/O1 /Ob2 /Gy /Gw /GF /GR- /GS- /Zc:inline /DLAZYTOOL_BUILD_CONFIG=\"release\"
    set EDITOR_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
    set PLAYER_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
) else (
    set EDITOR_FLAGS=/O2 /Ob2 /DLAZYTOOL_BUILD_CONFIG=\"profile\"
    set PLAYER_FLAGS=/O1 /Ob2 /Gy /Gw /GF /GR- /GS- /Zc:inline /DLAZYTOOL_BUILD_CONFIG=\"profile\"
    set EDITOR_LINK=/SUBSYSTEM:WINDOWS /INCREMENTAL
    set PLAYER_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO
)

echo [BUILD] %CONFIG%
rc /nologo /fobin\lazyTool.res app.rc || goto failed

cl %COMMON% %EDITOR_FLAGS% %DEFINES% %INCLUDES% src\unity_editor.cpp bin\lazyTool.res ^
   /Fe:bin\lazyTool.exe /Fo:bin\obj_editor\ ^
   /link %LIBS% %EDITOR_LINK% || goto failed

cl %COMMON% %PLAYER_FLAGS% %DEFINES% %INCLUDES% /DLAZYTOOL_PLAYER_ONLY /DLAZYTOOL_NO_LOG src\unity_player.cpp bin\lazyTool.res ^
   /Fe:bin\lazyPlayer.exe /Fo:bin\obj_player\ ^
   /link %LIBS% %PLAYER_LINK% /MANIFEST:NO || goto failed

if exist assets\NUL xcopy assets bin\assets /E /I /Y >nul
if exist projects\NUL xcopy projects bin\projects /E /I /Y >nul
if exist shaders\NUL xcopy shaders bin\shaders /E /I /Y >nul

echo [RUN] bin\lazyTool.exe
start "" bin\lazyTool.exe
exit /b 0

:failed
echo [FAILED]
exit /b 1
