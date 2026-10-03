#!/usr/bin/env bash
# Build and run every scheduling micro-benchmark.  These are DELIBERATELY noisy (they spawn 2x the
# core count as spinners), so they must not run while a leg is flying.
set -u

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE/out}"
mkdir -p "$OUT"

if pgrep -x gnb >/dev/null; then
  echo "REFUSING: a gnb leg is flying (discipline 11: no build/loopback/load while a leg flies)"
  exit 1
fi

CFLAGS="-O2 -Wall"
FAIL=0
# BOTH extensions are iterated: the .mm probes (Metal, added 2026-10-03 with 10_) have no .c twin, and the
# glob used to be `[0-9][0-9]_*.c` - which would simply never build them, silently, and the harness would
# report a clean run with a probe missing from it.
for src in "$HERE"/[0-9][0-9]_*.c "$HERE"/[0-9][0-9]_*.mm; do
  [ -f "$src" ] || continue
  case "$src" in
    *.mm) name="$(basename "$src" .mm)" ;;
    *)    name="$(basename "$src" .c)" ;;
  esac
  bin="$OUT/$name"
  case "$src" in
    *.mm)
      # Objective-C++ probes need the frameworks. They stay OUT of the tree's engine on purpose: what they
      # measure is the platform mechanism (encode+commit), not the pipeline's wrappers around it.
      if ! clang++ -O2 -std=c++17 -ObjC++ -fobjc-arc -framework Metal -framework Foundation \
           -o "$bin" "$src" 2>"$OUT/$name.build.log"; then
        echo "BUILD FAILED: $name (see $OUT/$name.build.log)"
        FAIL=1
        continue
      fi
      ;;
    *)
      if ! clang $CFLAGS -o "$bin" "$src" 2>"$OUT/$name.build.log"; then
        echo "BUILD FAILED: $name (see $OUT/$name.build.log)"
        FAIL=1
        continue
      fi
      ;;
  esac
  echo "======================================================================"
  echo "== $name"
  echo "======================================================================"
  case "$name" in
    02_* | 07_*) "$bin" hostile 2>&1 | tee "$OUT/$name.log" ;;
    10_*)        "$bin" idle    2>&1 | tee "$OUT/$name.idle.log"
                 "$bin" hostile 2>&1 | tee "$OUT/$name.hostile.log" ;;
    *)           "$bin"         2>&1 | tee "$OUT/$name.log" ;;
  esac
  echo
done

# The two arms that need the machine IDLE as a contrast (02 has an idle mode).
if [ -x "$OUT/02_wakeup_latency_regimes" ]; then
  echo "======================================================================"
  echo "== 02_wakeup_latency_regimes idle"
  echo "======================================================================"
  "$OUT/02_wakeup_latency_regimes" idle 2>&1 | tee "$OUT/02_wakeup_latency_regimes.idle.log"
fi

echo
echo "logs in $OUT"
exit $FAIL
