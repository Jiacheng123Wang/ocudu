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
