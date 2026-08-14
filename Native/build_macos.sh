#!/bin/bash
# Builds the LiteHtmlUnity native plugin as a universal (arm64 + x86_64) macOS
# bundle and drops it into the Unity project's Plugins folder.
#
# Deliberately does not use CMake: litehtml is a flat list of translation units,
# and driving clang directly keeps the toolchain requirement down to Xcode.
#
#   ./build_macos.sh            release build
#   ./build_macos.sh debug      -O0 -g, assertions on
#   ./build_macos.sh harness    also builds the standalone test harness

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LITEHTML="$ROOT/third_party/litehtml"
BUILD="$ROOT/build/macos"
OUT="$ROOT/../Assets/LiteHtmlUnity/Plugins/macOS/LiteHtmlUnity.bundle"

MODE="${1:-release}"

if [ "$MODE" = "debug" ]; then
  OPT="-O0 -g -DLHU_DEBUG=1"
else
  OPT="-O2 -DNDEBUG"
fi

ARCHS="-arch arm64 -arch x86_64"
MIN_OS="-mmacosx-version-min=11.0"

INCLUDES=(
  -I"$LITEHTML/include"
  # litehtml's own .cpp files include their headers unqualified ("url.h"), which
  # matches CMakeLists' PRIVATE include/litehtml entry.
  -I"$LITEHTML/include/litehtml"
  -I"$LITEHTML/src"
  -I"$LITEHTML/src/gumbo/include"
  -I"$LITEHTML/src/gumbo/include/gumbo"
  -I"$ROOT/third_party"
  -I"$ROOT/src"
)

CXXFLAGS="-std=c++17 -fvisibility=hidden -fvisibility-inlines-hidden -fPIC -Wall $OPT $ARCHS $MIN_OS"
CFLAGS="-std=c99 -fvisibility=hidden -fPIC $OPT $ARCHS $MIN_OS"

mkdir -p "$BUILD/obj"

echo "==> compiling litehtml ($(ls "$LITEHTML"/src/*.cpp | wc -l | tr -d ' ') files) + gumbo + wrapper"

# Emit one compile command per source file and run them in parallel. Sources are
# namespaced by directory so litehtml's document.cpp cannot collide with ours.
compile_list="$BUILD/compile.txt"
: > "$compile_list"

