# Apple Silicon Heterogeneous gNB Long-Term Plan (Metal GPU / Performance Cores / NPU Cooperation)

> Status: kicked off 2026-08-30. This document records the high-level OCUDU roadmap on Apple
> Silicon and is the common reference for the Metal porting work of the individual PHY modules
> (FFT / Channel Estimation / Equalization / MIMO Detection / LDPC ...). The implementation-level
> records of each module live in its own `metal/PLAN.md`
> (CE: `lib/phy/upper/signal_processors/channel_estimator/metal/Metal_MMSE_Channel_Estimator_PLAN.md`,
> LDPC: `lib/phy/upper/channel_coding/ldpc/metal/PLAN.md`).
>
> Chinese original: `doc_chinese/apple_silicon_heterogeneous_gnb_plan.md` (kept out of the git tree).

## 0. The two meanings of "heterogeneous"

The **heterogeneous** in this plan is doubly heterogeneous, and it maps exactly onto the
heterogeneous requirements of V2X:

| Layer | Heterogeneity axis | Composition | Advantage |
|---|---|---|---|
| **Compute heterogeneity** | Compute resources | P performance cores / E efficiency cores / **GPU** / **NPU** | Latency-critical work stays on CPU cores, high concurrency goes to the GPU, AI inference goes to the NPU |
| **Storage heterogeneity** | Where data lives | Registers/caches / **unified memory** (RAM) / **flash** (mmap page cache) | Hot data resides zero-copy in unified memory, cold data (e.g. GB-scale matrices/model weights) stays in flash and is paged in on demand |

**A natural match to heterogeneous requirements:**

- **V2X control signalling**: extremely low latency, extremely high reliability, small packets -
  runs on the P/E cores (CPU path, a microsecond-scale chain) plus hot caches (everything needed
  for full readiness resident in memory; supplies first).
- **V2X camera data**: extremely high bandwidth, huge data volume, comparatively relaxed latency -
  runs on the GPU (high-concurrency throughput) with large objects tiered across unified
  memory/flash (elastic page-cache residency, the mmap scheme of §2.2).

Two layers of heterogeneity - compute and storage - cooperating inside a single gNB is a
structural answer to heterogeneous requirements, and it is OCUDU's core competitive advantage on
Apple Silicon relative to pure x86 solutions.

## 1. Positioning: the core advantage and the applicable scenarios of the GPU/Metal path

- **Metal's advantage is high concurrency, not single-chain low latency.** The Apple GPU hides
  latency with thousands of threads; its value lies in **many users, wide bandwidth, high
  throughput** scenarios, not in one small-packet chain of one UE.
- Therefore **a module's Metal implementation is allowed to be more than 10x slower than the CPU
  path** (module-level comparison basis, e.g. today's CE metal ~200 us vs cpu ~64 us, LDPC metal
  ~550-981 us vs cpu ~10-70 us). This is the expected state during the algorithm-skeleton
  migration, not a defect.
- There is one hard constraint though: **the latency of the E2E chain must fit the slot budget**
  (15 kHz = 1 ms, 30 kHz = 0.5 ms) so that the E2E test itself can run and attach/data-plane
  behaviour can be verified.

## 2. Combination strategy for E2E testing (budget control)

- An E2E chain may **enable the Metal path of only one module while the others stay on the CPU
  path**, controlled independently with switches such as
  `expert_phy --pusch_channel_estimator_algo metal_mmse` and
  `expert_phy --pusch_ldpc_decoder_type metal`, keeping the total latency inside the budget so
  functional verification can complete (current practice: CE-only Metal passes end-to-end;
  CE+LDPC dual Metal also passes end-to-end, see §5).
- **Enabling all Metal modules at once is a better but non-mandatory goal** - it requires every
  module's in-slot latency to converge (today the dispatch-chain overhead of the LDPC layered
  variant is the main gap; the `metal_persistent` single-dispatch variant is the ready-made
  mitigation, see LDPC PLAN §4.16/§4.17).

### 2.1 Iron rule: supplies before the troops (initialization must never sit on the packet path)

The measurement basis only covers PUSCH with crc=OK, which means a lazy design where "engine
initialization happens after the packet arrives" is a **logic error**: the gNB must reach maximum
readiness before the first data packet arrives, and run at full speed the moment a packet lands.

