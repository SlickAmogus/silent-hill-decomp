@echo off
rem Builds SDL2's WinRT backend for x64 UWP with Ninja; the vcpkg port wants
rem the Visual Studio generator, which needs the UWP workload.
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat" x64 store 10.0.26100.0 >nul || exit /b 1
set SRC=%1
set OUT=%2
set PREFIX=%3
cmake -S "%SRC%" -B "%OUT%" -G Ninja -DCMAKE_BUILD_TYPE=Release ^
  -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 ^
  -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl ^
  -DCMAKE_INSTALL_PREFIX="%PREFIX%" ^
  -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF -DSDL_VULKAN=OFF -DSDL_LIBC=ON ^
  -DSDL_INSTALL_CMAKEDIR=cmake > "%OUT%_configure.log" 2>&1 || exit /b 2
cmake --build "%OUT%" > "%OUT%_build.log" 2>&1 || exit /b 3
cmake --install "%OUT%" > "%OUT%_install.log" 2>&1 || exit /b 4
