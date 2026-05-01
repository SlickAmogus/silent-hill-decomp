@echo off
REM ==========================================================================
REM run_capture.bat — launch SilentHillPC.exe with crash-friendly capture
REM
REM Drops next to SilentHillPC.exe in the build directory. Just double-click
REM (or run from a terminal) to launch.
REM
REM What this captures:
REM   - SilentHill.log         <- all SH_DBG output (already happens normally)
REM   - stdout_capture.txt     <- anything printed to stdout that wasn't
REM                               redirected by main_pc.c (mostly empty unless
REM                               show_console=1 in config.cfg)
REM   - stderr_capture.txt     <- anything to stderr (PsyCross errors, GL,
REM                               libc warnings, abort() messages, etc.)
REM   - crash_dumps\*.dmp      <- minidumps if procdump.exe is in PATH
REM
REM SETUP: download ProcDump from
REM   https://learn.microsoft.com/sysinternals/downloads/procdump
REM and either drop procdump.exe into this folder OR add its folder to PATH.
REM Without procdump, the script still runs and captures stdout/stderr/log;
REM you just don't get a stack-trace .dmp on crash.
REM
REM USAGE:
REM   1. Run this file (double-click or `run_capture.bat`).
REM   2. Reproduce the bug.
REM   3. If the game crashes / hangs, close it.
REM   4. Send back: SilentHill.log + stderr_capture.txt + crash_dumps\*.dmp
REM ==========================================================================

setlocal
cd /d "%~dp0"

REM make sure crash dump folder exists
if not exist crash_dumps mkdir crash_dumps

REM Try to launch ProcDump in the background watching for any crash on the
REM exe's PID.  Procdump exits cleanly when the watched process exits.
where procdump.exe >nul 2>nul
if %ERRORLEVEL%==0 (
    echo [capture] procdump.exe found — minidump on crash will be written to crash_dumps\
    start "procdump" /B procdump.exe -accepteula -ma -e -x crash_dumps\ SilentHillPC.exe
    REM small pause so procdump attaches before exe runs
    timeout /t 1 /nobreak >nul
    REM ProcDump's -x mode launches the exe itself; nothing more to do
) else (
    echo [capture] procdump.exe not in PATH — skipping crash-dump capture.
    echo [capture]   To enable: drop procdump.exe in this folder, or add its
    echo [capture]   directory to PATH. Download from:
    echo [capture]   https://learn.microsoft.com/sysinternals/downloads/procdump
    echo.
    SilentHillPC.exe 1>stdout_capture.txt 2>stderr_capture.txt
)

echo.
echo [capture] Process exited with code %ERRORLEVEL%
echo [capture] Logs:
echo   - SilentHill.log         (game SH_DBG log)
echo   - stdout_capture.txt
echo   - stderr_capture.txt
if exist crash_dumps\*.dmp (
    echo   - crash_dumps\*.dmp     (open in Visual Studio or WinDbg for stack)
)
echo.
echo Press any key to close this window...
pause >nul
