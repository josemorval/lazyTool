@echo off
setlocal

rem Usage:
rem   build.bat              fast build, clean first, run editor
rem   build.bat fast         fast build, clean first, run editor
rem   build.bat profile      profile build, clean first, run editor
rem   build.bat release      release build, clean first, run editor
rem   build.bat fast norun   fast build, clean first, do not run editor

set CONFIG=%~1
set RUN_AFTER_BUILD=1
if "%CONFIG%"=="" set CONFIG=fast
if /I "%CONFIG%"=="norun" (
    set CONFIG=fast
    set RUN_AFTER_BUILD=0
)
if /I "%CONFIG%"=="build" (
    set CONFIG=fast
    set RUN_AFTER_BUILD=0
)
if /I "%CONFIG%"=="compile" (
    set CONFIG=fast
    set RUN_AFTER_BUILD=0
)
if /I "%~2"=="norun" set RUN_AFTER_BUILD=0
if /I "%~2"=="build" set RUN_AFTER_BUILD=0
if /I "%~2"=="compile" set RUN_AFTER_BUILD=0

if /I "%CONFIG%"=="fast" goto config_ok
if /I "%CONFIG%"=="profile" goto config_ok
if /I "%CONFIG%"=="release" goto config_ok
echo Usage: build.bat [fast^|profile^|release] [norun]
exit /b 1

:config_ok
set "ROOT=%CD%"
call :clock TOTAL_START
call :clock STEP_START

