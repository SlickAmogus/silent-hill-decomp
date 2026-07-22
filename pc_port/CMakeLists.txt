cmake_minimum_required(VERSION 3.16)
project(SilentHillPC C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Build everything position-independent. On Linux the map overlays are .so
# shared objects that link the static psycross_static archive; linking a
# static (non-PIC) archive into a shared object fails, so force PIC globally.
# Harmless on Windows/MinGW (PE code is already position-independent).
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# =============================================================================
# Options
# =============================================================================
option(SH_USE_OPENGL "Use OpenGL renderer (via PsyCross)" ON)
option(SH_DEBUG "Enable debug output" OFF)
option(SH_ASAN "Enable AddressSanitizer for memory corruption detection" OFF)
# FMV ffmpeg fallback (native h264/h265/vp9/av1 in mp4/mkv/webm/avi override files).
# ON by default and free to be: ffmpeg is loaded at runtime, so this costs only the
# headers at build time and nothing is bundled or linked. See the ffmpeg section below.
option(SH_FMV_FFMPEG "Enable ffmpeg FMV fallback decoder (h264/h265/vp9/av1, mp4/mkv/webm)" ON)

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
# PC PORT: drop the combat_target.c aiming shim; the real decompiled func_8005CD38
# (src/bodyprog/bodyprog_combat_8005BF38.c) now provides target selection.
list(FILTER PC_PORT_SOURCES EXCLUDE REGEX "combat_target\\.c$")

# Windows: compile the .rc so the exe carries an application icon (Explorer,
# taskbar, title bar). windres resolves the ICON path relative to the .rc dir.
if(WIN32)
    enable_language(RC)
    # windres resolves the ICON path relative to its include dirs, not the .rc
    # location, so point it at res/ explicitly.
    # res/ for the icon; binary dir for the generated sh_version.h.
    set(CMAKE_RC_FLAGS "${CMAKE_RC_FLAGS} -I \"${CMAKE_SOURCE_DIR}/res\" -I \"${CMAKE_BINARY_DIR}\"")
    set(SH_APP_RESOURCES "${CMAKE_SOURCE_DIR}/res/app.rc")
    # The .rc embeds app_icon.ico and includes the generated sh_version.h;
    # neither is visible to the generator's dep scan, so list them explicitly.
    # sh_version.h is rewritten every build, so the version resource (and only
    # the resource) recompiles each build.
    set_source_files_properties(${SH_APP_RESOURCES} PROPERTIES
        OBJECT_DEPENDS "${CMAKE_SOURCE_DIR}/res/app_icon.ico;${CMAKE_BINARY_DIR}/sh_version.h")
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

# Shipped runtime assets: pc_port/assets/ mirrors the game folder layout
# (assets/gamedata/decal.png -> <build>/gamedata/decal.png), so a fresh build
# folder runs with them. release-nightly.ps1 and the CI stage steps package the
# same directory — add new custom assets there, never to the build folder.
add_custom_command(TARGET SilentHillPC POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets" "$<TARGET_FILE_DIR:SilentHillPC>"
    COMMENT "Copying shipped runtime assets (pc_port/assets/)"
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
set_property(SOURCE "${CMAKE_SOURCE_DIR}/src/xa_player.c" APPEND PROPERTY
    COMPILE_DEFINITIONS
        "XaPlayer_PlayWithParams=PcLegacyXa_PlayWithParams"
        "XaPlayer_Play=PcLegacyXa_Play"
        "XaPlayer_Stop=PcLegacyXa_Stop"
        "XaPlayer_Update=PcLegacyXa_Update"
        "XaPlayer_SetVolume=PcLegacyXa_SetVolume"
        "XaPlayer_SetPauseHold=PcLegacyXa_SetPauseHold"
        "Xa_IsVoiceAudioDraining=PcLegacyXa_IsVoiceAudioDraining"
        "Xa_VoiceGapHold=PcLegacyXa_VoiceGapHold"
        "XaPlayer_SetMasterVolume=PcLegacyXa_SetMasterVolume"
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
        -DVER_FILE=${CMAKE_BINARY_DIR}/sh_version.h
        -DGIT_DIR=${DECOMP_DIR}
        -P ${CMAKE_SOURCE_DIR}/cmake/gen_build_info.cmake
    BYPRODUCTS ${CMAKE_BINARY_DIR}/sh_build_info.h ${CMAKE_BINARY_DIR}/sh_version.h
    COMMENT "Generating sh_build_info.h + sh_version.h"
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
# OpenAL headers (xa_player.c uses AL directly). Set by build.sh on macOS to
# point at Homebrew openal-soft; unset on the Windows/MSYS2 build (no-op).
if(OPENAL_INCLUDE_DIR)
    target_include_directories(SilentHillPC PRIVATE "${OPENAL_INCLUDE_DIR}")
endif()

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

# On Linux/ELF the map overlay .so files no longer link their own copy of
# PsyCross (see maps/CMakeLists.txt) — they import the entire PsyCross runtime
# from this executable at dlopen so there is a single, initialized instance of
# its singleton state (glad GL table, GTE registers, vertex/split buffers).
# A static archive only contributes the object files the exe itself references,
# so a PsyCross function a map calls but the exe never does would be missing
# from the dynamic symbol table. Force the WHOLE archive in so -rdynamic
# (ENABLE_EXPORTS) exports the complete public PsyCross API to dlopen'd maps.
if(UNIX AND NOT APPLE)
    target_link_options(SilentHillPC PRIVATE
        "LINKER:--whole-archive"
        "$<TARGET_FILE:psycross_static>"
        "LINKER:--no-whole-archive"
    )
endif()

if(SDL2_FOUND)
    target_link_libraries(SilentHillPC PRIVATE SDL2::SDL2)
    # The code includes <SDL2/SDL.h>, which needs the dir CONTAINING SDL2/ on
    # the search path. On Linux/Windows the SDL2 include dir is .../include
    # (so SDL2/ resolves). Homebrew's SDL2Config points it at .../include/SDL2
    # (the subdir itself), so only <SDL.h> would resolve — add the parent so
    # the SDL2/ prefix works there too. Harmless on Linux/Windows.
    get_target_property(_sdl2_incs SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
    if(_sdl2_incs)
        foreach(_d IN LISTS _sdl2_incs)
            if(_d MATCHES "/SDL2$")
                get_filename_component(_parent "${_d}" DIRECTORY)
                target_include_directories(SilentHillPC PRIVATE "${_parent}")
            endif()
        endforeach()
    endif()
elseif(SDL2_LIBRARIES)
    target_link_libraries(SilentHillPC PRIVATE ${SDL2_LIBRARIES})
    target_include_directories(SilentHillPC PRIVATE ${SDL2_INCLUDE_DIRS})
    # Same <SDL2/SDL.h> prefix fix for the pkg-config path.
    foreach(_d IN LISTS SDL2_INCLUDE_DIRS)
        if(_d MATCHES "/SDL2$")
            get_filename_component(_parent "${_d}" DIRECTORY)
            target_include_directories(SilentHillPC PRIVATE "${_parent}")
        endif()
    endforeach()
endif()

if(JPEG_LIBRARY)
    target_link_libraries(SilentHillPC PRIVATE ${JPEG_LIBRARY})
    if(JPEG_INCLUDE_DIR)
        target_include_directories(SilentHillPC PRIVATE ${JPEG_INCLUDE_DIR})
    endif()
else()
    target_link_libraries(SilentHillPC PRIVATE jpeg)
endif()

# =============================================================================
# ffmpeg (optional FMV fallback: h264/h265/vp9/av1 in mp4/mkv/webm/avi)
# =============================================================================
# Headers only — the libs are resolved at runtime by fmv_player.cpp through
# dll_loader, so nothing ffmpeg lands in the exe's import table and we bundle no
# ffmpeg binaries (we cannot redistribute the GPL prebuilts, and an import-table
# dependency would stop the exe launching for every user without the DLLs).
# Users who want container overrides drop shared libs whose major versions match
# the headers this was built against next to the exe, or install them system-wide
# (fmv_player.cpp derives the soname it looks for from those same headers).
# Missing headers only cost the feature, never the build, so a plain checkout
# still compiles.
#
# ffmpeg 6.0 (libavcodec 60) is the floor: the decoder uses AVChannelLayout and
# swr_alloc_set_opts2, which do not exist before ffmpeg 5.1, and 6.0 is the
# oldest release we actually validate against. Newer majors are accepted.
set(SH_FFMPEG_MIN_AVCODEC_MAJOR 60)
set(SH_FFMPEG_PREFIX "" CACHE PATH
    "Prefix holding ffmpeg development headers to build against (uses <prefix>/include)")
if(SH_FMV_FFMPEG)
    set(FFMPEG_HEADER_DIRS "")
    set(FFMPEG_HEADERS_FOUND FALSE)
    if(SH_FFMPEG_PREFIX)
        # An explicit prefix is a deliberate override (side-installed headers that
        # must win over whatever the system offers), so pkg-config is skipped
        # entirely rather than allowed to contradict it.
        if(EXISTS "${SH_FFMPEG_PREFIX}/include/libavcodec/avcodec.h")
            set(FFMPEG_HEADER_DIRS "${SH_FFMPEG_PREFIX}/include")
            set(FFMPEG_HEADERS_FOUND TRUE)
        else()
            message(STATUS "SH_FFMPEG_PREFIX=${SH_FFMPEG_PREFIX} has no "
                "include/libavcodec/avcodec.h")
        endif()
    else()
        if(PkgConfig_FOUND OR PKG_CONFIG_FOUND)
            pkg_check_modules(FFMPEG
                libavformat>=60 libavcodec>=${SH_FFMPEG_MIN_AVCODEC_MAJOR}
                libavutil>=58 libswscale>=7 libswresample>=4)
        endif()
        if(FFMPEG_FOUND)
            # pkg-config omits -I for compiler-default include dirs, so an EMPTY
            # FFMPEG_INCLUDE_DIRS means "already on the search path", not "missing".
            set(FFMPEG_HEADER_DIRS ${FFMPEG_INCLUDE_DIRS})
            set(FFMPEG_HEADERS_FOUND TRUE)
        else()
            find_path(FFMPEG_INCLUDE_DIR libavcodec/avcodec.h)
            if(FFMPEG_INCLUDE_DIR)
                set(FFMPEG_HEADER_DIRS ${FFMPEG_INCLUDE_DIR})
                set(FFMPEG_HEADERS_FOUND TRUE)
            endif()
        endif()
    endif()

    # The major is ALWAYS read from the header the compiler will actually open,
    # never from pkg-config's claim: a .pc whose includedir points at a different
    # tree reports one version while the compiler silently gets another, and the
    # runtime soname fmv_player.cpp derives comes from the header, not the .pc.
    set(_ff_major 0)
    set(_ff_header_root "")
    if(FFMPEG_HEADERS_FOUND)
        foreach(_dir IN LISTS FFMPEG_HEADER_DIRS)
            if(EXISTS "${_dir}/libavcodec/avcodec.h")
                set(_ff_header_root "${_dir}")
                break()
            endif()
        endforeach()
        if(NOT _ff_header_root)
            find_path(FFMPEG_DEFAULT_INCLUDE_DIR libavcodec/avcodec.h)
            if(FFMPEG_DEFAULT_INCLUDE_DIR)
                set(_ff_header_root "${FFMPEG_DEFAULT_INCLUDE_DIR}")
            endif()
        endif()

        # version_major.h since ffmpeg 5.0, version.h before that.
        set(_ff_version_header "${_ff_header_root}/libavcodec/version_major.h")
        if(NOT EXISTS "${_ff_version_header}")
            set(_ff_version_header "${_ff_header_root}/libavcodec/version.h")
        endif()
        if(_ff_header_root AND EXISTS "${_ff_version_header}")
            file(STRINGS "${_ff_version_header}" _ff_major_lines
                 REGEX "^#define[ \t]+LIBAVCODEC_VERSION_MAJOR[ \t]+[0-9]+")
            if(_ff_major_lines)
                list(GET _ff_major_lines 0 _ff_major_line)
                string(REGEX REPLACE "^.*LIBAVCODEC_VERSION_MAJOR[ \t]+([0-9]+).*$" "\\1"
                       _ff_major "${_ff_major_line}")
            endif()
        endif()
        if(_ff_major LESS SH_FFMPEG_MIN_AVCODEC_MAJOR)
            message(STATUS "ffmpeg headers at ${_ff_header_root} are libavcodec major "
                "${_ff_major}; ${SH_FFMPEG_MIN_AVCODEC_MAJOR}+ (ffmpeg 6.0+) required")
            set(FFMPEG_HEADERS_FOUND FALSE)
        endif()
    endif()

    if(FFMPEG_HEADERS_FOUND)
        if(FFMPEG_HEADER_DIRS)
            target_include_directories(SilentHillPC PRIVATE ${FFMPEG_HEADER_DIRS})
        endif()
        target_compile_definitions(SilentHillPC PRIVATE SH_FMV_FFMPEG)
        message(STATUS "ffmpeg FMV fallback ENABLED (runtime-loaded, libavcodec major "
            "${_ff_major}, headers: ${_ff_header_root})")
    else()
        message(WARNING "SH_FMV_FFMPEG=ON but ffmpeg 6.0+ development headers were not found "
            "(MSYS2: pacman -S mingw-w64-x86_64-ffmpeg; Debian/Ubuntu: apt install libavcodec-dev "
            "libavformat-dev libavutil-dev libswscale-dev libswresample-dev). Building WITHOUT the "
            "ffmpeg FMV fallback; mp4/mkv/webm overrides will be ignored.")
    endif()
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
    if(APPLE)
        # AppleClang is stricter than GCC on the WIP decomp's implicit decls and
        # qualifier/conversion mismatches; demote these to warnings on macOS.
        target_compile_options(SilentHillPC PRIVATE
            -Wno-implicit-function-declaration
            -Wno-ignored-qualifiers
            -Wno-int-conversion
            -Wno-return-mismatch
        )
    endif()
    # GCC 14 promoted several long-standing C warnings to hard errors by
    # default (implicit function/int declarations, int<->pointer conversions,
    # missing parameter types). The WIP decomp deliberately relies on these
    # being warnings (as they are on the MinGW GCC 13 the Windows build uses),
    # so demote them back to warnings. C-only to avoid cc1plus noise on .cpp.
    #
    # Gated to GCC >= 14: -Wdeclaration-missing-parameter-type and
    # -Wreturn-mismatch were introduced in GCC 14, and GCC errors out hard on
    # -Wno-error=<unknown-warning> (unlike a plain -Wno-<unknown>, which it
    # silently defers). On the MinGW GCC 13 the Windows build uses these two
    # names don't exist, so an unconditional block fails every C TU. On GCC 13
    # the warnings here are not errors anyway, so the block is a no-op there.
    if(CMAKE_C_COMPILER_ID STREQUAL "GNU" AND CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 14)
        target_compile_options(SilentHillPC PRIVATE
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-function-declaration>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=implicit-int>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=int-conversion>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=incompatible-pointer-types>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=declaration-missing-parameter-type>
            $<$<COMPILE_LANGUAGE:C>:-Wno-error=return-mismatch>
        )
    endif()
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
