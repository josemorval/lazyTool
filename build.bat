@echo off
setlocal

:: lazyTool build script
::
:: Usage (run from VS Developer Command Prompt so cl.exe is in PATH):
::   build.bat        -> release build
::   build.bat run    -> release build + run
::   build.bat copy   -> copy assets/projects/shaders to bin without compiling

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
if not "%1"=="" (
    echo [WARN] Unknown argument: %1
    shift
    goto parse_args
)

if "%COPY_ONLY%"=="1" (
    call :copy_folders
    if %ERRORLEVEL% neq 0 (
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

set CFLAGS=/W3 /O2 /MT /EHsc /nologo /std:c++17 /DNDEBUG
echo [BUILD] release

set SRCS=^
 %SRCDIR%\main.cpp ^
 %SRCDIR%\log.cpp ^
 %SRCDIR%\dx11_ctx.cpp ^
 %SRCDIR%\shader.cpp ^
 %SRCDIR%\resources.cpp ^
 %SRCDIR%\commands.cpp ^
 %SRCDIR%\project.cpp ^
 %SRCDIR%\app_settings.cpp ^
 %SRCDIR%\user_cb.cpp ^
 %SRCDIR%\ui.cpp ^
 %SRCDIR%\impl.cpp ^
 %EXTDIR%\imgui\imgui.cpp ^
 %EXTDIR%\imgui\imgui_draw.cpp ^
 %EXTDIR%\imgui\imgui_tables.cpp ^
 %EXTDIR%\imgui\imgui_widgets.cpp ^
 %EXTDIR%\imgui\backends\imgui_impl_win32.cpp ^
 %EXTDIR%\imgui\backends\imgui_impl_dx11.cpp

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
if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED]
    exit /b 1
)

cl %CFLAGS% %DEFINES% %INCLUDES% %SRCS% %RES% ^
   /Fe:%OUTDIR%\lazyTool.exe ^
   /Fo:%OUTDIR%\ ^
   /Fd:%OUTDIR%\lazyTool.pdb ^
   /link %LIBS% /SUBSYSTEM:WINDOWS

if %ERRORLEVEL% neq 0 (
    echo.
    echo [FAILED]
    exit /b 1
)

echo.
echo [OK] %OUTDIR%\lazyTool.exe

call :copy_folders
if %ERRORLEVEL% neq 0 (
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

:copy_folders
xcopy assets %OUTDIR%\assets /E /I /Y >nul
if %ERRORLEVEL% geq 4 exit /b %ERRORLEVEL%
xcopy projects %OUTDIR%\projects /E /I /Y >nul
if %ERRORLEVEL% geq 4 exit /b %ERRORLEVEL%
xcopy shaders %OUTDIR%\shaders /E /I /Y >nul
if %ERRORLEVEL% geq 4 exit /b %ERRORLEVEL%
exit /b 0
