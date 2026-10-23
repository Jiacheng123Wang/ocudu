# `depth3_e1_2heads_1022` — the two-reporting-head bundle (archived, **not** the default)

Trained 2026-10-22 from the old corpus (`d3_dmrs2711h_train.npz`, 30 777 samples) with the deployed recipe and exactly one
variable changed against `d3_modcond_c32h`: the two reporting heads (`ch_est`, `dmrs_noise`) are absent, so the bundle has
**two** outputs instead of four and 26 185 parameters become 16 969.

| File | What |
|---|---|
| `d3_e1_2heads.mlmodelc` | the full-band bundle (612 subcarriers) |
| `buckets/base_w{108..612}.mlmodelc` | the eight narrow-band graphs of the same weights; the arm binds them automatically because they are a sibling `buckets/` directory |
| `d3_e1_2heads.mlpackage` | the source form, for inspection and re-compilation |
| `depth3_best.keras` | the checkpoint the artefacts were exported from |

## Why it is archived rather than deployed

It is **not measurably better or worse than `d3_modcond_c32h` on air**, and the comparison is not finished:

* offline, it is better: sign agreement 99.01 % against 98.73 %, and 27 of 40 attach captures decode against 26;
* on air, goodput is 10.95 Mbit/s against 10.56 (means of two legs each, E1's legs 3 % apart and c32h's 19 %);
* on air, CRC-OK is 77.60 % against 78.35 % — the one number that favours the deployed bundle;
* published RSRP/EPRE come from the shared measurement layer for this bundle, and that layer was checked against the
  classical estimator on 16 captures: bias 0.01 dB, worst 0.08 dB;
* latency: the medians are equal within noise once each model's own run-to-run drift is taken out.

So the air evidence does not separate them, and the default stays `d3_modcond_c32h` (design document sections 19.333–19.344).

## The open question this bundle carries

In the engine-level probe (`coreml_latency_probe`, one model per process, 300 predictions, ten repetitions) this bundle never
exceeded 612 µs, while `d3_modcond_c32h` reached 1 023 µs at p99 and once 9 243 µs. **That difference has not reproduced on
air** — both bundles sit between 0.5 and 1.2 ms at p99 over tens of thousands of receptions — so it is an unexplained probe
observation, not yet a property anyone should rely on. `MLComputePlan` per-layer placement is the next step.
