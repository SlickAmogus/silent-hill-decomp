#!/usr/bin/env bash
# ===========================================================================
#  Silent Hill PC Port - macOS app bundle
#
#  Packages an already-built game (pc_port/build, via ./build.sh) into a
#  self-contained "Silent Hill PC.app": the SwiftUI launcher, the game binary,
#  the map .dylibs, shipped assets and every Homebrew library the game links
#  (SDL2 / SDL3, openal-soft, jpeg-turbo), so players need no Homebrew.
#
#  Usage (from anywhere):
#    launcher/macos/make_app.sh           -> pc_port/build/macos/Silent Hill PC.app
#    launcher/macos/make_app.sh --dmg     -> ...plus "Silent Hill PC.dmg" next to it
#
#  Requires the Xcode command line tools (swiftc, codesign, iconutil).
# ===========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PC_PORT="$(cd "$HERE/../.." && pwd)"
BUILD_DIR="${SH_BUILD_DIR:-$PC_PORT/build}"
OUT_DIR="$BUILD_DIR/macos"
APP_NAME="Silent Hill PC"
APP="$OUT_DIR/$APP_NAME.app"
WIN_RES="$PC_PORT/launcher/SilentHillPC_Launcher/Resources"
MAKE_DMG=0
[ "${1:-}" = "--dmg" ] && MAKE_DMG=1

[ "$(uname -s)" = "Darwin" ] || { echo "ERROR: macOS only." >&2; exit 1; }
[ -x "$BUILD_DIR/SilentHillPC" ] || { echo "ERROR: $BUILD_DIR/SilentHillPC not found - run ./build.sh first." >&2; exit 1; }
ls "$BUILD_DIR"/maps/*.dylib >/dev/null 2>&1 || { echo "ERROR: no map .dylibs in $BUILD_DIR/maps." >&2; exit 1; }

VERSION="$(sed -n 's/^#define SH_VER_MAJOR[[:space:]]*\([0-9]*\)/\1/p;s/^#define SH_VER_MINOR[[:space:]]*\([0-9]*\)/\1/p;s/^#define SH_VER_BUILD[[:space:]]*\([0-9]*\)/\1/p' "$BUILD_DIR/sh_version.h" 2>/dev/null | paste -sd. -)"
[ -n "$VERSION" ] || VERSION="0.0.0"

echo "=== Assembling $APP_NAME.app ($VERSION) ==="
rm -rf "$APP"
CONTENTS="$APP/Contents"
FW="$CONTENTS/Frameworks"
GAME_RES="$CONTENTS/Resources/game"
mkdir -p "$CONTENTS/MacOS" "$FW" "$GAME_RES/maps"

# --- Launcher --------------------------------------------------------------
swiftc -O -parse-as-library -target arm64-apple-macos13.0 \
    -o "$CONTENTS/MacOS/SilentHillLauncher" "$HERE"/Sources/*.swift

# --- Game + assets ----------------------------------------------------------
cp "$BUILD_DIR/SilentHillPC" "$CONTENTS/MacOS/SilentHillPC"
cp "$BUILD_DIR"/maps/*.dylib "$GAME_RES/maps/"
cp -R "$PC_PORT/assets/." "$GAME_RES/"
cp "$PC_PORT/config.cfg" "$GAME_RES/config.cfg"
cp "$WIN_RES/launcher.png" "$CONTENTS/Resources/launcher.png"

# --- Bundle Homebrew libraries ----------------------------------------------
# Copy every /opt/homebrew or /usr/local dependency into Frameworks/ and point
# the load commands at it, recursively.
bundle_deps() {
    local bin="$1" dep base
    for dep in $(otool -L "$bin" | tail -n +2 | awk '{print $1}'); do
        case "$dep" in /opt/homebrew/*|/usr/local/*) ;; *) continue ;; esac
        base="$(basename "$dep")"
        install_name_tool -change "$dep" "@executable_path/../Frameworks/$base" "$bin" 2>/dev/null
        if [ ! -f "$FW/$base" ]; then
            cp -L "$dep" "$FW/$base"
            chmod u+w "$FW/$base"
            install_name_tool -id "@executable_path/../Frameworks/$base" "$FW/$base" 2>/dev/null
            echo "  bundled $base"
            bundle_deps "$FW/$base"
        fi
    done
}
bundle_deps "$CONTENTS/MacOS/SilentHillPC"

# Homebrew's sdl2 is sdl2-compat, which dlopen()s SDL3 at runtime and looks
# for it at @loader_path/libSDL3.dylib first.
for sdl2 in "$FW"/libSDL2*.dylib; do
    [ -f "$sdl2" ] || continue
    if grep -q "Failed loading SDL3" "$sdl2"; then
        SDL3_LIB="$(brew --prefix sdl3 2>/dev/null)/lib/libSDL3.0.dylib"
        [ -f "$SDL3_LIB" ] || { echo "ERROR: sdl2-compat needs SDL3:  brew install sdl3" >&2; exit 1; }
        cp -L "$SDL3_LIB" "$FW/libSDL3.dylib"
        chmod u+w "$FW/libSDL3.dylib"
        install_name_tool -id "@loader_path/libSDL3.dylib" "$FW/libSDL3.dylib" 2>/dev/null
        echo "  bundled libSDL3.dylib (for sdl2-compat)"
        bundle_deps "$FW/libSDL3.dylib"
    fi
done

if otool -L "$CONTENTS/MacOS/SilentHillPC" "$FW"/*.dylib | grep -E '^\s+(/opt/homebrew|/usr/local)/'; then
    echo "ERROR: Homebrew paths still referenced (above)." >&2
    exit 1
fi

# --- Icon -------------------------------------------------------------------
ICON_TMP="$(mktemp -d)"
trap 'rm -rf "$ICON_TMP"' EXIT
sips --padToHeightWidth 384 384 "$WIN_RES/icon.png" --out "$ICON_TMP/square.png" >/dev/null
mkdir "$ICON_TMP/AppIcon.iconset"
for sz in 16 32 128 256 512; do
    sips -z $sz $sz "$ICON_TMP/square.png" --out "$ICON_TMP/AppIcon.iconset/icon_${sz}x${sz}.png" >/dev/null
    sips -z $((sz * 2)) $((sz * 2)) "$ICON_TMP/square.png" --out "$ICON_TMP/AppIcon.iconset/icon_${sz}x${sz}@2x.png" >/dev/null
done
iconutil -c icns "$ICON_TMP/AppIcon.iconset" -o "$CONTENTS/Resources/AppIcon.icns"

# --- Info.plist ---------------------------------------------------------------
sed "s/@VERSION@/$VERSION/g" "$HERE/Info.plist" > "$CONTENTS/Info.plist"

# --- Ad-hoc sign: nested code first, then the bundle. Unsigned .dylibs make
# Gatekeeper prompt once per map overlay. ------------------------------------
for f in "$FW"/*.dylib "$GAME_RES"/maps/*.dylib "$CONTENTS/MacOS/SilentHillPC"; do
    codesign --force --sign - "$f" 2>/dev/null
done
codesign --force --sign - "$APP" 2>/dev/null
echo "Built: $APP"

if [ "$MAKE_DMG" = 1 ]; then
    DMG="$OUT_DIR/$APP_NAME.dmg"
    STAGE="$(mktemp -d)"
    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"
    rm -f "$DMG"
    hdiutil create -volname "$APP_NAME" -srcfolder "$STAGE" -ov -format UDZO "$DMG" >/dev/null
    rm -rf "$STAGE"
    echo "Built: $DMG"
fi
