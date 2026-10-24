# depth3/ai_train — training code and design summary for the depth-3 receiver

The depth-3 receiver handles **channel estimation, equalisation and demapping as one unit**, with a single
bundle serving all four modulations (QPSK, 16QAM, 64QAM, 256QAM). Its training code lives here. The AI-CE
(helena) training code stays in `signal_processors/channel_estimator/metal/ai_train/`, and the DSP
utilities the two share — DM-RS sequence generation, TDL channel synthesis, label construction, LDPC
decoding — stay there too; `depth3_paths.py` is the one place that names their location.

> Authoritative design record: `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md`
> (§19.2xx) and `doc_chinese/llr_ai_detection/model_network_design.md` (§0.4 for the latency table, §46.x
> for the reporting heads). Those documents are in Chinese by project convention; the code tree is English.

## 1. The network

| Item | Value |
|---|---|
| Input | `[1, 612, 14, 16]`: 12 contract channels (per-symbol I/Q) plus 4 Qm one-hot channels |
| Trunk | entry 1×1, then **18 residual blocks** (`DepthwiseConv2D 3×3` with dilation cycling 1,2,3,4 → ReLU → `FoldedScaleConv 1×1` → ReLU → Add), then four heads |
| Heads | `llr` (`head_stride=8` channels per symbol), `post_eq_noise`, `ch_est`, `dmrs_noise` |
| Width | `--c-hid 32` (26 284 parameters) or 64 (89 420 parameters) |
| Receptive field | 127 subcarriers (dilating to 32 reaches 631 and runs off the allocation; see §0.4) |
| Deployed latency | c32 **441–448 µs**, c64 926–942 µs (ANE, `predict`) |

`build()` in `train_depth3.py` is the only implementation. `--layout folded|spatial` decides whether time
is a folded axis or a spatial one; **the deployment contract is spatial**, so training and export must
agree on it.

## 2. Data

**Real captures** (`OCUDU_UL_DUMP=<prefix>` records PUSCH grids, `OCUDU_HELENA_DUMP_DIR` writes TBs on CRC
pass) become samples through `build_depth3_dataset.py`: `X [612,64]`, `H`, `r` (the reference amplitude,
**not** a code rate — `nof_tb_bits/nof_bits` is the rate), `sigma2_over_r2`, `bits`, `tb_bits`, `qm`,
`alloc_*`, `dmrs_bitmap`, `snr_nominal`, `source`.

**Synthetic samples** (`--source synthetic`) fit a TDL channel to a capture and re-inject noise at a
**prescribed SNR**:

- `--synth-prb`, `--synth-start-prb`, `--synth-rate` relocate the fitted channel to a chosen cell
  (modulation × width × code rate);
