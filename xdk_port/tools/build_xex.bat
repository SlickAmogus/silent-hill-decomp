@echo off
rem build_xex.bat - build hello_xex.xex with the Xbox 360 XDK.
rem
rem Needs the FULL XDK (compiler + include\xbox + lib\xbox), which the
rem tools-only install lacks: install VS2010, then re-run XDKSetupXenon
rem with all features. Never copy XDK files into the repo.
setlocal
if "%XEDK%"=="" set XEDK=C:\Program Files (x86)\Microsoft Xbox 360 SDK
set CL=%XEDK%\bin\win32\cl.exe
if not exist "%CL%" (
  echo ERROR: %CL% not found - the XDK compiler is not installed yet.
  echo Install VS2010, then re-run XDKSetupXenon21256.3.exe with all features.
  exit /b 1
)
set OUT=%~dp0..\bin
if not exist "%OUT%" mkdir "%OUT%"

"%CL%" /nologo /O2 /Ox /Zi /GS- /D _XBOX /I "%XEDK%\include\xbox" ^
    %~dp0..\src\hello_xex.c ^
    /link /libpath:"%XEDK%\lib\xbox" xapilibi.lib d3d9i.lib xboxkrnl.lib ^
    /machine:PPCBE /subsystem:xbox /out:"%OUT%\hello_xex.exe"
if errorlevel 1 exit /b 1

"%XEDK%\bin\win32\imagexex.exe" /nologo "%OUT%\hello_xex.exe" /out:"%OUT%\hello_xex.xex"
if errorlevel 1 exit /b 1
echo.
echo Built %OUT%\hello_xex.xex - copy its folder to the console and launch from xbGuard.
