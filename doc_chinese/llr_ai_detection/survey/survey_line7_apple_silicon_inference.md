# LINE 7 Survey — Neural-network inference (esp. transformers/attention) on Apple Silicon, for a real-time 5G-NR PHY receiver

> Audience: the OCUDU team planning a transformer-based 5G-NR physical-layer receiver
> (time–frequency resource grid → LLRs) that must run inside a real-time over-the-air gNB,
> on Apple-Silicon GPUs via CoreML / Metal.
>
> Survey date: **2026-10-07**. Working directory context: `ocudu` checkout on Apple Silicon
> (see `docs/apple_silicon_heterogeneous_gnb_plan_english.md`, `docs/build_macOS_note_english.md`).
>
> **Tag legend**
> `[Apple-published]` — first-party Apple documentation, Apple ML research, WWDC, Apple-owned repos.
> `[third-party measurement]` — a named person/org measuring their own or Apple's stack (includes vendor blogs; noted).
> `[vendor-independent benchmark]` — academic or neutral-body measurement with a stated method.
> `[unverified]` — I could not retrieve the primary text; existence/claim only.
> Anything without a tag is **derived arithmetic** by me from the cited 3GPP parameters.

---

## 0. Method and source-access notes (read this before trusting anything below)

* `web_fetch` was mostly blocked in this environment (`resolves to a non-public IP address`), **but `curl` from `bash` worked**, so nearly every source below was retrieved directly over HTTP and read from the local copy. Sources are therefore **primary-text verified** unless tagged `[unverified]`.
* **Could not retrieve (blocked / paywalled), tagged `[unverified]` below:**
  * `developer.apple.com/videos/...` — WWDC session pages return **HTTP 401** to non-browser clients. Session **titles** are verified via search results; **session contents are not**.
  * `mlcommons.org/benchmarks/client/` and MLPerf Client results pages — **Cloudflare interstitial (403)**. No MLPerf Client/Mobile number in this survey is verified.
  * `ieeexplore.ieee.org/document/11683190` — returns **HTTP 202 with an empty body**. The GNSS/ANE paper is real (title + venue verified via two independent indexes) but **no number from it is verified**.
  * `zenodo.org/records/22136104` — **403**.
* Apple developer-documentation prose was retrieved through Apple's own DocC JSON endpoint
  (`developer.apple.com/tutorials/data/documentation/...json`), which is the same content as the rendered page.
* **Terminology warning.** Several sources retrieved are dated mid/late-2026 and describe **"Core AI"**, described as
  Apple's successor to Core ML (iOS 27 / macOS 27, `coreai-torch` export to `.aimodel` bundles, `coreai-build` AOT compile).
  I report these because they carry the most recent Apple-Silicon inference numbers, but **I could not verify "Core AI" against
  any first-party Apple document** — treat the Apple-side claims about Core AI as `[unverified]` even where the measurement is
  third-party. Core ML itself is fully first-party-verified.

---

## (1) Apple's own documentation and WWDC material

### 1.1 Compute-unit selection and `MLModelConfiguration`

* `MLComputeUnits` is the enum that gates which engines a model may use: **`cpuOnly`, `cpuAndGPU`, `cpuAndNeuralEngine`, `all`**.
  Apple's own words: *"Use `all` to allow the OS to select the best processing unit to use (including the neural engine, if available)."*
  Note the phrasing — **the OS selects; there is no "neuralEngineOnly"**.
  [Apple-published] <https://developer.apple.com/documentation/coreml/mlcomputeunits>
* `MLModelConfiguration` is where `computeUnits` lives, alongside `parameters`, `allowLowPrecisionAccumulationOnGPU`, etc. It is set at `MLModel` creation time from a URL.
  [Apple-published] <https://developer.apple.com/documentation/coreml/mlmodelconfiguration> ·
  <https://developer.apple.com/documentation/coreml/mlmodelconfiguration/computeunits>
* **Consequence for a PHY workload:** you can *express a preference* for the ANE, you can *exclude* it (`cpuOnly`, `cpuAndGPU`),
  but there is **no configuration that forces the ANE**, and **no runtime API that reports which unit actually executed a call**.
  This is stated first-party by Apple's own `.all` wording, and stated bluntly by the ANEForge authors: *"no configuration requires the
  ANE, the CPU remains a permitted fallback, and no runtime interface reports which unit executed a given call."* `[vendor-independent benchmark]`
  <https://arxiv.org/abs/2606.17090>

### 1.2 `MLComputePlan` — the offline per-operation placement/cost API

* `MLComputePlan.load(contentsOf:configuration:)` loads a plan *without running the model*; you walk `modelStructure` →
  `program.functions["main"].block.operations` and query per-op:
  * `computePlan.deviceUsage(for: operation)` → which device the compiler *intends* to use,
  * `computePlan.estimatedCost(of: operation)` → an estimated cost.
  Apple's stated purpose: *"estimate the necessary cost and resources of the model before running the predictions."*
  [Apple-published] <https://developer.apple.com/documentation/coreml/mlcomputeplan-1w21n> ·
  <https://developer.apple.com/documentation/coreml/mlcomputeplan-1w21n/estimatedcost(of:)>
* **This is an estimate, not a guarantee.** ANEForge: *"Xcode's placement reports and MLComputePlan are offline developer estimates, not a
  runtime guarantee."* `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.17090>
* The most complete *public* worked use of this API for an ANE-targeted model is ANEMLL's `ane_profiler.py` (CoreMLTools ≥ 9.0, macOS 15+):
  it enumerates total/executable ops and prints the ANE/GPU/CPU split, the list of GPU-fallback ops, and a per-compute-unit timing table.
  Their published example: **4 630 total ops, 1 634 executable, ANE 1 622 (99.3 %), GPU 12, CPU 0**, with the 12 GPU ops named
  (e.g. `ios18.cast:current_pos_to_int16`, `select`). `[third-party measurement]`
  <https://github.com/Anemll/Anemll/blob/main/anemll/utils/ANE_PROFILER.md>

### 1.3 `MLTensor`, mlpackage/ML Program, compile & specialization cost

* **`MLTensor`** — *"A multi-dimensional array of numerical or Boolean scalars tailored to ML use cases, containing methods to perform
  transformations and mathematical operations efficiently using a ML compute device."* It is Core ML's Swift-side eager tensor type and the
  surface on which stateful models and `MLState` (KV-cache-style residency) sit. [Apple-published]
  <https://developer.apple.com/documentation/coreml/mltensor>
