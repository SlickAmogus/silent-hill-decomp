cmake_minimum_required(VERSION 3.16)
project(SilentHillPC C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# =============================================================================
# Options
# =============================================================================
option(SH_USE_OPENGL "Use OpenGL renderer (via PsyCross)" ON)
option(SH_DEBUG "Enable debug output" OFF)
option(SH_ASAN "Enable AddressSanitizer for memory corruption detection" OFF)

# =============================================================================
# Version defines - default to USA
# =============================================================================
set(SH_VERSION "USA" CACHE STRING "Game version (USA, EUR, JAP0, JAP1, JAP2)")

# =============================================================================
# AddressSanitizer (optional, for debugging memory corruption)
# =============================================================================
if(SH_ASAN)
    message(STATUS "AddressSanitizer ENABLED")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

# =============================================================================
# PsyCross (PSY-Q Hardware Abstraction Layer)
# Build this BEFORE setting game-specific defines so they don't pollute PsyCross
# =============================================================================

# PGXP: COMPILE-TIME OFF. The intent was for this to be compiled in
# and runtime-gated by g_PsxUsePgxp (driven by config.cfg `use_pgxp`),
# but flipping USE_PGXP=1 introduced regressions on the runtime-OFF
# path that runtime gating could not contain:
#   1. The shader's GTE_PERSPECTIVE_CORRECTION macro is selected at
#      compile time (#if USE_PGXP) — its 2D path used z=0.5 instead
#      of z=0.0, producing Z-fight / edge bleed-through.
#   2. The gte_stsxy* macro family in inline_c.h is also #if USE_PGXP
#      gated — when on, screen XY values flow through g_FP_SXYZ
#      (FP-truncated short, NO Lm_G1/Lm_G2 ±1024 clamp) instead of
#      MFC2() with PSX clamping. Far prims could escape the screen-
#      space envelope, producing huge-pixelated 2D plane artifacts.
#
# To make this fully optional in a built binary (the goal), the
# preprocessor branches above need to become runtime branches that
# read g_PsxUsePgxp. Until that's done, USE_PGXP=0 here so the
# runtime config option `use_pgxp = 0` actually behaves as if PGXP
# isn't present at all.
#
# To re-enable for testing: flip back to 1 here, rebuild, set
# `use_pgxp = 1` in config.cfg.
add_definitions(-DUSE_PGXP=0)

# Silent Hill doesn't read rendered pixels back from VRAM. Disabling the
# framebuffer-to-VRAM store prevents rendered pixel data from overwriting
# CLUT / texture data that coexists at the same VRAM addresses.
add_definitions(-DPSYX_SKIP_FRAMEBUFFER_STORE)

set(PSYCROSS_DIR "${CMAKE_SOURCE_DIR}/PsyCross" CACHE PATH "Path to PsyCross")

if(NOT EXISTS "${PSYCROSS_DIR}/CMakeLists.txt")
    set(PSYCROSS_DIR "${CMAKE_SOURCE_DIR}/../PsyCross")
endif()

if(NOT EXISTS "${PSYCROSS_DIR}/CMakeLists.txt")
    set(PSYCROSS_DIR "${CMAKE_SOURCE_DIR}/../../PsyCross")
endif()

if(EXISTS "${PSYCROSS_DIR}/CMakeLists.txt")
    add_subdirectory(${PSYCROSS_DIR} ${CMAKE_BINARY_DIR}/psycross)
    message(STATUS "PsyCross found at: ${PSYCROSS_DIR}")
else()
    message(FATAL_ERROR "PsyCross not found. Set PSYCROSS_DIR to the PsyCross repo path.")
endif()

# =============================================================================
# SDL2
# =============================================================================
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
    # Try pkg-config
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 sdl2)
    endif()
endif()

# =============================================================================
# Source files - decomp game code
# =============================================================================
set(DECOMP_DIR "${CMAKE_SOURCE_DIR}/..")

# Main executable sources (exclude PSX-specific files)
file(GLOB MAIN_SOURCES
    "${DECOMP_DIR}/src/main/*.c"
)
# Remove PSX-only files: main.c (replaced by main_pc.c), memcpy.c (MIPS asm)
list(FILTER MAIN_SOURCES EXCLUDE REGEX "(main|memcpy)\\.c$")

# BODYPROG engine sources
file(GLOB_RECURSE BODYPROG_SOURCES
    "${DECOMP_DIR}/src/bodyprog/*.c"
)
# Remove duplicate: func_80032CE8 is defined in both bodyprog_80032D1C.c and sys/fs_screens.c
list(FILTER BODYPROG_SOURCES EXCLUDE REGEX "bodyprog_80032D1C\\.c$")
# Remove JPN-only text_draw_jp.c (conflicts with text_draw.c for USA build)
list(FILTER BODYPROG_SOURCES EXCLUDE REGEX "text_draw_jp\\.c$")

