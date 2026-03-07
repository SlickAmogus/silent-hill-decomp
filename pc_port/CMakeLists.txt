cmake_minimum_required(VERSION 3.16)
project(SilentHillPC C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# =============================================================================
# Options
# =============================================================================
option(SH_USE_OPENGL "Use OpenGL renderer (via PsyCross)" ON)
option(SH_DEBUG "Enable debug output" OFF)

# =============================================================================
# Version defines - default to USA
# =============================================================================
set(SH_VERSION "USA" CACHE STRING "Game version (USA, EUR, JAP0, JAP1, JAP2)")

# =============================================================================
# PsyCross (PSY-Q Hardware Abstraction Layer)
# Build this BEFORE setting game-specific defines so they don't pollute PsyCross
# =============================================================================

# Disable PGXP: game C code writes short x0/y0, but PGXP makes VERTTYPE=half in C++.
# The raw short bit patterns get misinterpreted as half-floats, producing NaN vertices.
# USE_EXTENDED_PRIM_POINTERS stays ON for 64-bit pointer support in P_TAG.
add_definitions(-DUSE_PGXP=0)

# Silent Hill doesn't read rendered pixels back from VRAM. Disabling the
# framebuffer-to-VRAM store prevents rendered pixel data from overwriting
# CLUT / texture data that coexists at the same VRAM addresses.
add_definitions(-DPSYX_SKIP_FRAMEBUFFER_STORE)

set(PSYCROSS_DIR "${CMAKE_SOURCE_DIR}/../PsyCross" CACHE PATH "Path to PsyCross")

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

# Screen overlay sources
file(GLOB_RECURSE SCREEN_SOURCES
    "${DECOMP_DIR}/src/screens/*.c"
)

# Map overlay sources
# On PSX, only one map overlay is loaded at a time. Statically linking all maps
# causes 500+ symbol collisions (shared AI, particle, player code #included in each).
# For now, compile only the starting map (map0_s00).
set(MAP_SOURCES
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_2.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_anim_info.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_events_data.c"
    "${DECOMP_DIR}/src/maps/map0_s00/map0_s00_header.c"
    "${DECOMP_DIR}/src/maps/map0_s00/Chara_Cheryl.c"
)
# Each map overlay was compiled with its own define on PSX
set_source_files_properties(${MAP_SOURCES} PROPERTIES
    COMPILE_FLAGS "-DMAP0_S00"
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

add_executable(SilentHillPC
    ${PC_PORT_SOURCES}
    ${MAIN_SOURCES}
    ${BODYPROG_SOURCES}
    ${SCREEN_SOURCES}
    ${MAP_SOURCES}
)

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
# Install
# =============================================================================
install(TARGETS SilentHillPC RUNTIME DESTINATION bin)
