# SPDX-License-Identifier: GPL-3.0-or-later
#
# UWP (Xbox One / Series dev mode, Windows 10+) toolchain: clang with the GNU
# command line, MSVC ABI, linked against the Store flavour of the VC runtime.
#
# Clang rather than cl because the decomp is GNU C (statement expressions,
# __attribute__, -include), and the MSVC ABI rather than MinGW because the
# libraries a UWP app has to load -- SDL2's WinRT backend, ANGLE, the app-local
# VC runtime -- are all MSVC builds, and an Xbox has no msvcrt.dll for a MinGW
# binary to bind to.
#
# Needs no Visual Studio UWP workload: the Store runtime libraries ship with the
# ordinary C++ toolset (VC/Tools/MSVC/<ver>/lib/x64/store) and WindowsApp.lib
# with the Windows SDK.
#
#   cmake -G Ninja -DCMAKE_TOOLCHAIN_FILE=uwp/toolchain-uwp-clang.cmake \
#         -DSH_UWP_DEPS=<vcpkg>/installed/x64-uwp ..

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR AMD64)

set(SH_UWP ON CACHE BOOL "Building the UWP package" FORCE)

set(SH_UWP_LLVM "C:/Program Files/LLVM" CACHE PATH "LLVM install (clang, lld-link, llvm-objcopy)")
set(SH_UWP_VCTOOLS "" CACHE PATH "MSVC toolset dir (VC/Tools/MSVC/<ver>); newest VS 2022 toolset if empty")
set(SH_UWP_WINSDK "C:/Program Files (x86)/Windows Kits/10" CACHE PATH "Windows 10 SDK root")
set(SH_UWP_WINSDK_VER "" CACHE STRING "Windows SDK version; newest installed if empty")

# try_compile() re-runs this file in a scratch project that does not see the
# cache, so the resolved paths have to be handed down explicitly.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES
     SH_UWP_LLVM SH_UWP_VCTOOLS SH_UWP_WINSDK SH_UWP_WINSDK_VER)

if(NOT SH_UWP_VCTOOLS)
    file(GLOB _vc_candidates
         "C:/Program Files/Microsoft Visual Studio/2022/*/VC/Tools/MSVC/*")
    list(SORT _vc_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(_c IN LISTS _vc_candidates)
        if(EXISTS "${_c}/lib/x64/store/msvcrt.lib")
            set(SH_UWP_VCTOOLS "${_c}")
            break()
        endif()
    endforeach()
endif()
if(NOT SH_UWP_VCTOOLS)
    message(FATAL_ERROR "No MSVC toolset with lib/x64/store found; set SH_UWP_VCTOOLS.")
endif()

if(NOT SH_UWP_WINSDK_VER)
    file(GLOB _sdk_candidates RELATIVE "${SH_UWP_WINSDK}/Lib" "${SH_UWP_WINSDK}/Lib/10.*")
    list(SORT _sdk_candidates COMPARE NATURAL ORDER DESCENDING)
    foreach(_c IN LISTS _sdk_candidates)
        if(EXISTS "${SH_UWP_WINSDK}/Lib/${_c}/um/x64/WindowsApp.lib")
            set(SH_UWP_WINSDK_VER "${_c}")
            break()
        endif()
    endforeach()
endif()
if(NOT SH_UWP_WINSDK_VER)
    message(FATAL_ERROR "No Windows SDK with WindowsApp.lib found; set SH_UWP_WINSDK_VER.")
endif()

set(CMAKE_C_COMPILER   "${SH_UWP_LLVM}/bin/clang.exe")
set(CMAKE_CXX_COMPILER "${SH_UWP_LLVM}/bin/clang++.exe")
set(CMAKE_AR           "${SH_UWP_LLVM}/bin/llvm-ar.exe")
set(CMAKE_C_COMPILER_TARGET   x86_64-pc-windows-msvc)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-windows-msvc)

set(_sdk_inc "${SH_UWP_WINSDK}/Include/${SH_UWP_WINSDK_VER}")
set(_sdk_lib "${SH_UWP_WINSDK}/Lib/${SH_UWP_WINSDK_VER}")

