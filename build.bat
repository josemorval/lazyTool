@echo off
setlocal

:: lazyTool build script
::
:: Usage (run from VS Developer Command Prompt so cl.exe is in PATH):
::   build.bat             -> release build, normal multi-TU mode
::   build.bat unity       -> release build, unity mode: one TU for editor + one TU for player
::   build.bat run         -> build + run editor
::   build.bat unity run   -> unity build + run editor
::   build.bat copy        -> copy assets/projects/shaders to bin without compiling

set OUTDIR=bin
set SRCDIR=src
set EXTDIR=external

if not exist %OUTDIR% mkdir %OUTDIR%

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
 /D_CRT_SECURE_NO_WARNINGS

set RUN_AFTER=0
set COPY_ONLY=0
set UNITY_BUILD=0
set BUILD_MODE=multi-tu
set UNITY_DEFINES=

:parse_args
if /I "%1"=="run" (
    set RUN_AFTER=1
    shift
    goto parse_args
)
if /I "%1"=="copy" (
    set COPY_ONLY=1
    shift
    goto parse_args
)
if /I "%1"=="unity" (
    set UNITY_BUILD=1
    set BUILD_MODE=unity
    set UNITY_DEFINES=/DLAZYTOOL_UNITY_BUILD
    shift
    goto parse_args
)
if /I "%1"=="multi" (
    set UNITY_BUILD=0
    set BUILD_MODE=multi-tu
    set UNITY_DEFINES=
    shift
    goto parse_args
)
if not "%1"=="" (
    echo [WARN] Unknown argument: %1
    shift
    goto parse_args
)

if "%COPY_ONLY%"=="1" (
    call :copy_folders
    if errorlevel 1 (
        echo.
        echo [FAILED]
        exit /b 1
    )
    echo.
    echo [OK] copied folders to %OUTDIR%
    if "%RUN_AFTER%"=="1" if exist %OUTDIR%\lazyTool.exe (
        echo [RUN] %OUTDIR%\lazyTool.exe
        start "" %OUTDIR%\lazyTool.exe
    )
    exit /b 0
)

call :reset_outdir
if errorlevel 1 (
    echo.
    echo [FAILED]
    exit /b 1
)

:: Two executables are built from the same repo:
::   lazyTool.exe         editor + exporter
::   lazyPlayer.exe       normal packed player for asset-heavy projects
::
:: `unity` mode compiles src/unity_editor.cpp and src/unity_player.cpp. This is
:: usually faster for full release builds because Windows/D3D/imgui headers are
:: parsed once per executable instead of once per .cpp. Use `multi` or omit
:: `unity` when you want normal isolated translation units.
set CFLAGS=/W3 /O2 /MT /EHsc /nologo /std:c++17 /DNDEBUG /Gy /Gw /GF
set PLAYER_CFLAGS=/W3 /O1 /MT /EHsc /nologo /std:c++17 /DNDEBUG /DLAZYTOOL_PLAYER_ONLY /DLAZYTOOL_NO_LOG /Gy /Gw /GF

echo [BUILD] release %BUILD_MODE%

if "%UNITY_BUILD%"=="1" goto set_unity_sources
goto set_multi_sources

:set_unity_sources
set EDITOR_SRCS=%SRCDIR%\unity_editor.cpp
set PLAYER_SRCS=%SRCDIR%\unity_player.cpp
goto after_sources

:set_multi_sources
set EDITOR_SRCS=^
 %SRCDIR%\main.cpp ^
 %SRCDIR%\log.cpp ^
 %SRCDIR%\dx11_ctx.cpp ^
 %SRCDIR%\shader.cpp ^
 %SRCDIR%\resources.cpp ^
 %SRCDIR%\commands.cpp ^
 %SRCDIR%\project.cpp ^
 %SRCDIR%\app_settings.cpp ^
 %SRCDIR%\embedded_pack.cpp ^
 %SRCDIR%\timeline.cpp ^
 %SRCDIR%\user_cb.cpp ^
 %SRCDIR%\ui.cpp ^
 %SRCDIR%\impl.cpp ^
 %EXTDIR%\imgui\imgui.cpp ^
 %EXTDIR%\imgui\imgui_draw.cpp ^
 %EXTDIR%\imgui\imgui_tables.cpp ^
 %EXTDIR%\imgui\imgui_widgets.cpp ^
 %EXTDIR%\imgui\backends\imgui_impl_win32.cpp ^
 %EXTDIR%\imgui\backends\imgui_impl_dx11.cpp

set PLAYER_SRCS=^
 %SRCDIR%\main.cpp ^
 %SRCDIR%\log.cpp ^
 %SRCDIR%\dx11_ctx.cpp ^
 %SRCDIR%\shader.cpp ^
 %SRCDIR%\resources.cpp ^
 %SRCDIR%\commands.cpp ^
 %SRCDIR%\project.cpp ^
 %SRCDIR%\embedded_pack.cpp ^
 %SRCDIR%\timeline.cpp ^
 %SRCDIR%\user_cb.cpp ^
 %SRCDIR%\impl.cpp

goto after_sources

:after_sources
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
if errorlevel 1 (
    echo.
    echo [FAILED]
    exit /b 1
)

cl %CFLAGS% %UNITY_DEFINES% %DEFINES% %INCLUDES% %EDITOR_SRCS% %RES% ^
   /Fe:%OUTDIR%\lazyTool.exe ^
   /Fo:%OUTDIR%\ ^
   /Fd:%OUTDIR%\lazyTool.pdb ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO

if errorlevel 1 (
    echo.
    echo [FAILED]
    exit /b 1
)

echo.
echo [OK] %OUTDIR%\lazyTool.exe

if not exist %OUTDIR%\player mkdir %OUTDIR%\player
cl %PLAYER_CFLAGS% %UNITY_DEFINES% %DEFINES% %INCLUDES% %PLAYER_SRCS% %RES% ^
   /Fe:%OUTDIR%\lazyPlayer.exe ^
   /Fo:%OUTDIR%\player\ ^
   /Fd:%OUTDIR%\lazyPlayer.pdb ^
   /link %LIBS% /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /INCREMENTAL:NO /MANIFEST:NO

if errorlevel 1 (
    echo.
    echo [FAILED]
    exit /b 1
)

echo [OK] %OUTDIR%\lazyPlayer.exe

call :copy_folders
if errorlevel 1 (
    echo.
    echo [FAILED]
    exit /b 1
)

if "%RUN_AFTER%"=="1" (
    echo [RUN] %OUTDIR%\lazyTool.exe
    start "" %OUTDIR%\lazyTool.exe
)

endlocal
exit /b 0

:reset_outdir
if exist %OUTDIR% (
    rmdir /S /Q %OUTDIR%
    if exist %OUTDIR% (
        echo [ERROR] Could not clean %OUTDIR%. Check for locked files.
        exit /b 1
    )
)
mkdir %OUTDIR%
if not exist %OUTDIR% (
    echo [ERROR] Could not create %OUTDIR%.
    exit /b 1
)
exit /b 0

:copy_folders
if exist assets\NUL (
    xcopy assets %OUTDIR%\assets /E /I /Y >nul
    if errorlevel 4 exit /b %ERRORLEVEL%
)
if exist projects\NUL (
    xcopy projects %OUTDIR%\projects /E /I /Y >nul
    if errorlevel 4 exit /b %ERRORLEVEL%
)
if exist shaders\NUL (
    xcopy shaders %OUTDIR%\shaders /E /I /Y >nul
    if errorlevel 4 exit /b %ERRORLEVEL%
)
exit /b 0
