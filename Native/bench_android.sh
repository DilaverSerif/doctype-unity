#!/bin/bash
# Cross-compiles the benchmark for Android arm64, pushes it to a connected
# device and runs it there.
#
# This deliberately skips Unity: the CPU side of this system (HTML parse, CSS,
# layout, quad recording) is plain C++ and does not need a player build to be
# measured. It answers "how fast is this on the actual phone" in one command,
# instead of extrapolating from desktop numbers.
#
# What it does NOT measure: the GPU. Overdraw, vertex bandwidth and shader cost
# need a real Unity build on the device.
#
#   ./bench_android.sh [iterations]
#
# Requires: Android NDK (sdkmanager "ndk;28.2.13676358"), a device with USB
# debugging enabled and authorised.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LITEHTML="$ROOT/third_party/litehtml"
BUILD="$ROOT/build/android-bench"

ITERATIONS="${1:-200}"

# API 24 is the floor Unity itself targets; nothing here needs anything newer.
API=24
ABI_TRIPLE="aarch64-linux-android$API"

NDK="${ANDROID_NDK_HOME:-}"
if [ -z "$NDK" ]; then
  NDK="$(ls -d "$HOME/Library/Android/sdk/ndk/"* 2>/dev/null | sort -V | tail -1 || true)"
fi

if [ -z "$NDK" ] || [ ! -d "$NDK" ]; then
  echo "Android NDK not found. Install it with:" >&2
  echo "  \$HOME/Library/Android/sdk/cmdline-tools/latest/bin/sdkmanager 'ndk;28.2.13676358'" >&2
  exit 1
fi

HOST_TAG="$(ls "$NDK/toolchains/llvm/prebuilt/" | head -1)"
TOOLS="$NDK/toolchains/llvm/prebuilt/$HOST_TAG/bin"
CXX="$TOOLS/${ABI_TRIPLE}-clang++"
CC="$TOOLS/${ABI_TRIPLE}-clang"

if [ ! -x "$CXX" ]; then
  echo "toolchain not found at $CXX" >&2
  exit 1
fi

if ! adb get-state >/dev/null 2>&1; then
  echo "No device. Check that:" >&2
  echo "  * the cable carries data (charge-only cables enumerate nothing)" >&2
  echo "  * the phone is plugged into the Mac directly, not through a hub" >&2
  echo "  * USB debugging is on and this computer is authorised" >&2
  exit 1
fi

DEVICE="$(adb shell getprop ro.product.model 2>/dev/null | tr -d '\r') / $(adb shell getprop ro.soc.model 2>/dev/null | tr -d '\r')"
echo "==> device: $DEVICE"

INCLUDES=(
  -I"$LITEHTML/include"
  -I"$LITEHTML/include/litehtml"
  -I"$LITEHTML/src"
  -I"$LITEHTML/src/gumbo/include"
  -I"$LITEHTML/src/gumbo/include/gumbo"
  -I"$ROOT/third_party"
  -I"$ROOT/src"
)

# -static-libstdc++ so the binary runs from /data/local/tmp without needing the
# NDK's shared C++ runtime pushed alongside it.
CXXFLAGS="-std=c++17 -O2 -DNDEBUG -fexceptions -frtti -fPIE"
CFLAGS="-std=c99 -O2 -DNDEBUG -fPIE"

mkdir -p "$BUILD/obj"

echo "==> cross-compiling for $ABI_TRIPLE"

NCPU="$(sysctl -n hw.ncpu)"
running=0
FAILED="$BUILD/failed.txt"
rm -f "$FAILED"

# `set -o pipefail` would turn into a failed build — hence the subshell.
NEWEST_HEADER="$(set +o pipefail
  find "$ROOT/src" "$ROOT/third_party" "$LITEHTML/include" "$LITEHTML/src" \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -type f \
    -exec stat -f '%m %N' {} + 2>/dev/null \
    | sort -rn | head -1 | cut -d' ' -f2- || true)"

for f in "$LITEHTML"/src/*.cpp "$ROOT"/src/*.cpp; do
  obj="$BUILD/obj/$(basename "$(dirname "$f")")_$(basename "$f" .cpp).o"
  if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ] || { [ -n "$NEWEST_HEADER" ] && [ "$NEWEST_HEADER" -nt "$obj" ]; }; then
    ("$CXX" $CXXFLAGS "${INCLUDES[@]}" -c "$f" -o "$obj" || echo "$f" >> "$FAILED") &
    running=$((running + 1))
    if [ "$running" -ge "$NCPU" ]; then wait; running=0; fi
  fi
done
wait

for f in "$LITEHTML"/src/gumbo/*.c; do
  obj="$BUILD/obj/gumbo_$(basename "$f" .c).o"
  if [ ! -f "$obj" ] || [ "$f" -nt "$obj" ] || { [ -n "$NEWEST_HEADER" ] && [ "$NEWEST_HEADER" -nt "$obj" ]; }; then
    ("$CC" $CFLAGS "${INCLUDES[@]}" -c "$f" -o "$obj" || echo "$f" >> "$FAILED") &
    running=$((running + 1))
    if [ "$running" -ge "$NCPU" ]; then wait; running=0; fi
  fi
done
wait

if [ -f "$FAILED" ]; then
  echo "compile failures:" >&2
  cat "$FAILED" >&2
  exit 1
fi

"$CXX" $CXXFLAGS "${INCLUDES[@]}" -I"$ROOT/tests" \
  "$ROOT/tests/bench.cpp" "$BUILD"/obj/*.o \
  -static-libstdc++ -pie \
  -o "$BUILD/lhu_bench"

echo "    $BUILD/lhu_bench ($(du -h "$BUILD/lhu_bench" | cut -f1))"

# The benchmark reads a system font. Android keeps its fonts in /system/fonts,
# and the desktop build hardcodes a macOS path, so push a font next to the
# binary and point the benchmark at it.
FONT_ON_DEVICE=""
for candidate in /system/fonts/Roboto-Regular.ttf /system/fonts/DroidSans.ttf; do
  if adb shell "[ -f $candidate ] && echo yes" 2>/dev/null | tr -d '\r' | grep -q yes; then
    FONT_ON_DEVICE="$candidate"
    break
  fi
done

if [ -z "$FONT_ON_DEVICE" ]; then
  echo "no usable font found on the device under /system/fonts" >&2
  exit 1
fi

echo "==> pushing to /data/local/tmp"
adb push "$BUILD/lhu_bench" /data/local/tmp/lhu_bench >/dev/null
adb shell chmod 755 /data/local/tmp/lhu_bench

echo "==> running on device"
echo
adb shell "LHU_FONT=$FONT_ON_DEVICE /data/local/tmp/lhu_bench $ITERATIONS" 2>&1 | tr -d '\r'
