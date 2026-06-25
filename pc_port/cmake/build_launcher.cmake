# Build the C# launcher with MSBuild and copy its exe next to SilentHillPC.exe.
#
# Run via `cmake -P` from the launcher custom target. Kept as a separate script
# (not inline custom-target commands) for two reasons:
#   1. Failures are non-fatal — a missing toolchain or a launcher compile error
#      warns and returns 0 so the game build is never blocked.
#   2. The build normally runs from the msys2 login shell, which exports almost
#      none of the Windows env vars MSBuild/NuGet expect (ProgramData, APPDATA,
#      USERPROFILE, ... are all empty). We reconstruct a self-contained
#      toolchain HOME under the build dir so NuGet restore has somewhere to
#      live without depending on the (stripped) real Windows profile.
#
# Expected -D vars: MSBUILD, PROJ, LDIR (launcher source dir), BDIR (build dir).

# --- Reconstruct the environment MSBuild / NuGet need ------------------------
if("$ENV{SystemDrive}" STREQUAL "")
    set(ENV{SystemDrive} "C:")
endif()
if("$ENV{ProgramData}" STREQUAL "")
    set(ENV{ProgramData} "$ENV{SystemDrive}/ProgramData")
endif()
if("$ENV{ProgramFiles}" STREQUAL "")
    set(ENV{ProgramFiles} "$ENV{SystemDrive}/Program Files")
endif()

# Self-contained toolchain home so NuGet restore doesn't need the real profile.
set(_home "${BDIR}/launcher-toolchain-home")
file(MAKE_DIRECTORY "${_home}/AppData/Roaming" "${_home}/AppData/Local" "${_home}/nuget-packages")
if("$ENV{USERPROFILE}" STREQUAL "")
    set(ENV{USERPROFILE} "${_home}")
endif()
if("$ENV{APPDATA}" STREQUAL "")
    set(ENV{APPDATA} "${_home}/AppData/Roaming")
endif()
if("$ENV{LOCALAPPDATA}" STREQUAL "")
    set(ENV{LOCALAPPDATA} "${_home}/AppData/Local")
endif()
# Keep restored packages inside the build tree (no dependency on %USERPROFILE%).
set(ENV{NUGET_PACKAGES} "${_home}/nuget-packages")

# --- Restore + build (non-fatal) ---------------------------------------------
execute_process(
    COMMAND "${MSBUILD}" "${PROJ}" -restore
            /t:Build /p:Configuration=Release /p:Platform=AnyCPU
            /nologo /v:minimal
    RESULT_VARIABLE _rc)
if(NOT _rc EQUAL 0)
    message(WARNING
        "Launcher build failed (MSBuild exit ${_rc}); skipping. "
        "The game build is unaffected. First build needs internet for the "
        "NuGet reference-assemblies restore.")
    return()
endif()

# --- Copy exe + config next to SilentHillPC.exe ------------------------------
foreach(_f "SilentHillPC_Launcher.exe" "SilentHillPC_Launcher.exe.config")
    if(EXISTS "${LDIR}/bin/Release/${_f}")
        execute_process(COMMAND "${CMAKE_COMMAND}" -E copy_if_different
            "${LDIR}/bin/Release/${_f}" "${BDIR}/${_f}")
    endif()
endforeach()
message(STATUS "Launcher: built and copied next to SilentHillPC.exe")