# Screen overlay sources
file(GLOB_RECURSE SCREEN_SOURCES
    "${DECOMP_DIR}/src/screens/*.c"
)
# Remove JPN-only anti-modchip overlays (undefined symbols on USA)
list(FILTER SCREEN_SOURCES EXCLUDE REGEX "(hp_safe1|s__safe2)\\.c$")

# Map overlay sources
# On PSX, only one map overlay is loaded at a time. Statically linking all maps
# causes 500+ symbol collisions (shared AI, particle, player code #included in each).
# For now, compile only the starting map (map0_s00).
set(MAP_SOURCES
    "${DECOMP_DIR}/src/maps/map0_s00/player.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_2.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_anim_info.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_events_data.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_header.c"
    "${DECOMP_DIR}/src/maps/map0_s00/Chara_Cheryl.c"
    "${DECOMP_DIR}/src/maps/map0_s00/Chara_Stalker.c"
    "${DECOMP_DIR}/src/maps/map0_s00/particle.c"
)
# Extracted per-map cutscene data (voice cmds, timers, camera waypoints)
# generated by tools/extract_map_data.py from disc_extract/VIN/MAP*.BIN.
set(MAP0_S00_EXTRACTED "${CMAKE_SOURCE_DIR}/build_gen/extracted_data/map0_s00_extracted_data.c")
if(EXISTS "${MAP0_S00_EXTRACTED}")
    list(APPEND MAP_SOURCES "${MAP0_S00_EXTRACTED}")
endif()
# Each map overlay was compiled with its own define on PSX
set_source_files_properties(${MAP_SOURCES} PROPERTIES
    COMPILE_FLAGS "-DMAP0_S00 -DSH_MAP_NAME=map0_s00"
)

# PC port specific sources
file(GLOB_RECURSE PC_PORT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

# =============================================================================
# Executable
# =============================================================================
# Exclude map overlay stub since we now compile map0_s00 which provides g_MapOverlayHeader
list(FILTER PC_PORT_SOURCES EXCLUDE REGEX "map_overlay_stub\\.c$")

# Windows: compile the .rc so the exe carries an application icon (Explorer,
# taskbar, title bar). windres resolves the ICON path relative to the .rc dir.
if(WIN32)
    enable_language(RC)
    # windres resolves the ICON path relative to its include dirs, not the .rc
    # location, so point it at res/ explicitly.
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I \"${CMAKE_SOURCE_DIR}/res\"")
    set(SH_APP_RESOURCES "${CMAKE_SOURCE_DIR}/res/app.rc")
endif()

add_executable(SilentHillPC
    ${PC_PORT_SOURCES}
    ${MAIN_SOURCES}
    ${BODYPROG_SOURCES}
    ${SCREEN_SOURCES}
    ${MAP_SOURCES}
    ${SH_APP_RESOURCES}
)

# Export symbols from the executable so map overlay DLLs can link against it.
# On Linux, ENABLE_EXPORTS adds -rdynamic so dlopen'd libraries see exe symbols.
# On Windows/MinGW, we additionally need --export-all-symbols because MinGW
# only exports __declspec(dllexport)-marked symbols by default.
set_target_properties(SilentHillPC PROPERTIES ENABLE_EXPORTS ON)
if(MINGW)
    target_link_options(SilentHillPC PRIVATE "-Wl,--export-all-symbols")
    # Build as a GUI (windows) subsystem app so Windows doesn't spawn a console
    # window at launch. We keep the standard C entry (SDL_MAIN_HANDLED is set, so
    # main() is the real entry); force -emainCRTStartup because the windows
    # subsystem default entry is WinMainCRTStartup, which we don't provide.
    # External-console mode calls AllocConsole() at runtime (see main_pc.c).
    target_link_options(SilentHillPC PRIVATE "-Wl,--subsystem,windows" "-Wl,-emainCRTStartup")
endif()

# Game-specific defines (only on our target, NOT PsyCross)
target_compile_definitions(SilentHillPC PRIVATE
    VER_${SH_VERSION}
    SH_PC_PORT
    SKIP_ASM
)

