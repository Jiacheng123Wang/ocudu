# `depth3_e4_real_1023` — the **default** bundle since 2026-10-23

Two reporting heads (`llr`, `post_eq_noise`), 26 185 parameters, trained on the **E4 corpus**: the old corpus plus 14 589 real
captures collected in Hong Kong on 2026-10-23 (19 984 captured, 16 141 labelled, 14 589 built), which moved the real share from
8 % to 35.7 % and brought QPSK and 16QAM at low SNR into training for the first time.

| File | What |
|---|---|
| `d3_e4.mlmodelc` | the full-band bundle (612 subcarriers) |
| `buckets/base_w{108..612}.mlmodelc` | the eight narrow-band graphs of the same weights; the arm binds them automatically because they are a sibling `buckets/` directory |
| `d3_e4.mlpackage` | the source form, for inspection and re-compilation |
| `depth3_best.keras` | the checkpoint the artefacts were exported from |

## Why it is the default

It is the only bundle that passes all three offline gates **and** wins the air comparison on the criterion settled earlier --
delivered goodput, which folds decode rate, scheduling and retransmissions into one number:

| Gate | Requirement | Result |
|---|---|---|
| sign agreement vs the classical chain | ≥ 98.7 % | **99.93 %** (c32h 98.73, E1 99.01, E2 98.54) |
| attach decodes (40 captures) | ≥ 26 | **27** (c32h 26, E1 27, E2 21) |
| per-bin BER on held-out real low-SNR captures | no bin regresses | **better in every bin**, QPSK and 16QAM |

Air comparison, six valid legs (one discarded for 1 191 real-time failures against 92–223 elsewhere), grouped by latency state
because the engine flips between a neural-engine state and a GPU one from leg to leg **for both bundles**:

| State | c32h goodput | E4 goodput | c32h retrans | E4 retrans |
|---|---|---|---|---|
| low | 6.92 | **7.85, 8.01** | 5 139 | **2 303, 2 380** |
| high | 7.41, 7.57, 7.37 | **8.43** | 3 611–4 365 | **2 262** |

E4 leads by 13–16 % in both states with 40 % fewer retransmissions, and gives up about 1.5 points of CRC-OK -- the trade the
scheduler makes when it spends decode rate on delivered traffic.

## What it does not have

The four-head `depth3_modcond_c32h` bundle remains in the tree and is one `--pusch_receiver_model` away; its CRC-OK is higher and
its published SINR goes through its own calibration rather than the shared measurement layer. That layer was verified against the
classical estimator on sixteen captures (RSRP bias 0.01 dB, worst 0.08 dB), which is what makes a two-head default safe.
