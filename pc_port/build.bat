@echo off
setlocal EnableDelayedExpansion
REM ==========================================================================
REM  Silent Hill PC Port - Windows build  (MSYS2 + MinGW-w64 + Ninja)
REM
REM  This is the canonical Windows build. It drives the MSYS2 toolchain that
REM  the project actually uses (Ninja generator + map DLLs). Do NOT use a
REM  Visual Studio generator - the project is built with MinGW-w64.
REM
REM  PREREQUISITES (one-time):
REM    1. Install MSYS2:  https://www.msys2.org/
REM    2. In an "MSYS2 MinGW x64" shell, install the toolchain:
REM         pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake \
REM             mingw-w64-x86_64-ninja mingw-w64-x86_64-SDL2 mingw-w64-x86_64-openal
REM       (optional ffmpeg FMV fallback, -DSH_FMV_FFMPEG=ON: also install
REM        mingw-w64-x86_64-ffmpeg for dev/link — see docs/fmv_files.md)
REM    3. Fetch the PsyCross submodule (from the repo root):
REM         git submodule update --init --recursive
REM    4. Put the disc image at:  pc_port\build\gamedata\Silent Hill (USA).bin
REM
REM  USAGE  (double-click for a menu, or run with an argument):
REM    build.bat                incremental build (configure first if needed)
REM    build.bat build          same as above
REM    build.bat rebuild        clean rebuild (cmake --build --clean-first)
REM    build.bat configure      force a fresh cmake configure, then build
REM    build.bat run            build, then launch the game
REM    build.bat nuke           wipe the build dir (keeps gamedata/save/config)
REM
REM  If MSYS2 is not at C:\msys64, set MSYS2_ROOT to your install, e.g.:
REM    set MSYS2_ROOT=D:\msys64 && build.bat
REM ==========================================================================

