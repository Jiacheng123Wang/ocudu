# doc_chinese/ - the design and planning record of the Apple Silicon work

This is where the **Chinese** documents of the OCUDU Apple Silicon / Metal port live: the long-term
plan, the per-module plans and memos, the audits, the cross-cutting analyses (architecture
comparison, competitiveness, platform portability), and the session handoffs that carry the state
from one working session to the next. A few of them have English translations under `docs/`
(`docs/apple_silicon_heterogeneous_gnb_plan_english.md`, `docs/build_macOS_note_english.md`).

Two rules the tree is kept by:

* **Chinese belongs here and nowhere else.** Code, configs, CMake and bench scripts carry English
  comments and English operator messages - including the scripts that used to narrate their progress
  in Chinese (`lib/.../channel_estimator/metal/ai_train/**/*.sh`). If a Chinese document shows up in
  `lib/`, `tests/` or `configs/`, it belongs in one of the directories below.
* **Documents are tracked; working material is not.** Bench logs, IQ captures, gate dumps, reference
  binaries, worktree backups and scratch tables stay out of the history - see `.gitignore` in this
  directory for the exact exclusions and the reasoning. Up to 2026-09-19 the whole tree was
  deliberately untracked; that policy was reversed when the documents became the record the
  repository should carry.

## What is here