for f in "$LITEHTML"/src/*.cpp; do
  echo "cxx|$f|$BUILD/obj/lh_$(basename "$f" .cpp).o" >> "$compile_list"
done

for f in "$LITEHTML"/src/gumbo/*.c; do
  echo "cc|$f|$BUILD/obj/gumbo_$(basename "$f" .c).o" >> "$compile_list"
done

for f in "$ROOT"/src/*.cpp; do
  echo "cxx|$f|$BUILD/obj/lhu_$(basename "$f" .cpp).o" >> "$compile_list"
done

FAILED="$BUILD/failed.txt"
rm -f "$FAILED"

# Hand-rolled job control rather than make/xargs: the project path contains a
# space, which make handles badly, and the full include list overflows xargs'
# command-line assembly.
NCPU="$(sysctl -n hw.ncpu)"
running=0

# Newest header anywhere in the build. Any object older than it is rebuilt.
#
# This is deliberately coarse rather than per-file depfile tracking: clang's .d
# files escape spaces, and this project's path contains one, so parsing them in
# shell is a correctness hazard. Getting this wrong is not a cosmetic problem —
# editing a struct in a shared header and rebuilding only some of its users
# produces two different layouts in one binary, which shows up as heap
# corruption at a completely unrelated call site. Rebuilding everything on any
# header change costs about two seconds.
# `stat` prints "<mtime> <path>" for every header and a single global sort picks
# the newest. Piping into `ls -t` instead would be wrong: xargs splits a list
# this long across several `ls` calls, each sorted on its own, so the first line
# is only the newest of the first batch.
#
# `head -1` closes the pipe early and kills the producer with SIGPIPE, which
# `set -o pipefail` would turn into a failed build — hence the subshell.
NEWEST_HEADER="$(set +o pipefail
  find "$ROOT/src" "$ROOT/third_party" "$LITEHTML/include" "$LITEHTML/src" \
    \( -name '*.h' -o -name '*.hpp' -o -name '*.inc' \) -type f \
    -exec stat -f '%m %N' {} + 2>/dev/null \
    | sort -rn | head -1 | cut -d' ' -f2- || true)"

while IFS='|' read -r kind src obj; do
  [ -z "${kind:-}" ] && continue

  # Skip anything already up to date, against both its source and every header.
  if [ -f "$obj" ] && [ "$obj" -nt "$src" ] &&
     { [ -z "$NEWEST_HEADER" ] || [ "$obj" -nt "$NEWEST_HEADER" ]; }; then
    continue
  fi

  (
    if [ "$kind" = "cxx" ]; then
      clang++ $CXXFLAGS "${INCLUDES[@]}" -c "$src" -o "$obj" || echo "$src" >> "$FAILED"
    else
      clang $CFLAGS "${INCLUDES[@]}" -c "$src" -o "$obj" || echo "$src" >> "$FAILED"
    fi
  ) &

  running=$((running + 1))
  if [ "$running" -ge "$NCPU" ]; then
    wait
    running=0
  fi
done < "$compile_list"

wait

if [ -f "$FAILED" ]; then
  echo "compile failures:" >&2
  cat "$FAILED" >&2
  exit 1
fi

echo "==> linking bundle"

mkdir -p "$OUT/Contents/MacOS"

clang++ $CXXFLAGS -bundle \
  "$BUILD"/obj/*.o \
  -o "$OUT/Contents/MacOS/LiteHtmlUnity"

cat > "$OUT/Contents/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDevelopmentRegion</key><string>en</string>
  <key>CFBundleExecutable</key><string>LiteHtmlUnity</string>
  <key>CFBundleIdentifier</key><string>com.litehtmlunity.native</string>
  <key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
  <key>CFBundleName</key><string>LiteHtmlUnity</string>
  <key>CFBundlePackageType</key><string>BNDL</string>
  <key>CFBundleShortVersionString</key><string>0.1.0</string>
  <key>CFBundleVersion</key><string>1</string>
  <key>NSHumanReadableCopyright</key><string>litehtml BSD-3-Clause, gumbo Apache-2.0, stb_truetype public domain</string>
</dict>
</plist>
PLIST

echo "==> $OUT"
lipo -info "$OUT/Contents/MacOS/LiteHtmlUnity"
echo "    exported symbols: $(nm -gU "$OUT/Contents/MacOS/LiteHtmlUnity" | grep -c ' T _lhu_')"

if [ -f "$ROOT/tests/harness.cpp" ]; then
  echo "==> building test harness"
  mkdir -p "$BUILD/bin"

  LODEPNG="$LITEHTML/containers/test/lodepng.cpp"

  # lodepng is slow to compile and never changes; cache its object file.
  if [ ! -f "$BUILD/obj/test_lodepng.o" ] || [ "$LODEPNG" -nt "$BUILD/obj/test_lodepng.o" ]; then
    clang++ $CXXFLAGS -I"$LITEHTML/containers/test" -c "$LODEPNG" -o "$BUILD/obj/test_lodepng.o"
  fi

  clang++ $CXXFLAGS "${INCLUDES[@]}" -I"$ROOT/tests" -I"$LITEHTML/containers/test" \
    "$ROOT/tests/harness.cpp" "$BUILD/obj/test_lodepng.o" "$BUILD"/obj/lh_*.o "$BUILD"/obj/gumbo_*.o \
    "$BUILD"/obj/lhu_*.o \
    -o "$BUILD/bin/lhu_harness"

  echo "    $BUILD/bin/lhu_harness"
fi

if [ -f "$ROOT/tests/bench.cpp" ]; then
  echo "==> building benchmark"
  mkdir -p "$BUILD/bin"

  clang++ $CXXFLAGS "${INCLUDES[@]}" -I"$ROOT/tests" \
    "$ROOT/tests/bench.cpp" "$BUILD"/obj/lh_*.o "$BUILD"/obj/gumbo_*.o "$BUILD"/obj/lhu_*.o \
    -o "$BUILD/bin/lhu_bench"

  echo "    $BUILD/bin/lhu_bench"
fi

# EXPERIMENT E2. Both of these are separate binaries on purpose: adding passes
# to bench.cpp has been shown on this project to move the timings of untouched
# code by 10-28%, so the A/B has to be one binary run twice with a different
# LHU_EXP_QUADCACHE, not one binary with more work bolted on.
for e2 in verify_quadcache bench_frames; do
  if [ -f "$ROOT/tests/$e2.cpp" ]; then
    echo "==> building $e2"
    mkdir -p "$BUILD/bin"

    clang++ $CXXFLAGS "${INCLUDES[@]}" -I"$ROOT/tests" \
      "$ROOT/tests/$e2.cpp" "$BUILD"/obj/lh_*.o "$BUILD"/obj/gumbo_*.o "$BUILD"/obj/lhu_*.o \
      -o "$BUILD/bin/lhu_$e2"

    echo "    $BUILD/bin/lhu_$e2"
  fi
done
