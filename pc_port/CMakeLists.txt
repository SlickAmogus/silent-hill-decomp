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

# Map overlay sources - DISABLED for now
# On PSX, only one map overlay is loaded at a time. Statically linking all maps
# causes 500+ symbol collisions (shared AI, particle, player code #included in each).
# TODO: Build maps as separate shared libraries or use a map selection mechanism.
# file(GLOB_RECURSE MAP_SOURCES "${DECOMP_DIR}/src/maps/*.c")

# PC port specific sources
file(GLOB_RECURSE PC_PORT_SOURCES
    "${CMAKE_SOURCE_DIR}/src/*.c"
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
)

# =============================================================================
# Executable
# =============================================================================
add_executable(SilentHillPC
    ${PC_PORT_SOURCES}
    ${MAIN_SOURCES}
    ${BODYPROG_SOURCES}
    ${SCREEN_SOURCES}
)

# Game-specific defines (only on our target, NOT PsyCross)
target_compile_definitions(SilentHillPC PRIVATE
    VER_${SH_VERSION}
    SH_PC_PORT
    SKIP_ASM
    "static_assert=_Static_assert"
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
    # Original decomp headers
    "${DECOMP_DIR}/include"
    "${DECOMP_DIR}/include/decomp"
    # PsyCross PSX-compatible headers
    "${PSYCROSS_DIR}/include/psx"
    "${PSYCROSS_DIR}/include"
)

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
