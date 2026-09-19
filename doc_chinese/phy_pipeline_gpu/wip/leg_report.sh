#!/usr/bin/env bash
# The verdict of one leg, in the terms THIS line is judged by: the pipeline contract (which states
# what the selected mode claims), the host-side sample counters, and the device-side "who did it"
# counters. Nothing about microseconds.
#
# usage: bash leg_report.sh <log | log.stderr | prefix> [...]
set -u

contract_block()
{
  # The contract is emitted more than once (once when the stop is requested, once at exit), and the
  # block as a whole is what matters. Keep the LAST complete block: an earlier one describes a
  # shorter run and reading it would understate the leg.
  #
  # \note The header pattern must NOT be plain "^\[phy_pipeline\] contract" - the summary line
  # ("contract MET (7 of 7 …)" / "contract NOT MET: 1 of 8 …") starts with exactly that too, and it
  # would reset the buffer and print the verdict without the evidence. The header is the one carrying
  # "(mode=…)".
  #
  # \note And the terminator must match "NOT MET" as well as "MET". It did not, once: a block whose
  # checks FAILED never terminated, so the reader printed nothing at all for exactly the legs that
  # mattered most - the ones reporting an unmet requirement.
  awk '/^\[phy_pipeline\] contract \(mode=/  { buf = ""; inb = 1 }
       inb                                { buf = buf $0 "\n" }
       inb && /contract (MET|NOT MET)/    { last = buf; inb = 0 }
       END                                { printf "%s", last }' "$1"
}

for p in "$@"; do
  case "$p" in
    *.stderr) log=${p%.stderr}; err=$p ;;
    *)        log=$p; err=$p.stderr; [ -f "$err" ] || err=$p ;;
  esac
  echo "================ $(basename "$p")"
  [ -f "$err" ] || { echo "  MISSING stderr capture: $err  (contract + counters unavailable)"; continue; }

  ok=$( { grep -c "crc=OK" "$log"; } 2>/dev/null || true); ok=${ok:-0}
  ko=$( { grep -c "crc=KO" "$log"; } 2>/dev/null || true); ko=${ko:-0}
  rtf=$( { grep -c "Real-time failure in RF" "$log"; } 2>/dev/null || true); rtf=${rtf:-0}
  slots=$(grep -a -oE '\[ul_rx\] blocks=[0-9]+' "$err" 2>/dev/null | tail -1 | grep -oE '[0-9]+')
  echo "-- run"
  echo "  crc OK/KO            : $ok / $ko"
  echo "  Real-time failures   : $rtf / ${slots:-?} slots"
  # Startup provenance from stdout, when the leg captured it (run_leg.sh does since S2): the commit
  # and the cell line. Absent for legs run before the capture existed - say so rather than print
  # nothing, so an old leg cannot be mistaken for one whose banner was missing.
  if [ -f "$log.stdout" ]; then
    echo "  commit               : $(grep -a -oE 'OCUDU gNB \(commit [0-9a-f]+\)' "$log.stdout" | head -1 | grep -oE '[0-9a-f]{6,}')"
    echo "  cell                 : $(grep -a -m1 -E '^Cell pci=' "$log.stdout")"
  else
    echo "  commit / cell        : (no .stdout captured by this leg)"
  fi

  echo "-- the pipeline contract (the mode states a property; this is whether it held)"
  contract_block "$err" | sed 's/^/  /'

  echo "-- host side (samples)"
  grep -a "^\[ul_host\]" "$err" 2>/dev/null | sed 's/^/  /' | tail -1

  echo "-- device side (who did the work)"
  # The mmse_ce line ends with "refusals=...": WHY a device stage did not run on a hop (batch S13-P1).
  # "<none>" is the shape a leg is expected to show - it means every hop took the device route - and it
  # is printed rather than omitted so that "no line" cannot be mistaken for "no refusals".
  for pat in '^\[metal_stats\] mmse_ce' '^\[metal_stats\] burst' '^\[metal_stats\] lane fence' '^\[metal_stats\] dft'; do
    grep -a "$pat" "$err" 2>/dev/null | sed 's/^/  /' | tail -1
  done

  echo "-- lane (present when the fused lane ran)"
  grep -a "^\[ul_gpu_lane\] lanes" "$err" 2>/dev/null | sed 's/^/  /' | tail -1
  grep -a "^\[ul_gpu_lane\] busy split" "$err" 2>/dev/null | sed 's/^/  /' | tail -1

  # The shutdown latency series. They are not a verdict on the crossing contract - that is the block
  # above - but a leg is the only place they are produced (the replay tools do not print them), and
  # [ul_gpu_pipeline] is the fused lane's own number: IQ samples in -> LLRs out. The phase-segment
  # series only exist outside the lane (they are the same span, split); inside it, [ul_pipeline] minus
  # [ul_gpu_pipeline] is what follows the LLRs (rate matching, LDPC, CRC, FAPI).
  echo "-- latency (shutdown series)"
  for pat in '^\[ul_pipeline\]' '^\[ul_gpu_pipeline\]' '^\[ul_time_frequency\]' '^\[ul_channel_estimation\]' \
             '^\[ul_equalization_demod\]' '^\[ul_ldpc_decode\]' '^\[ul_fapi_mac\]'; do
    grep -a "$pat" "$err" 2>/dev/null | sed 's/^/  /' | tail -1
  done
  echo
done
