# d3_modcond_ft — modulation-conditioned depth-3 receiver (N78 / 20 MHz / TDD / 30 kHz / 51 PRB)

One bundle replacing four per-modulation models: a shared trunk, a **shared set of reporting heads**, and a
wide main head conditioned on Qm. This was the default until 2026-10-19 and is kept for rollback and A/B
against its successor, `../depth3_modcond_c32h/`.

## Contract (declared inside the artefact; both the arm and the engine read it)

| Key | Value | Meaning |
|---|---|---|
| `depth3_layout` | `modcond` | Modulation-conditioned contract (Qm as 4 one-hot plane channels) |
| `depth3_qm_set` | `2,4,6,8` | One bundle serves four modulations |
| `depth3_head_stride` | `8` | 8 channels per symbol in the main head |
| `depth3_snr_range` | `12.00,48.00` | Range the reporting heads were trained on (dB); outside it a reception falls back to the classical chain and is counted |
| `depth3_nsc` | `612` | Plane width; the arm assembles a fixed 612 and refuses a bundle whose geometry differs |

Input `[1, 612, 14, 16]` (12 contract channels plus 4 Qm one-hot), four heads
(`llr`, `post_eq_noise`, `ch_est`, `dmrs_noise`). It declares **no** SINR calibration, which is what tells it
apart from its successor by metadata alone.

## Training

- Data: `d3_modcond_train.npz` (12 000 samples, four modulations) plus `d3_modcond_val.npz`. The dataset is
  too large to version; see `ai_train/merge_depth3_datasets.py` and `ai_train/build_depth3_dataset.py` for
  how it is rebuilt.
- Main run: 40 epochs, `--modcond --layout spatial --blocks 18`, best val loss **0.00033**.
- Fine-tune: `--w-noise 0.3 --lr 3e-4 --epochs 8` (raising head 4's weight from the default 0.01), which
  produced the checkpoint in this directory.

## Verification

- **Adoption gate**: four modulations × 300 captures, bit-identical decisions against the per-modulation
  specialists (1200/1200) and zero degradations (§19.148, §19.154).
- **Reporting-head accuracy**: head 3 NMSE −25 to −27 dB with a power ratio of ±0.1 dB; head 4 within
  +0.26 to +1.22 dB over 12–30 dB (§19.149).
- **Live network**: with `net` reporting, RSRP/EPRE agree with the independent estimator, but the arm's
  published SINR sits **2.5 dB low**, which makes MCS more conservative and costs 12.6% throughput
  (§19.161). Hence the default remained `classic`, and `net` awaited calibration.
- **Known limitation**: trained on DM-RS 2,11 only. In the field it decodes 2,11 and collapses on 2,7,11
  (§19.211); this is the defect its successor was retrained to fix, and the reason it is not the default.
