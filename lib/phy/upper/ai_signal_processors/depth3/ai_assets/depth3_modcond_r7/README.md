# depth3_modcond_r7 — the calibration bundle with the widened low end

A modulation-conditioned depth-3 bundle: ONE model computes Qm = {2, 4, 6, 8} for the four modulations of
this cell, replacing the per-modulation bundles. This directory is the **default** the build binds.

## What it is

| property | value |
|---|---|
| contract | `depth3_layout=modcond`, `depth3_qm_set=2,4,6,8`, `depth3_head_stride=8`, `depth3_nsc=612` |
| input plane | `[1, 612, 14, 16]`: the contract's 12 channels plus 4 one-hot modulation channels |
| heads | `llr` (112 channels, read with the stride), `post_eq_noise` (per RE), `ch_est` (Re/Im per RE), `dmrs_noise` (scalar) |
| declared domain | **0 to 48 dB** (`depth3_snr_range`) — this is the point of this bundle, see below |
| calibration | declared in the bundle (`depth3_sinr_cal`): `2:1.3935,-5.9723;4:1.0156,-1.0015;6:1.0113,-0.1344;8:1.1785,-2.7343` |
| parameters | 26 284, c32/h18 (the same shape as `depth3_modcond_c32h`) |
| training | 33 977 samples, `--snr-range 0,48`, 40 epochs, 15 608 s, best val 0.10664 |
| size | 164 KB (`.mlmodelc`) |

## Why the low end

The predecessor (`depth3_modcond_c32h`) declares **12 to 48 dB** because its training data started at 12 dB, and
its reporting heads extrapolate badly below that: measured against prescribed SNR on a six-point ladder, head 4 is
off by **+15.42 dB at 0 dB and +6.69 dB at 6 dB**, against +0.26 to +1.22 inside the range. A reported SINR that
far out sets an MCS the channel cannot carry, so the arm refused (and still refuses, with this declaration) every
reception below the floor.

This bundle was trained with data down to 0 dB. Measured on synthetic units with known truth, the worst
|published - true| error by band:

| band | predecessor (12-48) | this bundle (0-48) |
|---|---|---|
| <= 10 dB | 5.65 / 6.44 / 9.03 dB | **0.58 / 0.81 / 0.89 / 0.93 dB** |
| 10-30 dB | 1.48 / 1.72 / 2.43 dB | 1.82 / 2.20 / 2.45 dB |
| >= 30 dB | 1.58 / 5.21 / 5.33 dB | 1.52 / 2.20 / 2.65 dB |

The low end improves by six to ten times, which is what makes a floor of 0 dB defensible rather than merely
wider. The mid band and the top are unchanged within the measurement's own scatter; the ~2 dB there is the
synthetic pipeline's per-capture channel-fit bias, not the heads (see the design document, section 19.235).

On the air, over the 630 first transmissions of three capture sets: target cell (64QAM, 5-8 PRB) 96.4% against
the classical chain's 87.4% (+28), 256QAM at <= 4 PRB 83.3% where the predecessor had regressed to 73.8%, and
92.1% overall against 91.0%. Bit agreement with the classical chain 99.80% (target >= 99.5). Publication dynamic
range against the classical chain: 64QAM sd ratio 0.91 (r +0.90), 256QAM 0.91 (r +0.83), 16QAM 0.78, QPSK 0.85.

## What it does NOT claim

- **The mid band is not better.** Worst-case error there is ~2 dB in both bundles, and that error belongs to the
  synthetic pipeline, not to the model.
- **Above 36 dB the noise head saturates** (`noise = 10^(a*tanh(u)+b)`), reading about +2 dB high at 40 dB. The
  48 dB ceiling is kept for that reason and this bundle does not change it.
- **The sixteen-symbol plane contract is unchanged**: 612 subcarriers, 14 symbols, normal CP. A different
  geometry needs a different contract and a retrain.

## Rebuilding

```bash
# training (see ai_train/README.md for the dataset builder)
~/ai_ce_work/venv/bin/python ai_train/train_depth3.py \
    --train <dataset.npz> --val <val.npz> --out <run-dir> \
    --modcond --layout spatial --in-channels 12 --blocks 18 --c-hid 32 \
    --snr-range 0,48 --epochs 40 --batch 8 --w-noise 0.3
# export with the declared range and the calibration this bundle carries
SNR_RANGE=0,48 bash probe/eval_narrow_c32.sh <run-dir> --label <label>
```

## Rolling back

The predecessor is still in the tree; point the build at it and reconfigure:

```bash
cmake -B build -DOCUDU_DEPTH3_DEFAULT_MODEL=$PWD/lib/phy/upper/ai_signal_processors/depth3/ai_assets/depth3_modcond_c32h/d3_modcond_c32h.mlmodelc
```

Nothing else changes: the arm reads the declared range from whichever bundle is bound, so the gate's floor
follows the bundle rather than the code.
