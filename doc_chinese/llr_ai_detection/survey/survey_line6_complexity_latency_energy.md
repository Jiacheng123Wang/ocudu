# LINE 6 — Complexity, Latency and Energy of Transformer/Attention-Based Wireless Receivers on Real Hardware

**Survey date:** 2026-10-07
**Scope:** inference cost (params / FLOPs / O(N²) attention / latency vs. 5G NR slot timing), FPGA/ASIC/GPU/DSP implementations, small & edge transformer / linear-attention / Mamba receivers, SDR & over-the-air testbeds, and the measured-on-hardware vs. simulated-only distinction.

### Methodology / confidence note (read first)

`web_fetch` was effectively unusable in this environment (nearly every host resolved to a non-public IP). Two exceptions worked and were used heavily:

- `https://papers.cool/arxiv/<id>` — mirrors arXiv **abstracts** and rendered the full abstract text. Most entries below are grounded in that fetched abstract text.
- `nvlabs.github.io` (Sionna docs) — fetched successfully but returned only navigation chrome, no substantive numbers.

Everything else is grounded in `web_search` results. Where a number appeared only in a **search-result snippet** rather than in a fully fetched document, it is marked `[snippet-only]`. Where I could not confirm a number at all, it is listed in the **uncertainties** section at the end and is *not* asserted in the body. Nothing below is fabricated; recalled-but-unverified items are flagged explicitly.

**Tag legend:** `[measured on hardware]` = the paper's own numbers come from a real device (GPU/FPGA/ASIC/SDR). `[simulated/estimated only]` = numbers are from simulation, FLOP/bit-op counting, or an analytic cost model. `[mixed]` = both.

**5G NR timing reference (for the deadline discussion):** NR slot = 1 ms for 15 kHz SCS (numerology µ=0), 0.5 ms for 30 kHz SCS (µ=1); NR transmission-time-interval deadlines are conventionally quoted as the 0.5 ms slot. Several papers below use "0.5 ms slot" as their budget. *(Standard numerology, 3GPP TS 38.211 — background knowledge, not verified against the spec text in this session.)*

---

## (1) Reported inference cost of Transformer-based receivers/detectors

**Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR** — arXiv 2409.02912 / IEEE (Wiesmayr, Cammerer, Aït Aoudia, Hoydis, Zakrzewski, Keller) — [link](https://arxiv.org/abs/2409.02912)
The canonical "does it actually fit in a slot?" paper. Deploys an MU-MIMO neural receiver (NRX) into a real 5G NR system with adaptive MCS support requiring **no re-training and no additional inference cost**. The architecture is latency-optimized to achieve **inference times < 1 ms on an NVIDIA A100 GPU using TensorRT**. The latency constraint "effectively limits the size of the NN," and the authors quantify the resulting **SNR degradation as < 0.7 dB** versus a non-real-time NRX. TensorRT experiment source code is released. **Tag: [measured on hardware]** (A100 + TensorRT; the <0.7 dB figure is simulation over a ray-tracing channel model). *Note: the abstract does not give a parameter count or FLOP count — see uncertainties.*

**Sionna Research Kit: A GPU-Accelerated Research Platform for AI-RAN** — arXiv 2505.15848 / IEEE (Cammerer, Marcus, Zirr, Aït Aoudia, Maggi, Hoydis, Keller) — [link](https://arxiv.org/abs/2505.15848)
GPU-accelerated research platform **powered by the NVIDIA Jetson AGX Orin**, built on OpenAirInterface (OAI), delivering "high throughput and real-time signal processing" from a software-defined stack. Demonstrates a **real-time neural receiver — trained with Sionna and using TensorRT for inference — in a 5G NR cellular network with commercial user equipment**. This is the clearest published example of a neural receiver closed-loop with real UEs on an embedded GPU. **Tag: [measured on hardware]** (Jetson AGX Orin + live commercial UE). The abstract gives no per-slot latency figure.

**A Neural Receiver for 5G NR Multi-user MIMO** — arXiv 2312.02601 (Wiesmayr et al.) — [link](https://arxiv.org/abs/2312.02601)
Predecessor work to 2409.02912 by the same NVIDIA/NTU group, establishing the MU-MIMO NRX formulation that the real-time paper then compresses to fit the latency budget. Found via search; **abstract not fetched** — treat architecture/complexity details as unverified here.

**Real-Time dApps for AI-RAN: Measured Interface Requirements for Inline PHY and Slot-Level Control** — arXiv 2609.07805 (O'Shea, Pennybacker, Kharchenko) — [link](https://arxiv.org/abs/2609.07805)
Directly addresses whether a neural receiver can finish inside a slot. Key argument: the conventional dApp boundary (external process, indication in → control message out) "cannot express a neural receiver that must finish inside a slot." An audited corpus of **39 runtime AI-RAN use cases, sized by 5G NR timing, shows more than half cannot cross the observer boundary** — inline PHY work because an indication has no return path into the same slot, bounded control because of tail latency under load. Measured on a quiet host *and* with a live cell on the air: **every carrier meets a 100 µs control deadline on an idle host, and only the in-process paths still do once the DU is running.** Four dApps of all three classes validated together on one over-the-air cell. **Tag: [measured on hardware]**. `[snippet-only]` A-03 (neural receiver → LLRs) is characterised as **1.47 MB in, up to 4.4 MB out per slot, with ≤500 µs occupancy** — this per-slot bandwidth/occupancy figure came from a search snippet of the HTML version, not from the fetched abstract, so treat the exact numbers as provisional.

**The OCUDU dApp Platform: An Open Runtime and E3 Interface for Real-Time AI-RAN** — arXiv 2609.07843 (O'Shea, Pennybacker, Kharchenko) — [link](https://arxiv.org/abs/2609.07843)
Defines three timing contracts for AI-RAN applications inside a production DU: **Class A resident on the GPU receive chain; Class B inside the scheduler's 100 µs admitted deadline; Class C never-blocking observers.** ML's largest gains are located in "the band below 10 ms inside a 3GPP NR 5G DU" (link adaptation, per-slot scheduling, channel estimation, the receiver itself). On a **GB10 gNB with attached handsets**, dApps of all three classes — **including an out-of-tree neural equalizer** — ran together on a live cell **without a single fallback**, and equalizer variants were compared over the air purely by lifecycle operations. Platform/SDK are public under BSD-3-Clause-Clear. `[snippet-only]` Class B direct call is reported at **0.29 µs P99.9 quiet; 5.2 µs with the cell on air** (from a snippet of the HTML version). **Tag: [measured on hardware]**.

**Computationally Efficient Neural Receivers via Axial Self-Attention** — arXiv 2510.12941 (Yellapragada, Kocharlakota, Costa, Ollila, Vorobyov) — [link](https://arxiv.org/abs/2510.12941)
The most explicit O(N²)→structured-complexity treatment in this survey. Factorizing attention along temporal and spectral axes **reduces the quadratic complexity of conventional multi-head self-attention from O((TF)²) to O(T²F + TF²)**, yielding substantially fewer total FLOPs and attention-matrix multiplications per transformer block than global self-attention. Relative to convolutional neural-receiver baselines it achieves **significantly lower computational cost with a fraction of the parameters**. Validated under 3GPP CDL channels; under CDL-C NLOS it beats global self-attention, CNN receivers and LS-LMMSE at 10% BLER, and at 1% BLER it keeps working at high user speed where LS-LMMSE fails to converge. **Tag: [simulated/estimated only]** for the complexity/latency claims (FLOP counting + BLER simulation); the abstract claims suitability for "resource-constrained edge environments" but reports no device measurement.

**HELENA: High-Efficiency Learning-based Channel Estimation using dual Neural Attention** — arXiv 2506.13408 / IEEE (Camelo Botero, Aycan Beyazit, Slamnik-Kriještorac, Marquez-Barja) — [link](https://arxiv.org/abs/2506.13408)
Concrete latency-vs-accuracy trade against a ViT baseline (CEViT): HELENA **reduces inference time by 45.0% (0.175 ms vs. 0.318 ms)**, achieves comparable accuracy (**−16.78 dB vs. −17.30 dB** NMSE), and needs **8× fewer parameters (0.11 M vs. 0.88 M)**. A lightweight convolutional backbone + patch-wise multi-head self-attention + squeeze-and-excitation. **Tag: [measured on hardware]** for the latency (inference-time measurement), against a **0.5 ms slot budget**. Follow-up work below pins the GPU model.

**HELENA for 5G NR LEO NTN Channel Estimation: A Comparative Evaluation** — arXiv 2609.14735 (Camelo Botero, Slamnik-Kriještorac, Marquez-Barja) — [link](https://arxiv.org/abs/2609.14735)
The single best **measured latency + energy + edge-feasibility** data point in this survey. Retrains HELENA unchanged for LEO NTN (paired receiver-compensated "NTN-1" and residual-impaired "NTN-2" datasets) and compares against eight terrestrial-origin models plus NTN-specific MDELAN-SISO. HELENA gets the **lowest SNR-averaged NMSE among DL estimators in both conditions, 55.8–62.7% lower linear-scale NMSE than MDELAN-SISO**. On an **RTX PRO 4500: 0.0595 ms 99th-percentile (P99) inference latency, 88.1% below the 0.5 ms budget, with lower energy than its closest attention-based competitors.** On a **10 W Jetson Orin NX it retains a favorable accuracy–energy trade-off, but no model meets the P99 budget** — i.e. embedded tail latency is the open problem, not mean latency. **Tag: [measured on hardware]** (RTX PRO 4500 and Jetson Orin NX).

**A Unified Transformer Architecture for Low-Latency and Scalable Wireless Signal Processing** — arXiv 2508.17960 (Kawai, Koodli) — [link](https://arxiv.org/abs/2508.17960)
A single compact attention-driven model integrating channel estimation, interpolation and demapping, adapted to different output formats by **modifying only the final projection layer**. Evaluated on three use cases: an end-to-end receiver (pilots → bit decisions), channel frequency interpolation (**implemented and tested within a 3GPP-compliant OAI + Aerial system**), and full-band channel estimation from sparse pilots. Claims to satisfy "latency constraints imposed by practical systems" and to beat classical baselines on accuracy, robustness and computational efficiency. **Tag: [mixed]** — the OAI+Aerial integration implies a real system, but the fetched abstract gives no latency/parameter numbers. `[snippet-only]` An IEEE table associated with this line of work lists receiver configs with **Params (K), FLOPs and Latency (ms)** columns — the exact values were not retrievable.

**Novel Deep Neural OFDM Receiver Architectures for LLR Estimation** — arXiv 2503.20500 (Karakoca, Çevik, Hökelek, Görçin) — [link](https://arxiv.org/abs/2503.20500)
Proposes **Dual Attention Transformer (DAT)** and **Residual Dual Non-Local Attention Network (RDNLA)**, predicting LLRs directly from IQ and jointly doing channel estimation + equalization. Both outperform traditional systems and existing neural receivers on BER/BLER. **Tag: [simulated/estimated only]** — no hardware, latency or parameter numbers in the abstract.

**A DNN-based MIMO signal detector using transformer architecture for next-generation wireless networks** — Elsevier (ScienceDirect S2949715925000551) — [link](https://www.sciencedirect.com/science/article/pii/S2949715925000551)
Transformer MIMO detector benchmarked against a VAE-DNN detector with "basic inference-time optimizations" applied to the baseline; complexity comparison is done at 64-QAM. `[snippet-only]` A snippet states the approach is "fully parallelizable, leveraging the self-attention mechanism." Publisher page returned HTTP 403; **numbers not verified.**

**Large AI Models for Wireless Physical Layer** — arXiv 2508.02314 — [link](https://arxiv.org/abs/2508.02314) — and **Large Wireless Foundation Models: Stronger over Bigger** — arXiv 2601.10963 — [link](https://arxiv.org/abs/2601.10963)
Survey/position papers useful as *negative evidence*: the LWFM paper argues directly that "wireless-system constraints hinder a direct transfer of LLM-style success to the wireless domain" and that paradigm choice trades parameter count against inference overhead. Both frame latency/energy as first-class constraints on large PHY models. **Tag: [simulated/estimated only]** / conceptual.

---

## (2) Hardware implementations (FPGA, ASIC, DSP/GPU)

### FPGA

**Efficient Implementation of an Adaptive Transformer Accelerator for Massive MIMO Outdoor Localization** — arXiv 2605.13507 (Yaman, Cheng, Edfors, Liu) — [link](https://arxiv.org/abs/2605.13507)
A genuine **transformer accelerator on FPGA** for a 5G massive-MIMO task. Exploits beam-delay sparsity with a **row-wise skipping mechanism removing low-energy beam components**, mixed input-/output-stationary dataflow on a heterogeneous vector engine with parallel PEs and adder trees, plus a lightweight **single-layer-perceptron router** for runtime model switching. Implemented on **Xilinx Zynq UltraScale+ and evaluated on real-world massive MIMO measurements**: up to **65% row sparsity**, **~2× peak computational speedup**, **inference latency 0.51–2.11 ms**, **throughput up to 1961 positions/s**, below 1.15 m accuracy, with **<10% average accuracy degradation** vs. the floating-point baseline. Targets **sub-10 ms real-time positioning**. **Tag: [measured on hardware]**. *(Caveat: this is localization, not symbol detection — but it is the closest thing to a measured transformer-on-FPGA in the 5G PHY neighbourhood, and it is the only transformer FPGA paper here with a concrete latency number.)*

**SwiftChannel: Algorithm-Hardware Co-Design for Deep Learning-Based 5G Channel Estimation** — arXiv 2605.01931 / IEEE TMC (Lyu, She, Duan, Ni, Chan, Luo, Cheung, Xu) — [link](https://arxiv.org/abs/2605.01931)
Attention-enhanced CNN (a **parameter-free attention mechanism**) reconstructing full-resolution spatial-frequency channel matrices from low-resolution LS estimates, compressed via **knowledge distillation + convolution re-parameterization + quantization-aware training**. On a **Zynq UltraScale+ RFSoC** with an HLS-designed fine-grained pipeline: **sub-millisecond latency, up to 24× speed-up and >33× better energy efficiency than GPU-based solutions.** **Tag: [measured on hardware]**. *(Note: attention, not a full transformer — the attention blocks are parameter-free.)*

**Low Complexity Deep Learning Augmented Wireless Channel Estimation for Pilot-Based OFDM on Zynq System on Chip** — arXiv 2403.01098 (Sharma, Haq, Darak) — [link](https://arxiv.org/abs/2403.01098)
The usefully *negative* FPGA data point: implementing existing SOTA DL channel estimators on Zynq SoC (ARM + FPGA) via HW/SW co-design and fixed-point analysis revealed their **"high complexity, execution time, and power consumption."** The proposed LSiDNN (LS-augmented interpolated DNN) instead offers **88–90% lower execution time and 38–85% lower resource utilization** than SOTA DL-based CE at identical MSE/BER, and **75% lower execution time and 90–94% lower resource utilization than LMMSE**. **Tag: [measured on hardware]** (Zynq SoC).

**An End-to-End Neural Network Transceiver Design for OFDM System with FPGA-Accelerated Implementation** — arXiv 2512.13263 (Luo, Xiang, Luo, Yang, Zhong, Chen) — [link](https://arxiv.org/abs/2512.13263)
DFT-Net + Demod-Net jointly replace IDFT/DFT and demodulation, trained end-to-end on BER. A customized **DFT-Demodulation Net Accelerator (DDNA)** maps them to FPGA with fine-grained pipelining and block matrix operations: **~1.5 dB BER gain over conventional OFDM and up to 66% lower execution time** with only a modest increase in hardware resource usage. **Tag: [measured on hardware]** (FPGA). *(Not a transformer — included because it is the clearest recent "E2E NN transceiver on FPGA with a quantified execution-time win" datapoint.)*

**A Heterogeneous Neural Network Accelerator for End-to-End Multitask RF Signal Recognition** — arXiv 2607.24669 (Song, Stratigopoulos, Aboushady) — [link](https://arxiv.org/abs/2607.24669)
Attention-enhanced CNN + a learnable streaming decimator, with a dual-pipeline fused convolution-pooling engine and DMA-based streaming to minimise memory traffic and latency. **Sustains 98 µs end-to-end inference latency per frame** at ≥99% AMR accuracy (RadioML2018, >4 dB SNR), 90% on HT-CC, 99.5% on GNSS jamming. **Tag: [measured on hardware]** (accelerator). Another attention-based PHY-adjacent accelerator with a measured µs-scale latency.

**RFSoC Modulation Classification With Streaming CNN: Data Set Generation & Quantized-Aware Training** — IEEE Open Journal of the Communications Society, 2024 (MacLellan et al.) — [link](https://ieeexplore.ieee.org/document/10772713)
Streaming-CNN FPGA architecture running directly on RFSoC data converters, with **three quantisation configurations evaluated: 16-bit weights/16-bit activations, 8-bit weights/…**. `[snippet-only]` The implementation targeted a **100 MHz clock** in Vivado; a related per-layer resource table lists e.g. `dense1` with 6921/13372 (k-LUT/FF-like) values and 72 DSPs. **Tag: [measured on hardware]** (RFSoC). *(Retrieval of the full text failed — the EXACT latency and the 8-bit accuracy penalty are unverified; see uncertainties.)*

**LAMANet: A Real-Time, Machine Learning-Enhanced Approximate Message Passing Detector for Massive MIMO** — IEEE TVLSI (Research Explorer, Edinburgh) — [link](https://www.research.ed.ac.uk/en/publications/lamanet-a-real-time-machine-learning-enhanced-approximate-message/)
Real-time ML-enhanced AMP detector on FPGA. `[snippet-only]` A snippet from the accepted version states the design causes **"more than a doubling in required DSP resources and a more than tenfold increase in BRAM resources"** relative to the baseline detector. **Tag: [measured on hardware]** (FPGA). *(No latency/power figure retrieved — see uncertainties.)*

**LwAMP-Net: A Lightweight Network-Based AMP Detector on FPGA for Massive MIMO** — MDPI Electronics 15(7):1494 — [link](https://www.mdpi.com/2079-9292/15/7/1494)
Lightweight AMP detector aimed at FPGA. `[snippet-only]` A snippet frames the motivation: "In addition to throughput, latency and power consumption are critical metrics for real-time deployment." **Tag: [measured on hardware]** (FPGA). *(Publisher returned non-public IP; no utilisation/latency numbers retrieved.)*

### ASIC / custom silicon

**A 18.1 TFLOPS/W Transformer Accelerator with Fine-Grained Per-Query Latency and Power Management in 12-nm FinFET** — IEEE (ISSCC-lineage; also "22.9 A 12nm 18.1TFLOPs/W Sparse Transformer Processor with Entropy-Based Early Exit, Mixed-Precision Predication and Fine-Grained Power Management") — [link](https://ieeexplore.ieee.org/document/11435429)
A **measured silicon transformer accelerator**: **18.1 TFLOPS/W in 12-nm FinFET**, with per-query latency and power management. `[snippet-only]` for the headline efficiency. Not wireless-specific, but it is the state of the art for "attention accelerator, measured, with an energy number," and therefore the right reference point for what an attention ASIC can do. **Tag: [measured on hardware]**.

**Transformer Accelerator (TFA): A Macro-Op INT8 Hardware Chip for Transformer Inference and Machine Translation** — arXiv 2608.23582 (Shashank) — [link](https://arxiv.org/abs/2608.23582)
Synthesizable, parameterizable **INT8 memory-to-memory engine**; eight 512-bit macro-op descriptors; output-stationary MAC array with ping-pong buffers overlapping DMA and compute. Verification: **zero mismatches across 25 tests and 34 constrained-random runs, 100% functional coverage, 94.96% code coverage**; the verification config achieved **~20× end-to-end speedup over a 22-thread CPU**; larger designs are *projected* to reduce **energy per token by ~1000×**; logic area **2.73 mm²** after RAM inference recoding, DRC-clean P&R on **SkyWater sky130**. **Tag: [mixed]** — RTL verification and area are real; the 1000× energy-per-token figure is an explicit projection, not a measurement.

**A 5.76 Gb/s 79.7 pJ/b 128×32 Massive Deep-Learning Uplink MIMO Detector in 28nm CMOS Technology** — IEEE (POSTECH/KAIST) — [link](https://ieeexplore.ieee.org/document/10849071)
**Measured deep-learning MIMO detector ASIC**: **5.76 Gb/s, 79.7 pJ/b, 128×32, 28 nm CMOS**. This is the strongest ASIC-level energy-per-bit data point for a *learned* detector. **Tag: [measured on hardware]** (silicon measurements). *(Headline numbers come from the title, which is itself the reported result.)*

**A Deep-Unfolding-Optimized Coordinate-Descent Data-Detector ASIC for mmWave Massive MIMO** — IEEE JSAC 2025 — [link](https://ieeexplore.ieee.org/document/10845825)
Deep-unfolding (model-driven, not transformer) data detector ASIC for mmWave massive MIMO. Included as the model-driven-hardware comparison point. **Tag: [measured on hardware]**. *(Area/power numbers not retrieved.)*

**TensorPool: A 3D-Stacked 8.4 TFLOPS/4.3 W Many-Core Domain-Specific Processor for AI-Native Radio Access Networks** — also as "A 8.4 TFLOPS@16b/4.3W General-Purpose Programmable Accelerated Cluster for AI-Native RAN", ACM Computing Frontiers — [link](https://dl.acm.org/doi/full/10.1145/3801487.3801815)
Purpose-built RAN AI accelerator: **8.4 TFLOPS at 16-bit for 4.3 W**, 3D-stacked many-core. This is the most directly relevant ASIC-class result for "AI-native RAN baseband" energy efficiency. `[snippet-only]` for the headline figure (it is in the title). **Tag: [measured on hardware]**.

**28 nm / analog & in-memory alternatives:** **Analog neural network–based wireless receiver** — Science Advances (DOI 10.1126/sciadv.aeg7153) — [link](https://www.science.org/doi/10.1126/sciadv.aeg7153)
**Proof-of-concept analog wireless receiver in which multiple NNs for signal processing are distributed** across the analog domain. `[snippet-only]` Supplementary material states **energy efficiency = 0.254 TOPS / 11.922 mW = 21.3 TOPS/W**; the paper itself flags as a limitation **"the energy and area efficiency overhead arising from the requirement for DACs and ADCs in the memristive…"** front-end. **Tag: [measured on hardware]** (proof-of-concept analog hardware). Not a transformer, but the strongest "compute-in-the-front-end" energy datapoint.

**RF-Photonic / photonic attention (context only, not PHY receivers):** **Optical Comb-Based Monolithic Photonic-Electronic Accelerators for Self-Attention Computation** — [link](https://par.nsf.gov/servlets/purl/10553811); **MDTransformer: A Hardware-Software Co-Design of Mode-Division Photonic Transformer Accelerator** — arXiv 2607.26016 — [link](https://arxiv.org/abs/2607.26016). MDTransformer reports **40.4% area reduction, 63.6% power saving, 40.6% energy saving and comparable latency** vs. prior photonic transformer accelerators, with **sub-4-bit effective precision** and inter-modal crosstalk **< −30 dB**, evaluated on DeiT-Tiny/Small/Base and BERT-Base/Large. **Tag: [mixed]** — the comparisons are against a *modelled* SOTA PTA, not silicon, so treat as projected.

### GPU / DSP / embedded

**Six Times to Spare: LDPC Acceleration on DGX Spark for AI-Native Open RAN** — arXiv 2602.04652 (Barker, Afghah) — [link](https://arxiv.org/abs/2602.04652)
The cleanest **5G NR TTI-budget measurement on a modern GPU** in this survey, and a useful calibration of what "0.5 ms slot" really costs for a non-transformer PHY kernel. LDPC decoding must complete within **0.5 ms TTI while sharing the budget with FFT, channel estimation, demapping, HARQ and MAC scheduling.** Offloading LDPC5G from a **Grace CPU to the integrated Blackwell GB10 GPU on DGX Spark** gives an **average ~6× GPU/CPU throughput speedup**; **per-codeword CPU latency reaches ≈0.71 ms at 20 iterations (exceeding the 0.5 ms slot), while the GB10 GPU remains within 6–24% of the slot for the same workloads.** CPU-based decoding consumes **~10 Grace cores**, whereas GPU decoding adds only **≈10–15 W over GPU idle** while leaving most CPU capacity free. The authors stress these are **conservative lower bounds** because they use high-level Sionna layers, not hand-tuned CUDA. **Tag: [measured on hardware]** (DGX Spark GB10/Grace, with power telemetry).

**HELENA for 5G NR LEO NTN** (above) — [link](https://arxiv.org/abs/2609.14735) — is the key **embedded-GPU** (Jetson Orin NX, 10 W) measurement: **no model meets the P99 0.5 ms budget on the Orin NX**, whereas the desktop RTX PRO 4500 hits 0.0595 ms P99 (88.1% headroom). **Tag: [measured on hardware]**.

**Sionna Research Kit** (above) — [link](https://arxiv.org/abs/2505.15848) — real-time TensorRT neural receiver on **Jetson AGX Orin** with commercial UEs. **Tag: [measured on hardware]**.

**Neuromorphic In-Context Learning for Energy-Efficient MIMO Symbol Detection** — arXiv 2404.06469 / IEEE (Song, Simeone, Rajendran) — [link](https://arxiv.org/abs/2404.06469)
Replaces ANNs with **spiking neural networks and implements the attention mechanism via stochastic computing — no multiplications, only logical AND operations and counting.** On conventional digital CMOS the implementation **preserves accuracy while reducing power consumption by 5.4× to 26.8×** depending on model size, versus ANN implementations. This is the most aggressive attention-energy reduction reported here. **Tag: [simulated/estimated only]** for the power reduction (the abstract says "when using conventional digital CMOS hardware, the proposed implementation is shown to preserve accuracy, with a reduction in power consumption" — that reads as an estimate, not a silicon measurement). Companion: **DNN-SNN Co-Learning for Sustainable Symbol Detection in 5G Systems on Loihi Chip** — IEEE — [link](https://ieeexplore.ieee.org/document/10285018) — targets Intel Loihi; **Tag: [measured on hardware]** if the Loihi figures are on-chip, but I could not retrieve them (see uncertainties).

**CENTRIC D2.1 "Evaluation of GPU Implementation of ML-based Receiver"** — Zenodo record 18745043 — [link](https://zenodo.org/records/18745043) — and **CENTRIC D2.3 "Digital hardware architectures"** — [link](https://zenodo.org/records/15739575); **D2.4 Mixed Analog-Digital Hardware Architecture** — [link](https://zenodo.org/records/18745077) — EU project deliverables explicitly scoped to GPU/digital/analog hardware architectures for neural receivers. **Tag: [not verified]** — Zenodo was unreachable; these are flagged as high-value targets for a follow-up pass.

**AtlasRAN: Modeling and Performance Evaluation of Open 5G Platforms for Ubiquitous Wireless Networks** — arXiv 2603.14661 (Barker, Seyfi, Dorcheh, Boone, Afghah, Boccuzzi) — [link](https://arxiv.org/abs/2603.14661)
Methodologically important caveat for anyone benchmarking on GPU: in a CU-DU uplink load study on a coherent CPU-GPU edge platform, for both CPU-only and GPU-accelerated LDPC variants, **aggregate goodput drops sharply as user count rises 1 → 12 while fairness stays near ideal and compute utilization *decreases*.** The authors attribute this to **time-scale dilation and online I/O starvation in the emulation harness, not decoder saturation** — i.e. reported "limits" may reflect the execution harness rather than the wireless design. **Tag: [measured on hardware]** (coherent CPU-GPU edge platform).

---

## (3) Small / edge transformer receivers for the PHY

**Chain-of-Thought Enhanced Shallow Transformers for Wireless Symbol Detection (CHOOSE)** — arXiv 2506.21093 (Li Fan, Peng Wang, Jing Yang, Cong Shen) — [link](https://arxiv.org/abs/2506.21093)
The paper named in the task. Prior ICL-based Transformer receivers "rely on deep architectures with many layers to achieve satisfactory performance, resulting in substantial storage and computational costs." CHOOSE introduces **autoregressive latent reasoning steps within the hidden space**, which **significantly improves the reasoning capacity of shallow models (1–2 layers) without increasing model depth.** The design lets lightweight Transformers reach detection performance **comparable to much deeper models**, making them suitable for **resource-constrained mobile devices**; the latent CoT reasoning loop is **fully compatible with causal masked self-attention.** **Tag: [simulated/estimated only]** — the abstract claims storage/computational efficiency but gives **no parameter count, FLOP count or measured latency**, and there is no hardware deployment. **This is an important gap: the flagship "small transformer receiver" paper is a simulation-only result with qualitative efficiency claims.**

**Computationally Efficient Neural Receivers via Axial Self-Attention** — arXiv 2510.12941 — [link](https://arxiv.org/abs/2510.12941) — see (1). **O((TF)²) → O(T²F + TF²)**, "a fraction of the parameters" vs. CNN baselines. **Tag: [simulated/estimated only]**.

**CPMamba: Selective State Space Models for MIMO Channel Prediction in High-Mobility Environments** — arXiv 2512.16315 (Luo, Xie, Che, Yao, Tian, Feng, Wu) — [link](https://arxiv.org/abs/2512.16315)
Stacked residual Mamba modules with an input-dependent selective mechanism over historical CSI, **maintaining linear computational complexity** while capturing long-range CSI dependencies. Under 3GPP channel models it achieves SOTA prediction accuracy with **~50% fewer parameters** than baselines at comparable-or-better performance, "significantly lowering the barrier for practical deployment." **Tag: [simulated/estimated only]**.

**Signal Detector for MIMO Molecular Communication Based on the Mamba Model** — ACM (DOI 10.1145/3760544.3765645) — [link](https://dl.acm.org/doi/pdf/10.1145/3760544.3765645) — Mamba-based MIMO detector outside the radio PHY but architecturally transferable. **Tag: [not verified]**.

**NeuFAS: Edge-Efficient Mamba-Based Channel Estimation for Fluid Antenna Systems** — IEEE LCN 2026 — [link](https://ieeexplore.ieee.org/document/11660767) — uses a **selective state-space model inside a "FluidMamba"** design explicitly targeted at *edge-efficient* channel estimation. **Tag: [not verified]** — no parameters/latency numbers retrieved; flagged as high-value follow-up.

**Efficient Mamba-Based Accelerator Architecture for Massive-MIMO Indoor Localization Using the LuViRA Dataset** — Lund University master's thesis (LUP 9232796) — [link](https://lup.lub.lu.se/student-papers/search/publication/9232796)
A **Mamba/SSM hardware accelerator** for massive-MIMO localization. `[snippet-only]` The thesis motivates "fine-grained pipeline design for the SSM path, since improving this part has the largest impact on the end-to-end…". **Tag: [measured on hardware]** (accelerator implementation) — but the numbers were not retrievable. Note the shared authorship lineage with 2605.13507 (Yaman, Edfors, Liu), so this is likely the SSM counterpart of the transformer FPGA accelerator.

**Linear Attention Based Channel Estimation Scheme for V2X Communications** — IEEE (Fu, Yuan et al.) — [link](https://ieeexplore.ieee.org/document/10779439)
Applies **linear attention** to V2X channel estimation, i.e. the O(N) alternative to softmax attention in a PHY estimator. **Tag: [not verified]** — no complexity/parameter/latency numbers retrieved. High-value follow-up.

**The Transformer as a Parametric Filter for Sequence MIMO Equalization** — IEEE 2026 (Yu, Gao et al.) — [link](https://ieeexplore.ieee.org/document/11441010) — reframes the transformer as a parametric filter for sequence MIMO equalization. **Tag: [not verified]**.

**Axial Attention-Based Transformer Receiver for Robust OFDM-CPM Signal Detection** — TechRxiv preprint — [link](https://www.techrxiv.org/doi/full/10.36227/techrxiv.177083633.33301384/v1) — axial attention applied to OFDM-CPM detection; same complexity-reduction family as 2510.12941. **Tag: [not verified]**.

**Common-Loss Parameter-Efficiency Analysis of MLP and KAN Neural Receivers for Digital Communications** — arXiv 2609.25847 (Ertan, Tokluoglu, Cavus) — [link](https://arxiv.org/abs/2609.25847)
Useful parameter-efficiency datapoint for the *small-receiver* question, on a deliberately verifiable AWGN-BPSK benchmark: a **KAN receiver with 3281 parameters reaches test BER 2.45×10⁻⁴ while an MLP baseline with 8513 parameters reaches 2.50×10⁻⁴ — ≈61.5% fewer trainable parameters at a comparable BER operating point.** The authors explicitly tie parameter count to "memory footprint, parameter access, inference latency, energy consumption, and hardware feasibility on embedded, SDR, FPGA, ASIC, and edge communication platforms." **Tag: [simulated/estimated only]** (the parameter/latency/energy linkage is argued, not measured).

**CSI-Free Symbol Detection for Atomic MIMO Receivers via In-Context Learning** — arXiv 2507.04040 (Song, Peng, Xiao, Rajendran, Simeone) — [link](https://arxiv.org/abs/2507.04040)
ICL maps pilot-response pairs directly to symbol predictions, avoiding cascaded channel-estimation error and the **"high computational complexity"** of the two-step iterative baseline; claims **higher computational efficiency** at competitive accuracy. **Tag: [simulated/estimated only]**.

**PolymoRF: Polymorphic Wireless Receivers Through Physical-Layer Deep Learning** — arXiv 2005.02262 (Restuccia, Melodia) — [link](https://arxiv.org/abs/2005.02262)
Embedded deep-learning architecture (**RFNet**) integrated with radio components on a custom SDR. Over-the-air experiments: RFNet matches SOTA accuracy with **52× and 8× latency and hardware reduction**, and PolymoRF achieves **throughput within 87% of a perfect-knowledge Oracle system.** **Tag: [measured on hardware]** (custom SDR prototype, over-the-air). *(Not a transformer, but one of very few "DL receiver, measured latency reduction, over the air" results.)*

### Quantization impact on receiver accuracy (INT8/INT4/FP8/FP4)

**Efficient Deep Neural Receiver with Post-Training Quantization** — arXiv 2508.06275 / IEEE (Yellapragada, Ollila, Costa) — [link](https://arxiv.org/abs/2508.06275)
Symmetric uniform PTQ with per-tensor and per-channel variants on a neural receiver: **8-bit per-channel quantization maintains BLER performance with minimal degradation; 4-bit shows promise but requires further optimization to hit target BLER.** Positions PTQ/QAT/pruning as necessary to meet "hard real-time processing demands of 5G and 6G." **Tag: [simulated/estimated only]**.

**Efficient Quantization-Aware Neural Receivers: Beyond Post-Training Quantization** — arXiv 2509.13786 / IEEE (Yellapragada, Ollila, Costa) — [link](https://arxiv.org/abs/2509.13786)
Benchmarks QAT vs. PTQ across 3GPP CDL profiles (LoS and NLoS, user velocities to 40 m/s): **4-bit and 8-bit QAT models achieve BLERs comparable to FP32 at a 10% target BLER**, and critically **QAT succeeds in NLoS scenarios where PTQ models fail to reach the 10% BLER target, while also yielding an 8× compression.** **Tag: [simulated/estimated only]**.

**Floating-Point Microformat Quantization and Pruning for Efficient MU-MIMO Neural Receivers** — arXiv 2609.31177 (Yellapragada, Ollila, Costa, Li) — [link](https://arxiv.org/abs/2609.31177)
The sharpest result on *format* vs. *bit-width*. For a standard-compliant MU-MIMO neural receiver with INT8/INT4 vs. FP8 (E4M3)/FP4 (E2M1) weights and INT8 post-ReLU activations, trained on 3GPP UMi and evaluated on TDL-B/TDL-C: **8-bit weight-activation models remain within 0.05 dB of FP32 at 10% and 1% BLER. At 4 bits, uniform INT4 loses 3.3–3.7 dB and falls below LS-LMMSE, whereas FP4 more than halves this loss (1.3–1.4 dB) and still outperforms LS-LMMSE by ~0.5 dB, even after pruning.** FP4's denser near-zero grid matches the trained weight distribution; **FP4 also avoids the residual-path over-pruning seen with INT4.** An analytic cost model projects **66× fewer bit-operations and 8.8× less weight storage for pruned 4-bit-weight inference.** **Tag: [simulated/estimated only]** (analytic cost model; no hardware).

**Data-Free Quantization of Neural Receivers: When 4-Bit Succeeds, Why 6-Bit Matters for 6G** — NeurIPS 2025 — [link](https://neurips.cc/virtual/2025/loc/san-diego/123232)
`[snippet-only]` A snippet reports that **in Scenario II the 4-bit quantized neural receiver surpasses LS estimation by approximately 2.5 dB**. **Tag: [simulated/estimated only]**. The paper's core thesis (4-bit sometimes succeeds, 6-bit matters for 6G) is directly relevant to choosing an accelerator datapath width.

**Other quantization-adjacent work:** **Model Compression for Sustainable AI in xG Wireless Networks** — IEEE — [link](https://ieeexplore.ieee.org/document/11478444) (survey of pruning/quantization for xG energy); **MF-QAT: Multi-Format Quantization-Aware Training for Elastic Inference** — arXiv 2604.00529 — [link](https://arxiv.org/abs/2604.00529) (one model robust across MXINT/MXFP formats, **Slice-and-Scale** runtime conversion from an MXINT8/MXFP8 anchor, enabling *runtime* precision selection by hardware support). Both **Tag: [simulated/estimated only]**. MF-QAT is the right technique if the receiver must serve both an INT8 accelerator and an FP4 accelerator.

---

## (4) SDR / over-the-air testbeds with a DL receiver in the loop

**Sionna Research Kit** — arXiv 2505.15848 — [link](https://arxiv.org/abs/2505.15848) — OAI-based software-defined stack on **Jetson AGX Orin**, with a **real-time TensorRT neural receiver (trained with Sionna) running in a 5G NR network with commercial UEs.** **Tag: [measured on hardware]**. The most complete openly-documented "DL receiver in the loop with real UEs" demonstration found.

**The OCUDU dApp Platform** — arXiv 2609.07843 — [link](https://arxiv.org/abs/2609.07843) — an **out-of-tree neural equalizer running inside a production DU on a GB10 gNB with attached handsets, over a live cell with zero fallbacks**, with equalizer variants A/B-compared over the air by lifecycle operations. **Tag: [measured on hardware]**. Platform/SDK released BSD-3-Clause-Clear — the most directly actionable artifact for an engineering team wanting to run its own receiver in the loop.

**Real-Time dApps for AI-RAN** — arXiv 2609.07805 — [link](https://arxiv.org/abs/2609.07805) — quantifies the *interface* cost of putting a neural receiver in the loop: **a 100 µs control deadline is met by every carrier on an idle host but only by in-process paths once the DU is running**; **four dApps of all three classes validated together on one over-the-air cell.** `[snippet-only]` neural-receiver dApp sized at 1.47 MB in / up to 4.4 MB out per slot with ≤500 µs occupancy. **Tag: [measured on hardware]**.

**5G OAI Neural Receiver Testbed with USRP X410** — Ettus Research Knowledge Base — [link](https://kb.ettus.com/index.php?title=5G_OAI_Neural_Receiver_Testbed_with_USRP_X410)
The reference how-to for an OAI + USRP X410 + GPU neural-receiver testbed. `[snippet-only]` Two X410 units are interconnected through **2×1 RF splitters and a 30 dB attenuator**; the KB page includes a component-by-component table contrasting a **Traditional Receiver (LS channel estimation, …) against a Neural Receiver**. **Tag: [measured on hardware]** (testbed description). *(kb.ettus.com was unreachable; the component table and any latency budget column could not be read. Third-party summaries exist at [sdrstore.eu](https://www.sdrstore.eu/5g-oai-neural-receiver-testbed-hardware-lab-usrp-gpu-sdr/).)*

**On the Impact of Site-Specific Training for a Real-World 5G NR System** — arXiv 2609.04004 (Wiesmayr, Baytekin, Dick, Studer) — [link](https://arxiv.org/abs/2609.04004)
New measurements from a **standard-compliant 5G NR testbed at ETH Zurich with dual-layer uplink transmission**, including campaigns **more than six months apart**. Site-specific finetuning substantially improves fully-trainable and model-driven neural receivers (marginal for the less-tunable model-based receiver); a **single neural receiver jointly finetuned for single- and dual-layer transmission closely matches receivers finetuned separately for each configuration**; and finetuning **remains effective across campaigns separated by more than six months**. With iterative detection and decoding, site-specific LMMSE channel estimation achieves the lowest error rate in their datasets. Finetuning code and **measurement datasets are public** at `github.com/IIP-Group/site_specific_training`. **Tag: [measured on hardware]** (real 5G NR testbed). Note this is about *accuracy* on hardware, not latency.

**Dorner et al., "Deep Learning Based Communication Over the Air"** — IEEE JSTSP 12(1), 2018 (Dorner, Cammerer, Hoydis, ten Brink) — [link](https://ieeexplore.ieee.org/document/8214233)
The historical anchor for OTA DL receivers: an autoencoder-based end-to-end communications system trained and evaluated **over the air**, with learned constellations, using SDR hardware and a standard DL framework; the transmitter's learned constellations are recovered from real channel measurements. **Tag: [measured on hardware]** (over-the-air). **Caveat:** I could **not** retrieve a specific *latency* number from this paper in this session — the IEEE page and mirrors were unreachable and the abstract does not contain one. Treat any latency figure attributed to this paper as unverified.

**O'Shea et al., "Demonstrating Deep Learning Based Communications Systems Over the Air In Practice"** — 2018 (O'Shea, Roy, West, Hilburn) — [link](https://researchr.org/publication/OSheaRWH18-0/related)
Companion demonstration paper on practical OTA DL communications. **Tag: [measured on hardware]** (OTA demonstration). **Latency numbers not verified** in this session.

**PolymoRF** — arXiv 2005.02262 — [link](https://arxiv.org/abs/2005.02262) — custom SDR prototype, **over-the-air experiments**, **52× latency reduction**, throughput within 87% of Oracle. **Tag: [measured on hardware]**.

**Rohde & Schwarz + NVIDIA neural receiver testing** — R&S press material (2025 and 2026) — [link](https://www.rohde-schwarz.com/lat/la-empresa/noticias-y-prensa/all-news/rohde-schwarz-advances-ai-ran-testing-using-digital-twins-with-nvidia_229356-1611024.html); vendor video "AI-powered 6G receiver – Hardware-in-the-loop testing of an AI receiver integrating DPoD" — [link](https://www.rohde-schwarz.de/jp/knowledge-center/videos/ai-powered-6g-receiver-hardware-in-the-loop-testing-of-an-ai-receiver-integrating-dpod_251220-1593576.html)
Instrument-vendor HIL/Digital-Twin testing of neural receivers with NVIDIA. **Tag: [vendor claim, measured on hardware but not peer-reviewed]** — I could **not** retrieve quantitative latency figures; treat as evidence that commercial HIL test capability exists, not as a performance datapoint.

**DeepSig "From Lab to Field" / OCUDU dApp platform** — vendor blog — [link](https://www.deepsig.ai/from-lab-to-field-evolving-deep-learning-for-communication-systems/) and [OCUDU dApp platform](https://www.deepsig.ai/ocudu-dapp-platform/) — commercial framing of the same OTA DL-receiver story. **Tag: [vendor claim]** — site unreachable; no numbers verified.

---

## (5) Measured on hardware vs. simulated/estimated only — the honest split

This is the most decision-relevant finding of the survey. The literature splits cleanly, and **the split does not follow the "transformer" label**.

### Genuinely measured on hardware (real device, reported numbers)

| Work | Hardware | Headline measured number |
|---|---|---|
| Wiesmayr et al. 2409.02912 | NVIDIA A100 + TensorRT | **<1 ms inference** (target of the design) |
| Cammerer et al. 2505.15848 (Sionna Research Kit) | **Jetson AGX Orin**, real UEs | real-time NE running in a live 5G NR network |
| O'Shea et al. 2609.07805 / 2609.07843 | GB10 gNB, live cell | **100 µs control deadline met in-process; 0.29 µs / 5.2 µs P99.9 direct call** `[snippet-only]` |
| Camelo Botero et al. 2506.13408 (HELENA) | GPU | **0.175 ms vs. 0.318 ms** (CEViT); 8× fewer params |
| Camelo Botero et al. 2609.14735 (HELENA+NTN) | **RTX PRO 4500 + 10 W Jetson Orin NX** | **0.0595 ms P99, 88.1% under the 0.5 ms budget (RTX); no model meets P99 on Orin NX** |
| Barker & Afghah 2602.04652 | **DGX Spark (Grace + Blackwell GB10)** | **CPU 0.71 ms/codeword vs. GPU 6–24% of the 0.5 ms slot; ~10–15 W added** |
| Yaman et al. 2605.13507 | **Xilinx Zynq UltraScale+ FPGA** | **0.51–2.11 ms latency, 1961 pos/s, ~2× speedup at 65% sparsity** |
| Lyu et al. 2605.01931 (SwiftChannel) | **Zynq UltraScale+ RFSoC** | **sub-ms latency; 24× speedup and >33× energy efficiency vs. GPU** |
| Sharma et al. 2403.01098 | **Zynq SoC** | **88–90% lower execution time, 38–85% lower resources** vs. SOTA DL CE |
| Luo et al. 2512.13263 | FPGA | **~1.5 dB BER gain, up to 66% lower execution time** |
| Song et al. 2607.24669 | NN accelerator | **98 µs end-to-end latency/frame** |
| MacLellan et al. (RFSoC streaming CNN) | **RFSoC** | 100 MHz target clock; quantisation configs incl. 8-bit (exact latency unverified) |
| LAMANet / LwAMP-Net | FPGA | resource deltas (**>2× DSP, >10× BRAM**) `[snippet-only]` |
| 28 nm DL MIMO detector ASIC | **28 nm CMOS silicon** | **5.76 Gb/s, 79.7 pJ/b, 128×32** |
| 12 nm transformer accelerator | **12 nm FinFET silicon** | **18.1 TFLOPS/W** |
| TensorPool | 3D-stacked many-core | **8.4 TFLOPS @ 16b for 4.3 W** |
| Analog NN wireless receiver | analog/memristive PoC | **21.3 TOPS/W** (0.254 TOPS / 11.922 mW) `[snippet-only]` |
| Restuccia & Melodia 2005.02262 (PolymoRF) | custom SDR, **over the air** | **52× / 8× latency & hardware reduction; 87% of Oracle throughput** |
| Wiesmayr et al. 2609.04004 | ETH Zurich 5G NR testbed | accuracy gains from site-specific finetuning, 6 months apart |
| Barker et al. 2603.14661 (AtlasRAN) | CPU-GPU edge | goodput collapse 1→12 users traced to **harness I/O starvation**, not the decoder |
| Song et al. 2404.06469 (neuromorphic ICL) | digital CMOS (est.) | **5.4×–26.8× power reduction** (borderline: reads as an estimate) |

### Simulated / analytically estimated only

- **CHOOSE (2506.21093)** — the headline "small transformer receiver" paper. 1–2 layer shallow transformer with latent CoT; **no params, no FLOPs, no latency, no hardware.**
- **Axial self-attention neural receiver (2510.12941)** — clean O((TF)²) → O(T²F + TF²) complexity result and FLOP reduction, but **no device measurement**.
- **All four quantization papers (2508.06275, 2509.13786, 2609.31177, Data-Free Quantization)** — BLER/bit-op/compression results only; the 66× bit-op and 8.8× storage figures in 2609.31177 are from an **analytic cost model**.
- **CPMamba (2512.16315)**, **KAN vs. MLP receivers (2609.25847)**, **DAT/RDNLA (2503.20500)**, **CSI-free atomic MIMO ICL (2507.04040)**, **Unified Transformer (2508.17960** — partly real via OAI+Aerial, but no numbers).
- **Online/edge-survey material**: 2508.02314, 2601.10963, 11478444, 2604.00529.

### The general pattern

1. **Every paper that reports a real per-slot latency number uses a CNN or a compact attention/convolution hybrid — not a full transformer.** HELENA (dual attention + conv backbone), SwiftChannel (parameter-free attention on a CNN), the RFSoC streaming CNN, the 98 µs attention-CNN accelerator. The measured, in-budget PHY receivers in this survey are *not* pure transformers.
2. **Where transformers are measured, the task is localization or channel estimation, not full detection** (2605.13507, 2506.13408/2609.14735) — or the transformer is on the *downstream* side of the PHY (RF recognition, 2607.24669).
3. **The one full-transformer "receiver" with an explicit real-time claim (2409.02912) needed a deliberate architecture-size reduction to fit, accepted <0.7 dB SNR loss for it, and reports a single aggregate "<1 ms on an A100" figure rather than a P99 latency distribution.**
4. **Tail latency, not mean latency, is the binding constraint on embedded hardware.** 2609.14735 is unambiguous: on the 10 W Jetson Orin NX, HELENA keeps a good accuracy–energy trade-off but **no model meets the P99 budget**, while the same model on an RTX PRO 4500 has 88.1% headroom. Any "meets the 0.5 ms slot" claim that quotes a mean is not evidence of real-time capability.
5. **Neural receivers are trained on simulated channels but increasingly validated on real ones** (2609.04004 at ETH Zurich, 2605.13507 on real massive-MIMO measurements, 2505.15848/2609.07843 with real UEs/handsets). The **latency** is measured; the **channel realism** is where sim-to-real gaps live.

---

## Uncertainties / could-not-verify

**Environment limitation.** `web_fetch` failed for almost all hosts ("resolves to a non-public IP address"): arxiv.org and all its mirrors (`ar5iv`, `web3`, `export`, `export-test`), ieeexplore.ieee.org, sciencedirect.com, mdpi.com, semanticscholar.org, openreview.net, developer.nvidia.com, github.com/raw, zenodo.org, kb.ettus.com, dl.acm.org (403), strathprints/pure.ed.ac.uk, core.ac.uk, scirate.com, alphaxiv.org, emergentmind. Only `papers.cool/arxiv/<id>` (abstracts) and `nvlabs.github.io` (nav only) worked. **Consequently: no paper *body* was read in this survey.** All numbers come from abstracts or from search-result snippets.

**Explicitly unverified items (do not cite as fact without checking the source):**

1. **Parameter counts and FLOP/MAC counts for the flagship real-time neural receivers.** 2409.02912, 2312.02601 and 2505.15848 abstracts contain **no parameter or FLOP figures**. The commonly-quoted NRX model size could not be confirmed.
2. **Per-slot latency distribution for the NVIDIA real-time NRX.** Only "<1 ms on an A100" is confirmed. Any P99/P99.9 figure, or any "<x % of the slot" claim, is unverified.
3. **O'Shea OCUDU figures.** "0.29 µs P99.9 quiet / 5.2 µs with the cell on air" and "1.47 MB in, up to 4.4 MB out per slot, ≤500 µs occupancy" are **snippet-only**; the ABI stream vs. protocol path attribution is also from snippets.
4. **CHOOSE (2506.21093) quantitative efficiency.** The abstract gives no numbers. I could not verify the layer count, hidden dimension, parameter count, FLOP count, or any latency claim. **A arXiv full-text read is required before this paper is used to justify a design.**
5. **SoftBank "Attention Over the Air" / +30% uplink throughput.** Confirmed only as a **vendor claim** (SoftBank press release 2025-08-21; secondary coverage reporting the downlink throughput *doubled* and uplink +30% in a live 5G network, using a transformer architecture "optimized for wireless signal processing" for high performance and low latency). I could **not** retrieve any latency number, model size, or GPU/platform detail — this is a headline claim, not a technical datapoint.
6. **LAMANet** latency, power and full utilisation numbers (only ">2× DSP, >10× BRAM" snippet).
7. **LwAMP-Net** DSP/BRAM/LUT/clock/latency/power numbers (MDPI unreachable).
8. **RFSoC streaming CNN** — exact latency, resource table, and the accuracy penalty of 8-bit weights/activations (only the quantisation config list and a fragmentary per-layer table were visible).
9. **Neuromorphic Loihi results** — the DNN-SNN co-learning paper's on-chip energy/inference numbers were not retrieved; also unclear whether 2404.06469's 5.4×–26.8× is measured or estimated.
10. **CENTRIC D2.1 / D2.3 / D2.4 / D5.2 / D5.4** — EU deliverables that almost certainly contain the GPU and digital-hardware-architecture numbers this survey is missing. Zenodo was unreachable. **Highest-value follow-up target.**
11. **NeuFAS (FluidMamba)**, **Linear Attention V2X CE**, **Transformer-as-Parametric-Filter**, **Axial-Attention OFDM-CPM**, **Mamba-Driven CE for High-Mobility Massive MIMO**, **Mamba molecular-communication detector**, **Mamba accelerator thesis (Lund)** — all identified and linked, **none with verified numbers**.
12. **Dorner et al. 2018 and O'Shea et al. 2018** — I could not verify any *latency* measurement from these papers. The task prompt suggests they may contain one; I found none.
13. **Rohde & Schwarz / Keysight / DeepSig vendor HIL demonstrations** — existence confirmed, no quantitative latency, and no peer review.
14. **5G NR slot numerology** (0.5 ms at 30 kHz SCS) is stated from background knowledge, not verified against 3GPP TS 38.211 in this session. Papers in this survey treat "0.5 ms slot" as the budget, which is consistent with 30 kHz SCS.
15. **"Data-Free Quantization of Neural Receivers" (NeurIPS 2025)** — only the snippet "4-bit quantized neural receiver surpasses LS estimation by approximately 2.5 dB in Scenario II" was retrievable; the 6-bit argument's supporting numbers are unverified.

**Recommended next retrieval steps (in priority order):** (a) CENTRIC deliverables D2.1/D2.3/D2.4 on Zenodo; (b) full text of arXiv 2506.21093 (CHOOSE) and 2409.02912 (real-time NRX) for parameter/FLOP tables; (c) the Ettus KB testbed page for the traditional-vs-neural receiver component/latency table; (d) full text of LAMANet and LwAMP-Net for FPGA utilisation; (e) NeuFAS and the Lund Mamba accelerator thesis for Mamba-on-hardware numbers.