# WINAPI_FAMILY_APP makes the SDK headers hide every API an app container may
# not call, so a desktop-only call is a compile error here instead of a load
# failure on the console.
set(_uwp_flags "-fuse-ld=lld -DWINAPI_FAMILY=WINAPI_FAMILY_APP -D_WIN32_WINNT=0x0A00 -DSH_UWP=1 -DPSYX_UWP=1")
string(APPEND _uwp_flags " -D_CRT_SECURE_NO_WARNINGS -D_CRT_NONSTDC_NO_DEPRECATE")
# Clang's own headers first: its <intrin.h> wraps the MSVC one, and found the
# other way round the two declare the same intrinsics incompatibly. The driver
# orders them this way by itself, but only for the toolset it auto-detects,
# which is the newest installed and not necessarily the one linked against.
file(GLOB _clang_res "${SH_UWP_LLVM}/lib/clang/*/include")
list(GET _clang_res 0 _clang_res)
string(APPEND _uwp_flags " -isystem \"${_clang_res}\"")
string(APPEND _uwp_flags " -isystem \"${SH_UWP_VCTOOLS}/include\"")
foreach(_d ucrt shared um winrt cppwinrt)
    string(APPEND _uwp_flags " -isystem \"${_sdk_inc}/${_d}\"")
endforeach()
# MSVC-compatibility mode resolves a quoted #include through the directory of
# every file on the include stack before the -I list. The port depends on -I
# order instead (pc_port/include/psyq shadows include/psyq), so C is compiled
# with the extensions but without that lookup. __STDC__ then becomes 1, which
# makes the CRT hide stat/strdup/access unless told otherwise.
set(CMAKE_C_FLAGS_INIT   "${_uwp_flags} -fno-ms-compatibility -fms-extensions -D_CRT_DECLARE_NONSTDC_NAMES=1")
# The VS 2022 17.14 STL asks for Clang 19; LLVM 18 builds it fine, and 18 is
# what llvm-objcopy's COFF symbol rewriting (SH_STATIC_MAPS) was verified on.
set(CMAKE_CXX_FLAGS_INIT "${_uwp_flags} -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH")

# Default libraries are dropped and named in full: the Store runtime has the
# same file names as the desktop one (msvcrt.lib, vcruntime.lib), so whichever
# directory the driver searched first would silently win.
set(_uwp_link "-Xlinker /NODEFAULTLIB -Xlinker /APPCONTAINER -Xlinker /SUBSYSTEM:WINDOWS")
string(APPEND _uwp_link " -Xlinker /MANIFEST:NO -Xlinker /DYNAMICBASE -Xlinker /NXCOMPAT")
set(CMAKE_EXE_LINKER_FLAGS_INIT    "${_uwp_link}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_uwp_link}")

set(_store "${SH_UWP_VCTOOLS}/lib/x64/store")
set(SH_UWP_RUNTIME_LIBS
    "${_store}/msvcrt.lib"
    "${_store}/vcruntime.lib"
    "${_store}/msvcprt.lib"
    "${_store}/oldnames.lib"
    # SDL's WinRT entry needs the main thread in the multithreaded apartment.
    # cl gets that from [Platform::MTAThread] on WinMain, which is C++/CX; this
    # is the object the CRT ships for callers that cannot spell the attribute.
    "${_store}/exe_initialize_mta.lib"
    "${_sdk_lib}/ucrt/x64/ucrt.lib"
    "${_sdk_lib}/um/x64/WindowsApp.lib"
)
# Not the _INIT spelling: the Windows-Clang platform file sets that one itself
# (kernel32, user32, ...), none of which an app may link.
set(CMAKE_C_STANDARD_LIBRARIES "")
foreach(_l IN LISTS SH_UWP_RUNTIME_LIBS)
    string(APPEND CMAKE_C_STANDARD_LIBRARIES " \"${_l}\"")
endforeach()
set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_C_STANDARD_LIBRARIES}")
# The compiler checks would otherwise link a console main() as a Store app.
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(SH_UWP_DEPS "" CACHE PATH "Prefix holding the UWP builds of SDL2, OpenAL Soft, libjpeg and ANGLE (vcpkg installed/x64-uwp)")
if(SH_UWP_DEPS)
    list(APPEND CMAKE_PREFIX_PATH "${SH_UWP_DEPS}")
    list(APPEND CMAKE_FIND_ROOT_PATH "${SH_UWP_DEPS}")
endif()
# Never pick up MSYS2/desktop libraries by accident.
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
