#!/usr/bin/env bash
# The 10 L1 geometries + the refusal probe, each bounded in wall clock (a GPU hang must not need a reboot).
cd /Users/jiachengwang/dev/ocudu/lib/phy/upper/signal_processors/channel_estimator/metal || exit 1
pass=0; fail=0
for g in "4 1 1" "4 2 1" "4 4 1" "25 3 1" "25 3 2" "25 3 4" "13 2 2" "12 3 4" "1 2 1" "106 3 1"; do
  out=$(mktemp /tmp/l1run.XXXXXX)
  /tmp/l1/l1 $g > "$out" 2>&1 &
  pid=$!
  ( sleep 90; kill -9 $pid 2>/dev/null ) & killer=$!
  wait $pid; rc=$?
  kill $killer 2>/dev/null
  line=$(grep -o "L1: .*" "$out" | tail -1)
  sm=$(grep -o "smoothed max_rel=[0-9.e+-]*" "$out" | tail -1)
  s2=$(grep -o "sigma2 dev=[0-9.e+-]* host=[0-9.e+-]* rel=[0-9.e+-]*" "$out" | tail -1)
  printf '%-12s rc=%-3s %-10s | %-28s | %s\n' "$g" "$rc" "$line" "$sm" "$s2"
  [ "$line" = "L1: PASS" ] && pass=$((pass+1)) || fail=$((fail+1))
  rm -f "$out"
done
# Negative probe: a geometry outside the kernel contract must be refused (not silently clamped).
out=$(mktemp /tmp/l1ref.XXXXXX)
L1_REFUSE=1 /tmp/l1/l1 4 2 1 > "$out" 2>&1 &
pid=$!; ( sleep 90; kill -9 $pid 2>/dev/null ) & killer=$!; wait $pid; kill $killer 2>/dev/null
printf 'REFUSE probe: %s\n' "$(grep -o "REFUSE:.*" "$out" | tail -1)"
rm -f "$out"
echo "L1_SUMMARY pass=$pass fail=$fail"
