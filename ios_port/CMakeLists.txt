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
    CACHE INTERNAL "iOS platform sources compiled into the game target")

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

set_target_properties(SilentHillPC PROPERTIES
    OUTPUT_NAME "SilentHill"
    MACOSX_BUNDLE_BUNDLE_NAME "Silent Hill"
    MACOSX_BUNDLE_GUI_IDENTIFIER "${SH_IOS_BUNDLE_ID}"
    MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/Resources/Info.plist.in"
    # Signed off-box; a CI build with no keychain cannot sign at all.
    XCODE_ATTRIBUTE_CODE_SIGNING_ALLOWED "NO"
    XCODE_ATTRIBUTE_CODE_SIGNING_REQUIRED "NO"
)

# The port's own shipped assets (decal.png, fonts, language pack, UI sounds).
# ios_bootstrap.m stages these into Documents on first run, because the bundle
# itself is read-only. Nothing copyrighted is here — the disc image stays the
# user's to supply.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../pc_port/assets")
    target_sources(SilentHillPC PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pc_port/assets")
    set_source_files_properties("${CMAKE_CURRENT_SOURCE_DIR}/../pc_port/assets"
        PROPERTIES MACOSX_PACKAGE_LOCATION "Resources")
endif()

# A launch storyboard has to be compiled, not copied: iOS looks for
# LaunchScreen.storyboardc in the bundle and ignores the .storyboard source.
# The Xcode generator would run ibtool for us; under Ninja it is an explicit
# step. Without it the app runs letterboxed in a 320x480 compatibility box.
find_program(SH_IBTOOL ibtool)
if(SH_IBTOOL)
    add_custom_command(TARGET SilentHillPC POST_BUILD
        COMMAND ${SH_IBTOOL}
                --errors --warnings --notices
                --output-format human-readable-text
                --compile "$<TARGET_BUNDLE_CONTENT_DIR:SilentHillPC>/LaunchScreen.storyboardc"
                "${CMAKE_CURRENT_SOURCE_DIR}/Resources/LaunchScreen.storyboard"
        COMMENT "Compiling LaunchScreen.storyboard"
        VERBATIM)
else()
    message(WARNING
        "ibtool not found — the launch storyboard will not be compiled into the "
        "bundle, and iOS will run the app letterboxed at 320x480.")
endif()

message(STATUS "iOS bundle: ${SH_IOS_BUNDLE_ID} (arch ${CMAKE_OSX_ARCHITECTURES}, "
               "min iOS ${CMAKE_OSX_DEPLOYMENT_TARGET})")