# static_assert -> _Static_assert only for C files (breaks C++ std headers)
file(GLOB_RECURSE ALL_C_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${DECOMP_DIR}/src/*.c"
)
set_source_files_properties(${ALL_C_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS "static_assert=_Static_assert"
)
if(SH_DEBUG)
    target_compile_definitions(SilentHillPC PRIVATE SH_DEBUG=1)
endif()

# =============================================================================
# Build identification
# =============================================================================
# Regenerated on EVERY build so the running exe can log exactly which commit
# it was built from ([SH] build line in SilentHill.log) — user crash logs were
# untriageable because nothing identified the game build (the PsyCross
# "Build date" stamp only changes when PsyCross itself recompiles).
add_custom_target(gen_build_info ALL
    COMMAND ${CMAKE_COMMAND}
        -DOUT_FILE=${CMAKE_BINARY_DIR}/sh_build_info.h
        -DGIT_DIR=${DECOMP_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_build_info.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/sh_build_info.h
    COMMENT "Generating sh_build_info.h"
)
add_dependencies(SilentHillPC gen_build_info)
target_include_directories(SilentHillPC PRIVATE ${CMAKE_BINARY_DIR})

# =============================================================================
# Include paths
# =============================================================================
target_include_directories(SilentHillPC PRIVATE
    # PC port compatibility headers (searched FIRST to override psyq/)
    "${CMAKE_SOURCE_DIR}/include"
    "${CMAKE_SOURCE_DIR}/include/psyq_compat"
    # PC port source (for fmv/ includes)
    "${CMAKE_SOURCE_DIR}/src"
    # Original decomp headers
    "${DECOMP_DIR}/include"
    "${DECOMP_DIR}/include/decomp"
    # PsyCross PSX-compatible headers
    "${PSYCROSS_DIR}/include/psx"
    "${PSYCROSS_DIR}/include"
)

# =============================================================================
# libjpeg (for FMV playback - MJPG decoding)
# =============================================================================
find_package(PkgConfig QUIET)
find_library(JPEG_LIBRARY NAMES jpeg turbojpeg)
find_path(JPEG_INCLUDE_DIR jpeglib.h)

# =============================================================================
# Linking
# =============================================================================
target_link_libraries(SilentHillPC PRIVATE
    psycross_static
)

if(SDL2_FOUND)
    target_link_libraries(SilentHillPC PRIVATE SDL2::SDL2)
elseif(SDL2_LIBRARIES)
    target_link_libraries(SilentHillPC PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(SilentHillPC PRIVATE ${SDL2_INCLUDE_DIRS})
endif()

if(JPEG_LIBRARY)
    target_link_libraries(SilentHillPC PRIVATE ${JPEG_LIBRARY})
    if(JPEG_INCLUDE_DIR)
        target_include_directories(SilentHillPC PRIVATE ${JPEG_INCLUDE_DIR})
    endif()
else()
    target_link_libraries(SilentHillPC PRIVATE jpeg)
endif()

if(WIN32)
    target_link_libraries(SilentHillPC PRIVATE opengl32 winmm m)
elseif(UNIX)
    find_package(OpenGL REQUIRED)
    target_link_libraries(SilentHillPC PRIVATE OpenGL::GL)
endif()

# =============================================================================
# Compiler flags
# =============================================================================
if(MSVC)
    target_compile_options(SilentHillPC PRIVATE
        /W3
        /wd4996  # deprecated functions
        /wd4244  # conversion warnings
        /wd4267  # size_t conversions
    )
else()
    target_compile_options(SilentHillPC PRIVATE
        -Wall
        -Wno-unused-variable
        -Wno-unused-function
        -Wno-sign-compare
        -Wno-missing-braces
        -Wno-pointer-to-int-cast
        -Wno-int-to-pointer-cast
        -Wno-incompatible-pointer-types
        -Wno-discarded-qualifiers
        -Wno-builtin-declaration-mismatch
    )
endif()

# =============================================================================
# Copy config.cfg to build dir (only if not already present)
# =============================================================================
# Only copy config.cfg if it doesn't already exist (preserves user edits)
if(NOT EXISTS "${CMAKE_BINARY_DIR}/config.cfg")
    file(COPY "${CMAKE_SOURCE_DIR}/config.cfg" DESTINATION "${CMAKE_BINARY_DIR}")
endif()
# Always copy run_capture.bat so the user has a debug-launcher next to the exe
if(WIN32 AND EXISTS "${CMAKE_SOURCE_DIR}/run_capture.bat")
    file(COPY "${CMAKE_SOURCE_DIR}/run_capture.bat" DESTINATION "${CMAKE_BINARY_DIR}")
endif()

# =============================================================================
# Map Overlay DLLs (optional — builds all maps as loadable modules)
# =============================================================================
option(SH_BUILD_MAP_DLLS "Build map overlays as DLLs for dynamic loading" OFF)
if(SH_BUILD_MAP_DLLS)
    add_subdirectory(maps)
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS SilentHillPC RUNTIME DESTINATION bin)