- **Every resource that can be prepared in advance (engines, pipelines, matrices, buffers, kernel
  JIT, slot construction) must be built at start-up/construction time**; only genuinely
  data-dependent computation may appear on the packet-processing path.
- **The cost must be traded off**: pre-building mainly costs memory and start-up time. Apple
  Silicon unified memory is a strength, but quantities still need weighing - the principle is
  "pre-build everything that is cheap to pre-build; keep the expensive ones lazy and state that
  explicitly in the documentation".
- **Current state (2026-08-30)**:
  - LDPC layered/persistent: CSR-only representation (~11 MB shared matrix + ~37 MB/instance
    engine buffers) -> **all 102 (BG, z) slots are built once at construction time** (see LDPC
    PLAN §4.19);
  - LDPC flooding/LLS/async: require packing H/H^T (the 0/1 pattern is packed into uint32 bits;
    the full H+H^T for both BGs is ~**1.4 GB**, beyond the RAM pre-build budget) -> stay lazily
    built, with only the family JIT warmed up at construction time;
  - CE: the engine and its warm-up have been done at construction time since A+B (10 instances,
    at start-up), with no lazy state left; the residual ~3 ms is the per-thread driver
    initialization of the first Metal call on an executor thread (one-off, on the first attach
    slot), and the mitigation (running one dummy submission on the UL executor thread at
    start-up) is on the to-do list.

### 2.2 Trade-off assessment of offline precomputation + flash mapping (flooding/LLS/async large matrices)

Alternative for GB-scale matrices: compute them offline, store them in flash, and `mmap` them
directly into the address space at run time (macOS file mapping: data is paged in from flash via
the page cache on demand, with no explicit read and no resident copy).

| Option | CPU computation | Memory | First-use latency | Extra cost |
|---|---|---|---|---|
| Runtime CPU build (current) | z=352 ~6.8 ms, small z ~us | 0 (resident after use) | 0 | none |
| Offline precompute + mmap | **0** | elastic (page cache, only touched pages) | ~5-20 ms per size (NVMe 3-6 GB/s on-demand paging) | file management/version consistency; flash footprint |

Quantified conclusion: **the first-use latency is of the same order for both** (mmap paging
~5-20 ms vs CPU build 0.01-6.8 ms). The value of mmap is "zero computation while the CPU is busy
or in the real-time slot", plus elastic page-cache residency; the cost is file-management
complexity. The offline file size is manageable (full H/H^T packing for both BGs is ~1.4 GB).
Current decision: the layered family uses full RAM pre-building (already landed, CSR ~11 MB is
cheap); flooding/LLS/async keep the lazy CPU build (they are experimental modes, not on the
default path); if they later become default and the CPU budget is tight, implement the offline
file + mmap scheme then.

## 3. Endgame architecture: one dispatch for the whole UL chain, fire and do not wait

Once every UL-chain module (**FFT, CE, MIMO Detection, LDPC**) is Metal ready:

- The CPU side **orchestrates the kernel sequence of the whole chain into one command buffer and
  dispatches it once**; data travels through the GPU stage by stage in zero-copy buffers
  (`newBufferWithBytesNoCopy` shared memory, no PCIe copies, no intermediate read-back);
- **Fire and do not wait for the data to come back**: the CPU executor thread returns immediately
  after submission to serve other slots/tasks, and the GPU wakes the downstream work through a
  callback when it finishes;
- Per-module development therefore evolves from "synchronous waitUntilCompleted per module" to
  "chained kernel orchestration" - the module-level GPU latencies are replaced by the
  **pipeline parallelism of a single dispatch** (today's per-module commit+wait round trip of
  ~20-30 us x N modules disappears with it).

### 3.1 Follow-up once Metal LDPC is done: hand the MAC PDU to FAPI without waiting

- Once Metal LDPC has run on the GPU (crc=OK -> the correct MAC PDU already sits in GPU
  memory/shared memory), **the CPU thread that dispatched it no longer calls
  `waitUntilCompleted`**: attach the decode-completion event back into OCUDU's executor task
  graph with `addCompletedHandler` (or move `waitUntilCompleted` to the last moment before FAPI
  consumption);
