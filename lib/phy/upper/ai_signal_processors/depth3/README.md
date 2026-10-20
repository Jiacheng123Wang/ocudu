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
| `depth3_modcond_r7/` | **Default model** since 2026-10-20 (`OCUDU_DEPTH3_DEFAULT_MODEL`): the same 26 284 parameters and c32 width, retrained with data from 0 dB and therefore declaring **0 to 48 dB** instead of 12 to 48 -- its reporting heads are six to ten times more accurate at and below 10 dB, which is what makes the lower floor defensible |
| `depth3_modcond_c32h/` | Previous default (2026-10-19), kept for rollback and A/B: identical shape, trained from 12 dB |
| `depth3_modcond_ft/` | The default before that (c64, 89 420 parameters), kept for the same reason |
| `ai_train/` | The training, data and calibration code, plus a design/data/training/testing summary |
| `test/` | The contract tests, registered in the project's unit-test suite via CTest |

The default is set in `apps/units/flexible_o_du/o_du_low/CMakeLists.txt`
(`OCUDU_DEPTH3_DEFAULT_MODEL`). It is a **cache variable**: an existing build directory keeps the old
path until it is reconfigured with `cmake -U OCUDU_DEPTH3_DEFAULT_MODEL -S . -B build`. Either bundle can
be selected per run with `--expert_phy.pusch_receiver_model=<path>` without touching the default.

Authoritative design record: `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md`
(the §19.2xx series) and `doc_chinese/llr_ai_detection/model_network_design.md`.
