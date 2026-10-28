# Sourced by the XDK build scripts: puts cl/link/imagexex on PATH and points
# them at the XDK's own CRT, never at a host Visual Studio.
#
# The XDK itself never enters the repo. It is referenced through %XEDK%, which
# its installer sets; override XEDK if it lives elsewhere.
XEDK="${XEDK:-C:\\Program Files (x86)\\Microsoft Xbox 360 SDK}"
XEDK_U="$(cygpath -u "$XEDK")"
if [ ! -x "$XEDK_U/bin/win32/cl.exe" ]; then
    echo "xdk_env.sh: no cl.exe under '$XEDK' - the XDK install is tools-only" >&2
    return 1 2>/dev/null || exit 1
fi
export PATH="$XEDK_U/bin/win32:$PATH"
export INCLUDE="$XEDK\\include\\xbox"
export LIB="$XEDK\\lib\\xbox"
# Git Bash rewrites /D and /I switches into paths. The scripts use the -D
# spelling cl also accepts, and this stops the rewrite for anything that slips.
export MSYS_NO_PATHCONV=1