- Mechanisms to plan (combining Apple Silicon with the OCUDU scheduler):
  1. **Callback -> executor task wake-up**: a Metal command-buffer completion callback runs on a
     GPU driver thread and should only do lightweight work (set a flag/enqueue); the OCUDU
     executor worker threads then run the (de)segmentation -> MAC PDU -> FAPI path, so that the
     protocol stack never runs on the driver thread;
  2. **Priorities and back-pressure**: command buffers of many UEs/slots share the GPU and
     submission order equals execution order; queues must be partitioned or tagged by UE latency
     class (see §4) so that large-packet GPU jobs cannot starve control signalling;
  3. **Cache-coherence window**: there is a cache-line invalidation cost between the GPU finishing
     a write and the CPU reading the MAC PDU; read-back points should be concentrated (read the
     whole PDU area in one go) instead of ping-ponging byte by byte.

## 4. Overall gNB operating architecture of the heterogeneous cooperation (market positioning)

Operating shape of a gNB based on Apple Silicon (P performance cores / E efficiency cores / GPU / NPU):

| Traffic type | Characteristics | Carrier | Rationale |
|---|---|---|---|
| **V2X control signalling / small packets** | Extremely latency-sensitive (ms-scale end to end) | **P cores/E cores (CPU path)** | The existing C++ chain has the lowest latency (CE ~64 us, LDPC ~10-70 us) and adds no GPU round trip |
| **V2X camera video / large packets** | Wide bandwidth, high throughput, comparatively relaxed latency | **GPU/Metal** | High-concurrency throughput; an **NPU** can later be layered on top to host AI-ified receivers (neural-network inference for channel estimation/detection) |

Inside one gNB, **CPU and GPU each do their job in heterogeneous cooperation**: the low-latency
control plane runs on performance cores, the high-throughput data plane runs on the GPU (plus
NPU for AI PHY in the future), creating a structural difference in cost/power/performance versus
pure x86 solutions - this is OCUDU's core competitiveness route on Apple Silicon.

## 5. Current status and next steps (2026-08-30 live-chain evidence, 500-ping update)

- **CE metal_mmse**: passes end-to-end; **all 500 pings pass** (2013 samples). Steady state
  median ~255 us (3 DM-RS symbols, 36 PRB), p99 457 us; one-off ~2.6 ms on the first slot
  (known, per-thread driver initialization, once at attach). CPU baseline 63.6 us.
- **LDPC metal_persistent**: passes end-to-end together with CE (500 pings, crc=OK 100%). Median
  **730 us** (min 288 / p95 1746 / p99 2122): at the 2-5 dB live-chain operating point,
  iter==max_iter (2-4 rounds using the whole budget, crc=OK) is normal convergence behaviour;
  each round is ~300-400 us = 46 x 1024-thread device-level barriers plus layer computation;
  **supplies-before-troops has landed** (102 slots built at start-up in ~12 ms, zero lazy
  construction at run time, 171x speed-up on the first decoded packet).
- **Pipeline** median 1133 us (~13% over the 1 ms slot length): ZMQ has no hard real-time
  guarantee and functionality is met; the budget-reduction directions are the per-round LDPC
  barrier chain (multi-TG + device-fence persistent / small z on the layered path).
- **Mid term**: CE engine singleton + cross-port batching (CE PLAN (D)); mitigation of the
  first-slot per-thread tax (dummy submission from the executor thread at start-up); replace the
  synchronous wait of each module with callback attachment (the mechanism of §3.1).
- **Next task**: **AI based channel estimation** (HELENA/MPSGraph, see
  `AI_channel_estimation_implementation_plan.md` - the L0 baseline (metal_mmse) is ready, so this
  goes straight to the G1 latency-prototype gate).
- **Long term**: Metal porting of FFT / Equalization / MIMO Detection (reusing the engine
  paradigm and lessons of CE/LDPC: occupancy first, construction-time warm-up, authoritative
  metallib paths, ocudulog diagnostics, accumulation order kept bit-exact, supplies before
  troops - CE PLAN §7.0.15); ultimately assembling the single-dispatch full-chain orchestration
  of §3 and the heterogeneous traffic split of §4.