REM --- Paths (derived from this script's location: it lives in pc_port\) ----
set "PCPORT_DIR=%~dp0"
if "%PCPORT_DIR:~-1%"=="\" set "PCPORT_DIR=%PCPORT_DIR:~0,-1%"
set "BUILD_DIR=%PCPORT_DIR%\build"
set "EXE_PATH=%BUILD_DIR%\SilentHillPC.exe"

REM --- MSYS2 location (override with MSYS2_ROOT) ----------------------------
if not defined MSYS2_ROOT set "MSYS2_ROOT=C:\msys64"
set "MSYS_BASH=%MSYS2_ROOT%\usr\bin\bash.exe"

REM MSYSTEM tells the login shell to set up the mingw64 PATH itself, so we do
REM not hardcode the toolchain location.
set "MSYSTEM=MINGW64"

REM msys2 bash accepts Windows paths with forward slashes (cd 'C:/foo'), so a
REM simple backslash->slash swap is all the conversion we need - drive agnostic.
set "BD_UNIX=%BUILD_DIR:\=/%"

REM --- Sanity checks --------------------------------------------------------
if not exist "%MSYS_BASH%" (
    echo ERROR: MSYS2 bash not found at "%MSYS_BASH%".
    echo Install MSYS2 from https://www.msys2.org/  ^(or set MSYS2_ROOT to your install path^).
    pause & exit /b 1
)
if not exist "%PCPORT_DIR%\PsyCross\CMakeLists.txt" (
    echo ERROR: PsyCross submodule missing at "%PCPORT_DIR%\PsyCross".
    echo From the repo root run:  git submodule update --init --recursive
    pause & exit /b 1
)

REM --- Mode dispatch (arg = non-interactive; no arg = menu) -----------------
set "MODE=%~1"
set "INTERACTIVE="
if "%MODE%"=="" (
    set "INTERACTIVE=1"
    goto menu
)
goto dispatch

:menu
cls
echo.
echo  ===========================================================
echo   Silent Hill PC Port - Build  ^(MSYS2 + Ninja^)
echo  ===========================================================
echo.
echo   [1]  Incremental build      ^(fast; source-only edits^)
echo   [2]  Clean rebuild          ^(cmake --build --clean-first^)
echo   [3]  Build + Run
echo   [4]  Run only               ^(launch existing exe^)
echo   [5]  Reconfigure + build    ^(re-run cmake, keep objects^)
echo   [6]  Nuke + rebuild         ^(wipe build dir, keep gamedata/save/config^)
echo   [Q]  Quit
echo.
choice /c 123456Q /n /m "Pick an option: "
set "RC=%ERRORLEVEL%"
if "%RC%"=="1" ( set "MODE=build"       & goto dispatch )
if "%RC%"=="2" ( set "MODE=rebuild"     & goto dispatch )
if "%RC%"=="3" ( set "MODE=run"         & goto dispatch )
if "%RC%"=="4" ( set "MODE=runonly"     & goto dispatch )
if "%RC%"=="5" ( set "MODE=configure"   & goto dispatch )
if "%RC%"=="6" ( set "MODE=nuke"        & goto dispatch )
goto end

:dispatch
if /i "%MODE%"=="runonly" goto run_only
if /i "%MODE%"=="nuke"    goto nuke

REM Game must not be running - the linker can't overwrite a live exe
REM (this is the confusing "cannot open output file ... Permission denied").
tasklist /fi "imagename eq SilentHillPC.exe" 2>nul | find /i "SilentHillPC.exe" >nul
if not errorlevel 1 (
    echo ERROR: SilentHillPC.exe is running - close the game before building.
    goto fail
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

REM Configure when: explicit 'configure'/'nuke', or no cache yet.
set "NEED_CONFIGURE="
if /i "%MODE%"=="configure" set "NEED_CONFIGURE=1"
if not exist "%BUILD_DIR%\CMakeCache.txt" set "NEED_CONFIGURE=1"
if /i "%MODE%"=="configure" if exist "%BUILD_DIR%\CMakeCache.txt" del /f /q "%BUILD_DIR%\CMakeCache.txt"

if defined NEED_CONFIGURE (
    echo.
    echo === Configuring ^(Ninja, map DLLs ON^) ===
    "%MSYS_BASH%" -lc "cd '%BD_UNIX%' && cmake .. -G Ninja -DSH_BUILD_MAP_DLLS=ON"
    if errorlevel 1 ( echo CMake configure failed. & goto fail )
)

set "BUILD_CMD=cmake --build ."
if /i "%MODE%"=="rebuild" set "BUILD_CMD=cmake --build . --clean-first"

echo.
echo === Building ===
"%MSYS_BASH%" -lc "cd '%BD_UNIX%' && %BUILD_CMD%"
if errorlevel 1 ( echo Build failed. & goto fail )

if not exist "%EXE_PATH%" ( echo ERROR: SilentHillPC.exe was not produced. & goto fail )
for %%I in ("%EXE_PATH%") do echo Built: %%~fI  ^(%%~zI bytes, %%~tI^)

if /i "%MODE%"=="run" goto run_only
goto ok

:run_only
if not exist "%EXE_PATH%" ( echo ERROR: "%EXE_PATH%" does not exist - build first. & goto fail )
echo Launching SilentHillPC.exe...
pushd "%BUILD_DIR%" & start "" "%EXE_PATH%" & popd
goto end

:nuke
echo.
echo === NUKE: wipe build dir ^(keeping gamedata/save/config.cfg/launcher^) ===
if defined INTERACTIVE (
    choice /c YN /n /m "Are you sure? [Y/N]: "
    if errorlevel 2 goto menu
)
set "KEEP_DIR=%TEMP%\sh_keep_%RANDOM%"
mkdir "%KEEP_DIR%" 2>nul
if exist "%BUILD_DIR%\gamedata"                 move "%BUILD_DIR%\gamedata" "%KEEP_DIR%\" >nul
if exist "%BUILD_DIR%\save"                     move "%BUILD_DIR%\save" "%KEEP_DIR%\" >nul
if exist "%BUILD_DIR%\config.cfg"               move "%BUILD_DIR%\config.cfg" "%KEEP_DIR%\" >nul
if exist "%BUILD_DIR%\SilentHillPC_Launcher.exe" move "%BUILD_DIR%\SilentHillPC_Launcher.exe" "%KEEP_DIR%\" >nul
if exist "%BUILD_DIR%" rmdir /s /q "%BUILD_DIR%"
mkdir "%BUILD_DIR%"
if exist "%KEEP_DIR%\gamedata"                  move "%KEEP_DIR%\gamedata" "%BUILD_DIR%\" >nul
if exist "%KEEP_DIR%\save"                       move "%KEEP_DIR%\save" "%BUILD_DIR%\" >nul
if exist "%KEEP_DIR%\config.cfg"                 move "%KEEP_DIR%\config.cfg" "%BUILD_DIR%\" >nul
if exist "%KEEP_DIR%\SilentHillPC_Launcher.exe"  move "%KEEP_DIR%\SilentHillPC_Launcher.exe" "%BUILD_DIR%\" >nul
rmdir /s /q "%KEEP_DIR%" 2>nul
set "MODE=rebuild"
goto dispatch

:ok
echo.
echo  ===========================================================
echo   Build OK.
echo  ===========================================================
if defined INTERACTIVE ( echo Press any key to return to menu... & pause >nul & goto menu )
goto end

:fail
if defined INTERACTIVE ( echo. & pause & goto menu )
endlocal & exit /b 1

:end
endlocal
