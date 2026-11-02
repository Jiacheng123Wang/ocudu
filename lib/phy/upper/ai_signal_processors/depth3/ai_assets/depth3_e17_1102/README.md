# `depth3_e17_1102` — `e17_c32_b18` exported for replay, 2026-11-02

| File | What |
|---|---|
| `d3_e17.mlmodelc` | the full-band (612 subcarrier) deployment graph |
| `buckets/base_w{108..612}.mlmodelc` | the eight narrow-band graphs of the same weights |
| `d3_e17.mlpackage` | the source form, for inspection and re-compilation |
| `depth3_best.keras` | the checkpoint the artefacts were exported from |
| `depth3_scales.txt` | `s_llr=20`, `a_noise=4`, `b_noise=-1` |

## Provenance

`~/ai_depth3/runs/e17_c32_b18/depth3_best.keras`, md5 `cfe8a7c866e2ccab697abe7071bc74b5`,
which matches the `SOURCE.txt` written when the run was copied from 107 on 2026-11-01.
26 185 parameters, 18 blocks, c-hid 32, spatial layout, two heads (`llr` `[612,112]`,
`post_eq_noise` `[612,14]`; no `ch_est` / `dmrs_noise`).

**It is modulation-conditioned, not unconditioned.** The graph input is
`[None, 612, 14, 16]`: the contract's 12-channel spatial plane plus the four Qm one-hot
channels. `run_w2_train.sh` says the shape is "E4/E17 VERBATIM ... modulation-conditioned".
Exporting it as a 12-channel per-modulation bundle would make the arm feed a 12-channel
plane to a 16-channel graph and refuse at bind.

## Export

The converter named in some notes,
`lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/convert_coreml.py`,
is a SavedModel→CoreML script with a hardcoded `(batch, 612, 14, 2)` input and no
conditioning support; it cannot convert this model. The depth-3 exporter is
`lib/phy/upper/ai_signal_processors/depth3/ai_train/train_depth3.py`:

```bash
train_depth3.py --export-only ~/ai_depth3/runs/e17_c32_b18/depth3_best.keras \
  --export-probe .../depth3_e17_1102/d3_e17.mlpackage \
  --export-buckets 108,180,252,324,396,468,540,612 \
  --modcond --layout spatial --modulation 256QAM --snr-range 0,48
xcrun coremlcompiler compile .../d3_e17.mlpackage .../depth3_e17_1102
```

## Declarations (verified from `metadata.json`)

`depth3_layout=modcond`, `depth3_qm_set=2,4,6,8`, `depth3_head_stride=8`,
`depth3_nsc=612`, `depth3_snr_range=0.00,48.00`; input `x` `[1,612,14,16]`;
outputs `llr` `[1,612,112]`, `post_eq_noise` `[1,612,14]`. The buckets declare
the same layout at their own `depth3_nsc`.

## Measurement (203, pure mode, E4's 1 866 units)

| Measurement | Result |
|---|---|
| first transmission | **1 781 / 1 866 = 95.44 %** |
| HARQ mode C (rv0 alone) | **1 / 400 = 0.25 %** (`chain_0017`, 16QAM, 12 PRB) |
| per-unit wall clock | median **0.435 s** (p90 0.445 s) |

Per modulation: QPSK 915/931 (98.28 %), 16QAM 393/404 (97.28 %),
64QAM 309/320 (96.56 %), 256QAM 164/211 (77.73 %).
Per width: `<=3` 381/412 (92.48 %), `<=8` 543/576 (94.27 %),
`<=24` 556/574 (96.86 %), `>24` 301/304 (99.01 %).

The failure corner is E4's: 31 failures at 256QAM/`<=8`, 13 at 256QAM/`<=3`,
43 of 85 failures in the 20–30 dB band; 256QAM is 69/107 at 20–30 dB and
95/100 at `>=30` dB (E4: 63/107 and 93/100).
