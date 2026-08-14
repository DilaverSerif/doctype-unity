#!/bin/bash
# E2 (rebased) — every on/off combination of the four experiment toggles.
#
#   E1 LHU_EXP_SUBTREE     layout short-circuit
#   E2 LHU_EXP_QUADCACHE   retained display list
#   E5 LHU_EXP_FLOATS      float-list index
#   E6 LHU_EXP_STYLECACHE  inline style= memoisation
#
# Three claims, checked separately:
#   1. lhu_harness stays at 74 checks / 0 failed in all 16.
#   2. lhu_bench's correctness sections report 0 failed in all 16.
#   3. every recorded frame is byte-identical across all 16. The verifier drives
#      a cache-on and a cache-off context in lockstep and memcmp's them, so one
#      run settles the E2 axis internally; it digests *both* contexts, so eight
#      runs over (E1, E5, E6) pin down all sixteen.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/macos/bin"
OUT="${1:-$ROOT/build/toggles}"
mkdir -p "$OUT"

fail=0
printf '%-6s %-6s %-6s %-6s | %-18s | %-22s | %s\n' E1 E2 E5 E6 harness bench-correctness verifier
printf -- '---------------------------------------------------------------------------------------------\n'

for e1 in 1 0; do for e2 in 1 0; do for e5 in 1 0; do for e6 in 1 0; do
  tag="e1${e1}_e2${e2}_e5${e5}_e6${e6}"
  export LHU_EXP_SUBTREE=$e1 LHU_EXP_QUADCACHE=$e2 LHU_EXP_FLOATS=$e5 LHU_EXP_STYLECACHE=$e6

  h=$("$BIN/lhu_harness" 2>&1 | tail -3 | grep -oE '[0-9]+ checks, [0-9]+ failed' | tail -1)
  [[ "$h" == "74 checks, 0 failed" ]] || fail=1

  "$BIN/lhu_bench" > "$OUT/bench_$tag.txt" 2>&1
  b=$(grep -cE '^\s*\[FAIL\]' "$OUT/bench_$tag.txt")
  bl=$(grep -E 'check\(s\) failed' "$OUT/bench_$tag.txt" | grep -vcE '  0 check')
  bs="0 failed"
  { [ "$b" -eq 0 ] && [ "$bl" -eq 0 ]; } || { bs="$b FAIL lines"; fail=1; }

  # The verifier's own E2 axis is internal, so it only needs the other three.
  if [ "$e2" = "1" ]; then
    LHU_VERIFY_DUMP="$OUT/frames_e1${e1}_e5${e5}_e6${e6}.txt" \
      "$BIN/lhu_verify_quadcache" > "$OUT/verify_$tag.txt" 2>&1
    v=$(tail -1 "$OUT/verify_$tag.txt")
    [[ "$v" == *" 0 failed" ]] || fail=1
  else
    v="(E2 axis covered inside the E2=1 runs)"
  fi

  printf '%-6s %-6s %-6s %-6s | %-18s | %-22s | %s\n' "$e1" "$e2" "$e5" "$e6" "$h" "$bs" "$v"
done; done; done; done

echo
echo "--- cross-combination frame identity ---"
ref="$OUT/frames_e11_e51_e61.txt"
same=0; diffs=0
for f in "$OUT"/frames_*.txt; do
  if diff -q "$ref" "$f" >/dev/null 2>&1; then same=$((same+1)); else
    diffs=$((diffs+1)); fail=1
    echo "  DIFFERS: $(basename "$f")"
    diff "$ref" "$f" | head -6
  fi
done
echo "  $same digest streams identical to the reference, $diffs differ"
echo "  ($(wc -l < "$ref" | tr -d ' ') frames per stream, each line carrying the"
echo "   cache-ON and cache-OFF quad hashes, so all 16 combinations are covered)"

echo
[ "$fail" -eq 0 ] && echo "ALL 16 TOGGLE COMBINATIONS OK" || echo "TOGGLE MATRIX FAILED"
exit $fail
