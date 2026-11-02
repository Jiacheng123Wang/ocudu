# `depth3_w2cond_1102` — `w2_cond_c32_b18` exported for replay, 2026-11-02

| File | What |
|---|---|
| `d3_w2cond.mlmodelc` | the full-band (612 subcarrier) deployment graph |
| `buckets/base_w{108..612}.mlmodelc` | the eight narrow-band graphs of the same weights |
| `d3_w2cond.mlpackage` | the source form, for inspection and re-compilation |
| `depth3_best.keras` | the checkpoint the artefacts were exported from |
| `depth3_scales.txt` | `s_llr=20`, `a_noise=4`, `b_noise=-1` |

## Provenance

`~/ai_depth3/runs/w2_cond_c32_b18/depth3_best.keras`, md5 `776f626715009ae6c41b920b51c7572b`.
26 185 parameters, 18 blocks, c-hid 32, spatial layout, modulation-conditioned
(12 plane + 4 Qm one-hot = 16 input channels — the same shape as E4 and e17),
two heads (`llr` `[612,112]`, `post_eq_noise` `[612,14]`; no `ch_est` / `dmrs_noise`).
Trained on the w2 corpus (`w2_train_mixed.npz`, 40 000-sample cap) with
`--modcond --layout spatial --in-channels 12 --no-reporting-heads --w-ch 0.1 --w-noise 0.3`.

## Export

```bash
train_depth3.py --export-only ~/ai_depth3/runs/w2_cond_c32_b18/depth3_best.keras \
  --export-probe .../depth3_w2cond_1102/d3_w2cond.mlpackage \
  --export-buckets 108,180,252,324,396,468,540,612 \
  --modcond --layout spatial --modulation 256QAM --snr-range 0,48
xcrun coremlcompiler compile .../d3_w2cond.mlpackage .../depth3_w2cond_1102
```

## Declarations (verified from `metadata.json`)

`depth3_layout=modcond`, `depth3_qm_set=2,4,6,8`, `depth3_head_stride=8`,
`depth3_nsc=612`, `depth3_snr_range=0.00,48.00`; input `x` `[1,612,14,16]`;
outputs `llr` `[1,612,112]`, `post_eq_noise` `[1,612,14]`.

## Measurement (203, pure mode, E4's 1 866 units)

| Measurement | Result |
|---|---|
| first transmission | **1 668 / 1 866 = 89.39 %** |
| HARQ mode C (rv0 alone) | **1 / 400 = 0.25 %** (`chain_0015`, 16QAM, 20 PRB) |
| per-unit wall clock | median **0.436 s** (p90 0.445 s) |

Per modulation: QPSK 908/931 (97.53 %), 16QAM 391/404 (96.78 %),
64QAM 304/320 (95.00 %), **256QAM 65/211 (30.81 %)**.
Per width: `<=3` 320/412 (77.67 %), `<=8` 499/576 (86.63 %),
`<=24` 549/574 (95.64 %), `>24` 300/304 (98.68 %).

The regression is specific to 256QAM at narrow widths and survives at high SNR:
256QAM is 29/100 at `<=3` PRB, 31/103 at `<=8` PRB, 29/107 at 20–30 dB and
**36/100 at `>=30` dB** (e17: 87/100, 72/103, 69/107, 95/100; E4: 81/100, 72/103,
63/107, 93/100). 64 of the 198 failures are at `>=30` dB, all 256QAM at `<=8` PRB.
This is a property of the trained checkpoint, not of the conversion: the Core ML
graph matches the `.keras` (mean |ΔLLR| 0.0024, max 0.093 over a random plane), and
20 failing narrow-band units re-run with `OCUDU_DEPTH3_BUCKETS=none` fail identically
on the full-band graph.
