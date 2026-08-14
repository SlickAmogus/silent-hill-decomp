#!/usr/bin/env bash
# Build the Android APK. Mirrors xbox_port/build_xbox.sh in spirit: one entry
# point that pins the toolchain so the build does not depend on whatever the
# calling shell happens to have on PATH.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# JDK 17: Android Gradle Plugin 8.x does not run on a newer JDK, and the
# machine's default java is 24.
export JAVA_HOME="${JAVA_HOME_ANDROID:-C:/Android/jdk-17}"
GRADLE="${GRADLE_BIN:-C:/Android/gradle-8.9/bin/gradle.bat}"

TASK="${1:-assembleDebug}"

cd "$HERE"
"$GRADLE" --no-daemon ":app:${TASK}"

APK="$HERE/app/build/outputs/apk/debug/app-debug.apk"
if [ -f "$APK" ]; then
    echo
    echo "APK: $APK"
    echo "Install with: adb install -r \"$APK\""
fi
