cmake_minimum_required(VERSION 3.16)
project(sh_master C)

set(CMAKE_C_STANDARD 99)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_executable(sh_master sh_master.c)

# The wire protocol header is shared verbatim with the game client; it is the
# only thing this program takes from the port.
target_include_directories(sh_master PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../pc_port/include")

if(WIN32)
    target_link_libraries(sh_master PRIVATE ws2_32)
endif()

if(NOT MSVC)
    target_compile_options(sh_master PRIVATE -Wall -Wextra)
endif()
