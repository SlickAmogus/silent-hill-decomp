cmake_minimum_required(VERSION 3.22)
project(SilentHillAndroid C CXX)

# The NDK puts -Werror=format-security in its default flags. PsyCross's logging
# helpers forward a runtime format string (PsyX_Log and friends take a caller's
# fmt), which is exactly what that check rejects, so it has to come back down to
# a warning or the whole audio/render layer fails to build.
add_compile_options(-Wno-error=format-security)


# SDL2 first: PsyCross and the game both link the SDL2 target, and pc_port's
# CMakeLists looks for it by target name (not find_package) when ANDROID is set.
# SDL_SHARED is what SDLActivity's default getLibraries() expects to load.
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
add_subdirectory(SDL)

# The game. Everything the APK needs is already in pc_port -- Android reuses
# the whole PC port rather than forking a HAL the way xbox_port had to, because
# PsyCross selects RENDERER_OGLES on __ANDROID__ on its own.
add_subdirectory(
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../pc_port"
    "${CMAKE_CURRENT_BINARY_DIR}/pc_port"
)
