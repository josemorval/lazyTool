@echo off
setlocal EnableExtensions

rem lazyTool unity-only build script.
rem Run from a Visual Studio Developer Command Prompt.
rem
rem Usage:
rem   build.bat                 -> dev unity build, editor + player
rem   build.bat run             -> dev unity build, editor + player, then run editor
rem   build.bat editor run      -> dev unity build, editor only, then run editor
rem   build.bat player          -> dev unity build, player only
rem   build.bat release         -> optimized unity build, editor + player
rem   build.bat clean           -> remove bin
rem   build.bat copy            -> copy projects/shaders/assets only

set OUTDIR=bin
set SRCDIR=src
set EXTDIR=external
set TARGET=all
set CONFIG=dev
set RUN_AFTER=0
set COPY_ONLY=0
set CLEAN_ONLY=0

:parse_args
if "%~1"=="" goto args_done
if /I "%~1"=="all" (
    set TARGET=all
    shift
    goto parse_args
)
if /I "%~1"=="editor" (
    set TARGET=editor
    shift
    goto parse_args
)
if /I "%~1"=="player" (
    set TARGET=player
    shift
    goto parse_args
)
if /I "%~1"=="dev" (
    set CONFIG=dev
    shift
    goto parse_args
)
if /I "%~1"=="release" (
    set CONFIG=release
    shift
    goto parse_args
)
if /I "%~1"=="run" (
    set RUN_AFTER=1
    shift
    goto parse_args
)
if /I "%~1"=="copy" (
    set COPY_ONLY=1
    shift
    goto parse_args
)
if /I "%~1"=="clean" (
    set CLEAN_ONLY=1
    shift
    goto parse_args
)

echo [ERROR] Unknown argument: %~1
echo Usage: build.bat [all^|editor^|player] [dev^|release] [run^|copy^|clean]
exit /b 1

:args_done
if "%CLEAN_ONLY%"=="1" (
    call :clean_bin
    exit /b %ERRORLEVEL%
)

if not exist "%OUTDIR%" mkdir "%OUTDIR%"
if not exist "%OUTDIR%" (
    echo [ERROR] Could not create %OUTDIR%.
    exit /b 1
)

if "%COPY_ONLY%"=="1" (
    call :copy_folders
    if errorlevel 1 goto failed
    echo [OK] copied folders to %OUTDIR%
    exit /b 0
)

if "%RUN_AFTER%"=="1" if /I "%TARGET%"=="player" (
    echo [ERROR] run requires an editor build. Use build.bat editor run or build.bat all run.
    exit /b 1
)

set INCLUDES=^
 /I%SRCDIR% ^
 /I%EXTDIR% ^
 /I%EXTDIR%\imgui ^
 /I%EXTDIR%\imgui\backends ^
 /I%EXTDIR%\cgltf ^
 /I%EXTDIR%\stb

set DEFINES=^
 /DWIN32 /D_WINDOWS ^
 /DUNICODE /D_UNICODE ^
 /D_CRT_SECURE_NO_WARNINGS ^
 /DLAZYTOOL_UNITY_BUILD

set COMMON_CFLAGS=/nologo /std:c++17 /EHsc /W3 /MT /DNDEBUG
set DEV_CFLAGS=/Od /Ob0
set RELEASE_CFLAGS=/O2 /Ob2 /Gy /Gw /GF
set COMMON_LINK=/SUBSYSTEM:WINDOWS
set DEV_LINK=/INCREMENTAL:NO
set RELEASE_LINK=/OPT:REF /OPT:ICF /INCREMENTAL:NO

if /I "%CONFIG%"=="release" (
    set EDITOR_CFLAGS=%COMMON_CFLAGS% %RELEASE_CFLAGS%
    set PLAYER_CFLAGS=%COMMON_CFLAGS% %RELEASE_CFLAGS% /DLAZYTOOL_PLAYER_ONLY /DLAZYTOOL_NO_LOG
    set LINK_FLAGS=%COMMON_LINK% %RELEASE_LINK%
) else (
    set EDITOR_CFLAGS=%COMMON_CFLAGS% %DEV_CFLAGS%
    set PLAYER_CFLAGS=%COMMON_CFLAGS% %DEV_CFLAGS% /DLAZYTOOL_PLAYER_ONLY /DLAZYTOOL_NO_LOG
    set LINK_FLAGS=%COMMON_LINK% %DEV_LINK%
)

set LIBS=^
 d3d11.lib ^
 dxgi.lib ^
 d3dcompiler.lib ^
 user32.lib ^
 gdi32.lib ^
 shell32.lib ^
 psapi.lib

set RES=%OUTDIR%\lazyTool.res
rc /nologo /fo%RES% app.rc
if errorlevel 1 goto failed

echo [BUILD] %CONFIG% unity %TARGET%

if /I "%TARGET%"=="player" goto build_player

:build_editor
if not exist "%OUTDIR%\obj_editor" mkdir "%OUTDIR%\obj_editor"
cl %EDITOR_CFLAGS% %DEFINES% %INCLUDES% %SRCDIR%\unity_editor.cpp %RES% ^
   /Fe:%OUTDIR%\lazyTool.exe ^
   /Fo:%OUTDIR%\obj_editor\ ^
   /link %LIBS% %LINK_FLAGS%
if errorlevel 1 goto failed

echo [OK] %OUTDIR%\lazyTool.exe
if /I "%TARGET%"=="editor" goto after_builds

:build_player
if not exist "%OUTDIR%\obj_player" mkdir "%OUTDIR%\obj_player"
cl %PLAYER_CFLAGS% %DEFINES% %INCLUDES% %SRCDIR%\unity_player.cpp %RES% ^
   /Fe:%OUTDIR%\lazyPlayer.exe ^
   /Fo:%OUTDIR%\obj_player\ ^
   /link %LIBS% %LINK_FLAGS% /MANIFEST:NO
if errorlevel 1 goto failed

echo [OK] %OUTDIR%\lazyPlayer.exe

:after_builds
call :copy_folders
if errorlevel 1 goto failed

if "%RUN_AFTER%"=="1" (
    if exist "%OUTDIR%\lazyTool.exe" (
        echo [RUN] %OUTDIR%\lazyTool.exe
        start "" "%OUTDIR%\lazyTool.exe"
    ) else (
        echo [ERROR] Cannot run editor because %OUTDIR%\lazyTool.exe was not built.
        exit /b 1
    )
)

endlocal
exit /b 0

:failed
echo.
echo [FAILED]
exit /b 1

:clean_bin
if exist "%OUTDIR%" (
    rmdir /S /Q "%OUTDIR%"
    if exist "%OUTDIR%" (
        echo [ERROR] Could not clean %OUTDIR%. Check for locked files.
        exit /b 1
    )
)
echo [OK] cleaned %OUTDIR%
exit /b 0

:copy_folders
if exist assets\NUL xcopy assets "%OUTDIR%\assets" /D /E /I /Y >nul
if errorlevel 4 exit /b %ERRORLEVEL%
if exist projects\NUL xcopy projects "%OUTDIR%\projects" /D /E /I /Y >nul
if errorlevel 4 exit /b %ERRORLEVEL%
if exist shaders\NUL xcopy shaders "%OUTDIR%\shaders" /D /E /I /Y >nul
if errorlevel 4 exit /b %ERRORLEVEL%
exit /b 0