echo [CLEAN]
taskkill /IM lazyTool.exe /F >nul 2>nul
taskkill /IM lazyPlayer.exe /F >nul 2>nul
if exist bin rmdir /S /Q bin
mkdir bin
mkdir bin\obj_editor
mkdir bin\obj_player
mkdir bin\generated
if not exist build mkdir build
rem Persist the monotonic counter only after a successful build. The visible
rem build code folds local yyMMddHHmm + counter into base36 so it looks compact.
set "BUILD_NUMBER_FILE=build\build_number.txt"
set /a LAST_BUILD_NUMBER=0
if exist "%BUILD_NUMBER_FILE%" set /p LAST_BUILD_NUMBER=<"%BUILD_NUMBER_FILE%"
set /a BUILD_NUMBER=LAST_BUILD_NUMBER+1
set "BUILD_CODE=%BUILD_NUMBER%"
for /f "usebackq delims=" %%B in (`powershell -NoProfile -ExecutionPolicy Bypass -Command "$n=[int64]$env:BUILD_NUMBER; $stamp=[int64](Get-Date -Format 'yyMMddHHmm'); $chars='0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'; function b36([int64]$v){ $s=''; do { $s=$chars[[int]($v%%36)]+$s; $v=[math]::Floor($v/36) } while ($v -gt 0); $s }; 'LT'+(b36 $stamp)+'-'+(b36 $n)"`) do set "BUILD_CODE=%%B"
(
    echo #pragma once
    echo #define LAZYTOOL_BUILD_NUMBER %BUILD_NUMBER%
    echo #define LAZYTOOL_BUILD_NUMBER_STR "%BUILD_NUMBER%"
    echo #define LAZYTOOL_BUILD_CODE_STR "%BUILD_CODE%"
) > bin\generated\build_info.h
echo [BUILD] %BUILD_CODE% ^(#%BUILD_NUMBER%^)
set "RELEASE_ZIP=dist\lazyTool_build_%BUILD_CODE%.zip"
call :stamp STEP_START "Clean"

set "INCLUDES=/I""%ROOT%\src"" /I""%ROOT%\bin\generated"" /I""%ROOT%\external"" /I""%ROOT%\external\imgui"" /I""%ROOT%\external\imgui\backends"" /I""%ROOT%\external\cgltf"" /I""%ROOT%\external\stb"""
set "DEFINES=/DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS /DLAZYTOOL_UNITY_BUILD"
set "LIBS=d3d11.lib dxgi.lib d3dcompiler.lib user32.lib gdi32.lib shell32.lib psapi.lib winmm.lib"
set "COMMON=/nologo /std:c++17 /EHsc /W3 /MT /DNDEBUG"

if /I "%CONFIG%"=="release" (
    set "EDITOR_FLAGS=/O2 /Ob2 /Gy /Gw /GF /DLAZYTOOL_BUILD_CONFIG=\"release\" /DLAZYTOOL_CONFIG_RELEASE=1"
    set "PLAYER_FLAGS=/O1 /Ob2 /Gy /Gw /GF /GR- /GS- /Zc:inline /DLAZYTOOL_BUILD_CONFIG=\"release\" /DLAZYTOOL_CONFIG_RELEASE=1"
    set "EDITOR_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO"
    set "PLAYER_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO"
) else if /I "%CONFIG%"=="fast" (
    set "EDITOR_FLAGS=/Od /Ob0 /DLAZYTOOL_BUILD_CONFIG=\"fast\" /DLAZYTOOL_CONFIG_FAST=1"
    set "PLAYER_FLAGS=/Od /Ob0 /GR- /DLAZYTOOL_BUILD_CONFIG=\"fast\" /DLAZYTOOL_CONFIG_FAST=1"
    set "EDITOR_LINK=/SUBSYSTEM:WINDOWS /INCREMENTAL"
    set "PLAYER_LINK=/SUBSYSTEM:WINDOWS /INCREMENTAL"
) else (
    set "EDITOR_FLAGS=/O2 /Ob2 /DLAZYTOOL_BUILD_CONFIG=\"profile\" /DLAZYTOOL_CONFIG_PROFILE=1"
    set "PLAYER_FLAGS=/O1 /Ob2 /Gy /Gw /GF /GR- /GS- /Zc:inline /DLAZYTOOL_BUILD_CONFIG=\"profile\" /DLAZYTOOL_CONFIG_PROFILE=1"
    set "EDITOR_LINK=/SUBSYSTEM:WINDOWS /INCREMENTAL"
    set "PLAYER_LINK=/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO"
)

echo [BUILD] %CONFIG%
call :clock STEP_START
rc /nologo /fobin\lazyTool.res app.rc || goto failed

cl %COMMON% %EDITOR_FLAGS% %DEFINES% %INCLUDES% "%ROOT%\src\unity_editor.cpp" bin\lazyTool.res ^
   /Fe:bin\lazyTool.exe /Fo:bin\obj_editor\ ^
   /link %LIBS% %EDITOR_LINK% || goto failed

cl %COMMON% %PLAYER_FLAGS% %DEFINES% %INCLUDES% /DLAZYTOOL_PLAYER_ONLY /DLAZYTOOL_NO_LOG "%ROOT%\src\unity_player.cpp" bin\lazyTool.res ^
   /Fe:bin\lazyPlayer.exe /Fo:bin\obj_player\ ^
   /link %LIBS% %PLAYER_LINK% /MANIFEST:NO || goto failed
call :stamp STEP_START "Compile"

call :clock STEP_START
if exist assets\NUL xcopy assets bin\assets /E /I /Y >nul
if exist projects\NUL xcopy projects bin\projects /E /I /Y >nul
if exist shaders\NUL xcopy shaders bin\shaders /E /I /Y >nul
call :stamp STEP_START "Copy"

if /I "%CONFIG%"=="release" (
    call :clock STEP_START
    if exist bin\obj_editor rmdir /S /Q bin\obj_editor
    if exist bin\obj_player rmdir /S /Q bin\obj_player
    if exist bin\generated rmdir /S /Q bin\generated
    del /Q bin\*.obj bin\*.res bin\*.exp bin\*.lib bin\*.ilk bin\*.pdb bin\*.idb bin\*.manifest 2>nul
    call :stamp STEP_START "Release cleanup"

    call :clock STEP_START
    if not exist dist mkdir dist
    if exist "%RELEASE_ZIP%" del /Q "%RELEASE_ZIP%"
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Compress-Archive -Path 'bin\*' -DestinationPath '%RELEASE_ZIP%' -CompressionLevel Fastest -Force" || goto failed
    echo [ZIP] %RELEASE_ZIP%
    call :stamp STEP_START "Release zip"
)

> "%BUILD_NUMBER_FILE%" echo %BUILD_NUMBER%
call :stamp TOTAL_START "Total"

if "%RUN_AFTER_BUILD%"=="0" (
    echo [READY] bin\lazyTool.exe
    exit /b 0
)

echo [RUN] bin\lazyTool.exe
start "" bin\lazyTool.exe
exit /b 0

:failed
call :stamp TOTAL_START "Total"
echo [FAILED]
exit /b 1

:clock
for /f "tokens=1-4 delims=:.," %%a in ("%time: =0%") do set /a "%~1=(((1%%a-100)*60+1%%b-100)*60+1%%c-100)*100+1%%d-100"
exit /b

:stamp
call :clock NOW
set /a DT=NOW-%~1
if %DT% LSS 0 set /a DT+=8640000
set /a S=DT/100, C=DT%%100
if %C% LSS 10 (echo [TIME] %~2 %S%.0%C%s) else echo [TIME] %~2 %S%.%C%s
exit /b
