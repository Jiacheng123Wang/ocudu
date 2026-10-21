# d3_modcond_c32h — modulation-conditioned depth-3 receiver (N78 / 20 MHz / TDD / 30 kHz / 51 PRB)

**This is the default model since 2026-10-19** (`OCUDU_DEPTH3_DEFAULT_MODEL`). It is a c32-width retrain
that closed three gaps at once: DM-RS 2,7,11 coverage, the 64QAM reporting head's dynamic range, and a
declared SINR calibration.

## Contract (declared inside the artefact; both the arm and the engine read it)

| Key | Value | Meaning |
|---|---|---|
| `depth3_layout` | `modcond` | Modulation-conditioned contract (Qm as 4 one-hot plane channels) |
| `depth3_qm_set` | `2,4,6,8` | One bundle serves four modulations |
| `depth3_head_stride` | `8` | 8 channels per symbol in the main head |
| `depth3_snr_range` | `12.00,48.00` | Range the reporting heads were trained on (dB); outside it a reception falls back to the classical chain and is counted |
| `depth3_nsc` | `612` | Plane width |
| `depth3_sinr_cal` | `2:1.2678,-3.8298;4:1.0911,-1.3579;6:1.1251,-1.6222;8:1.2822,-4.0420` | Per-modulation affine calibration of the published SINR |

Input `[1, 612, 14, 16]` (12 contract channels plus 4 Qm one-hot), four heads
(`llr`, `post_eq_noise`, `ch_est`, `dmrs_noise`).

## Training

- Architecture: `--modcond --layout spatial --blocks 18 --c-hid 32 --in-channels 12`, **26 284 parameters**.
- Data: `d3_dmrs2711h_train.npz`, **30 777 samples**, containing
  - four modulations × **two DM-RS patterns** (2,11 and 2,7,11): on 2,7,11 there are 3 667 64QAM, 1 680
    256QAM, 160 16QAM and 144 QPSK samples;
  - **high-SNR additions** (26/30/34 dB): 2 700 64QAM and 900 16QAM. The previous version's 64QAM data
    stopped at 24 dB, which is why its reporting head saturated at 24.4 dB (§19.220(e)).
- 40 epochs, `--w-noise 0.3`, best val loss **0.11016**.
- Calibration fitted per modulation **against truth** (the synthetic units carry the SNR and the noise they
  were built with): worst 64QAM residual **1.89 dB**, 16QAM 1.30 dB (the previous version: 4.24 dB with a
  1.27 slope, i.e. compression rather than bias).

## Verification (sections 19.220–19.225 of the design document)

- **Cells**: 630 first transmissions, classical chain 81.4% → arm **91.0%**; target cell 64QAM 5–8 PRB
  87.4% → **96.4% (+28)**; **no cell regresses against the classical chain** (the cost is recorded on 256QAM
  at ≤4 PRB: 81.0% → 73.8%).
- **Bit agreement**: 99.88% (worst 99.58%, floor 99.5); held-out slice (80 never-trained first
  transmissions) **98.8%** against classical 91.2%.
- **Publication range** against the classical chain on the same receptions: 64QAM sd ratio **0.95**, r
  **+0.92**; 16QAM 1.02/+0.98; 256QAM 0.78/+0.92.
- **Calibrated publication**: 64QAM 29.06 against classical 29.20 dB; 256QAM 30.44 against 30.96; 16QAM
  19.11 against 19.66.
- **The calibration does not move decoding**: 550 receptions, 8 cells, identical with and without it.
- **Live A/B** (six adjacent pairs): five positive, mean +27.8%; over 12 legs' own captures replayed
  through both arms, the arm decodes better on 11 and publishes within 1.2 dB of the classical chain.
- **Latency**: `forward` p50 **436–786 µs** (c64: 1 764–1 931); on the ANE 441–448 µs (c64: 926–942).

## Rollback

`../depth3_modcond_ft/` is still in the tree. To roll back, point `OCUDU_DEPTH3_DEFAULT_MODEL` at it in
`apps/units/flexible_o_du/o_du_low/CMakeLists.txt` and reconfigure — note it is a **cache** variable, so an
existing build directory needs `cmake -U OCUDU_DEPTH3_DEFAULT_MODEL -S . -B build` to drop the old value.
