#!/bin/bash
# Builds libLiteHtmlUnity.so for Android and installs it into the Unity project.
#
# Requirements (none of which this machine currently has — see README):
#   * Android NDK r23+   set ANDROID_NDK_HOME, or install via Android Studio
#   * cmake 3.22+ and ninja  (brew install cmake ninja)
#   * Unity's Android Build Support module, to actually build a player
#
#   ./build_android.sh                  arm64-v8a only (what modern devices use)
#   ./build_android.sh arm64-v8a armeabi-v7a
#
# NOTE: this script has not been executed in this repository — the toolchain is
# not installed here. It is written against the standard NDK CMake toolchain and
# is expected to work as-is, but treat the first run as unverified.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="$ROOT/../Assets/LiteHtmlUnity/Plugins/Android/libs"

ABIS=("$@")
if [ ${#ABIS[@]} -eq 0 ]; then
  ABIS=(arm64-v8a)
fi

NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}"

if [ -z "$NDK" ]; then
  # Unity ships an NDK with its Android module; prefer it so the toolchain
  # matches the one IL2CPP will use.
  for candidate in \
    /Applications/Unity/Hub/Editor/*/PlaybackEngines/AndroidPlayer/NDK \
    "$HOME/Library/Android/sdk/ndk/"* ; do
    if [ -d "$candidate" ]; then
      NDK="$candidate"
      break
    fi
  done
fi

if [ -z "$NDK" ] || [ ! -f "$NDK/build/cmake/android.toolchain.cmake" ]; then
  echo "Android NDK not found. Install it (Unity Hub > Add Modules > Android Build Support > NDK)" >&2
  echo "or set ANDROID_NDK_HOME." >&2
  exit 1
fi

echo "==> NDK: $NDK"

# Same layout bench_android.sh derives its compiler from; used below for llvm-strip.
HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt/" | head -1)"
TOOLS="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"

for ABI in "${ABIS[@]}"; do
  BUILD="$ROOT/build/android/$ABI"

  echo "==> configuring $ABI"
  cmake -S "$ROOT" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI="$ABI" \
    -DANDROID_PLATFORM=android-24 \
    -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release

  echo "==> building $ABI"
  cmake --build "$BUILD" --parallel

  mkdir -p "$OUT/$ABI"
  cp "$BUILD/libLiteHtmlUnity.so" "$OUT/$ABI/"

  # A Release build still carries its debug symbols, and Unity ships the .so into
  # the APK as-is: 21 MB unstripped against 1.8 MB stripped. The exported lhu_
  # symbols are what P/Invoke resolves and --strip-unneeded keeps all of them, so
  # this costs nothing but the ability to symbolise a native crash from this
  # exact binary -- keep $BUILD/libLiteHtmlUnity.so for that.
  STRIP="$TOOLS/llvm-strip"
  if [ -x "$STRIP" ]; then
    "$STRIP" --strip-unneeded "$OUT/$ABI/libLiteHtmlUnity.so"
  else
    echo "    warning: llvm-strip not found, shipping unstripped" >&2
  fi

  echo "    $OUT/$ABI/libLiteHtmlUnity.so ($(du -h "$OUT/$ABI/libLiteHtmlUnity.so" | cut -f1))"
done

echo
echo "In Unity, select each .so and set:"
echo "  Platform: Android, CPU: matching ABI"
