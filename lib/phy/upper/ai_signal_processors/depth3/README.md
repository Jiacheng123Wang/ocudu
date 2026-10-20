# depth3 — models and training code for the depth-3 receiver

The depth-3 receiver treats **channel estimation, equalisation and demapping as one unit**, so its models
and training code live here rather than under the classical channel estimator's tree. The neighbouring
`signal_processors/channel_estimator/metal/ai_assets/` directory belongs to the *classical* estimator and
holds its `helena_*` models; the two chains are not the same thing.

(Moved here on 2026-10-19 from `signal_processors/channel_estimator/metal/ai_assets/depth3_*`, with every
path reference updated. Chinese design records stay in `doc_chinese/llr_ai_detection/`; this tree's
documentation is English.)

| Path | Role |
|---|---|
| `depth3_modcond_r7/` | **NOT the default**: trial of 2026-10-20, reverted the same day — it is six to ten times more accurate below 10 dB against SYNTHETIC truth, but on air its published SINR is compressed against the classical chain (slope 0.43 at 64QAM against 0.96 for `c32h`) and the leg lost 46% of its throughput. Kept in the tree for the retrain that has to fix both. Its declared 0 to 48 dB and its evidence are in its own README |
| `depth3_modcond_c32h/` | **Default model** (`OCUDU_DEPTH3_DEFAULT_MODEL`) since 2026-10-19, reverted back to on 2026-10-20 after the r7 trial: 26 284 parameters, c32 width, declares 12 to 48 dB, and its published SINR tracks the classical chain to 0.4-1.0 dB with a slope of 0.94-0.96 -- which is what the scheduler needs |
| `depth3_modcond_ft/` | The default before that (c64, 89 420 parameters), kept for the same reason |
| `ai_train/` | The training, data and calibration code, plus a design/data/training/testing summary |
| `test/` | The contract tests, registered in the project's unit-test suite via CTest |

The default is set in `apps/units/flexible_o_du/o_du_low/CMakeLists.txt`
(`OCUDU_DEPTH3_DEFAULT_MODEL`). It is a **cache variable**: an existing build directory keeps the old
path until it is reconfigured with `cmake -U OCUDU_DEPTH3_DEFAULT_MODEL -S . -B build`. Either bundle can
be selected per run with `--expert_phy.pusch_receiver_model=<path>` without touching the default.

Authoritative design record: `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md`
(the §19.2xx series) and `doc_chinese/llr_ai_detection/model_network_design.md`.
