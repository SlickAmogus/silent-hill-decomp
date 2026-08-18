#!/usr/bin/env bash
# ===========================================================================
#  Silent Hill — iOS build
#
#  Produces an UNSIGNED SilentHill.ipa. Signing happens off-box: download the
#  artifact on Windows and install it with Sideloadly or AltServer using a free
#  Apple ID. Nothing here needs a keychain, a provisioning profile or a
#  developer account, which is what lets CI build it.
#
#  Requires macOS with Xcode (the toolchain, not the IDE) plus cmake and ninja.
#  There is no way around the macOS requirement: only Apple's clang can emit
#  arm64-apple-ios, and Homebrew GCC cannot target iOS at all.
#
#  Usage:
#    ./build_ios.sh              configure (if needed) + build + package
#    ./build_ios.sh rebuild      clean rebuild
#    ./build_ios.sh configure    force a fresh configure, then build
#    ./build_ios.sh simulator    build for the iOS Simulator instead of a device
# ===========================================================================
set -o pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
MODE="${1:-build}"

BUILD_DIR="$REPO_ROOT/build-ios"
SYSROOT="iphoneos"
if [ "$MODE" = "simulator" ]; then
    BUILD_DIR="$REPO_ROOT/build-ios-sim"
    SYSROOT="iphonesimulator"
fi

for tool in cmake ninja xcrun; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "ERROR: '$tool' not found. Needs macOS with Xcode + cmake + ninja." >&2
        exit 1
    }
done
if [ ! -e "$SCRIPT_DIR/SDL/CMakeLists.txt" ]; then
    echo "ERROR: SDL submodule missing at ios_port/SDL" >&2
    echo "  git submodule update --init --recursive ios_port/SDL" >&2
    exit 1
fi

need_configure=0
[ "$MODE" = "configure" ] && need_configure=1
[ -f "$BUILD_DIR/CMakeCache.txt" ] || need_configure=1
[ "$MODE" = "configure" ] && rm -rf "$BUILD_DIR"

if [ "$need_configure" = 1 ]; then
    echo "=== Configuring (iOS / $SYSROOT / arm64) ==="
    # Ninja, not the Xcode generator: signing is done off-box so Xcode's
    # signing integration buys nothing, and this keeps the build shaped like
    # every other one in the repo.
    cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_SYSTEM_NAME=iOS \
        -DCMAKE_OSX_ARCHITECTURES=arm64 \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0 \
        -DCMAKE_OSX_SYSROOT="$SYSROOT" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo || exit 1
fi

echo "=== Building ==="
if [ "$MODE" = "rebuild" ]; then
    cmake --build "$BUILD_DIR" --clean-first || exit 1
else
    cmake --build "$BUILD_DIR" || exit 1
fi

APP="$(find "$BUILD_DIR" -maxdepth 4 -name 'SilentHill.app' -type d | head -1)"
if [ -z "$APP" ]; then
    echo "ERROR: SilentHill.app was not produced." >&2
    exit 1
fi
echo "Built: $APP"

# An .ipa is just a zip with the .app inside a Payload/ directory. Unsigned is
# fine — Sideloadly re-signs on the way onto the device.
echo "=== Packaging .ipa ==="
STAGE="$BUILD_DIR/ipa"
rm -rf "$STAGE"
mkdir -p "$STAGE/Payload"
cp -R "$APP" "$STAGE/Payload/"
( cd "$STAGE" && zip -qry "$BUILD_DIR/SilentHill-unsigned.ipa" Payload ) || exit 1

echo "OK: $BUILD_DIR/SilentHill-unsigned.ipa"
echo
echo "This .ipa is UNSIGNED. To install it on a device from Windows:"
echo "  1. Install iTunes and iCloud from apple.com, NOT the Microsoft Store"
echo "     (the Store builds break both Sideloadly and AltServer)."
echo "  2. Open Sideloadly, drag the .ipa in, sign in with your Apple ID."
echo "  3. A free account's certificate lasts 7 days; leave Sideloadly's"
echo "     auto-refresh running, or a paid account raises that to a year."
echo
echo "The game needs your own Silent Hill disc image. Once installed, put the"
echo "BIN/CUE in Files.app under 'On My iPhone > Silent Hill > gamedata'."
