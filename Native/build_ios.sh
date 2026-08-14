#!/bin/bash
# Builds the Doctype native code as a static library for iOS.
#
# iOS plugins must be static: Unity links them into the generated Xcode project
# and P/Invoke resolves through `__Internal`, which is why HtmlNative.Lib
# switches to "__Internal" under UNITY_IOS.
#
#   ./build_ios.sh              device (arm64) + simulator (arm64)
#   ./build_ios.sh device       device only
#
# Output: Assets/Doctype/Plugins/iOS/libDoctype.a
#
# Only Xcode is required — no CMake, no NDK. The simulator slice is built too so
# the library can be smoke-tested without a signing profile, but Unity is given
# the device slice only: a fat static library containing both would fail App
# Store validation.

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LITEHTML="$ROOT/third_party/litehtml"
BUILD="$ROOT/build/ios"
OUT="$ROOT/../Assets/Doctype/Plugins/iOS"

TARGETS="${1:-all}"

MIN_IOS=13.0

INCLUDES=(
  -I"$LITEHTML/include"
  -I"$LITEHTML/include/litehtml"
  -I"$LITEHTML/src"
  -I"$LITEHTML/src/gumbo/include"
  -I"$LITEHTML/src/gumbo/include/gumbo"
  -I"$ROOT/third_party"
  -I"$ROOT/src"
)

build_slice() {
  local name="$1" sdk="$2" target="$3"
  local sysroot obj

  sysroot="$(xcrun --sdk "$sdk" --show-sdk-path)"
  obj="$BUILD/$name"
  mkdir -p "$obj"

  # -fembed-bitcode is deliberately absent: Apple removed bitcode support, and
  # passing it now only produces warnings.
  local common="-isysroot $sysroot -target $target -fvisibility=hidden -fPIC -O2 -DNDEBUG"
  local cxxflags="-std=c++17 $common -fvisibility-inlines-hidden -fexceptions -frtti"
  local cflags="-std=c99 $common"

  echo "==> $name ($target)"

  local failed="$obj/failed.txt"
  rm -f "$failed"

  local ncpu running=0
  ncpu="$(sysctl -n hw.ncpu)"

  for f in "$LITEHTML"/src/*.cpp "$ROOT"/src/*.cpp; do
    local o="$obj/$(basename "$(dirname "$f")")_$(basename "$f" .cpp).o"
    (xcrun --sdk "$sdk" clang++ $cxxflags "${INCLUDES[@]}" -c "$f" -o "$o" || echo "$f" >> "$failed") &
    running=$((running + 1))
    if [ "$running" -ge "$ncpu" ]; then wait; running=0; fi
  done
  wait

  for f in "$LITEHTML"/src/gumbo/*.c; do
    local o="$obj/gumbo_$(basename "$f" .c).o"
    (xcrun --sdk "$sdk" clang $cflags "${INCLUDES[@]}" -c "$f" -o "$o" || echo "$f" >> "$failed") &
    running=$((running + 1))
    if [ "$running" -ge "$ncpu" ]; then wait; running=0; fi
  done
  wait

  if [ -f "$failed" ]; then
    echo "compile failures in $name:" >&2
    cat "$failed" >&2
    exit 1
  fi

  xcrun --sdk "$sdk" libtool -static -o "$BUILD/libDoctype-$name.a" "$obj"/*.o 2>/dev/null

  echo "    $BUILD/libDoctype-$name.a ($(du -h "$BUILD/libDoctype-$name.a" | cut -f1))"
}

build_slice device iphoneos "arm64-apple-ios$MIN_IOS"

if [ "$TARGETS" = "all" ]; then
  build_slice simulator iphonesimulator "arm64-apple-ios$MIN_IOS-simulator"
fi

mkdir -p "$OUT"
cp "$BUILD/libDoctype-device.a" "$OUT/libDoctype.a"

echo
echo "==> $OUT/libDoctype.a"
lipo -info "$OUT/libDoctype.a"
echo "    exported lhu_ symbols: $(nm -g "$OUT/libDoctype.a" 2>/dev/null | grep -c ' T _lhu_')"
echo
echo "In Unity, select the .a and set Platform: iOS, then build. C# resolves it"
echo "through __Internal, so no further wiring is needed."
