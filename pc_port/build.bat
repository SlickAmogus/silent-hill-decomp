@echo off
REM Silent Hill PC Port - Windows Build Script
REM
REM Prerequisites:
REM   - CMake 3.16+
REM   - Visual Studio 2019+ (or MinGW-w64)
REM   - SDL2 development libraries
REM   - OpenAL-soft development libraries
REM
REM PsyCross must be cloned alongside the decomp repo:
REM   silenthill/
REM     silent-hill-decomp/
REM     PsyCross/

echo === Silent Hill PC Port Build ===
echo.

REM Create build directory
if not exist "build" mkdir build
cd build

REM Configure with CMake
cmake .. -G "Visual Studio 17 2022" -A x64 ^
    -DPSYCROSS_DIR="%~dp0..\..\PsyCross" ^
    -DSH_VERSION=USA ^
    -DSH_DEBUG=ON

if errorlevel 1 (
    echo.
    echo CMake configuration failed!
    echo Make sure SDL2 and OpenAL are installed and findable by CMake.
    echo You may need to set SDL2_DIR and OPENAL_DIR environment variables.
    pause
    exit /b 1
)

echo.
echo CMake configured successfully.
echo.
echo To build:
echo   cd build
echo   cmake --build . --config Debug
echo.
echo Or open build\SilentHillPC.sln in Visual Studio.

cd ..
