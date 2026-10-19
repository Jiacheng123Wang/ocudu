#!/usr/bin/env python3
"""The bundles in this tree declare the contract the engine reads, and a calibration that is safe.

WHY THIS EXISTS. The bundle's metadata is the only thing binding it to the receive path: the engine reads
`depth3_nsc` and refuses a bundle whose plane width it cannot assemble, reads `depth3_qm_set` to know
which modulations are served, and applies `depth3_sinr_cal` to what the scheduler sees. A bundle that
declares nothing still loads, so a missing or mis-signed declaration is invisible until it is in the field
-- which is exactly how a calibration once raised a reception published at 17.07 dB to 20.83 where it
belonged at 18.20, after which attach failed on every leg that carried it (design section 46.13ter).

So this test pins, for the DEFAULT bundle and for the ROLLBACK bundle:
  * the contract keys and their values;
  * the calibration, where declared, is well formed per modulation and inside the range that the
    fitted data actually produced (a slope near 1 and a small offset) -- a guard against the sign
    error above, not a substitute for the fit itself;
  * and the two bundles are distinguishable: exactly the deployed one declares a calibration.

It reads the metadata only: no Core ML, no TensorFlow, no device.
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEPTH3 = os.path.dirname(HERE)

DEFAULT_BUNDLE = os.path.join(DEPTH3, "depth3_modcond_c32h", "d3_modcond_c32h.mlmodelc")
ROLLBACK_BUNDLE = os.path.join(DEPTH3, "depth3_modcond_ft", "d3_modcond_ft.mlmodelc")

EXPECTED = {
    "depth3_layout": "modcond",
    "depth3_nsc": "612",
    "depth3_head_stride": "8",
    "depth3_qm_set": "2,4,6,8",
    "depth3_snr_range": "12.00,48.00",
}
# What the fit produced on 2026-10-19 (design section 19.222). The bounds are wide enough to allow a
# refit and narrow enough to catch the sign error and the compression mistake that came before it.
SLOPE_RANGE = (0.80, 1.60)
OFFSET_RANGE = (-8.0, 3.0)


def metadata(bundle):
    path = os.path.join(bundle, "metadata.json")
    with open(path) as handle:
        return json.load(handle)[0]["userDefinedMetadata"]


def parse_cal(text):
    """`2:1.2678,-3.8298;4:...` -> {qm: (slope, offset)}; raises on anything malformed."""
    out = {}
    for entry in text.split(";"):
        entry = entry.strip()
        if not entry:
            continue
        qm, pair = entry.split(":")
        slope, offset = pair.split(",")
        out[int(qm)] = (float(slope), float(offset))
    return out


def main():
    missing = [p for p in (DEFAULT_BUNDLE, ROLLBACK_BUNDLE) if not os.path.isdir(p)]
    if missing:
        print(f"NOT SHIPPED: {', '.join(missing)}")
        return 77

    default = metadata(DEFAULT_BUNDLE)
    for key, value in EXPECTED.items():
        assert key in default, f"the default bundle does not declare {key}"
        assert default[key] == value, f"{key}: {default[key]!r} != {value!r}"

    assert "depth3_sinr_cal" in default, "the default bundle must declare its calibration (design section 19.222)"
    cal = parse_cal(default["depth3_sinr_cal"])
    assert sorted(cal) == [2, 4, 6, 8], f"the calibration covers {sorted(cal)}, the contract serves 2,4,6,8"
    for qm, (slope, offset) in cal.items():
        assert SLOPE_RANGE[0] <= slope <= SLOPE_RANGE[1], \
            f"qm={qm}: slope {slope} outside {SLOPE_RANGE} -- a slope far from 1 means the publication's " \
            f"RANGE is wrong, and a calibration cannot put back a range that is not there"
        assert OFFSET_RANGE[0] <= offset <= OFFSET_RANGE[1], \
            f"qm={qm}: offset {offset:+} outside {OFFSET_RANGE} -- check the sign convention: the knob " \
            f"multiplies the PUBLISHED SINR, so a fitted NOISE-space offset must be negated"

    rollback = metadata(ROLLBACK_BUNDLE)
    for key, value in EXPECTED.items():
        if key == "depth3_sinr_cal":
            continue
        assert rollback.get(key) == value, f"the rollback bundle's {key}: {rollback.get(key)!r} != {value!r}"
    assert "depth3_sinr_cal" not in rollback, \
        "the rollback bundle must stay uncalibrated, or the two cannot be told apart by their declarations"

    print(f"OK  shipped bundles: default declares {len(cal)} calibrated modulation(s) "
          f"(slopes {min(s for s, _ in cal.values()):.2f}-{max(s for s, _ in cal.values()):.2f}), "
          f"rollback declares none")
    return 0


if __name__ == "__main__":
    sys.exit(main())
