# depth3 replay measurements — e17_c32_b18 and w2_cond_c32_b18 vs E4

Measured 2026-11-02 on 203 (`Mac`, Apple M2, 8 GB, macOS 26.6.2) with the replay binary
`~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay`
(sha256 `5fb30d2989a08c31ba5b3662d88a2ba927dbfe8e6a0fe5d57f981b3de6ea6b05`, the same hash
recorded in E4's `replay_run_meta.json`), pure mode throughout:

```
OCUDU_DEPTH3_FALLBACK=none OCUDU_REPLAY_MODEL=<bundle>.mlmodelc \
  ul_chain_replay <unit> --out /tmp/... --metal --phy_pipeline gpu --pusch_receiver_backend ai
```

Both new bundles were exported with the depth-3 exporter (`train_depth3.py --export-only`)
and carry `depth3_scales.txt` (`s_llr=20 a_noise=4 b_noise=-1`) beside the model.

## 1. The table

| Model | First transmission (of 1 866) | Rate | HARQ mode C (of 400) | Median wall / unit |
|---|---|---:|---:|---:|
| **E4** `depth3_e4_real_1023` (published) | 1 789 | **95.87 %** | **1** (0.25 %) | 0.46 s (published run) |
| **e17** `e17_c32_b18` | 1 781 | **95.44 %** | **1** (0.25 %) | **0.435 s** |
| **w2_cond** `w2_cond_c32_b18` | 1 668 | **89.39 %** | **1** (0.25 %) | **0.436 s** |

E4's per-unit median measured again on 203 with the same harness and 100 units was 0.434 s
(the 0.46 s above is the median of its published `elapsed_s`). The three models are
indistinguishable in wall clock.

## 2. First transmission, per modulation

| Model | QPSK | 16QAM | 64QAM | 256QAM |
|---|---:|---:|---:|---:|
| E4 | 921/931 (98.93 %) | 398/404 (98.51 %) | 313/320 (97.81 %) | 157/211 (74.41 %) |
| e17 | 915/931 (98.28 %) | 393/404 (97.28 %) | 309/320 (96.56 %) | **164/211 (77.73 %)** |
| w2_cond | 908/931 (97.53 %) | 391/404 (96.78 %) | 304/320 (95.00 %) | **65/211 (30.81 %)** |

## 3. First transmission, per width band

| Model | `<=3` PRB | `<=8` PRB | `<=24` PRB | `>24` PRB |
|---|---:|---:|---:|---:|
| E4 | 378/412 (91.75 %) | 545/576 (94.62 %) | 562/574 (97.91 %) | 304/304 (100 %) |
| e17 | 381/412 (92.48 %) | 543/576 (94.27 %) | 556/574 (96.86 %) | 301/304 (99.01 %) |
| w2_cond | 320/412 (77.67 %) | 499/576 (86.63 %) | 549/574 (95.64 %) | 300/304 (98.68 %) |

## 4. The failure corner

**A common floor first.** 13 of the 1 866 units are 1-PRB allocations that the arm refuses
before any model runs: `the input plane was refused: the allocation is narrower than the
trained envelope (2 PRB)`. They are the same 13 units for every model, all at `<=3` PRB
(8 QPSK, 4 16QAM, 1 64QAM), and E4 is KO on all 13 as well. They are 13 of E4's 77, e17's
85 and w2's 198 failures, and they sit in the `<=3` column of every row above. Subtracting
them, the model failures are **E4 64, e17 72, w2 185**.

**E4** (64 model failures, 77 raw): 54 are 256QAM and 50 of those are at `<=8` PRB; 44 of
the 64 are in 20–30 dB. 256QAM is 63/107 (58.9 %) at 20–30 dB and 93/100 (93 %) at `>=30` dB.
Dominant cells: 256QAM/`<=8` 31, 256QAM/`<=3` 19.

**e17 matches E4's corner.** 72 model failures (85 raw); 47 are 256QAM and 44 of those at
`<=8` PRB; 38 of 72 in 20–30 dB; dominant cells 256QAM/`<=8` 31, 256QAM/`<=3` 13,
64QAM/`<=24` 10, QPSK/`<=3` 5, 16QAM/`<=24` 4. 256QAM is 69/107 (64.5 %) at 20–30 dB and
95/100 (95 %) at `>=30` dB. Its non-256QAM failures (10 64QAM, 8 QPSK, 7 16QAM) are more
numerous than E4's (6, 2, 2), which is where the net −8 comes from: it recovers 16 units
E4 failed and loses 24 E4 passed. The per-modulation shift is small and two-sided:
+7 256QAM, −6 QPSK, −5 16QAM, −4 64QAM.

**w2_cond shows the same 20–30 dB narrow-256QAM corner, plus a second one E4 never had:
high-SNR narrow 256QAM.** 185 model failures (198 raw); 146 are 256QAM and 143 of those at
`<=8` PRB (72 at `<=8`, 71 at `<=3`); 78 in 20–30 dB and **64 at `>=30` dB, all of them
256QAM at `<=8`**. 256QAM is 29/107 (27.1 %) at 20–30 dB and **36/100 (36 %) at `>=30` dB**,
where E4 and e17 are at 93–95 %. At `<=3` PRB its 256QAM rate is 29/100 and at `<=8` 31/103
(E4 81/100 and 72/103; e17 87/100 and 72/103). 101 units fail only in w2_cond, 85 of them
256QAM at `<=3`/`<=8`.

This is a property of the w2 checkpoint, not of the deployment:

- the Core ML graph matches the Keras graph on a random plane (mean |ΔLLR| 0.0024,
  max 0.093; e17: 0.0011 / 0.026 — fp16 rounding only);
- 20 of the failing narrow-band 256QAM units were re-run with `OCUDU_DEPTH3_BUCKETS=none`:
  all 20 still fail, at 6 decoder iterations, on the full-band graph;
- the published SINR is bit-identical between all three models on all 1 866 units
  (max difference 0.0 dB), so the difference is entirely in the LLR path;
- the checkpoint's own 256QAM raw BER is worse: on 8 samples per band from
  `w2_monitor1.npz`, w2 is 0.91 % against e17's 0.32 % at 30+ dB and 1.54 % against
  1.09 % at 20–30 dB. The same test shows w2 strongly prefers the correct 256QAM
  one-hot (BER 0.015–0.066) over the QPSK one-hot (0.21–0.26), so its conditioning is
  not swapped — it is simply a weaker 256QAM model.

## 5. HARQ mode C

All three models recover **1 of 400** chains when the network sees the chain's rv0 capture
alone. Both new runs used the same 400 rv0 units, pure mode, no `--harq-chain`:

| Model | OK | Chain | Modulation | Width | SNR |
|---|---:|---|---|---:|---:|
| E4 | 1 | `chain_0015` | 16QAM | 20 PRB | 4.34 dB |
| e17 | 1 | `chain_0017` | 16QAM | 12 PRB | 9.55 dB |
| w2_cond | 1 | `chain_0015` | 16QAM | 20 PRB | 4.34 dB |

Per modulation all three are 0/120 QPSK, 1/96 16QAM, 0/59 64QAM, 0/125 256QAM.
Two of the 400 rv0 units (`chain_0327`, `chain_0332`) are 1-PRB QPSK allocations and hit
the same plane refusal as the 13 s2 units; they are KO for all three models.

E4's C log was checked for a per-reception fallback hand-over: it carries no such line
(the only 'degrades' text is the arm's startup description), and B (classical rv0 alone)
is KO for all 400, so the 1 OK is the network's. The new runs
force that with `OCUDU_DEPTH3_FALLBACK=none` and show the `PURE mode` line on every unit.

## 6. What was verified, and the traps avoided

| Check | Result |
|---|---|
| fallback disabled | 1 866/1 866 and 400/400 runs per model log `PURE mode`; 0 anomalies |
| `depth3_scales.txt` | present in both bundles; no `there is no sidecar at all` warning in any log |
| channel count | both bind as `modcond model ... plane 16 channels, LLR stride 8`; 8 buckets 108–612 bound; 0 bind failures |
| same unit set | both result sets are exactly the 1 866 units of `index.csv` and the 400 unique rv0 of `chains.csv` |
| same binary | sha256 equals the hash in E4's `replay_run_meta.json` |
| no no-verdicts | 0 `NO_VERDICT`, 0 non-zero exits, 0 bind failures |
| reception failures | only the 13 (s2) / 2 (HARQ) one-PRB `plane_refused` units above; the 13 s2 units are identical for both models and KO on E4 too, and the 2 HARQ rv0 units are the same refusal on both new models (E4's fallback run KOs them as well); 0 `forward_failed`, 0 `modulation_mismatch`, 0 `no_sequence_context` in all 4 532 logs |

Two corrections to the premises:

1. **`e17_c32_b18` is not unconditioned.** Its `.keras` input is `[None, 612, 14, 16]`:
   12 spatial channels plus the four modulation one-hots, exactly like `w2_cond_c32_b18`
   and E4. `run_w2_train.sh` says the shape is "E4/E17 VERBATIM ... modulation-conditioned".
   Both were exported with `--modcond`; a 12-channel export would have been refused at bind.
   A conditioning-swap test on a random plane confirms both graphs consume the four
   channels (e17 mean |ΔLLR| 0.09–0.23, w2 0.22–0.30 between Qm one-hots).

2. **The converter named in the task is the wrong tool.** `convert_coreml.py` is a
   SavedModel→CoreML script with a hardcoded `(batch, 612, 14, 2)` input and no
   conditioning option; the depth-3 exporter is `train_depth3.py --export-only`.

E4's published runs were made with **no** `depth3_scales.txt` in its bundle (its logs say
`there is no sidecar at all ... defaults A_n=4 B_n=-1`). That is numerically a no-op here:
the sidecar values are exactly the receiver defaults (`s_llr=20` is
`D3_RANGE_LIMIT_256QAM`, `a_noise=4`, `b_noise=-1`). A 100-unit A/B of the E4 bundle with
and without the sidecar gave 0 verdict differences and both matched the published CSV
exactly, so the new models' sidecar does not put them on a different footing.

## 7. Timing

| Model | n | median | p90 | max |
|---|---:|---:|---:|---:|
| e17 | 1 866 | 0.435 s | 0.445 s | 587.1 s (one OS-level stall) |
| w2_cond | 1 866 | 0.436 s | 0.445 s | 3.96 s |
| E4 (100-unit re-run) | 100 | 0.434 s | 0.444 s | 0.451 s |

The wall clock is the whole process (model load included), as in E4's `elapsed_s`. 203 was
not perfectly idle (OneDrive File Provider around 107 % CPU, `mds` indexing); the medians
are unaffected, the 587 s maximum is one outlier, not a failure (it returned OK).
