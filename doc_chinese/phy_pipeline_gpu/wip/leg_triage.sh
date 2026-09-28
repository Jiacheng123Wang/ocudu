#!/usr/bin/env bash
# Classify one leg's RADIO HOST INTERFACE and hand the operator the two commands that catch a bad one.
#
# WHY (dev doc 6.157 (10), user's reading accepted 2026-09-29). The receive tail `[ul_rx_timing] recv over
# 1ms / calls` is structurally prone on the GPU path (cpu 0/4 sick, cpu_gpu 10/24, gpu 56/61 - Fisher
# p = 7.0e-08 for cpu+cpu_gpu against gpu, and tonight's 11-minute contrast between a gpu leg at 0.0058% and
# two cpu_gpu legs at 0.0002%). Its PROXIMAL site is the host <-> radio interface: the DL transmit call's own
# `over 1ms` share moves with it at a stable ratio of 1.3-1.6 over 86 legs, in the other direction and on
# another thread. That covariate is therefore how a leg is CLASSIFIED - it is not an explanation, and it does
# not excuse the GPU path. Two things follow, and this script is both:
#
#   1. every measurement leg gets a class BEFORE its numbers are read (so a tailed leg is never again
#      attributed to code without saying what its transport was doing);
#   2. a bad class is the moment to catch the mechanism with an EXTERNAL sampler - `sample`/`spindump` show
#      whether UHD's own worker threads are off-CPU (host scheduling: our named threads run at
#      USER_INTERACTIVE/USER_INITIATED on the performance cores, macos_compat.cpp:285) or inside the kernel
#      (IOGPU / IOUSBHostFamily). Nothing inside the PHY can answer that, which is why the next instrument is
#      outside this process.
#
# The band is MEASURED, not chosen: over the legs on record the healthy side tops out at 0.0064% and the sick
# side starts at 0.0248% (a four-fold gap); 0.0064-0.0248% is reported as MARGINAL rather than folded into
# either side, because p114 sat there (0.0218%) while p125 sat on the healthy EDGE (0.0058%, with a 0.0095%
# receive tail) - and a leg whose transport is not clean must not license a tail reading.
#
# usage:
#   bash leg_triage.sh p125-n78-default          # classify the newest leg with that label
#   bash leg_triage.sh                            # list the newest few legs, classified
set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)
LOGDIR=$ROOT/doc_chinese/phy_pipeline_gpu/wip/logs
LABEL=${1:-}

classify() { # $1 = .log.stderr path
  python3 - "$1" <<'PY'
import re, sys
txt = open(sys.argv[1], errors="replace").read()
def last(rx):
    m = re.findall(rx, txt)
    return m[-1] if m else None
call = last(r"\[dl_tx_call\] calls=(\d+)[^\n]*?over 1ms=(\d+)")
rx   = last(r"\[ul_rx_timing\] calls=(\d+) recv\(max=\d+us over 1ms=(\d+)")
slip = last(r"slip\(max=(-?\d+)us over 1ms=(\d+)\)")
slack= last(r"\[dl_tx_slack\][^\n]*?below 1ms=(\d+),[^\n]*?AT/BELOW 0=(\d+)")
ulrx = last(r"\[ul_rx\] blocks=\d+[^\n]*?gaps=(\d+)[^\n]*?rx_overflows=(\d+)")
if not call or not rx:
    print("  不能分类：报告里缺 [dl_tx_call] 或 [ul_rx_timing]（'读不到'不等于'健康'）")
    raise SystemExit(0)
rate = 100.0 * int(call[1]) / int(call[0])
tail = 100.0 * int(rx[1]) / int(rx[0])
band = "健康" if rate < 0.0064 else ("边缘" if rate < 0.0248 else "病态")
print(f"  输运类别 = {band}   dl_tx_call over 1ms = {call[1]}/{call[0]} = {rate:.4f}%"
      f"   （带：健康 <=0.0064% / 边缘 0.0064-0.0248% / 病态 >=0.0248%）")
print(f"  同一腿的 RX 尾巴 = {rx[1]}/{rx[0]} = {tail:.4f}%" + (f"；slip>1ms={slip[1]}（max {int(slip[0])/1000:.2f} ms）" if slip else ""))
if slack:
    print(f"  dl_tx_slack below 1ms={slack[0]}, AT/BELOW 0={slack[1]}" + (f"；gaps={ulrx[0]} rx_overflows={ulrx[1]}" if ulrx else ""))
if band == "病态":
    print("  ⇒ 这条腿的 RX 尾巴不得归给代码（见开发文档 6.157(10)）：现在正是抓机制的时刻 ——")
    print("     另开一个终端： sudo sample $(pgrep -x gnb | head -1) 10 -file /tmp/sample_$(date +%H%M).txt")
    print("     然后： ps -M $(pgrep -x gnb | head -1) | wc -l   # 线程数；看 UHD 的 worker 在不在 CPU 上")
    print("     先断电重启 B200（纪律 13）再重飞，别在病态会话上花测量腿。")
elif band == "边缘":
    print("  ⇒ 边缘：这条腿的尾巴读数和输运同档，结论要写明（不要单独引用它的 max/p99）。")
else:
    print("  ⇒ 健康：这条腿的 RX 尾巴读数可用（这是唯一能用来判 RX 宿主路径的一类腿）。")
PY
}

if [[ -n $LABEL ]]; then
  leg=$(ls -1t "$LOGDIR"/gnb_*"$LABEL"*.log.stderr 2>/dev/null | head -1)
  [[ -n $leg ]] || { echo "no leg matched '$LABEL' in $LOGDIR" >&2; exit 2; }
  echo "== $(basename "$leg")"
  classify "$leg"
else
  for leg in $(ls -1t "$LOGDIR"/gnb_*.log.stderr 2>/dev/null | head -5); do
    echo "== $(basename "$leg")"
    classify "$leg"
  done
fi

# ---- the two facts that must be re-checked, not assumed (they were both true on 2026-09-29) ----------
echo
echo "== 电台在哪条总线上（B200 应独占一个 XHCI 控制器；若它与手机/网卡同总线，M-C 重新成立）"
ioreg -p IOUSB -w 0 2>/dev/null | grep -E 'USRP|XHCI' | sed 's/^/  /' | head -12
echo "== 现在有没有 gNB 在跑（病态腿要抓 sample 就得知道 pid）"
pid=$(pgrep -x gnb | head -1 || true)
if [[ -n $pid ]]; then
  echo "  pid=$pid  线程数=$(ps -M "$pid" 2>/dev/null | tail -n +2 | wc -l | tr -d ' ')"
  echo "  抓样： sudo sample $pid 10 -file /tmp/sample_$(date +%H%M).txt"
else
  echo "  没有 gNB 在跑（sample 必须在腿进行中抓）"
fi