- `--synth-dmrs-symbols` places the **DM-RS at a chosen set of symbols** (the channel still comes from the
  capture's own pilots), so a combination the corpus contains no transmission of — 256QAM at 2,7,11, for
  instance — can be manufactured without flying a new session first (§19.216).

Two lessons cost real time and are encoded above:

1. **Pattern coverage beats operating-point coverage beats label availability.** The first training set was
   12 000/12 000 on DM-RS 2,11 and collapsed the moment the field used 2,7,11 (§19.211).
2. **A reporting head's range is bounded by the highest SNR in its data.** With 64QAM samples stopping at
   24 dB the head saturated at 24.4 dB and no calibration could recover the missing range (§19.220(e), §19.215).

## 3. Training

```bash
# reference recipe: c32, two patterns, four modulations, high-SNR additions
train_depth3.py --train d3_dmrs2711h_train.npz --val d3_synth_narrow_val.npz \
  --out data/runs/d3_dmrs2711h_c32 --modcond --layout spatial --in-channels 12 --blocks 18 \
  --c-hid 32 --snr-range 12,48 --epochs 40 --batch 8 --w-noise 0.3 --seed 20261022
```

- `--w-noise` weights both `post_eq_noise` and `dmrs_noise`. **0.3 is what buys the head its range**; at
  0.01 the 256QAM publication was nearly a constant (§19.215(b)).
- `--jit` compiles the step with XLA and is **training only** (it cannot reach the exported graph). It buys
  1.43–1.48× on a CUDA backend and is 6.2× *slower* on this project's laptop CPU (§19.219).
- Training runs in fp32 while the deployed artefact is fp16 chosen by the conversion; that is the only
  divergence allowed between the two graphs.

## 4. Export and calibration

```bash
train_depth3.py --export-only d3_dmrs2711h_c32/depth3_best.keras \
  --export-probe .../d3_dmrs2711h_c32_cal.mlpackage --modcond --layout spatial --in-channels 12 \
  --blocks 18 --c-hid 32 --snr-range 12,48 \
  --sinr-cal "2:1.2678,-3.8298;4:1.0911,-1.3579;6:1.1251,-1.6222;8:1.2822,-4.0420"
xcrun coremlcompiler compile .../d3_dmrs2711h_c32_cal.mlpackage <out-dir>
```

The calibration changes **only the SINR published to the scheduler** (`stats.sinr_dB`); it does not touch
the LLRs — measured over 550 receptions and 8 cells, bit for bit identical with and without it
(§19.222(e)). It corrects a **bias**, never a missing **range**: get the data range right first.

Note the sign convention: the knob multiplies the *published SINR*, so a fit made in the noise space must
have its offset negated. Declaring it as-is is the mistake that broke attach in §46.13ter.

## 5. Testing

| Layer | Where | Needs |
|---|---|---|
| **Contract unit tests** (plane, label, bundle declarations) | `../test/`, registered in CTest | numpy only |
| Reporting heads against known truth | `probe/heads_vs_prescribed.py`, `probe/fit_sinr_cal.sh` | TensorFlow |
| Replay comparison (classical chain versus arm) | `probe/attach_ab.py`, `probe/leg_replay.sh` | Core ML and captures |
| Live A/B and its verdict | `probe/fly_arm.sh`, `probe/arms_summary.py`, `probe/cell_report.py` | A radio |

## 6. Files

| File | Role |
|---|---|
| `depth3.py` | The single implementation of the contract: packing and unpacking, masks, labels, scaling, the conditioned layout |
| `depth3_loss.py` | The four heads' losses (LLR BCE, channel NMSE, two noise terms) |
| `train_depth3.py` | `build()`, batching, the training loop, export, CLI |
| `build_depth3_dataset.py` | Dataset construction from captures (real and synthetic paths) |
| `augment_depth3_dataset.py` | Extend an existing dataset with per-modulation files and print the qm × width coverage before and after |
| `merge_depth3_datasets.py` | Merge datasets (deduplication, weights) |
| `calibrate_depth3.py` | Fit the calibration in the engine's space (mind the sign: the knob multiplies the published SINR) |
| `eval_depth3.py` | Decode and BER evaluation over a dataset |
| `check_reporting_heads.py` | Reporting heads against known truth |
| `depth3_paths.py` | Names the location of the AI-CE training tree, which owns the shared DSP utilities |

## 30. The whole pipeline, end to end

This section exists so the receiver can be trained for a **different deployment** — another band, another bandwidth, another subcarrier
spacing — without rediscovering the steps. Each stage names the tool that does it and the artefact it leaves behind.

```
flight ──► captures ──► labels ──► datasets ──► network ──► bundle ──► buckets ──► gates ──► default
          (PHY dump)   (replay)   (npz)        (keras)     (mlpackage) (mlmodelc)  (verdict)
```

### 30.1 Capture

The PHY dumps a reception when the environment asks for it (`lib/phy/upper/channel_processors/pusch/ul_capture.cpp`):

| Variable | Meaning |
|---|---|
| `OCUDU_UL_DUMP=<dir>` | where to write; the arm appends the slot and RNTI to this prefix |
| `OCUDU_UL_DUMP_COUNT=N` | how many receptions to keep (the default keeps only eight, which is not enough) |
| `OCUDU_UL_DUMP_MIN_RB=N` | skip grants narrower than this |
| `OCUDU_UL_DUMP_LLR=1` | also write `_llr.bin`, which is what the sign-agreement gate compares against |

One reception becomes four files sharing a stem `C_<sfn>_<slot>_<rnti>`: `.bin` (the frequency-domain grid), `_h.bin` (the channel the
instrumentation knew), `_ce.txt` (the estimator's sidecar) and optionally `_llr.bin`.

> The sidecar's `snr` field is written from the classical estimator's results object. In a **pure** AI run that object is the arm's own
> empty stub, so the number is meaningless — sidecars carrying 207.9 or 2828.3 "dB" are the normal outcome of a pure flight, not bad
> data. Take the SNR from the CPU chain's replay instead (§30.7).

### 30.2 Label

`build_depth3_dataset.py` turns captures into an `.npz` whose label is the **transmitted bits**, recovered by decoding the capture. Two
rules matter:

1. **Keep only CRC-passing captures.** A failing block is not a codeword, so its converged signs are not a target.
2. **Decide that with the CPU chain.** The Metal chain's channel estimator has a serious defect at 256QAM (13 % against the CPU's 83 % on
   sixty captures), so a Metal verdict discards good samples; `ul_chain_replay --cpu` is the reference (see
   `doc_chinese/metal_kernel_fusion/256qam_channel_estimator_defect_2026-10-24.md`).

### 30.3 Compose the corpora

| Tool | Purpose |
|---|---|
| `build_depth3_dataset.py` | captures → one `.npz` per modulation |
| `merge_depth3_datasets.py` | merges datasets of the SAME modulation only (it refuses a mixed input on purpose) |
| `doc_chinese/llr_ai_detection/work_tmp/probe/concat_datasets.py` | general concatenation that accepts mixed modulations and holds out a validation slice |
| `doc_chinese/llr_ai_detection/work_tmp/probe/split_by_qm.py` | splits an existing dataset by modulation order |
| `doc_chinese/llr_ai_detection/work_tmp/probe/self_label_captures.py` | replays captures and writes the dataset for the ones that decode |

**Balance the modulation mix to the deployment, not to the corpus.** Measured on the Hong Kong test bed, the air carries roughly 45 %
QPSK, 30 % 16QAM, 15 % 64QAM and 10 % 256QAM, while the corpora collected for high SNR carry 60–73 % in the last two. Every attempt to
improve the receiver by ADDING data failed (E3 synthetic narrow, E5 merged, E8 four heads) because the additions kept the same wrong mix;
E4, which accidentally moved the mix closer, is the one that helped.

### 30.4 Train

```
train_depth3.py --train <train.npz> --val <val.npz> --out <run-dir> \
    --modcond --layout spatial --blocks 18 --c-hid 32 --in-channels 12 \
    --w-ch 0.1 --w-noise 0.3 --batch 8 --epochs 40 --snr-range 0,48 --max-val 0 \
    [--no-reporting-heads]
```

The flags that were measured to matter, and the ones that were not:

| Knob | Finding |
|---|---|
| `--c-hid` | 16 is too narrow (19/30 decodes against 27/30), 24 matches 32 with 38 % fewer parameters — pin it by measurement, not by feel |
| `--blocks` | 12 decodes as well as 15 and 18 ⇒ depth can be cut by a third |
| `--w-noise 0.3` | the deployed value; `0.01` produces reporting heads that are clamped |
| `--no-reporting-heads` | drops `ch_est` and `dmrs_noise`; the shared measurement layer then supplies SINR, RSRP and EPRE, and does it better |

### 30.5 Export and buckets

```
python train_depth3.py --export-only <run>/depth3_best.keras --export-probe <dir>/<name>.mlpackage \
    --export-buckets 108,180,252,324,396,468,540,612 --modcond --layout spatial --modulation 256QAM --snr-range 0,48
xcrun coremlcompiler compile <dir>/<name>.mlpackage <dir>
# and the same for every <name>_w<width>.mlpackage, moved into <dir>/buckets/base_w<width>.mlmodelc
```

The arm binds a sibling `buckets/` directory automatically, choosing the smallest width that fits the grant plus four PRB of context. The
bucket widths depend on the deployment's PRB count: they are `(2, 4, 6, 8, 10, 12, 14, 16) × 3 PRB` in subcarriers, i.e. 36-sc spacing,
regenerated when the bandwidth changes.

> The default model is a **compile-time** path in `apps/units/flexible_o_du/o_du_low/CMakeLists.txt` and it is a `CACHE FILEPATH`: editing
> the `set()` line changes nothing until the cache is overridden with `-DOCUDU_DEPTH3_DEFAULT_MODEL=...` or cleared. Verify with
> `strings build/apps/gnb/gnb | grep <name>`.

### 30.6 Gates

`doc_chinese/llr_ai_detection/work_tmp/probe/verdict.py` runs all three and prints one line each:

| Gate | Threshold | Why |
|---|---|---|
| sign agreement | ≥ 98.7 % | the soft bits must point the same way as the classical chain's |
| attach decodes | ≥ 65 % of the captures | the mid-band behaviour must not regress |
| per-bin BER | no bin worse than the reference by 0.15 pp | the low-SNR bands are the reason the work exists |

Supporting analyses, same directory: `binned_goodput.py` and `binned_deep.py` (per-SNR decomposition of delivered bytes into chances ×
size × decode), `bler_by_tcr.py` / `bler_full.py` (block error rate against SINR per modulation and code rate), `sinr_waterfall.py`,
`leg_validity.py` (is a flight leg usable), `state_timeline.py` (duty cycle of the latency oscillation), `align_state.py` (host state
during the slow phase).

### 30.7 Porting to another band, bandwidth or SCS

Everything that depends on the deployment lives in **one** header and **one** export flag:

| Item | Where | Change |
|---|---|---|
| subcarrier count | `pusch_depth3_input_plane.h` (`NOF_SUBCARRIERS`) | the allocation size of the new bandwidth, plus four PRB of context |
| plane channels | same header (`NOF_CHANNELS`) | 12 contract + 4 Qm one-hot = 16; unchanged |
| symbols | same header (14) | unchanged |
| bucket widths | the export command | regenerate from the new PRB count |
| minimum grant | `MIN_PRB` in the same header | 2 today; must match what the scheduler can actually grant |
| reported range | the export `--snr-range` | the range the reporting heads were trained on; a bundle that declares none is gated by nothing |
| calibration | the sidecar next to the bundle | per modulation, fitted for the new deployment |

A new deployment therefore needs: a capture flight on that deployment, a re-labelled corpus, one training run, one export, and the three
gates. Nothing in the network changes.

### 30.8 Pitfalls that cost a day each

| Symptom | Cause |
|---|---|
| a leg decodes almost nothing, ping works, iperf is zero | `OCUDU_DEPTH3_COMPUTE_UNITS=cpu_gpu` — 1677 µs per reception against a 500 µs slot; the arm now refuses it |
| `sinr=na` on every reception | a headed bundle forced onto the shared layer (`OCUDU_DEPTH3_IGNORE_REPORTING_HEADS=1`) skipped the plane measurement; fixed |
| sidecar SNR in the hundreds | written by the pure-mode stub; use the CPU replay instead |
| the latency median flips between 355 and 760 µs inside one leg | a five-second oscillation, not two states; the leg median is a window snapshot |
| "the code has no 256QAM" | the PUSCH table delegates to the PDSCH one for `qam256`; read the branch at the top of the function |
| a new export does not take effect | the CMake `CACHE FILEPATH` above |
