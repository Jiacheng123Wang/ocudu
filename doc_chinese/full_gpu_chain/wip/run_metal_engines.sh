#!/usr/bin/env bash
# Metal engine self-tests, one command (per-leg gate: see the design document).
#
# Why this is a gate of its own: these binaries are BUILT but not registered with ctest, so a leg that
# only ran `ctest -L phy` never executed them - and they are the only place the host side of the Metal
# API is exercised outside the air path. A leg that armed the GPU-time probe after commit() (Metal
# asserts "Completed handler provided after commit call") passed ctest, the three capture networks and
# the two build gates, and aborted the gNB on the first real run.
#
# The estimator's own binary is in the list because it was NOT in any leg until S-7g-16: it aborted in
# Test 8 ("endEncoding has already been called", the standalone correlation build) for as long as that
# defect lived, and no gate ever noticed - a binary that is built but never run is not a test.
#
# usage: bash run_metal_engines.sh [build-dir]
set -u
BUILD=${1:-build}
rc_all=0
for t in \
  "$BUILD/lib/phy/generic_functions/metal/dft_processor_metal_unit_test" \
  "$BUILD/lib/phy/generic_functions/metal/ofdm_demodulator_metal_batch_test" \
  "$BUILD/lib/phy/upper/channel_modulation/metal/demodulation_mapper_metal_unit_test" \
  "$BUILD/lib/phy/upper/channel_processors/metal/channel_equalizer_metal_unit_test" \
  "$BUILD/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test" \
  "$BUILD/tests/unittests/gateways/baseband_gateway_buffer_metal_smoke_test" ; do
  if [ ! -x "$t" ]; then
    echo "MISSING $t (build it first)"
    rc_all=1
    continue
  fi
  if out=$("$t" 2>&1); then
    echo "OK   $(basename "$t"): $(echo "$out" | tail -1 | cut -c1-90)"
  else
    echo "FAIL $(basename "$t")"
    echo "$out" | tail -15
    rc_all=1
  fi
done
echo "METAL_ENGINES_RC=$rc_all"
exit $rc_all
