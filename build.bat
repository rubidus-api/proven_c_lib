@echo off
setlocal EnableDelayedExpansion

set COMPILER=msvc
set COMPILER_CMD=cl.exe

if /I "%~1"=="" goto run_build
if /I "%~1"=="-h" goto show_help
if /I "%~1"=="--help" goto show_help

:: Determine compiler explicitly
if /I "%~1"=="msvc"  ( set COMPILER=msvc& set COMPILER_CMD=cl.exe& shift & goto collect_flags )
if /I "%~1"=="gcc"   ( set COMPILER=gcc& set COMPILER_CMD=gcc.exe& shift & goto collect_flags )
if /I "%~1"=="clang" ( set COMPILER=clang& set COMPILER_CMD=clang.exe& shift & goto collect_flags )
if /I "%~1"=="pocc"  ( set COMPILER=pocc& set COMPILER_CMD=cc.exe& shift & goto collect_flags )
if /I "%~1"=="icx"   ( set COMPILER=icx& set COMPILER_CMD=icx.exe& shift & goto collect_flags )

:collect_flags
set FLAGS=
:loop_flags
if "%~1"=="" goto run_build
set FLAGS=!FLAGS! %1
shift
goto loop_flags

:show_help
echo Usage: build.bat [COMPILER] [OPTIONS...]
echo   COMPILER: msvc (default), gcc, clang, pocc, icx
echo   OPTIONS : Flags passed directly to compilation (e.g. /O2, -O3, /std:c17)
echo Examples:
echo   build.bat                 (Uses MSVC)
echo   build.bat msvc /O2 /MD    (Uses MSVC with optimization)
echo   build.bat pocc /Ze /O2    (Uses Pelles C with optimizations)
echo   build.bat gcc -O3 -Wall   (Uses GCC with standard flags)
exit /b 0

:run_build
:: Export for nob.c runtime evaluation
set NOB_COMPILER=!COMPILER!
set CC=!COMPILER_CMD!

echo =^> 1. Compiling build script (nob.c) with !COMPILER_CMD!...

if /I "!COMPILER!"=="msvc" (
    cl.exe /nologo /EHsc nob.c /Fenob.exe
    if exist nob.obj del nob.obj
) else if /I "!COMPILER!"=="pocc" (
    cc.exe /Ze nob.c /OUT:nob.exe
    if exist nob.obj del nob.obj
) else if /I "!COMPILER!"=="icx" (
    icx.exe /nologo nob.c /Fenob.exe
    if exist nob.obj del nob.obj
) else (
    !COMPILER_CMD! -o nob.exe nob.c
)

if !ERRORLEVEL! neq 0 ( exit /b !ERRORLEVEL! )

echo =^> 2. Running build script with custom options...
nob.exe !FLAGS!