* **Load path / recompilation cost** (this is the part that bites a slot-loop). Apple's coremltools guide documents the lifecycle precisely:
  1. `mlpackage` → compile to `mlmodelc` (fast);
  2. instantiate with the config's `compute_units`;
  3. **"another compilation occurs for backend device specialization, such as for the Neural Engine (NE), which may take a few seconds or
     even minutes for large models"**;
  4. the specialization result is **cached, keyed to the full filesystem path of the `mlmodelc` folder** — move the folder and you pay it again.
  [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/model-prediction.html>
* Independently measured magnitudes: **CoreML/ANE model load 13 224 ms on M4 Max** for a Qwen3.5-0.8B mlpackage, vs **1 028 ms for MLX 4-bit**
  on the same machine; on iPhone Air the ANE bundle first-load was ~1.15 × 10⁶ ms (including an LTE download) and a sideloaded Gemma-4-E2B
  ANE bundle took **33 s** to load (4.8 GB on disk). `[third-party measurement]`
  <https://github.com/john-rocky/PrivateFoundationModels/blob/main/docs/RUNTIME_COMPARISON.md>
* **First-call ramp is real even after load.** Measured on M1 for a sustained ANE workload: *"call 1 about 7.6 ms (3.6× steady), call 2 about
  4.6 ms, call 3 within 13 percent of steady, then flat at about 2.15 ms."* `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.22283>
* **Op-set divergence between macOS and iOS**: an LFM2.5-350M `.mlpackage` that loads on macOS 26.0 **failed to build on iOS 26.4.2**
  (`CoreML failed to build model`), attributed to an opset / SSM op not supported by the iOS Core ML compiler. `[third-party measurement]`
  <https://github.com/john-rocky/PrivateFoundationModels/blob/main/docs/RUNTIME_COMPARISON.md>

### 1.4 Flexible shapes, and the ANE's shape sensitivity

* Apple supports three flexible-shape forms: **`EnumeratedShapes`** (a finite set), **`RangeDim`** (bounded per-dimension range), and
  unbounded ranges (neural-network format only).
* **`EnumeratedShapes` is the performance-recommended path and is the only one Apple explicitly blesses for the ANE**:
  *"Use `EnumeratedShapes` for best performance. During compilation the model can be optimized on the device for the finite set of input shapes.
  You can provide up to 128 different shapes."* And: *"Core ML preallocates the memory for the default shape, so the first prediction with the
  default shape is fast. The first prediction with a non-default shape may be slower, but subsequent predictions should be more optimized."*
  [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/flexible-inputs.html>
* Unbounded ranges are **rejected for `mlprogram`**; a `RangeDim` without a positive upper bound raises. [Apple-published] (same page)
* Multi-input models: before iOS 18 only **one** input could use `EnumeratedShapes`; from iOS 18 multiple inputs may, but only **index-matched
  ("linear") shape combinations** are accepted — anything else is a **runtime error**. [Apple-published] (same page)
* **The ANE escape hatch for dynamic shapes:** *"Setting the Reshape Frequency Optimization Hint to `Infrequent` can allow flexible shaped models
  to run on the Neural Engine, with iOS 17.4 or later"* — via `optimization_hints={"reshapeFrequency": ct.ReshapeFrequency.Infrequent}` or
  `MLOptimizationHints.reshapeFrequency`. The default is `frequent` ("fast shape switching"). [Apple-published]
  <https://apple.github.io/coremltools/docs-guides/source/flexible-inputs.html> ·
  <https://developer.apple.com/documentation/coreml/mloptimizationhints-swift.struct/reshapefrequency-swift.property>
* Apple's FAQ is explicit that **conversion can silently break ANE residency**: *"The converted model will run on the NE, unless the conversion
  introduces dynamic layers not supported on the NE, such as converting a static reshape to a fully dynamic reshape."* [Apple-published]
  <https://apple.github.io/coremltools/docs-guides/source/faqs.html>
* **Practical reading for a PHY receiver:** make the per-slot tensor shape a *compile-time constant* (you know the PRB count, symbol count and
  port count at cell-configuration time). If shape must vary, enumerate a small bucket set and pad. `RangeDim` for a slot-varying sequence
  length is exactly the case that pushes work off the ANE.

### 1.5 Compression: quantization, palettization, pruning

* `coremltools.optimize` exposes **linear quantization**, **palettization** (k-means / differentiable k-means lookup tables), and **pruning**,
  at post-training and training-time. [Apple-published]
  <https://apple.github.io/coremltools/docs-guides/source/opt-overview.html> ·
  <https://apple.github.io/coremltools/docs-guides/source/opt-palettization-algos.html>
* **Device-dependent advice that matters here:**
  * *"it is recommended to use activation quantization only when your model is fully or mostly running on the Neural Engine (NE)"* — on CPU
    (and sometimes GPU) activation quantization causes **load-time/runtime weight decompression and a slowdown**.
  * *"In newer hardware with A17 Pro or M4 chips … there is increased throughput possible for int8-int8 compute on Neural Engine"*.
  * For **per-block** scales (iOS 18 / macOS 15): great memory and latency gains **when running on the GPU**; **on the NE, use per-channel scales**.
  [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/opt-quantization-perf.html>
* **No ANE-int8 guarantee.** The community reference (Hollance) notes Core ML 8-bit ops were historically supported *"only for the matrix
  multiplication layers"* and that whether they execute on the ANE is unknown. `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/16-bit.md>
* **Debugging numerics is a first-class, supported workflow.** coremltools ships `MLModelComparator` (compare a fp32 reference against a fp16
  target and get the list of ops that diverge), `MLModelValidator` (find ops producing NaN/Inf) and `MLModelInspector` (expose intermediate
  tensors as outputs) — all under `coremltools.models.ml_program.experimental`. For an LLR-producing model this is the tool you would use to
  answer "which op is eating my soft-bit accuracy". [Apple-published]
  <https://apple.github.io/coremltools/docs-guides/source/mlmodel-debugging-perf-utilities.html>

### 1.6 Latency / performance reporting

* Apple's own reporting surfaces are the **Xcode Performance tab / Core ML Performance Report** and the `MLComputePlan` estimate above.
  Apple's published methodology for its own numbers: *"latency numbers were captured using the Xcode Performance tab, using the `median`
  statistic. Compute unit selection is `all` unless otherwise noted."* [Apple-published]
  <https://apple.github.io/coremltools/docs-guides/source/opt-quantization-perf.html>
* **A documented mismatch between the report and reality**: a developer thread titled *"Core ML Model Performance report shows prediction speed
  much faster than actual app runs"*. `[third-party measurement]` <https://developer.apple.com/forums/thread/765772>
* Apple's own ANE guidance warns that **Xcode's report and `MLComputePlan` are estimates** and that **per-op device switches cost
  context-transfer overhead** — Apple: *"mitigating the inter-engine context-transfer overhead"* is a stated benefit of a fully-ANE model.
  [Apple-published] <https://machinelearning.apple.com/research/neural-engine-transformers>

### 1.7 WWDC material (titles verified; contents NOT verified)

| Session | Title | Status |
|---|---|---|
| WWDC23 10049 | *Improve Core ML integration with async prediction* | title verified; page **HTTP 401** → contents `[unverified]`. <https://developer.apple.com/videos/play/wwdc2023/10049/> |
| WWDC24 10161 | *Deploy machine learning and AI models on-device with Core ML* | title verified; **401** → contents `[unverified]`. <https://developer.apple.com/videos/play/wwdc2024/10161/> |
| WWDC24 10211 | *Support real-time ML inference on the CPU* | title verified; **401** → contents `[unverified]`. This is the Accelerate/BNNSGraph session and is the closest Apple-published artefact to "real-time inference" guidance. <https://developer.apple.com/videos/play/wwdc2024/10211/> |

The async-prediction session is nonetheless cited *by Apple* as the source of the model-lifecycle diagram in the coremltools guide
([Apple-published] <https://apple.github.io/coremltools/docs-guides/source/model-prediction.html>), which is how I could confirm the
compile→specialize→cache story above at first hand.

> **Gap flagged:** I found **no Apple-published statement, doc page or WWDC title that discusses a real-time *control loop* deadline on the
> ANE**, and none that discusses wireless/PHY workloads. Apple's real-time material is CPU-centric (WWDC24 10211).

---

## (2) Benchmarks with numbers

### 2.1 Apple's own published numbers

| What | Number | Platform | Source / tag |
|---|---|---|---|
| ANE peak throughput, fp16 | **15.8 TFLOPS** (A15, 16-core, 2021); 0.6 TFLOPS on A11 (2017) → 26× in 4 years | A11→A15 | [Apple-published] <https://machinelearning.apple.com/research/neural-engine-transformers> |
| HF **distilbert**, seq len 128, batch 1, ANE-optimized | **3.47 ms at 0.454 W**; 9.44 ms at 0.072 W | iPhone 13 | [Apple-published] (same) |
| Same model, optimization effect | **up to 10× faster** forward pass, **14× lower peak memory** | iPhone 13 | [Apple-published] (same) |
| ANE bandwidth-boundness | latency *"approximately constant across sequence lengths of 32, 64, and 128 (batch 1) even though the computational load quadruples"* | ANE | [Apple-published] (same) |
| Stable Diffusion 2.1-base, 512×512, 20 steps, 6-bit weights, fp16 activations | E2E **11.2 s** (iPad Pro M1), **7.0 s** (iPad Pro M2), 10.4 s (iPhone 13 Pro Max), 18.5 s (iPhone 12 mini, reduceMemory) | M1/M2 iPad | [Apple-published] <https://github.com/apple/ml-stable-diffusion#performance-benchmarks> |
| Same, diffusion speed | 2.19 iter/s (M1), 3.07 iter/s (M2) | M1/M2 | [Apple-published] (same) |
| MobileNetV2-1.0 fp16 → W8A8 (weight+activation int8) | **0.48 ms → 0.27 ms** (iPhone 14 Pro / A16); **0.49 ms → 0.20 ms** (iPhone 15 Pro / A17 Pro) | A16 / A17 Pro | [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/opt-quantization-perf.html> |
| ResNet50 fp16 → W8A8 | **1.52 ms → 0.94 ms** (A16); **1.38 ms → 0.77 ms** (A17 Pro) | A16 / A17 Pro | [Apple-published] (same) |
| ResNet50 2-bit palettization vs fp16 | **1.52 ms → 1.43 ms**, compression ratio 7.63× | iPhone 14 Pro | [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/opt-palettization-perf.html> |
| Stable Diffusion attention variants | best `--compute-unit` is *"model version and hardware-specific"*; Apple's numbers are **not** peak HW capability | — | [Apple-published] (ml-stable-diffusion README) |

Two Apple methodological caveats worth carrying into any planning:
* Apple's Stable-Diffusion text encoder is converted with a **static shape that computes all 77 tokens regardless of prompt length** — i.e.
  Apple's own recommended pattern is to *pad to a fixed envelope and eat the waste*, precisely because dynamic shapes cost placement.
  [Apple-published] <https://github.com/apple/ml-stable-diffusion#performance-benchmarks>
* Apple explicitly says the optimization principles in that repo (attention implementation etc.) *"are generally applicable to Transformers
  and not customized to Stable Diffusion."* [Apple-published] (same)

### 2.2 CoreML / ANE vs GPU vs CPU vs MLX vs llama.cpp — third-party and academic numbers

**ANE characterization (the most useful single source for feasibility):** arXiv:2606.22283, *Apple Neural Engine: Architecture, Programming,
and Performance* (S. Bryngelson, Jun 2026, 302 pp; direct measurement on **M1 and M5**, static analysis of the private stack; claims labelled
measured / decompile-derived / predicted). `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.22283>

| Metric | Value | Tag |
|---|---|---|
| Per-dispatch floor (M1) | **≈ 0.23 ms per evaluation**; a relu, sigmoid, average-pool and small conv all land at **0.23–0.26 ms**; a 64-element linear is 0.23 ms | [vendor-independent benchmark] |
| Compute roof (M1, fp16) | **12 TFLOP/s** marginal on fused matmul chains; **4.8 TFLOP/s** measured on a large (4096²) matmul, ≈ 87 % of a **5.5 TFLOP/s** theoretical peak; a single conv dispatch ≈ **1.8 TFLOP/s** | same |
| Bandwidth roof (M1) | **85 GB/s**; ridge point **141 FLOP/byte** | same |
| On-chip working-set limit | **≈ 2 MB (M1)**, **4.72 MB (M5)** — crossing it drops the same matmul from 12 → 4.8 TFLOP/s | same |
| Warmup | call 1 **7.6 ms** (3.6× steady), call 2 4.6 ms, call 3 within 13 % of steady, then flat **2.15 ms** | same |
| Generation mapping | M1=H13, M2=H14, M3=H15, M4=H16, M5=H17; M1 4-core (base) @ ~1.14 GHz, M5 16-core @ ~1.89 GHz | same |

**ANEForge** (S. Bryngelson, arXiv:2606.17090) — direct ANE programming bypassing Core ML, measured on **M5 Pro / macOS 26.5**, verified also
on M1 Max. `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.17090>

| Metric | Value |
|---|---|
| Small fused program, per call | **≈ 90 µs** |
| Per-program dispatch floor | **≈ 70 µs** |
| Pretrained ResNet-18 forward, end-to-end | **0.33 ms** |
| Weight streaming | int8 / int4-LUT / sparse from the engine's dequant path; footprint ÷~2 (int8) and ÷~4 (int4); a 4096² matmul weight blob 33.6 MB → 8.4 MB |
| Native fused attention (`af.sdpa`) | causal attention, single-query decode shape over cached KV, runtime additive mask |
| FFT demo | factored DFT on the engine, *"matches np.fft to fp16"* |

> **The M1↔M5 dispatch-floor gap (230 µs vs 70 µs) is the single most decision-relevant number in this survey.** It is a ~3.3× generation
> difference on the *fixed* cost of one ANE evaluation, and it is the cost that a per-slot PHY pipeline pays per fused program.

**Whisper encoder: ANE (direct) vs CoreML vs Metal** — whisper.cpp discussion #3903, encoder-only, same transcripts (cosine 0.999).
`[third-party measurement]` <https://github.com/ggml-org/whisper.cpp/discussions/3903>

| model | ANEForge (ANE) | CoreML | Metal | vs CoreML | vs Metal |
|---|---:|---:|---:|---:|---:|
| tiny | **5.7 ms** | 11.2 ms | 7.3 ms | 2.0× | 1.3× |
| base | **12.2 ms** | 23.0 ms | 13.5 ms | 1.9× | 1.1× |
| small | **40.3 ms** | 77.2 ms | 40.9 ms | 1.9× | 1.0× |
| medium | **117.6 ms** | 236 ms | 120 ms | 2.0× | 1.0× |
| tiny, 199 s audio, 8 windows | **8.2 ms/window** | — | 15.9 ms/window | — | 1.9× |

**ANE bandwidth vs system memory bandwidth** (ANEMLL-Bench, M1→M6 survey). `[third-party measurement]`
<https://github.com/anemll/anemll-bench/blob/main/Results.MD>

| Chip | ANE BW (GB/s) | System mem BW (GB/s) | ANE utilisation |
|---|---:|---:|---:|
| M1 | 61 | 68 | 89 % |
| M2 Pro | 62 | 200 | 31 % |
| M2 Max | 62 | 400 | 16 % |
| M3 | 63 | 100 | 63 % |
| M4 | 64 | 120 | 53 % |
| M4 Pro | 126 | 273 | 46 % |
| M4 Max | 119 | 546 | 22 % |
| M5 Max | 148 | 614 | 24 % |
| M5 Pro | 150 | 307 | 49 % |
| M6 | 152 | 170 | 89 % |

> Reading: **the ANE's own bandwidth has been nearly flat (61→64 GB/s) from M1 to M4** and only roughly doubles at M4 Pro / M5. Coupled with
> the ANE book's 85 GB/s roof on M1, an ANE-resident PHY kernel is bandwidth-limited long before it is FLOP-limited.

**llama.cpp on Apple Silicon (PP = prefill, TG = token generation, t/s)** — the canonical community table.
`[third-party measurement]` <https://github.com/ggml-org/llama.cpp/discussions/4167>

| Chip (GPU cores) | BW GB/s | F16 PP | F16 TG | Q4_0 PP | Q4_0 TG |
|---|---:|---:|---:|---:|---:|
| M1 (8) | 68 | — | — | 117.96 | 14.15 |
| M1 Pro (16) | 200 | 302.14 | 12.75 | 266.25 | 36.41 |
| M1 Max (32) | 400 | 599.53 | 23.03 | 530.06 | 61.19 |
| M2 Ultra (76) | 800 | 1401.85 | 41.02 | 1238.48 | 94.27 |
| M3 Max (40) | 400 | 779.17 | 25.09 | 759.70 | 66.31 |
| M4 Max (40) | 546 | 922.83 | 31.64 | 885.68 | 83.06 |
| M5 Pro (20) | 307 | 1588.78 | 21.55 | 1620.64 | 66.33 |

**Runtime cross-comparison, Mac Studio M2 Ultra, 192 GB, Qwen-2.5 family, up to 100 k tokens** (arXiv:2511.05502, *Production-Grade Local LLM
Inference on Apple Silicon*). `[vendor-independent benchmark]` <https://arxiv.org/abs/2511.05502>

* Sustained throughput ranking: **MLX ≈ 230 tok/s > MLC-LLM ≈ 190 > llama.cpp ≈ 150 (short-context only) > Ollama 20–40 > PyTorch MPS ≈ 7–9 tok/s**.
* MLX TTFT **5–7 ms**; MLC-LLM *"achieved ∼190 tokens/sec"* with the paper's lowest TTFT at moderate prompt sizes; PyTorch MPS degrades to
  **≈ 1.2 tok/s at 32 k context** *(the paper also prints a "12–13 ms" token-latency figure in its streaming table, but the framework it attaches to
  is ambiguous in the text I extracted — not quoted here).*
* Quantization improved MLC-LLM throughput (~190 → ~210 tok/s) at small quality cost.
* Paper's own conclusion: Apple-Silicon frameworks *"still trail NVIDIA GPU-based systems such as vLLM in absolute performance"*.

**CoreML/ANE vs MLX, same model × same harness** (Qwen3.x-0.8B; median of 3 after 1 warmup). `[third-party measurement]`
<https://github.com/john-rocky/PrivateFoundationModels/blob/main/docs/RUNTIME_COMPARISON.md>

| Device | Backend | Load | TTFT | Total | Decode |
|---|---|---:|---:|---:|---:|
| M4 Max | MLX / GPU (4-bit) | 1 028 ms | **43 ms** | 114 ms | **1 254 chars/s** |
| M4 Max | CoreML / ANE (FP16-ish) | **13 224 ms** | 526 ms | 1 921 ms | 217 chars/s |
| iPhone Air | MLX / GPU (4-bit) | 86 980 ms (incl. download) | **80 ms** | 231 ms | 589 chars/s |
| iPhone Air | CoreML / ANE (FP16-ish) | 1 151 978 ms (incl. download) | 560 ms | 2 062 ms | 202 chars/s |
| iPhone Air | CoreML / ANE, Gemma-4-E2B | 33 s (sideload) | 661 ms | — | **34.6 tok/s** |
| iPhone Air | MLX / GPU, Gemma-4-E2B 4-bit | 2.3 s (warm disk) | **84 ms** | — | **45.2 tok/s** |

**Per-step Core ML chunk timings on a Mac ANE** (Gemma 4 E2B, 4 decode chunks, `computeUnits: .cpuAndNeuralEngine`).
`[third-party measurement]` <https://github.com/john-rocky/CoreML-LLM/blob/main/docs/MAC_BENCH_2026-04-19.md>

* Steady state: `c1=5.3 c2=6.6 c3=7.4 c4=10.3` ms → **sum 29.6–29.8 ms/token, 33.1–33.5 tok/s**.
* ANE op residency audit per chunk: **chunk1 1058/1058 = 1.0000, chunk2 914/914 = 1.0000, chunk3 824/825 = 0.9988, chunk4 832/841 = 0.9893.**
* Energy/thermals: *"the GPU runtimes (MLX, LiteRT-LM) heat up and shed ~50–60 % of their throughput under sustained load, while the ANE barely
  moves (retains ~65 %)… The ANE draws ~half the package power (measured on Mac via `powermetrics`)."* `[third-party measurement]`
  <https://github.com/john-rocky/apple-silicon-llm-bench>

**On-device model placement depends on weight encoding, not on intent** (arXiv:2608.22110, M1 + M3, five checkpoints, fixed single-token forward,
compiler device plans + synchronised memory-controller counters + compute-unit exclusion controls). `[vendor-independent benchmark]`
<https://arxiv.org/abs/2608.22110>

* *"On an M1, the smaller fp16 export executes on the CPU despite permitting ANE execution, whereas its compressed counterparts exhibit ANE activity."*
* *"The int8 export reduces warm forward latency by a factor of 1.9."*
* *"The larger fp16 export also uses the ANE, indicating that dense encoding alone does not determine placement."*

**Real-time neural inference on Apple Silicon under deadline pressure** — DAFx-26, *Real-Time Neural Audio on Apple Silicon: Benchmarking Inference
Frameworks under Realistic DAW Contention* (Maynooth Univ.; MacBook Air **M3**, macOS 26.4; Tracktion Engine + Core Audio IOProc; buffer 128 @ 48 kHz
= **2.67 ms deadline**). This is the closest published analogue to "run a network inside a fixed real-time callback on a Mac". `[vendor-independent benchmark]`
<https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_16.pdf>

| Metric | Result |
|---|---|
| Isolated RTF (median/deadline), large models, buffer 128 @ 48 kHz | BNNSGraph **LSTM-L 0.053, TCN-L 0.022, WN-L 0.021**; RTNeural-Eigen 0.078/0.222/0.092; RTNeural-XSIMD 0.089/0.211/0.085; LibTorch 0.346/0.230/0.209; ONNX RT 0.100/0.115/0.063 — *"all backends achieve RTF < 1.0, with BNNSGraph using at most 5.3 % of the deadline"* |
| **The central result** | RTNeural-XSIMD TCN-Large has isolated RTF 0.211 *"suggesting 79 % of the deadline budget remains unused"*, yet under a real Core Audio callback **with zero contention tracks** its p99 utilisation reaches **158.5 % with 355 hardware xruns** |
| Under maximum contention (36 tracks) | BNNSGraph p99 **14.3 %**, **0 xruns**; RTNeural p99 88.0 %, 15 xruns |
| Scaling (TCN-Large, buffer 128, N instances) | BNNSGraph p99 4.0 % (N=1) → 27.1 % (N=16), 1 xrun; anira-LibTorch 44 324 / anira-ONNX-RT 14 022 inference underruns at N=16 |
| Mechanism | BNNSGraph (Core ML's AOT graph compiler) exposes **AMX** and is *"real-time safe"*: no heap allocation, no locks, no blocking syscalls, verified with RealtimeSanitizer; **accepts dynamic input shapes without recompilation**, `+0` buffers of added latency |
| ONNX Runtime / LibTorch | ONNX RT uses MLAS/NEON, **not AMX** ("we did not observe AMX usage by ONNX Runtime"); LibTorch reaches AMX only indirectly via Accelerate BLAS |

**Where Core ML's GPU path is headed (dispatch-bound, not FLOP-bound).** A community engineering analysis on M4 Max (Gemma 4 E2B, int8 custom
Metal kernels): *"core ~14.2 ms / ~70.5 tok/s, Swift end-to-end ~55 tok/s"*; *"int8 core 2.0 GB / 15.7 ms ≈ 127 GB/s ≈ 23 % of M4 Max peak (~546 GB/s)
⇒ efficiency/dispatch-bound, not bandwidth-bound (**≈1000 Metal dispatches/token**)"*; and *"Custom Metal kernels beat MPSGraph ONLY for big
memory-bound matmuls (FFN, the 262144-vocab head). Per-op kernelization of small ops is futile (measured: attn q/k/v/o int8 kernels were slower;
any single op-class ≤1.3 ms — MPSGraph already handles small ops well)."* `[third-party measurement]`
<https://john-rocky.github.io/coreai-model-zoo/knowledge/performance-ceiling.html>

**MLPerf.** No number is reported here. `mlcommons.org` is Cloudflare-gated (HTTP 403) from this environment and I could not verify any
MLPerf Client / MLPerf Mobile Apple-Silicon result. Note also that the DAFx authors state *"Apple has not submitted BNNSGraph results to MLPerf
or any other public benchmark suite."* `[vendor-independent benchmark]` (DAFx-26) — so **the absence of Apple numbers in MLPerf is itself a
documented finding**, not just an access failure.

---

## (3) CoreML/ANE limits that matter for a PHY (LLR) workload

### 3.1 Precision: the ANE is fp16-only, and fp16's problem is *range*, not mantissa

* *"The ANE appears to use float16 for everything. This means that if your model has activations that are relatively large (`> 1e2`) or
  relatively small (`< 1e-4`), you will lose precision. Or worse, the model may not actually work correctly at all — i.e. very small numbers
  become 0."* On the GPU, by contrast, Core ML uses fp16 storage with fp32 accumulation unless you set
  `allowLowPrecisionAccumulationOnGPU`. `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/16-bit.md>
* The ANE's fp16 numerics **differ per generation by about 1 ULP** and are otherwise stable; the architecture is one compiler for all targets.
  `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.22283>
* **A directly transferable study — half-precision FFT and SAR on Apple Silicon** (arXiv:2605.28451, M1, Metal):
  * FP16 FFT is **mantissa-limited at 56–61 dB SQNR** vs a double reference — *"comfortably radar-usable"*.
  * A naïve FP16 SAR pipeline produced **only NaN**: the conjugate-FFT-conjugate inverse transform grows magnitudes by ~N, and the
    matched-filter product (~5×10⁶ at N=4096) **overflows FP16's 65 504 ceiling**.
  * Fix = **fixed-shift block-floating-point (BFP)**: one 1/N scale before each inverse transform bounds every intermediate below 4096.
  * Result: *"the first quality-preserving FP16 SAR pipeline: peak/integrated sidelobe ratios, target SNR, and resolution match the FP32
    reference to within 0.1 dB at 42 dB end-to-end SQNR"*, at **306 GFLOPS (radix-8 FP16 FFT, N=4096, batch 256) vs a 139 GFLOPS FP32 baseline**
    on a fanless M1 (2.2×).
  * **FP8 (E4M3/E5M2) collapses to 14–20 dB SQNR** — *"FP16 today's precision floor for FFT-based radar"*. Also: *"Apple GPUs have no native FP8
    datapath: the M1–M4 GPU cores compute in"* fp16/fp32 only.
  * Paper's own framing: *"the binding constraint for FFT … is not mantissa precision but the 5-bit exponent's dynamic range."*
  `[vendor-independent benchmark]` <https://arxiv.org/abs/2605.28451>
* **Transfer to LLRs (my analysis, not published).** LLRs are log-domain and unbounded in magnitude, while the ANE's *storable* range is ±65 504
  with ~11 bits of mantissa. Two concrete consequences:
  1. **Clamping/saturation must be inside the model** (a tanh/soft-sign or an explicit clamp on the LLR head); letting an LLR grow to ±10⁵ will
     saturate, and letting it grow past 65 504 will produce Inf/NaN.
  2. **The read-out precision floor is fp16 mantissa**, so an LLR of magnitude ~10 has ~0.005 absolute resolution; near the decision boundary
     that is fine, in the tail it is not. A block-floating-point output scale (à la the SAR fix) is the obvious mitigation — and there is
     **no published work doing this for LLRs** (see §6).
* **Model-level proof that this bites in practice**: ANEMLL ships an FP16 compatibility checker because Gemma-3 (BF16-trained) residual streams
  exceed ±65 504 and produce NaN/Inf on the ANE, *"which can only represent values up to ±65,504"*, affecting all Gemma-3 sizes.
  `[third-party measurement]` <https://github.com/Anemll/Anemll#fp16-compatibility-for-ane>
* Use `MLModelComparator` (fp32 reference vs fp16 target) to localise which op destroys accuracy — first-party tooling for exactly this job (§1.5).

### 3.2 Quantization / palettization support

* Supported and first-party: **linear quantization** (weight-only and weight+activation; per-tensor, per-channel, per-block scales) and
  **palettization** (1/2/4/6/8-bit LUT, k-means or differentiable k-means).
  [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/opt-overview.html>
* Measured effect on latency (Apple's own table): W8A8 int8 buys **~1.8× over fp16 on A17 Pro** (ResNet50 1.38 → 0.77 ms); **2-bit palettization on
  ResNet50 buys only 1.52 → 1.43 ms** (7.63× compression, ~6 % latency). So on Apple silicon, **bits buy memory, not necessarily time**.
  [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/opt-quantization-perf.html> ·
  <https://apple.github.io/coremltools/docs-guides/source/opt-palettization-perf.html>
* **Where quantization actually helps**: the ANE book's roofline says a workload above the 2 MB working set is bandwidth-bound at ~85 GB/s (M1);
  the ANE's weight stream can be int8/int4-LUT dequantized on-engine (ANEForge: int8 ÷~2, int4 ÷~4 footprint), so quantization is the lever that
  moves a bandwidth-bound ANE kernel. `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.17090> · <https://arxiv.org/abs/2606.22283>
* Practical advice conflict to note: Apple says **per-channel scales on the NE, per-block on the GPU**; the community perf analysis says
  **int8 is the practical exactness floor** for LLM argmax (int4 flips it) — for LLRs, which are *more* sensitive than argmax, expect int8/weight-only
  to be the usable regime.

### 3.3 Can you use custom Metal kernels inside a Core ML model? — **No, not on the ANE.**

* `MLCustomLayer` / MIL custom ops **can only supply CPU and GPU implementations**; there is no public API to run custom code on the ANE:
  *"Because there is no public API to program the ANE, custom layers cannot run on the ANE."* `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/unsupported-layers.md>
* Apple's own guidance is to avoid them: *"Use a custom operator only if you can't get the performance you want, and only as a last resort"*,
  and *"Whenever possible, use a composite operator, which is more efficient than a custom operation, and compiles down to various hardware
  backends available on your device."* `[Apple-published]`
  <https://apple.github.io/coremltools/docs-guides/source/custom-operators.html>
* Therefore: **a hand-written Metal kernel and an ANE-resident model are mutually exclusive inside one Core ML model.** The only ways to combine
  them are (a) split the model in two and sequence Core ML ↔ your own Metal pipeline, paying an engine-switch and a synchronization each way, or
  (b) abandon Core ML and write the whole thing in Metal/MPS (losing the ANE).
* Apple's documented custom-layer route ([Apple-published]
  <https://developer.apple.com/documentation/coreml/creating-and-integrating-a-model-with-custom-layers>) is the bridge for a *GPU* custom op only.

### 3.4 Per-op supported-device constraints (the ANE's denylist)

Community-measured list of Core ML layers known **not** to run on the ANE `[third-party measurement]`
<https://github.com/hollance/neural-engine/blob/master/docs/unsupported-layers.md>:

* custom layers
* RNN layers (**LSTM, GRU**)
* `gather`
* **dilated convolutions**
* broadcastable and "ND" layer types (`AddBroadcastable`, `MultiplyBroadcastable`, `ConcatND`, `LoadConstantND`, …)
* certain broadcasting ops (e.g. multiply of `C×H×W` with `C×1×1`)
* **pooling with kernel > 13 or stride > 2**
* **upsampling with scale factor > 2**

Two structural facts to design around:
* **Partitioning behaviour**: *"If your model is `S → U → S → U → S → U` … Core ML will probably do ANE → GPU just once … However,
  `ANE → CPU → ANE → CPU → etc` does seem to happen, as it's cheaper to switch between the ANE and the CPU than between the ANE and the GPU."*
  So a single unsupported op can serialise the model across engines.
* **The ANE's native layout is 4-D channels-first `(B, C, 1, S)`**, and the **last axis must be contiguous and aligned to 64 bytes and is not
  packed** — using a singleton as the last axis pads to 64 B, i.e. *"32 times the memory cost in 16-bit and 64 times in 8-bit"*. Apple also
  recommends **replacing `nn.Linear` with `nn.Conv2d`** to reach that layout, and chunking attention into per-head matmuls for L2 residency.
  [Apple-published] <https://machinelearning.apple.com/research/neural-engine-transformers>
  This is directly relevant if your receiver reshapes a `[RE, subband]` grid into a transformer sequence.

**Attention specifically**: the ANE has a **native fused attention** primitive reachable via ANEForge (`af.sdpa`: causal, single-query decode over
cached KV, runtime additive mask) `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.17090>; through Core ML the route is the
`scaled_dot_product_attention` MIL op plus `MLState` for KV residency. **Apple publishes no per-op attention placement table** — Apple's guidance is
architectural (channels-first 4-D, per-head chunking, one einsum `bchq,bkhc->bkhq`, avoid reshape/transpose) rather than an op allowlist.
[Apple-published] <https://machinelearning.apple.com/research/neural-engine-transformers>

### 3.5 Dynamic shape, recompile avoidance, batch, memory

* **Dynamic sequence length is the enemy of ANE residency.** The supported pattern is `EnumeratedShapes` (≤128 shapes, all inputs index-matched from
  iOS 18) or `RangeDim` + `reshapeFrequency = .infrequent` (iOS 17.4+). First prediction on a non-default shape is slower; subsequent ones "should be
  more optimized". Apple's Stable-Diffusion conversion pads to a static 77-token envelope. [Apple-published] (§1.4)
* **Recompile cost is real and path-keyed.** Device specialization is cached against the `mlmodelc` path; a Python `MLModel("x.mlpackage")` in a new
  process regenerates the temp `mlmodelc` and re-specializes every run. Apple's documented workaround: `CompiledMLModel` / pin the `.mlmodelc` to a
  fixed directory. [Apple-published] <https://apple.github.io/coremltools/docs-guides/source/model-prediction.html>
  Independent evidence of the cost: a *"first-load ANE compilation makes its load time high"* and per-shape re-specialization is cited as a
  ~60–80× collapse if you switch to fixed-shape buckets instead of recompiling per shape. `[third-party measurement]`
  <https://github.com/john-rocky/apple-silicon-llm-bench> · <https://john-rocky.github.io/coreai-model-zoo/knowledge/performance-ceiling.html>
* **Batch prediction**: Core ML supports multi-array batch providers and, for modern `mlprogram` models, `MLTensor`-based batched prediction
  (`MLModel.prediction(from:options:)` family / `MLTensor` APIs). **I did not verify a first-party page stating the batch-latency scaling behaviour** —
  flagged as an uncertainty in §7. Apple's own ANE blog does state the *principle*: *"One way to escape the bandwidth-bound regime is to increase
  the batch size for batch inference workloads."* [Apple-published] <https://machinelearning.apple.com/research/neural-engine-transformers>
* **Async prediction**: Core ML exposes asynchronous prediction (`MLModel.prediction(from:options:)` async variants; WWDC23 10049 is titled for it).
  A concrete async design in the wild uses a **serial prediction queue with IOSurface-backed buffers and ping-pong/ring buffer patterns** to
  eliminate ANE race conditions — i.e. **Core ML/ANE prediction is not re-entrant enough to just fire two predictions at once**. `[third-party measurement]`
  <https://github.com/Anemll/Anemll>
* **Memory limits**: unified memory helps (no host↔device copies), but the ANE has a hard **on-chip working-set limit (~2 MB M1 / 4.72 MB M5)** above
  which you fall onto the bandwidth roof, and iOS applies jetsam (the phone benchmarks above report jetsam under sustained load). For a slot-sized
  grid the *input* is small — a 273-PRB × 14-symbol × 4-port complex fp16 grid is ~0.12 MB — but a transformer over resource elements will exceed
  2 MB quickly, so **chunk by symbol/subband and fuse**.

### 3.6 "Is the ANE usable from macOS the way people think?" — mostly no, and this is the crux

* **Through Core ML: yes, as a *scheduling outcome*, not a target.** `computeUnits = .all` or `.cpuAndNeuralEngine` permits the ANE; nothing requires it;
  nothing reports what ran. Available on Apple-Silicon Macs since M1. [Apple-published] §1.1 + `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/running-on-ane.md>
* **Direct programming: no public API.** *"Unfortunately not. You can only use the Neural Engine through Core ML at the moment. There currently is no
  public framework for programming the ANE … Apple rejects apps that use private frameworks."* `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/programming-ane.md>
* **Metal cannot reach the ANE.** *"Metal cannot be used to program the ANE, it's exclusively for the GPU."* Core ML's backends are BNNS (CPU),
  MPS (GPU) and **private frameworks** (ANE). `[third-party measurement]`
  <https://github.com/hollance/neural-engine/blob/master/docs/ane-vs-gpu.md>
* **The private route now exists but is explicitly unsafe for shipping.** ANEForge enters at the `e5rt` interface and dispatches to the engine only,
  bypassing Core ML's scheduler; it documents that *"The private symbols carry no API contract, so each release is verified against a recorded macOS
  and ANE-compiler version"*, executability requires physical Apple Silicon in the verified range, and *"hosted continuous-integration runners
  virtualize macOS without exposing the engine"*. Historical precedent: tinygrad dispatched single ops to the ANE in 2020–21 only by *"signing an
  Apple entitlement into the Python interpreter and relaxing system-integrity protection."* `[vendor-independent benchmark]`
  <https://arxiv.org/abs/2606.17090>
* **Documented reality checks from developers**: *"CoreML not using Neural Engine even though it should"*
  `[third-party measurement]` <https://developer.apple.com/forums/thread/729942>; and even with `computeUnits: .cpuAndNeuralEngine` and 100 % ANE
  residency by op count, throughput was ~33 tok/s vs ~45 tok/s for an MLX GPU 4-bit path on the same phone. `[third-party measurement]`
  <https://github.com/john-rocky/PrivateFoundationModels/blob/main/docs/RUNTIME_COMPARISON.md>
* **Bottom line for us:** on macOS the ANE is reachable **only via Core ML**, with **no residency guarantee**, **no runtime placement telemetry**,
  a **~70–230 µs fixed per-dispatch floor** depending on generation, and **no way to inject a custom Metal kernel into an ANE-resident graph**. If a
  PHY block must be provably deterministic in latency, the ANE is the wrong engine; if it is a large, fixed-shape, bandwidth-bound, single-fused-graph
  block, it is plausibly the *most energy-efficient* engine.

### 3.7 Vendored/other limits worth knowing

* Core ML's GPU path goes through **MPSGraph**; *"AOT (coreai-build) helps FIRST-RUN latency + per-shape re-specialization — NOT steady-state decode
  tok/s."* `[third-party measurement]` <https://john-rocky.github.io/coreai-model-zoo/knowledge/performance-ceiling.html>
* Sustained-load throttling asymmetry: **GPU runtimes shed ~50–60 % of burst throughput under sustained load; the ANE retains ~65 %** and draws
  ~half the package power. For a gNB that runs 24/7, this is a strong argument for the ANE *if* the latency envelope holds.
  `[third-party measurement]` <https://github.com/john-rocky/apple-silicon-llm-bench>
* The engine **does not throttle on a 3.5-minute sustained workload** on M1 and holds a single clock state (5.5 W whole-run, drift < 3 %).
  `[vendor-independent benchmark]` <https://arxiv.org/abs/2606.22283>

---

## (4) Prior art: Apple Silicon / CoreML / Metal / ANE for wireless, SDR, and real-time DSP

### 4.1 The single most relevant paper — and it is CUDA-only

**arXiv:2609.07843 — *The OCUDU dApp Platform: An Open Runtime and E3 Interface for Real-Time AI-RAN*** (Timothy O'Shea, Matthew Pennybacker,
Andriy Kharchenko; 7 Sep 2026; 17 pp; preview release of the OCUDU AI-RAN Working Group 2, BSD-3-Clause-Clear).
`[vendor-independent benchmark]` <https://arxiv.org/abs/2609.07843>

This is the platform our own project is adjacent to, and it defines exactly the timing contracts a neural receiver would need:

| Contract | Meaning | Measured / budget |
|---|---|---|
| **Class A** | resident **on the GPU receive chain**, host's own CUDA stream, zero-copy device tensors | budgets **100 µs for estimation** and **150 µs to completion** for the deeper depths; fallback = conventional stage on the same stream; 8 consecutive misses open a per-lane circuit breaker; soft wall budget ~2 ms |
| **Class B** | direct call at the **scheduler's decision boundary**, returns validated intents | **100 µs admitted deadline** (a host default, deliberately not a YAML knob); late/invalid results discarded, committed ones rolled back |
| **Class C** | never-blocking observer; scheduler consumes results later | producer cost is a try-lock + eventfd; full ring = counted drop, never a wait |

Measured on an **NVIDIA GB10 (DGX Spark, Arm)** host with a split-8 USRP B210, band n78 3410.1 MHz, 20 MHz (51 PRB) TDD at 30 kHz SCS, one TX /
one RX antenna, Open5GS core, commercial handsets: **`[vendor-independent benchmark]`**

| Checkpoint | Value |
|---|---|
| Class B direct call | **0.29 µs P99.9** quiet host; **5.2 µs** with the cell on air (P50 tick below) |
| Class C ring publish, 68 KB slot | **2.0 µs P50, 2.3 µs P99.9** |
| E3AP control round trip | **11 µs P99.9** |
| Receiver kernels, live shape | **82 µs P50, 112 µs P99.9** (suite); 92.5 / 105.1 µs (runtime) |
| Idle gate / rate floor / context cache read | 1 atomic load / 1 CAS / **48 ns P50** |
| Host-inline Class C full-grid copy | **50–100 µs per slot** for a 273-PRB, four-port grid |
| Slot | **500 µs at 30 kHz SCS**; *"about 100 µs of host jitter already means fronthaul lateness"* |
| System-level result | dApps of all three classes + an **out-of-tree neural equalizer** ran on a live cell with **no fallback**; learned equalizer gave **7–11 percentage points lower first-transmission BLER at MCS 11–15** |
| Class A tensor interface | full-slot device grid in **complex BF16**, DM-RS pilot tensor, data-RE indices, typed PUSCH metadata; outputs at three depths incl. **FP16 soft bits in `[data_re, layer, bit]` order before descrambling** |

**Critical negative finding for us:** the platform's declared backends are *"a CPU (x86, ARM) or CUDA backend"* and the emphasis is *"GPU-first,
because the compute headroom beside the baseband lets AI-RAN applications be tried at full fidelity."* Class A is defined on **CUDA streams and
CUDA events**. **There is no Metal, MPS, Core ML or ANE backend, and no Apple-Silicon measurement, anywhere in the paper, the platform, or the SDK.**
Additionally, **the dApp seams are not present in our checkout** (`lib/phy/upper/dapp/`, `lib/scheduler/dapp/`, `lib/dapp/` do not exist); the
platform is a separate preview repo (`gitlab.com/ocudu/work_groups/wg2_ai_ran`). So an Apple-Silicon neural receiver has **no published platform path** —
we would be authoring a Metal/MPS Class-A backend ourselves.

### 4.2 Neural receivers — the latency reference points (all non-Apple)

| Work | Latency / claim | Tag |
|---|---|---|
| NVIDIA, *Real-Time Neural Receivers Drive AI-RAN Innovation* — NN replacing channel estimation + equalization + demapping, TensorRT | **"under 1 ms inference latency on an NVIDIA A100 GPU"** | [third-party measurement] (vendor: NVIDIA) <https://developer.nvidia.com/blog/real-time-neural-receivers-drive-ai-ran-innovation> |
| NVIDIA Sionna research code / `neural_rx` | real-time inference "enabled through NVIDIA TensorRT on GPU-accelerated hardware"; **no per-slot latency table published in the tutorial** | [third-party measurement] (vendor) <https://nvlabs.github.io/sionna/v2.1.0/rk/tutorials/neural_receiver/> |
| 5G-style **LDPC** on Grace CPU vs integrated Blackwell GB10 (DGX Spark), Sionna PHY, 16-QAM, AWGN | CPU **≈ 0.71 ms per codeword at 20 BP iterations — exceeding the 0.5 ms slot**; GPU stays within **6–24 % of the slot**; **≈ 6× mean GPU/CPU throughput speedup**; CPU path consumes ~10 Grace cores, GPU adds ≈ 10–15 W over idle | [vendor-independent benchmark] <https://arxiv.org/abs/2602.04652> |
| ViT-based neural receiver for data-dependent superimposed training (arXiv:2605.29995) | ViT encoder + CNN decoder for channel estimation, CNN-LSTM demappers; **no runtime/latency figure published** | [vendor-independent benchmark] <https://arxiv.org/abs/2605.29995> |

> The neural-receiver literature's latency floor on a *datacentre* GPU is ~1 ms (A100, TensorRT). Our target is **≤ 500 µs of slot budget**, and
> the OCUDU dApp Class A budget is **100–150 µs**. That is 1–2 orders of magnitude tighter than the published neural-receiver numbers.

### 4.3 SDR / baseband on Apple hardware — real, but qualitative and x86-era ports

* **srsRAN 4G v25.10 + GNU Radio 3.10.12 + UHD 4.10 running natively on macOS ARM64** on a MacBook Pro M4, with a LibreSDR B220 Mini (Ettus B210
  clone, AD9361, 70 MHz–6 GHz, 56 MHz BW, USB 3.0). The lab decodes GSM BCCH from raw wideband captures, inventories 2G/4G cells, and runs
  **srsue + srsenb + srsepc in loopback — no Docker, no VM, no Linux host.** Required work: **21 patches to libosmocore**, a **~1 150-line SCTP
  compatibility shim** over usrsctp (XNU has no SCTP), and **23 patches + three Darwin shims** for srsRAN 4G. `[third-party measurement]`
  <https://www.andreigosman.ro/srsran-libosmocore-macos-arm64/>
  **No latency, sample-rate or real-time-margin numbers are reported** — it is a functional port report.
* **GNU Radio on macOS**: open issue *"Plots (QT GUI Time Sink) are very slow and laggy on GNU Radio 3.10.12 via Radioconda on macOS"* (macOS
  Sequoia 15.5) — GUI thread, not DSP throughput. `[third-party measurement]` <https://github.com/gnuradio/gnuradio/issues/7829>
* **SDRangel on Apple Silicon**: issue #1909 *"MAC M1 with Funcube Pro+"* — device runs at 192 kSa/s but **audio is choppy and pitch-shifted**
  ("chipmunks"), and the reporter confirmed the same hardware works under Windows; the maintainer could not reproduce. **No throughput measurement —
  a bug report, not a benchmark.** `[third-party measurement]` <https://github.com/f4exb/sdrangel/issues/1909>
* I found **no published GNU Radio / SDRangel / SoapySDR benchmark with a measured sample rate or real-time margin on Apple Silicon**, and **no
  srsRAN-Project/OCUDU 5G gNB over-the-air report on macOS**. The 4G lab above is the strongest public evidence that a Mac can *host* a
  software radio stack, and it is 4G and loopback for the baseband part.

### 4.4 Real-time GPU DSP on Metal — the strongest Apple-Silicon signal-processing prior art

* **Kernel-fused SAR on Apple Silicon** (arXiv:2604.03585, Bergach): first kernel-fused SAR Range-Doppler pipeline on any GPU — FFT + matched-filter
  multiply + IFFT fused into **one Metal compute dispatch**, intermediates kept in **32 KiB on-chip threadgroup memory**; a 4096×4096 complex scene
  in **370 ms on an M1 GPU**, a **22× speedup over the 8.16 s multi-dispatch baseline**; first FFT to use Apple's `simdgroup_matrix` 8×8 hardware MMA;
  all five point targets show **0.0 dB SNR deviation** from the unfused fp32 reference. Also documents a two-tier memory model: **208 KiB registers
  (Tier 1)** and **32 KiB threadgroup memory (Tier 2)**, reaching 138 GFLOPS radix-8 Stockham fp32. `[vendor-independent benchmark]`
  <https://arxiv.org/abs/2604.03585>
* Companion precision study (arXiv:2605.28451): FP16 FFT **56–61 dB SQNR**, **306 GFLOPS fp16 vs 139 GFLOPS fp32** radix-8 at N=4096 on a fanless M1,
  BFP needed to avoid fp16 overflow — numbers quoted in full in §3.1. `[vendor-independent benchmark]` <https://arxiv.org/abs/2605.28451>
* **Cross-platform GPU compute with deadline/jitter reporting on Apple Silicon** — VkVIO (arXiv:2609.30459) runs a visual-inertial-odometry frontend on
  any Vulkan GPU. **On Apple M3 specifically**: frontend speedup **1.4× (TR2) to 6.8× (HF151)**, E2E **1.2–3.9×**; on a 36.5-minute endurance session
  (MIPB08) VkVIO finished in **384 s vs 1257 s** for the CPU baseline at equal accuracy (ATE 0.603 m vs 0.597 m) and **lower power (9.9 W vs 10.3 W)**,
  i.e. **3.4× less energy, 32 mJ vs 110 mJ per frame**; a 50-frame rolling-mean trace shows VkVIO stage times staying *"within a band of about 5 ms"*
  where the CPU baseline swung 20–45 ms. (The tighter jitter figures in that paper — frame-to-frame spread 0.37–1.45 ms, p99 E2E 42.98 → 12.99 ms —
  are measured on the **RTX 3070** comparison machine, not on the M3; do not attribute them to Apple Silicon.) `[vendor-independent benchmark]`
  <https://arxiv.org/abs/2609.30459>
  This is the best published evidence that an Apple GPU dispatch pipeline can be *low-jitter*, and it also shows the honest limit: **sub-millisecond,
  not sub-100-microsecond**.
* **Real-time neural inference inside a Core Audio callback on M3** — DAFx-26 (numbers in §2.2): proves a Core ML-family AOT graph (BNNSGraph) can be
  *real-time safe* and hold **p99 14.3 % of a 2.67 ms deadline under maximum contention with zero xruns**, where a naive C++ backend hit 158.5 % and
  355 xruns. The lesson transfers directly: **AOT-compiled, pre-allocated, single-call-per-buffer graph = low jitter; anything per-op = deadline misses.**
  `[vendor-independent benchmark]` <https://www.dafx.de/paper-archive/2026/papers/DAFx26_paper_16.pdf>

### 4.5 Wireless signal processing actually *on the ANE* — exactly one hit, and I could not read it

* **"End-to-End GNSS Interference Classification Using CNNs with Neural Engine Acceleration"** — IEEE Xplore document **11683190**; also listed in the
  **ION GNSS+ 2026** programme. This is the *only* published work I found that runs a wireless/RF signal-processing CNN on the Apple Neural Engine.
  IEEE Xplore returned **HTTP 202 with an empty body**, and the ION programme PDF was unreachable (TLS hostname mismatch), so **I have no abstract,
  no accuracy and no latency number for it.** `[unverified]`
  <https://ieeexplore.ieee.org/document/11683190> · <https://m.iongnss.org/gnss/upload/GNSS26Program.pdf>
* Related but not ANE: a large literature on GNSS interference/jamming classification with CNNs (e.g. PoliTo's *A Deep Neural Network Approach for
  Classification of GNSS Interference and Jamming*, <https://iris.polito.it/retrieve/handle/11583/2992616/...pdf>) and ultra-lightweight edge
  classifiers (GAC-KAN). None reports Apple-Silicon latency.
* An Apple **patent application** surfaced about devices reporting their AI/ML compute capability to the cellular network (compute budgets, CSI
  prediction, beam management). It is a patent, not a measurement or a product. Not load-bearing; noted for completeness.
  `[unverified]` (search-result snippet only)

### 4.6 Internal (workspace) data points — **not literature**, but the right comparison basis

From `docs/apple_silicon_heterogeneous_gnb_plan_english.md` (2026-08-30, live-chain evidence, 500 pings, 36 PRB, 3 DM-RS symbols):

| Path | Latency | Status |
|---|---|---|
| PUSCH channel estimation, CPU baseline | **63.6 µs** | reference |
| CE `metal_mmse` | **~255 µs median, p99 457 µs**; one-off ~2.6 ms on the first slot | passes E2E, 500/500 pings |
| LDPC `metal_persistent` | **730 µs median (min 288 / p95 1746 / p99 2122)** | passes E2E with CE, crc=OK 100 % |
| Full UL pipeline (CE + LDPC Metal + ZMQ) | **1133 µs median ≈ 13 % over the 1 ms slot** | functional, no hard real-time guarantee |

These are **internal measurements, not published**, and they are our own honest baseline: today a *single* Metal module already costs 2–15× the
OCUDU dApp Class-A budget (100–150 µs), and the whole UL chain overshoots a 1 ms slot.

---

## (5) The real-time inference budget arithmetic for 5G NR

### 5.1 The 3GPP timing frame (first-party, extracted from ETSI-published TS text)

**Numerologies** — 3GPP TS 38.211 V17.5.0, §4.2, Table 4.2-1 "Supported transmission numerologies" `[3GPP]`
<https://www.etsi.org/deliver/etsi_ts/138200_138299/138211/17.05.00_60/ts_138211v170500p.pdf>

| µ | Δf = 2^µ · 15 kHz | Cyclic prefix |
|---|---|---|
| 0 | 15 kHz | Normal |
| 1 | 30 kHz | Normal |
| 2 | 60 kHz | Normal, Extended |
| 3 | 120 kHz | Normal |
| 4 | 240 kHz | Normal |
| 5 | 480 kHz | Normal |
| 6 | 960 kHz | Normal |

**Frame/slot structure** — same document, §4.3.1–4.3.2: a frame is **10 ms**, a subframe is **1 ms**, and `N_symb^slot = 14` OFDM symbols per slot with
`N_slot^subframe,µ = 2^µ` slots per subframe. Therefore:

| µ | SCS | **Slot length** | Slots per 1 ms subframe |
|---|---|---|---|
| 0 | 15 kHz | **1 ms** | 1 |
| 1 | 30 kHz | **0.5 ms** | 2 |
| 2 | 60 kHz | **0.25 ms** | 4 |
| 3 | 120 kHz | **0.125 ms** | 8 |

**OFDM symbol duration** (derived): useful symbol `T_u = 1/Δf` = **66.67 µs at 15 kHz** / 33.33 µs at 30 kHz. With the standard normal-CP split
(4.69 µs for `l ≠ 0`, 5.21 µs for `l = 0` at 15 kHz), the total symbol is **≈ 71.4 µs** (`l ≠ 0`) and **≈ 71.9 µs** (`l = 0`) at 15 kHz — and exactly
half that, **≈ 35.7 µs**, at 30 kHz. This closes correctly: `71.9 + 13 × 71.4 = 999.6 µs ≈ 1 ms`.
*(The CP split is derived arithmetic: my text extraction of the R17 PDF mangled the CP-length table glyphs, so I state the split as derived rather
than quoted.)*

**Latency KPI** — 3GPP TR 38.913 V15.0.0, §7.5 "User plane latency" `[3GPP]`
<https://www.etsi.org/deliver/etsi_tr/138900_138999/138913/15.00.00_60/tr_138913v150000p.pdf>:
* *"For URLLC, the target for user plane latency should be **0.5 ms for UL, and 0.5 ms for DL**."* (average value, without an associated high-reliability requirement)
* *"For eMBB, the target for user plane latency should be **4 ms for UL, and 4 ms for DL**."*

**HARQ timing** — 3GPP TS 38.213 V17.5.0, §9.2.3 `[3GPP]`
<https://www.etsi.org/deliver/etsi_ts/138200_138299/138213/17.05.00_60/ts_138213v170500p.pdf>:
the UE reports HARQ-ACK *"only in a HARQ-ACK codebook that the UE includes in a PUCCH or PUSCH transmission in slot `n + k1`"* where *"`k1` is a number of
slots indicated by the **PDSCH-to-HARQ_feedback timing indicator** field in a corresponding DCI format, or provided by `dl-DataToUL-ACK`
(or `dl-DataToUL-ACK-r16` / `-DCI-1-2` / `-r17`) if the field is not present."*
So **k1 is a slot count, not a millisecond count**: at 30 kHz SCS, `k1 = 4` means the HARQ-ACK opportunity is **2 ms** after the PDSCH slot; at 15 kHz
it is 4 ms. The gNB-side retransmission loop is `k1 + (re)processing`, so a first-order HARQ round trip at 30 kHz with `k1 = 4` is a few slots.

### 5.2 What a per-slot inference deadline actually means

Take the OCUDU dApp contract as the concrete operating point (it is the only *published* real-time AI-RAN contract, §4.1) and the 3GPP numbers above:

| Deadline | Time | What it covers |
|---|---|---|
| Slot @ 30 kHz SCS | **500 µs** | the whole uplink slot arrives; the receive chain must complete inside it |
| OFDM symbol @ 30 kHz | **≈ 35.7 µs** | the granularity at which a symbol-level pipeline stage can be scheduled |
| OCUDU Class A **estimation** budget | **100 µs** | channel estimation (a neural estimator's whole allowance) |
| OCUDU Class A **completion** budget (deeper depths) | **150 µs** | equalization / demapping through to pre-descrambling soft bits |
| OCUDU Class B scheduler admitted deadline | **100 µs** | feature construction → inference → validation → commit |
| OCUDU measured receiver kernels (GPU, live shape) | **82 µs P50 / 112 µs P99.9** | the *conventional* chain's own cost — i.e. the neural block must fit in what is left |
| TR 38.913 URLLC user-plane | **0.5 ms UL + 0.5 ms DL** | the end-to-end target the slot budget ultimately serves |
| HARQ opportunity | `k1` slots (**2 ms** for `k1 = 4` @ 30 kHz) | the soft deadline for a *retransmission*, i.e. the "you may be late once" allowance |

**The budget, stated plainly for our receiver:**

```
per-slot inference budget (30 kHz SCS)        = 500 µs
  minus conventional-chain tail                  ~100–150 µs (measured 82/112 µs at GB10)
  minus framework/host overhead, jitter          must be ≪ 100 µs (OCUDU: 100 µs of host
                                                 jitter already causes fronthaul lateness)
  ⇒ realistic neural-receiver allowance          ~100–150 µs per slot, every slot
  ⇒ at 14 symbols/slot that is ~7–11 µs per symbol of *amortised* work
```

Against the ANE numbers in §2.2/§3.6:

* **One ANE dispatch costs 70 µs (M5-class) to 230 µs (M1-class).** A design that needs *two or more* ANE dispatches per slot is already
  at or over budget on M1-class silicon, and consumes half the budget on M5-class silicon before any arithmetic happens.
* Therefore the only ANE shape that can work is **one fused program per slot** (or per symbol-group), with all weights resident, all shapes fixed
  at compile time, and the output produced directly in the grid→LLR form — precisely the shape ANEForge/ANE-Forge-style direct dispatch and
  Apple's own channels-first/one-einsum guidance push you toward.
* **Jitter, not mean, is the binding constraint.** The DAFx result (isolated RTF 0.211 that became p99 158.5 % of deadline with 355 xruns,
  §2.2/§4.4) is the cautionary tale: an isolated mean of 100 µs tells you almost nothing about slot-miss behaviour. Any Apple-Silicon PHY block must
  be specified with a **p99.9/p99.99** target and a **deterministic conventional fallback** — exactly the OCUDU Class-A contract (per-invocation
  fallback armed on the same stream, 8 consecutive misses open a breaker).
* **HARQ gives you one escape hatch.** Because `k1` is a *slot count* (typically several slots, §5.1), a receiver that is late on the *first*
  transmission can still meet the HARQ retransmission opportunity. But the OCUDU platform is explicit that a late completion is **not replayed**:
  *"the host has already committed to consuming the module's result for that grant, so the result is used, an incident with the observed latency is
  recorded, a counter increments, and eight consecutive misses open the lane's breaker."* [vendor-independent benchmark] <https://arxiv.org/abs/2609.07843>

---

## 6. What is NOT in the literature (gaps)

Ordered roughly by how much they block us.

1. **No published neural receiver / AI-PHY work on Apple Silicon at all.** Zero papers, zero blog posts, zero benchmark tables. Every neural-receiver
   latency number in existence is NVIDIA (A100 ~1 ms TensorRT; GB10 Class-A 82–112 µs kernels). The OCUDU dApp platform — the only open real-time
   AI-RAN runtime — is **CUDA-only** and has **no Metal/MPS/Core ML/ANE backend**.
2. **No ANE latency figure for any wireless/PHY workload.** The ANE's own literature (ANE book, ANEForge, the placement study) benchmarks LLMs,
   ResNet-18, a sentence encoder, a ViT, a Stable-Diffusion U-Net and an FFT demo. The **one** wireless-adjacent ANE paper (GNSS interference
   classification, ION GNSS+ 2026 / IEEE 11683190) is paywalled and I could not extract a single number from it.
3. **No published fp16/quantization accuracy study for LLRs or soft bits.** The fp16-SQNR literature is radar/SAR (56–61 dB FFT SQNR, BFP fix) and
   the quantization literature is image-classification top-1 or LLM argmax. **Nobody has published the LLR-vs-precision curve**, which is the single
   most important accuracy question for a learned receiver that feeds an LDPC decoder.
4. **No published ANE per-operation supported-device matrix from Apple.** The community denylist (Hollance) is explicitly *"incomplete and possibly
   wrong"* and dates from the neuralnetwork-format era; the ANE book has an operation-by-device matrix but it is reverse-engineered. Apple publishes
   none. There is also **no Apple-published way to ask at runtime which device executed a call.**
5. **No measured Core ML batch-prediction latency-scaling curve on M-series.** Apple's guidance is qualitative ("increase the batch size to escape
   the bandwidth-bound regime"); I found no first-party or third-party table of latency vs batch for `mlprogram` transformer models on Mac.
6. **No Apple-silicon ANE-vs-Metal end-to-end jitter study for a fixed-deadline loop** other than the audio one (DAFx-26, BNNSGraph on M3, 2.67 ms
   deadline). Nothing at the 100 µs–1 ms scale, nothing with p99.99, nothing over hours.
7. **No published Metal implementation of any 5G-NR PHY kernel** (FFT, channel estimation, MIMO detection, LDPC) with latency numbers. The closest
   is our own unpublished internal work and the SAR/FFT papers.
8. **No GNU Radio / SDRangel / SoapySDR throughput benchmark on Apple Silicon.** The public record is bug reports (choppy audio, slow GUI) and one
   functional srsRAN-4G-on-macOS-ARM64 port report with no timing data.
9. **No MLPerf Client or MLPerf Mobile result for Apple Silicon that I could verify** (site gated), and the DAFx authors independently state Apple has
   not submitted BNNSGraph to MLPerf. So there is **no vendor-independent, apples-to-apples Apple-Silicon inference league table**.
10. **No Apple-published real-time guidance for the ANE.** WWDC24 10211 (*Support real-time ML inference on the CPU*) covers the CPU/Accelerate path.
    There is no equivalent "real-time on the ANE" material, and no Apple statement about ANE dispatch floor, jitter, or deadline behaviour.
11. **No published work combining a Core ML/ANE model with a custom Metal kernel in one pipeline** with measured switch cost. Apple documents that
    custom layers are CPU/GPU-only; nobody has published what the *engine-switch* costs in a slot loop.
12. **No published over-the-air 5G gNB (srsRAN/OCUDU) running on macOS Apple Silicon** — the public port report is 4G and loopback.

---

## 7. Uncertainties

**About the sources**
1. **"Core AI" is unverified against Apple.** Multiple mid/late-2026 community sources describe it as Core ML's successor (iOS/macOS 27, `.aimodel`,
   `coreai-torch`, `coreai-build`). I could not reach any Apple page confirming it. If it is real, everything in this survey about "Core ML" may be
   one generation stale — and the ANE-vs-GPU trade-off may have shifted (community numbers suggest Core AI's compute unit is fixed **by the export
   shape**, not by a runtime flag: static export → ANE, dynamic export → GPU).
2. **WWDC contents are unverified** (HTTP 401). All claims I attribute to WWDC are title-level only.
3. **MLPerf is unverified** (Cloudflare). I cannot rule out that Apple-Silicon numbers exist there.
4. **The two most quantitative community repos** (`john-rocky/*`, `anemll/*`) are single-maintainer projects with self-reported methodology. Their
   numbers are internally consistent and methodologically disclosed, but they are **not** peer-reviewed and cross-session comparisons are explicitly
   warned against by the authors themselves (*"device state moves between sessions… a ratio between two cells of one row is not a measurement"*).
5. **The GNSS/ANE paper is `[unverified]`** — I have title + venue + IEEE ID only. It might contain exactly the number we want.
6. **ANE book numbers are reverse-engineered.** The author labels each claim measured / decompile-derived / predicted; the M1 and M5 are measured,
   **M3 and M4 are decompile-derived and not individually measured**. My §5.2 budget arithmetic leans on the M1 0.23 ms and M5 Pro 0.07 ms floors —
   the Mac generations we are most likely to deploy on (M3/M4) are interpolated, not measured.
7. **The 3GPP CP split is derived, not quoted** (§5.1) — my PDF text extraction mangled the CP-length table. The numerology table, frame/subframe
   durations and 14 symbols/slot are first-party quoted.

**About the engineering conclusions**
8. **Nothing here establishes that an ANE-resident neural receiver can meet a 500 µs slot.** What the evidence supports is narrower: the ANE's
   *fixed* per-dispatch cost (70–230 µs) and its ~2–4.7 MB working-set cliff make it viable only for a single fused, fixed-shape, bandwidth-bound
   program per slot — and no one has built or measured one for a PHY workload.
9. **fp16 range vs LLR dynamic range is an open risk I could not quantify.** The radar analogy (needs BFP, needs a 1/N scale) is suggestive but the
   LLR head's actual dynamic range depends on our own training. Unknown until measured.
10. **Core ML's async/batch semantics for a slot loop are unverified.** I could not find a first-party statement on whether two predictions may be
    in flight concurrently on the ANE; the community evidence (serial prediction queue, IOSurface ping-pong) suggests **not safely**.
11. **The macOS/iOS op-set divergence** (a model that loads on macOS failing to build on iOS) is a single unreproduced report. If real and general,
    it matters for any iOS-side development or for cross-platform CI.
12. **Our own Metal baseline may not be representative of an optimized end state.** The internal numbers (CE 255 µs, LDPC 730 µs, pipeline 1133 µs)
    are mid-migration skeleton ports; the plan document itself says a module is *"allowed to be more than 10× slower than the CPU path"* during
    migration. So the gap between "today's Metal" and "a real-time Metal neural receiver" is not yet bounded by measurement.
13. **Whether an Apple-Silicon Class-A equivalent is even architecturally admissible** in an upstreamed OCUDU dApp platform is unknown: the platform
    declares "CPU (x86, ARM) or CUDA" backends. Whether Metal/MPS would be accepted as a third backend, or whether the FAPI/executor integration
    points permit it, is an upstream/design question, not a literature one.

---

## 8. Compact bottom line

* **The ANE is reachable on macOS only through Core ML**, never *required*, never *reported*, and never *extensible* with our own kernels. Its fixed
  per-dispatch cost is **70 µs (M5-class) to 230 µs (M1-class)** — a fatal fraction of a 500 µs slot unless the whole per-slot computation is one
  fused program.
* **Core ML's own recommended architecture is a good match for PHY if you accept its constraints**: fixed/enumerated shapes, channels-first 4-D,
  one einsum for attention, per-head chunking, everything pre-allocated, one call per slot, AOT-compiled and warm. Apple's distilbert result
  (**3.47 ms at 0.454 W**, 10× faster / 14× smaller after ANE-shaping) is the proof that this works well *when it works*.
* **The accuracy risk is exponent range, not mantissa.** fp16 FFT gives 56–61 dB SQNR (fine); fp16 *pipelines* NaN out at 5×10⁶ (fatal). LLRs are the
  most range-hostile quantity in a receiver, and nobody has published the curve.
* **The performance risk is jitter, not mean.** The published real-time-on-Apple-Silicon study shows an isolated-RTF-0.211 model hitting 158.5 % of
  its deadline with 355 xruns under a real callback. Plan for p99.9 and keep the conventional receiver armed as a per-slot fallback — which is
  exactly what the OCUDU dApp Class-A contract already specifies.
* **Metal/MPS is the pragmatic engine** if we accept losing the ANE: 255 µs CE / 730 µs LDPC today, ~1000 dispatches per token in the LLM analogue,
  and the OCUDU Class-A budget is 100–150 µs. The published SAR work shows a single-dispatch fused Metal pipeline is worth **22×** over multi-dispatch —
  so **fusion, not per-op porting, is where the wins are.**
