@echo off
setlocal enabledelayedexpansion

set "HERE=%~dp0"
if not defined JAVA_HOME (
    if exist "C:\Users\sergi\scoop\apps\zulu17-jdk\current" (
        set "JAVA_HOME=C:\Users\sergi\scoop\apps\zulu17-jdk\current"
    )
)

set "TASK=%~1"
if "%TASK%"=="" set "TASK=assembleDebug"

cd /d "%HERE%"
call "%HERE%gradlew.bat" ":app:%TASK%"
if errorlevel 1 exit /b %errorlevel%

set "APK=%HERE%app\build\outputs\apk\debug\app-debug.apk"
if exist "%APK%" (
    echo.
    echo APK: %APK%
    echo Install with: adb install -r "%APK%"
)