| Directory | Content | Entry point |
|---|---|---|
| `phy_pipeline_gpu/` | The fused-lane port (batch 5a-5g: the whole IQ -> LLR chain in one device pipeline). Its contract claims **zero host <-> device data crossings**, measured on air - read that as contract check 5, which counts only the **declared** modules and excludes the IQ upload and the LLR download; the 5.9.120 milestone audit records exactly what the number does and does not cover, and the goal's "exactly two crossings" half has **no counter at all**. Living design document, goal/gap, A/B protocol, per-batch work docs, session handoffs. | `phy_pipeline_gpu/README.md`, then `phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` |
| `full_gpu_chain/` | The first full-chain GPU/UMA zero-copy attempt (before the fused lane): plan, S2 design, audit, session handoffs. | `full_gpu_chain/s2_full_chain_design.md` |
| `metal_ldpc/` | Metal LDPC decoder: the first module ported, and the paradigm the others followed. | `metal_ldpc/PLAN.md` |
| `metal_ce/` | Metal MMSE channel estimator (the 2D MMSE engine, K1/K2/K3/K4/K5, the TA port). | `metal_ce/Metal_MMSE_Channel_Estimator_PLAN.md` |
| `ai_ce/` | The AI (HELENA) channel estimator: implementation plan, 20 MHz plan, training and G-5 online-adaptation memos, plus `ai_train/` (the operator's manual and README for the training tooling that lives in `lib/.../channel_estimator/metal/ai_train/`). | `ai_ce/AI_channel_estimation_implementation_plan.md` |
| `macos_compat_refactor/` | macOS compatibility work: phase reports, and `NOT_RUN_AUDIT.md` - why each test that does not run on macOS is disabled and what would enable it. | `macos_compat_refactor/phase0_audit_report.md` |
| `test_environment/` | The bench: OAI UE over ZMQ + UHD end-to-end memo, the Hong Kong test environment and phone-attach notes, the VoNR/IMS interop plan. | `test_environment/OAI_UE_ZMQ_UHD_E2E_memo.md` |

Root-level documents:

| Document | Content |
|---|---|
| `apple_silicon_heterogeneous_gnb_plan.md` | The long-term plan (English version in `docs/`). |
| `build_macOS_note.md` | Building on macOS (English version in `docs/`). |
| `metal_vs_cuda_architecture.md` | Architecture comparison: the upstream CUDA addition is a **lookaside** ADT (isolated device address space, `cudaMalloc`, host spans, explicit copies, host polls/waits), while this port is **inline** on unified memory. It also evaluates the proposed "hardware-neutral device-backend interface layer" against our architecture and finds it wanting (8 of 10 requirements unmet, one of them semantically inverted). **Its purpose is NOT to prepare an upstream merge** (see its section 7): the comparison exists to avoid adopting the wrong abstraction and to recognise which engineering practices are genuinely reusable. |
| `apple_silicon_competitiveness_analysis.md` | Independent competitiveness analysis (v1.1). **Evaluation criterion: the value created for users - E2E performance in the V2X-style edge scenario where URLLC and eMBB coexist - not reviewer approval.** Derives the binding constraint from first principles (bandwidth is **not** it - the count of times the same data must be moved is), redone at equal price with the correct x86 part (L4/RTX 4000 Ada class, not an H100), and settles the vendor-risk question by **splitting the capability class in half**: the *hardware* half is commoditised (unified memory + programmable GPU + integrated NPU, five vendors), but the *software* half is not - on Linux only Apple has all three engines plus the ability to compile a model locally (AMD is NPU-only with Windows-side model generation, Qualcomm's X1E platform enablement was incomplete enough that a whole laptop project was cancelled, Intel is the closest x86 vendor). Its section 5.3 records that as an explicit **self-correction of v1.0**, which had merged the two halves into one criterion; section 5.3.4 then uses this repository's own Core ML measurements (ANE 191 us vs the GPU route's 570-620 us per grant for the same functional block) to argue that the engine-agnostic dispatch layer is what makes the NPU route discoverable at all. Also keeps the four mis-comparisons this project has made, so they are not repeated. |
| `platform_portability_design_rules.md` | **The design constraints that make the software portable once another candidate platform appears.** Follows from the point above: what we depend on is the *required hardware capabilities* plus *software applicability*, not Apple Silicon or macOS. Part A is a nine-item capability contract (C1-C9) phrased vendor-neutrally, each with what it degrades into when missing - written as a questionnaire for a candidate platform. Part B is the six applicability dimensions (S1-S6) that decide whether the stack is usable **today**. Part C is **seventeen design rules (R1-R17)**, each tied to the measured failure or near-miss it came from (e.g. R3: an ordering token must be scoped to the producer-consumer pair, not to "the newest thing on the queue" - the cross-lane stall measured as `commit->start = 5.0028 s`; R4: hazard tracking keyed on resource objects rather than addresses means the caller must canonicalise identity at the address level). It names the five patterns this repository already gets right (`dft_processor_grid_write`, `resource_grid_device_view`, the three `grid_ready`-family hooks, `phy_pipeline_mode` + the tri-state contract, and the four-line engine A/B loop in `ai_train/convert_coreml.py`), then keeps an honest **portability ledger**: the assets, nine debt items with file:line and a replacement shape, and the four blocking capability gaps. Its opening criterion - *a port may only touch files under `*/metal/`* - **fails today**, and the ledger says exactly where (seven of the nine debts are naming/registration issues with zero-behaviour-change replacements). |
| The two phone-connectivity notes of 2026-09-05/06. | Bench notes from the Hong Kong test environment. |

> **⚠ Number labelling in `apple_silicon_competitiveness_analysis.md`:** every figure there carries
> one of **【官方】** (vendor spec/vendor documentation, with a source link), **【实测】** (measured in
> this repository), **【推算】** (derived estimate, **must not be quoted as measured**) or
> **【待核实】** (no first-party source found yet; **must not enter external material**).
> The Apple GPU TFLOPS figures are **【推算】** - Apple does not publish them - and must be measured
> before they are cited anywhere.

## Not tracked here

`*/work_tmp/`, `*/logs/`, `*/air_logs*/`, `*/worktree_backups/`, `*/gates/`, `*/ref/`, `*/corpus/`
and every `*.log` / `*.bin` / `*.metallib`: they are the bench's working material (see the note at
the top of `doc_chinese/.gitignore`). `work_tmp/README.md` documents the convention those scratch
directories follow.
