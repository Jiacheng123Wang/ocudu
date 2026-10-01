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
for src in "$HERE"/[0-9][0-9]_*.c; do
  name="$(basename "$src" .c)"
  bin="$OUT/$name"
  if ! clang $CFLAGS -o "$bin" "$src" 2>"$OUT/$name.build.log"; then
    echo "BUILD FAILED: $name (see $OUT/$name.build.log)"
    FAIL=1
    continue
  fi
  echo "======================================================================"
  echo "== $name"
  echo "======================================================================"
  case "$name" in
    02_* | 07_*) "$bin" hostile 2>&1 | tee "$OUT/$name.log" ;;
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
