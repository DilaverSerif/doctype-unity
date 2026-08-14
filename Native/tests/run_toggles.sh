#!/bin/bash
# E7 (lhu_set_style) — every on/off combination of the five experiment toggles.
#
#   E1 LHU_EXP_SUBTREE     layout short-circuit
#   E2 LHU_EXP_QUADCACHE   retained display list
#   E5 LHU_EXP_FLOATS      float-list index
#   E6 LHU_EXP_STYLECACHE  inline style= memoisation
#   E7 LHU_EXP_SETSTYLE    inline style mutation without a re-parse
#
# Three claims, checked separately:
#   1. lhu_harness stays at 74 checks / 0 failed in all 32.
#   2. lhu_bench's correctness sections report 0 failed in all 32. The style
#      section drives E7 by hand so that it runs identically either way, exactly
#      as the layout section drives E1 -- a toggle that made its own tests
#      vacuous would prove nothing.
#   3. every recorded frame is byte-identical across all 32. The verifier drives
#      a cache-on and a cache-off context in lockstep and memcmp's them, so one
#      run settles the E2 axis internally; it digests *both* contexts, so
#      sixteen runs over (E1, E5, E6, E7) pin down all thirty-two.
set -uo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/build/macos/bin"
OUT="${1:-$ROOT/build/toggles}"
mkdir -p "$OUT"

fail=0
printf '%-4s %-4s %-4s %-4s %-4s | %-18s | %-22s | %s\n' E1 E2 E5 E6 E7 harness bench-correctness verifier
printf -- '-------------------------------------------------------------------------------------------------\n'

for e1 in 1 0; do for e2 in 1 0; do for e5 in 1 0; do for e6 in 1 0; do for e7 in 1 0; do
  tag="e1${e1}_e2${e2}_e5${e5}_e6${e6}_e7${e7}"
  export LHU_EXP_SUBTREE=$e1 LHU_EXP_QUADCACHE=$e2 LHU_EXP_FLOATS=$e5 LHU_EXP_STYLECACHE=$e6 LHU_EXP_SETSTYLE=$e7

  h=$("$BIN/lhu_harness" 2>&1 | tail -3 | grep -oE '[0-9]+ checks, [0-9]+ failed' | tail -1)
  [[ "$h" == "74 checks, 0 failed" ]] || fail=1

  "$BIN/lhu_bench" 40 > "$OUT/bench_$tag.txt" 2>&1
  b=$(grep -cE '^\s*\[FAIL\]' "$OUT/bench_$tag.txt")
  bl=$(grep -E 'check\(s\) failed' "$OUT/bench_$tag.txt" | grep -vcE '  0 check')
  bs="0 failed"
  { [ "$b" -eq 0 ] && [ "$bl" -eq 0 ]; } || { bs="$b FAIL lines"; fail=1; }

  # The verifier's own E2 axis is internal, so it only needs the other four.
  if [ "$e2" = "1" ]; then
    LHU_VERIFY_DUMP="$OUT/frames_e1${e1}_e5${e5}_e6${e6}_e7${e7}.txt" \
      "$BIN/lhu_verify_quadcache" > "$OUT/verify_$tag.txt" 2>&1
    v=$(tail -1 "$OUT/verify_$tag.txt")
    [[ "$v" == *" 0 failed" ]] || fail=1
  else
    v="(E2 axis covered inside the E2=1 runs)"
  fi

  printf '%-4s %-4s %-4s %-4s %-4s | %-18s | %-22s | %s\n' "$e1" "$e2" "$e5" "$e6" "$e7" "$h" "$bs" "$v"
done; done; done; done; done

echo
echo "--- cross-combination frame identity (verifier) ---"
ref="$OUT/frames_e11_e51_e61_e71.txt"
same=0; diffs=0
for f in "$OUT"/frames_*.txt; do
  if diff -q "$ref" "$f" >/dev/null 2>&1; then same=$((same+1)); else
    diffs=$((diffs+1)); fail=1
    echo "  DIFFERS: $(basename "$f")"
    diff "$ref" "$f" | head -6
  fi
done
echo "  $same digest streams identical to the reference, $diffs differ"

# The verifier never calls lhu_set_style, so the check above cannot see E7. The
# bench does, and its correctness sections print the quad count and document size
# of every mutation -- so those lines, compared across all 32 runs, are the E7
# axis of the same claim.
echo
echo "--- cross-combination identity of the mutation results (bench) ---"
bref="$OUT/mut_e11_e21_e51_e61_e71.txt"
for f in "$OUT"/bench_*.txt; do
  tag="${f##*/bench_}"; tag="${tag%.txt}"
  # Two diagnostics in those lines are toggle-dependent by design and are not
  # output: how many of the 600 mutations reported a change, and how many entries
  # the E6 memo holds (zero when E6 is off). Everything else -- quad counts,
  # document sizes, byte-identity verdicts -- must match across all 32.
  grep -E '^\s*\[(PASS|FAIL)\]' "$f" \
    | sed -e 's/[0-9]\{1,\} of 600 applied; //' \
          -e 's/inline-style memo [0-9]\{1,\} -> [0-9]\{1,\} entries/inline-style memo (n\/a)/' \
    > "$OUT/mut_$tag.txt"
done
bsame=0; bdiff=0
for f in "$OUT"/mut_*.txt; do
  if diff -q "$bref" "$f" >/dev/null 2>&1; then bsame=$((bsame+1)); else
    bdiff=$((bdiff+1)); fail=1
    echo "  DIFFERS: $(basename "$f")"
    diff "$bref" "$f" | head -8
  fi
done
echo "  $bsame check streams identical to the reference, $bdiff differ"
echo "  ($(wc -l < "$bref" | tr -d ' ') checks per stream, each carrying its quad count and document size)"

echo
[ "$fail" -eq 0 ] && echo "ALL 32 TOGGLE COMBINATIONS OK" || echo "TOGGLE MATRIX FAILED"
exit $fail
