# =============================================================================
# Silent Hill — iOS app bundle
# =============================================================================
# The iOS analogue of android_port/app/jni/CMakeLists.txt: build SDL2 from the
# submodule, then add the whole of pc_port on top. iOS reuses the PC port
# unchanged rather than forking a HAL the way xbox_port had to, because
# PsyCross selects RENDERER_OGLES on TARGET_OS_IPHONE by itself.
#
# Configure with the plain iOS system name and Ninja (NOT the Xcode generator):
# signing happens off-box with Sideloadly, so Xcode's signing integration buys
# nothing and Ninja keeps this shaped like every other build here.
#
#   cmake -S ios_port -B build-ios -G Ninja \
#         -DCMAKE_SYSTEM_NAME=iOS \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 \
#         -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
#
# See build_ios.sh, which wraps that and packages the .ipa.
# =============================================================================
cmake_minimum_required(VERSION 3.22)
project(SilentHilliOS C CXX OBJC)

if(NOT IOS)
    message(FATAL_ERROR
        "ios_port is an iOS-only project. Configure with -DCMAKE_SYSTEM_NAME=iOS "
        "(see the header of this file), or build pc_port directly for desktop.")
endif()

# PsyCross's logging helpers forward a caller-supplied runtime format string,
# which -Werror=format-security rejects outright. Android hits the same thing
# through the NDK's default flags.
add_compile_options(-Wno-error=format-security)

# Apple deprecated OpenGL ES in iOS 12 and has not removed it in the eight years
# since; PPSSPP still ships GL as its primary iOS backend. Every GL call in
# PsyCross and SDL would otherwise emit a deprecation warning, burying real
# diagnostics in thousands of lines of noise.
add_compile_definitions(GLES_SILENCE_DEPRECATION)

# -----------------------------------------------------------------------------
# SDL2
# -----------------------------------------------------------------------------
# Static, unlike Android: an iOS app may only load dynamic libraries that ship
# inside the signed bundle, and a static SDL avoids that question entirely.
# Pinned to the same commit android_port uses so the two mobile ports never
# diverge on SDL behaviour.
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

# pc_port looks for an SDL2 target by name (not find_package) on mobile. SDL's
# static target is SDL2-static; alias it so both mobile ports use one spelling.
if(TARGET SDL2-static AND NOT TARGET SDL2)
    add_library(SDL2 ALIAS SDL2-static)
endif()

# -----------------------------------------------------------------------------
# iOS-specific sources, compiled into the game target
# -----------------------------------------------------------------------------
# Platform code lives here, never in pc_port/src or src/ — same rule the other
# ports follow.
set(SH_IOS_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ios_paths.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ios_bootstrap.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ios_ra_http.m"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/ios_ra_login.m"
    CACHE INTERNAL "iOS platform sources compiled into the game target")

# pc_port owns the bundle POST_BUILD steps (ibtool, asset copy) because
# add_custom_command in TARGET form only works in the directory that created the
# target. It needs to find Resources/ from there.
set(SH_IOS_PORT_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    CACHE INTERNAL "ios_port source directory")

# -----------------------------------------------------------------------------
# The game
# -----------------------------------------------------------------------------
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../pc_port"
    "${CMAKE_CURRENT_BINARY_DIR}/pc_port"
)

# -----------------------------------------------------------------------------
# Bundle metadata
# -----------------------------------------------------------------------------
# The bundle identifier is load-bearing and must not change: re-signing keeps
# the app's container (and therefore the user's disc image and saves) only while
# it stays the same. Matches the Android applicationId.
set(SH_IOS_BUNDLE_ID "com.silenthill.port" CACHE STRING "iOS bundle identifier")

# Configure the plist ourselves rather than leaning on CMake's
# MACOSX_BUNDLE_INFO_PLIST substitution, which only promises to fill the
# MACOSX_BUNDLE_* names. The keys that decide whether an installer accepts the
# bundle at all are ours, so resolve them explicitly here.
set(SH_IOS_EXECUTABLE "SilentHill")
if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(SH_IOS_PLATFORM      "iPhoneSimulator")
    set(SH_IOS_PLATFORM_NAME "iphonesimulator")
else()
    set(SH_IOS_PLATFORM      "iPhoneOS")
    set(SH_IOS_PLATFORM_NAME "iphoneos")
endif()
# The SDK version the bundle claims has to be the one it was actually built
# against, so ask the toolchain instead of hardcoding a number that rots.
execute_process(COMMAND xcrun --sdk "${SH_IOS_PLATFORM_NAME}" --show-sdk-version
                OUTPUT_VARIABLE SH_IOS_SDK_VERSION
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT SH_IOS_SDK_VERSION)
    set(SH_IOS_SDK_VERSION "${CMAKE_OSX_DEPLOYMENT_TARGET}")
endif()
set(SH_IOS_SDK_NAME "${SH_IOS_PLATFORM_NAME}${SH_IOS_SDK_VERSION}")
set(SH_IOS_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
               "${CMAKE_CURRENT_BINARY_DIR}/Info.plist" @ONLY)

set_target_properties(SilentHillPC PROPERTIES
    OUTPUT_NAME "SilentHill"
    MACOSX_BUNDLE_BUNDLE_NAME "Silent Hill"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${SH_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist"
    # Signed off-box; a CI build with no keychain cannot sign at all.
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
)

# The bundle POST_BUILD steps (ibtool for the launch storyboard, and copying
# the port's shipped assets in) live in pc_port/CMakeLists.txt under if(IOS),
# because add_custom_command in TARGET form only works in the directory that
# created the target.

message(STATUS "iOS bundle: ${SH_IOS_BUNDLE_ID} (arch ${CMAKE_OSX_ARCHITECTURES}, "
               "min iOS ${CMAKE_OSX_DEPLOYMENT_TARGET})")
