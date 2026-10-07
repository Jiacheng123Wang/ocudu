# AI-based 5G-NR uplink detection chain: literature review

Scope: replacing the NR uplink physical-layer detection chain (time-frequency resource grid → soft bits / LLRs)
with an AI model, deployed on Apple-Silicon GPUs via CoreML/Metal, in a real over-the-air gNB testbed (OCUDU).

## 0. Method note / confidence labelling

This survey was produced from a sandbox in which **almost all direct web fetching is blocked**
(`arxiv.org`, `ieeexplore.ieee.org`, `semanticscholar.org`, `openreview.net`, `github.com`,
`huggingface.co`, `raw.githubusercontent.com` and most vendor domains all fail DNS/host policy; only a
handful of hosts such as `nvlabs.github.io` were reachable). The research therefore ran through the
search tool, whose index *does* contain the full text of arXiv/IEEE/Elsevier PDFs and exposes matched
passages. Consequences the reader should keep in mind:

* Titles, venues, years, URLs, and quoted formula/fragment text below were seen in an indexed source.
* Numbers marked **[unverified]** were *not* successfully read from a source; they are either recalled
  from prior knowledge or structurally inferred. They are flagged rather than silently asserted.
* Where a claim is an inference of mine (not the authors'), it is marked **inference**.

**Companion documents in this directory** (produced by the same survey effort, with per-claim verbatim
evidence and explicit "could not verify" lists):

* `survey_line_a_model_based_mimo_detection.md` — deep-unfolding detectors (DetNet / OAMP-Net2 / MMNet /
  LoRD-Net), their output types, and the surveys.
* `survey_line_b_llr_output_receivers.md` — LLR-native receivers, loss functions, and the LLR
  calibration/scaling/quantisation evidence base, including **negative evidence** for searches that found
  nothing.

---

## 1. Transformer / attention-based wireless receivers and symbol detection

### 1.1 The DEFINED line (arXiv 2503.16594) — full characterisation

**Identity and versions** (verified against the arXiv abstract page itself via `curl`, not via search):

| Item | Value |
|---|---|
| arXiv ID | **2503.16594** ([abs](https://arxiv.org/abs/2503.16594), [HTML v2](https://arxiv.org/html/2503.16594v2), [ar5iv](https://ar5iv.labs.arxiv.org/html/2503.16594)) |
| Canonical title | ***Decision Feedback In-Context Learning for Wireless Symbol Detection*** |
| Authors | **Li Fan, Wei Shen, Jing Yang, Cong Shen** — Charles L. Brown Dept. of ECE, **University of Virginia** |
| Version history | v1 submitted **20 Mar 2025**; v2 (latest) **7 Jul 2025** |
| Preliminary version | *Decision Feedback In-Context Symbol Detection Over Block-Fading Channels* — **IEEE ICC 2025** ([IEEE Xplore 11161684](https://ieeexplore.ieee.org/document/11161684), [arXiv 2411.07600](https://ar5iv.labs.arxiv.org/html/2411.07600), [NSF PAR manuscript](https://par.nsf.gov//servlets/purl/10631659), [SpectrumX](https://www.spectrumx.org/publication/decision-feedback-in-context-symbol-detection-over-block-fading-channels/)) |
| Code | `https://github.com/ShenGroup/DEFINED` (stated in the paper's own footnote) |

> **Title-alias warning.** Several secondary indexes — Semantic Scholar, ar5iv, and some arXiv HTML
> mirrors — list this same arXiv ID under the title ***Transformer-based Wireless Symbol Detection Over
> Fading Channels***, which is the name used in the workstream brief. The arXiv abstract page and the
> fetched v2 full text both carry the DEFINED title, and the version list shows only v1 and v2. Treat the
> two names as the same work, and cite the DEFINED title.

*(Meta-note: the earlier draft of this section had the v1/v2 titles the other way round, inferred from
search snippets. The full-text fetch corrected it — a good illustration of why the team should not trust
title metadata from aggregators.)*

> **Evidence upgrade:** after a sibling survey found that `curl` works from the sandbox shell even though
> `web_fetch` is DNS-sinkholed, the whole v2 HTML was retrieved and read. Everything in §1.1 below is now
> read from the **full text**, not from search snippets.

**What it actually does — DEFINED = DEcision Feedback IN-context DEtection.** (Full-text verified.)

* **Abstract's own statement of the problem and contribution:** *"Transformer-based wireless receivers,
  where prompts consist of the pilot data in the form of transmitted and received signal pairs, have shown
  high detection accuracy when pilot data are abundant. However, pilot information is often costly and
  limited in practice. In this work, we propose DEcision Feedback IN-ContExt Detection (DEFINED) … which
  bypasses channel estimation and directly performs symbol detection using the (sometimes extremely)
  limited pilot data. The key innovation in DEFINED is the proposed decision feedback mechanism in ICL,
  where we sequentially incorporate the detected symbols into the prompts as pseudo-labels to improve the
  detection for subsequent symbols."*
* **System model.** Block-fading channel; each frame is at most `T` symbols; the first `k` transmitted
  symbols are known pilots, so the context set is the pilot-pair set
  `D_k = {(y_1,x_1), …, (y_k,x_k)}`. Detection is of `x_{k+1}…x_T`. Experiments use **T = 31 pairs max**,
  SISO BPSK/QPSK/16QAM/64QAM plus 2×2 MIMO QPSK. Training channels are i.i.d. Rayleigh `CN(0,1)` with the
  noise variance drawn per block from a range (so the model sees many SNRs during training but is
  evaluated at fixed SNRs).
* **Architecture.** Decoder-only GPT-2-style backbone, causal masked multi-head self-attention, linear IQ
  tokenisation, classification head over the constellation. Sizes: embedding `d_e = 64`, `L = 8` decoder
  layers, `h = 8` heads → **≈ 0.42 M parameters**, which the paper contrasts with GPT-J 6B being
  "approximately 14,000 times larger". **This is a genuinely small model** — the paper explicitly argues
  its size "enables its deployment on mobile devices" and "significantly shortens the inference time".
* **Classification, not regression.** DEFINED formulates detection as multi-class classification over the
  constellation and optimises SER directly, explicitly criticising prior ICL receivers that regress with
  MSE and then need a projection step ("leading to a mismatch and losing optimality").
* **Decision feedback mechanism (the actual prompt design).** The DF prompt at step `t` is
  `S^DF_t = {y_1,x_1,…,y_k,x_k, y_{k+1},x̂_{k+1}, …, y_{t−1},x̂_{t−1}, y_t}` — i.e. **each previously
  detected symbol is appended as a pseudo-pilot pair**, and detection proceeds sequentially (autoregressive
  over the block). The paper notes the "typical error event" that makes this work: incorrect detections
  cluster near the true constellation point, so even noisy pseudo-labels carry usable channel information.
* **Training is two-stage, and this is the expensive part.** (i) **ICL pre-training** on clean data;
  (ii) **DF fine-tuning** on a mixed clean + decision-feedback-generated dataset. During DF data
  generation gradients are frozen and *"each step of detection and feedback requires a forward pass
  through the model"*; the paper states **ICL training is ≈10× faster than DF training**, and that DF
  training is required because a model trained only on clean data is mismatched at DF test time. They also
  use **curriculum learning on context length**, noting that for 64QAM with few pilots and low SNR *"the
  model may fail to converge without curriculum learning."*
* **Theoretical results.** An error lower bound and generalisation analysis: for BPSK SISO the prediction
  error of a well-trained transformer is lower-bounded at order **O(1/k)**, and asymptotically perfect
  prediction is achievable with an infinite prompt. Theorem 2: a transformer trained at one noise level
  generalises to different `σ²` and different class means `μ_0, μ_1` — under SNR and LoS mismatch *"only a
  multiplicative constant changes, while the form of the estimator remains unchanged."* The paper is
  explicit that the **decision-feedback analysis is left open**: *"A rigorous theoretical analysis of
  Transformer-based in-context learning with decision feedback remains an open and compelling direction
  for future research."*
* **Standards-relevant design claim.** *"there is no need to change the existing frame structure or the
  design of pilot signals. Rather, the innovation is entirely at the receiver side … it allows for
  backward compatibility with the existing standard."* This is the strongest argument in the paper for a
  testbed team — it is a receiver-only change.

**Pilot abundance — the assumption, settled from the full text.** The paper's framing is the *inverse* of
what the workstream brief assumed: it targets the **pilot-scarce** regime and explicitly criticises prior
ICL receivers because *"many methods require an abundance of pilot pairs to achieve reasonable
performance, which may not be feasible in practice."* DEFINED's claim is that with a **single pilot pair**
(two pilots in some figures) it approaches what conventional methods achieve with **more than 4 pilot
pairs**. The prompt is still *built* from pilots (it is pilot-fed, not pilot-free); DF is what supplies the
extra effective context. Note the paper's own caveats: the extra context is *noisy self-generated* data,
and the theoretical bound assumes ground-truth in-context samples.

**Reported numbers (all read from the full text / figure captions).**

| Setting | Reported decision-feedback gain `gain_DF` |
|---|---|
| BPSK, SNR 15 dB, 1–2 pilots | **22.8 %** |
| QPSK, SNR 20 dB, 1–2 pilots | **19.3 %** |
| 16QAM, SNR 30 dB, 1–2 pilots | **55.3 %** |
| 64QAM, SNR 35 dB, 1–2 pilots | **62.6 %** |
| 2×2 MIMO QPSK, SNR 15 dB, 4 pilots | **50.2 %** |
| SISO Rician 64QAM, SNR 25 dB, 1 pilot | **67.8 %** |

where `gain_DF = (SER_k(θ) − SER_{T−1}(θ)) / SER_k(θ) × 100 %` — the relative SER reduction from the
decision-feedback process relative to the same model with only the `k`-pilot prompt.

Other concrete results:

* **Channel-distribution mismatch (Rayleigh → Rician).** Trained on Rayleigh NLOS, tested on Rician with
  LOS at SNR 25 dB: SER falls **0.118 → 0.038** (the LOS component *helps*).
* **Pilot + SNR mismatch.** Tested on Rayleigh at SNR 25 dB with 2 pilots (below the training SNR range):
  **23.2 % SER gain, SER 0.138 → 0.106**, while the classical **MMSE-P30 (30 pilots) achieves SER 0.096** —
  i.e. DEFINED with 2 pilots lands within ~10 % relative SER of a 30-pilot MMSE estimator.
* **Versus non-coherent MLSD.** With one pilot, DEFINED *"empirically aligns with the MLSD estimator at
  short sequence lengths"* while avoiding MLSD's exponential complexity; with more pilots it transitions to
  coherent detection and outperforms MLSD.
* **Baselines used:** `MMSE-P_k` (MMSE with k pilots), **MMSE-DF** (decision-directed/data-aided MMSE —
  the classical analogue of DEFINED's idea), `ICL-ICL` (vanilla ICL, train and test without feedback) and
  `ICL-DF` (the deliberately mismatched training/test combination).

**Complexity and latency — what the paper does and does not say.** What is stated: ≈0.42 M parameters;
detection is **sequential/autoregressive over the block** (one forward pass per detected symbol during
decision feedback — the same mechanism that makes DF *training* ≈10× slower than ICL training); the
comparison to MLSD is framed as avoiding exponential complexity. What is **not** in the paper: measured
inference latency, FLOPs, an inference-time complexity table, or any hardware result. **Inference (mine):**
per-block cost is ~`T` sequential forward passes with a context that grows to ~2T tokens, so the DF variant
is intrinsically a latency-for-accuracy trade, and attention over 62 tokens is trivial in FLOPs but the
*serialisation* is the real-time risk. The paper's own scheduling context is a block-fading frame of ≤31
symbols — i.e. **not** an NR slot with per-slot HARQ deadlines.

### 1.2 Rest of the transformer-receiver line

* **Chain-of-Thought Enhanced Shallow Transformers for Wireless Symbol Detection (CHOOSE)** — [arXiv 2506.21093](https://ar5iv.labs.arxiv.org/html/2506.21093) (2025; also on [NSF PAR](https://par.nsf.gov/biblio/10686368/media/xml)). Injects a *latent* chain-of-thought reasoning loop into a **shallow** transformer; the paper states the latent CoT loop is *"fully compatible with causal masked self-attention"* **[verified snippet]**. Directly relevant to the latency problem: the claim is that reasoning depth substitutes for transformer depth, which is the right lever if you must hit a slot deadline.
* **Leveraging Large Language Models for Wireless Symbol Detection via In-Context Learning** — [arXiv 2409.00124](https://ar5iv.labs.arxiv.org/html/2409.00124v2), IEEE [Xplore 10901317](https://ieeexplore.ieee.org/abstract/document/10901317), [NSF PAR](https://par.nsf.gov/biblio/10581289-leveraging-large-language-models-wireless-symbol-detection-via-context-learning). Uses a GPT-2-class pretrained LM as a symbol detector conditioned on in-context pilot examples — the "LLM as receiver" variant. Practical interest is low for a real-time gNB (autoregressive decoding), but it is the canonical citation for ICL symbol detection with a *pretrained* model rather than a task-trained transformer.
* **Turbo-ICL: In-Context Learning-Based Turbo Equalization** — [arXiv 2505.06175](https://ar5iv.labs.arxiv.org/html/2505.06175) (2025). ICL-based turbo equaliser; results are reported against baselines *"at the fifth turbo iteration"* **[verified snippet]**, so it is an iterative detector with an external iteration budget — the same latency/iterations trade-off the DEFINED decision-feedback loop has.
* **A Semi-Blind Receiver for Time-Selective Channels Using Transformer-Based In-Context Learning** — [PDF (IISc)](https://ece.iisc.ac.in/~achockal/pdf_files/Time_sel_Transformer.pdf) (Chockalingam group, IISc; venue **[unverified]**). Semi-blind ICL receiver for time-selective channels, i.e. the pilot-prompt idea pushed toward fewer pilots — the natural counterpart to DEFINED's pilot-fed assumption.
* **Multi-Task Transformer Receiver for OFDM Channel Estimation and Symbol Detection** — [NeurIPS 2025 listing](https://nips.cc/virtual/2025/loc/san-diego/123215). One transformer for both CE and detection (the "unified receiver" trend).
* **The Transformer as a Parametric Filter for Sequence MIMO Equalization** — [IEEE Xplore 11441010](https://ieeexplore.ieee.org/document/11441010) (2025/26). Frames the transformer explicitly as a *filter* rather than a classifier — useful conceptually because it maps onto the linear-filter vocabulary of a conventional chain.
* **A Unified Transformer Architecture for Low-Latency and Scalable Wireless Signal Processing** — [arXiv 2508.17960](https://ar5iv.labs.arxiv.org/html/2508.17960). Title claims low latency + scalability; its baseline set includes the *"CNN-based End-to-End Receiver: A single-user model provided as a reference implementation by the Sionna framework"* **[verified snippet]**, i.e. it benchmarks against the Sionna neural receiver.
* **A DNN-based MIMO signal detector using transformer architecture for next-generation wireless networks** — [ScienceDirect S2949715925000551](https://www.sciencedirect.com/science/article/pii/S2949715925000551). Transformer MIMO detector; the authors emphasise it *"is fully parallelizable, leveraging the self-attention mechanism"* **[verified snippet]** — the parallelism argument is the standard latency defence for attention in PHY.
* **FM-Receiver: A Foundation Model Enabled Unified Inner and Outer Neural Receiver** — [arXiv 2607.12555](https://ar5iv.labs.arxiv.org/html/2607.12555v1) (2026). Foundation-model receiver spanning inner (PHY) and outer (decoding) functions; a 2026 data point for where this line is heading.
* **Transformer-empowered receiver design of OFDM communication systems** — *Computer Communications*, 2024 — [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0140366424003074) / [ACM DL](https://dl.acm.org/doi/abs/10.1016/j.comcom.2024.107960).

---

## 2. In-context learning (ICL) for wireless: pilot prompts, prompt tuning, test-time adaptation

**The core idea in this line** is that the receiver is *conditioned* on pilot pairs at inference time
instead of updating weights: the prompt is the channel-state carrier. This is exactly the mechanism a
DEFINED-style detector uses, and the papers below are where its theory and variants live.

* **Transformers are Provably Optimal / Provably Efficient In-context Estimators for Wireless Communications** — Vaibhav Tripathi Kunde et al., **AISTATS 2025**, [PMLR v258](https://proceedings.mlr.press/v258/kunde25a.html), [PDF](https://raw.githubusercontent.com/mlresearch/v258/main/assets/kunde25a/kunde25a.pdf), earlier version [arXiv 2311.00226](https://browse.dev.arxiv.org/pdf/2311.00226). The theoretical backbone: transformers as in-context *estimators* (channel estimation flavoured), with optimality/efficiency guarantees. If the team needs a defensible "why should this generalise?" argument, this is the citation.
* **In-Context Learning for Gradient-Free Receiver Adaptation: Principles, Applications, and Theory** — Zecchin, Raviv et al., [arXiv 2506.15176](https://ar5iv.labs.arxiv.org/html/2506.15176), IEEE [Xplore 11499401](https://ieeexplore.ieee.org/abstract/document/11499401). The reference formulation of pilot-prompt ICL: *"Based on the received pilot signals and knowledge of the pilot sequences, the prompt used to estimate the data symbol R…"* **[verified snippet]**. Its selling point is adaptation to a new channel **without gradient updates** — for a gNB this is the "no online retraining" story, and the reason the approach is attractive in a testbed where you cannot fine-tune per deployment.
* **Cell-Free Multi-User MIMO Equalization via In-Context Learning** — [arXiv 2404.05538](https://export-test.arxiv.org/pdf/2404.05538v1). ICL equalisation with explicit complex→real token mapping by concatenating I and Q **[verified snippet]**; the multi-user cell-free setting is closer to a real gNB than the single-link toy setup.
* **Deep In-Context Learning (ICL) for Wireless Communications** — Lund University master's thesis, [LUP full text](https://lup.lub.lu.se/luur/download?func=downloadFile&recordOId=9235508&fileOId=9238393) / [LUP record](https://lup.lub.lu.se/student-papers/search/publication/9235508). A useful consolidated summary of the ICL-for-wireless line (training phases, prompt construction), including the two-phase ICL training recipe also described in the DEFINED preprint.
* **Test-time adaptation without weight updates (the "learn during inference" branch):**
  * *Learning During Inference: Adapting Neural Wireless Receivers Via Demodulation Pilots* — [IEEE Xplore 11586832](https://ieeexplore.ieee.org/document/11586832). Adapts the receiver at inference time using demodulation pilots — the most direct "test-time adaptation in a real receiver" framing.
  * *Asynchronous Unsupervised Online Learning of Bayesian Deep Receivers* — [NeurIPS 2025](https://neurips.cc/virtual/2025/loc/san-diego/123193). Bayesian online adaptation without labels.
  * *ChannelMAE: Self-Supervised Learning Assisted Online Adaptation of Neural Channel Estimators* — [IEEE Xplore 11571223](https://xplorestaging.ieee.org/document/11571223). Self-supervised (masked-autoencoder) online adaptation of a channel estimator.
* **Soft-prompt / prefix / prompt-tuning in wireless** — this is the **thinnest sub-area**. What exists:
  * *Adaptive Phase Shift Information Compression for IRS Systems: A Prompt Conditioned Variable Rate Framework* — [arXiv 2511.03923](https://ar5iv.labs.arxiv.org/html/2511.03923), which uses **soft prompts + FiLM conditioning** (its results table literally lists `+Soft + FiLM (proposed)`) **[verified snippet]**. It is a compression/CSI task, **not** a detector.
  * *Channel-Adaptive Wireless Image Semantic Transmission with Learnable Prompts* — [arXiv 2411.10178](https://ar5iv.labs.arxiv.org/html/2411.10178) — learnable prompt tensors `{p^e, p^d}` for semantic transmission; again not PHY detection.
  * *Large Language Models (LLMs) for Wireless Networks: An Overview from the Prompt Engineering Perspective* — [arXiv 2411.04136](https://ar5iv.labs.arxiv.org/html/2411.04136) — survey of prompt engineering for wireless, useful as the umbrella citation.
  * **Gap:** I found **no** published work that applies *learned soft prompts / prefix tuning* to a **5G-NR uplink detection chain** with a real decoder behind it. The wireless "prompt" literature is dominated by *hard* prompts made of pilot/symbol pairs (DEFINED-style) plus a few soft-prompt papers in adjacent tasks (CSI compression, semantic comms).

---

## Cross-cutting: the "measured on a real system" anchors

These are the works that already do something close to what the team proposes, and they set the baseline
the team must beat or extend.

* **Real-Time dApps for AI-RAN: Measured Interface Requirements for Inline PHY and Slot-Level Control** — [arXiv 2609.07805](https://arxiv.org/pdf/2609.07805v1.pdf) (Sept 2026). The single most on-point paper found: it measures the *interface* cost of an inline-PHY AI application, including an entry **"A-03 neural receiver to LLRs | 1.47 MB in, up to 4.4 MB out per slot; ≤500 μs occupancy"** **[verified snippet]**, and frames requirements in terms of *"the measured P99.9 of the mechanism"* across a boundary **[verified snippet]**. This is the byte-volume + slot-occupancy budget any grid→LLR model must fit.
* **The OCUDU dApp Platform: An Open Runtime and E3 Interface for Real-Time AI-RAN** — [arXiv 2609.07843](https://arxiv.org/pdf/2609.07843v1.pdf) (Sept 2026). Companion paper; reports interface latency for a "Class B direct call (suite)" of **0.29 μs P99.9 quiet; 5.2 μs with the cell on air** **[verified snippet]**. That is the *transport* overhead, not the model cost — important for separating "our model is slow" from "our plumbing is slow".
* **Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR** — IEEE [Xplore 11140048](https://ieeexplore.ieee.org/document/11140048), [NVIDIA Research](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr). Standard-compliant, real-time neural receiver for NR — the closest prior art on the *compliance* axis (fitting a learned receiver into an NR-conformant chain).
* **Practical platform prior art (OCUDU ecosystem):** [DeepSig – *Opening the Fastest Part of the 5G Radio to AI: The OCUDU dApp Platform*](https://www.deepsig.ai/ocudu-dapp-platform/); [*Building an Open Platform for AI-Native RAN with OCUDU*](https://www.deepsig.ai/building-an-open-platform-for-ai-native-ran-with-ocudu/); [AI-RAN with OCUDU and GNU Radio (GRCon)](https://events.gnuradio.org/event/28/contributions/856/contribution.pdf); [CNS RA-NN on OCUDU case study](https://ocudu.org/wp-content/uploads/sites/32/2026/08/OCUDU_CNS_Case-Study_8.7.26.pdf); [OAI workshop – Building an Open AI-RAN Platform](https://openairinterface.org/wp-content/uploads/2026/06/Summer-2026-OAI-Workshop-Matt.pdf).
* **Over-the-air neural-receiver testbeds (USRP + OAI):** [NI 5G Neural Receiver Testbed – gNB/CN side](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran), [UE side](https://github.com/EttusResearch/ni-5g-oai-neural-receiver-testbed-ran-ue), and the [Ettus KB article on the USRP X410 5G OAI neural receiver testbed](https://kb.ettus.com/index.php?title=5G_OAI_Neural_Receiver_Testbed_with_USRP_X410&oldid=6235). These are the published OTA testbeds closest to the team's plan.
* **Vendor results with measured (not simulated) gains:**
  * SoftBank: *"Boosts 5G AI-RAN Throughput by 30% with New Transformer AI"* — [press release, 21 Aug 2025](https://www.softbank.jp/en/corp/news/press/sbkk/2025/20250821_02/) (JP version [here](https://www.softbank.jp/corp/news/press/sbkk/2025/20250821_02/)), plus the technical deck [*AI-Native Mobile RAN Signal Processing: A Transformer-based Approach*](https://www.softbank.jp/en/corp/set/data/technology/research/topics/material-251002/img/pdf/softbank_transformer_webinar.pdf). Approximately +30 % 5G throughput attributed to a transformer-based radio-processing architecture, with low-latency claims. **[verified headline; the underlying measurement conditions are unverified]**
  * Rohde & Schwarz + Nokia Bell Labs: *AI-powered 6G receiver boosts performance & efficiency* — [R&S knowledge centre](https://www.rohde-schwarz.com/fr/knowledge-center/videos/ai-powered-6g-receiver-boosts-performance-efficiency_251220-1614825.html), demonstrated at MWC Barcelona 2026 (AI/AA-based 6G base-station receiver for distorted uplink signals). **[verified existence; numbers unverified]**
  * T-Mobile + Ericsson AI-RAN "world first" — [Microwave Journal](https://www.microwavejournal.com/articles/45725-t-mobile-5g-advanced-network-achieves-world-first-with-ericsson-ai-ran-innovation). Live-network AI RAN claim.
  * Keysight + MediaTek AI-driven uplink optimisation and model life-cycle management — [Keysight press release, Mar 2026](https://www.keysight.com/be/en/about/newsroom/news-releases/2026/0302_pr26-042-keysight-and-mediatek-advance-ai-driven-uplink-optimization-and-model-life-cycle-management-for-radio-access-networks.html). Notable because it is about **model life-cycle management** for AI in the RAN, which is the operational problem a testbed workstream eventually hits.
* **Standards framing:** 3GPP **TR 38.753** (Study on AI/ML for the NR air interface, V19.0.0, 2025-09) — [copy](https://whatthespec.net/friendlyspec/pdf/38753/38753-j00.pdf); Rel-18 study = TR 38.843; Rel-19 brought the first normative WI on AI/ML for the NR air interface. See also [*Toward AI-Native 6G Air Interface: A 3GPP Perspective on Protocol Framework*](https://arxiv.org/pdf/2606.27466v2) and the ETSI view in [TR 121 919](https://www.etsi.org/deliver/etsi_tr/121900_121999/121919/19.00.00_60/tr_121919v190000p.pdf).

---

## 3. Model-based deep learning for MIMO detection

The model-based / deep-unfolding line is the mature core of "AI for detection". The key question for this
team — **what comes out of the model** — has a consistent answer: symbols or hard bits, *not* calibrated
LLRs, with one important exception (IDD-style unfolding).

* **DetNet** — N. Samuel, T. Diskin, A. Wiesel. *Deep MIMO Detection*, **IEEE SPAWC 2017**
  (DOI 10.1109/SPAWC.2017.8227772, [arXiv 1706.01151](https://ar5iv.labs.arxiv.org/html/1706.01151)),
  journal version ***Learning to Detect***, **IEEE Transactions on Signal Processing, vol. 67, no. 10,
  p. 2554, 2019** ([ADS record](https://ui.adsabs.harvard.edu/abs/2019ITSP...67.2554S/abstract)).
  Unfolds a projected-gradient descent into ~30 layers with per-layer trainable parameters; each layer
  emits a **soft symbol estimate** (`q_k`, `x̂_k`) and a hard decision is taken for BER. It is a
  symbol/hard-bit detector and does **not** produce LLRs.
  **Correction to a common citation:** the companion survey could find **no** "DetNet with one-bit
  quantisation, IEEE TSP 2019" paper, nor any 2017 **IEEE SPL** DetNet paper — those two citations appear
  to conflate records. The real low-resolution model-based deep detectors are **LoRD-Net: Unfolded Deep
  Detection Network With Low-Resolution Receivers**, **IEEE TSP 2021** ([arXiv 2102.02993](https://ar5iv.labs.arxiv.org/html/2102.02993)),
  *Model-Based Deep Learning for One-Bit Compressive Sensing* ([IEEE 9187438](https://ieeexplore.ieee.org/document/9187438)),
  and *Regularized Neural Detection for One-Bit Massive MIMO* ([arXiv 2305.15543](https://ar5iv.labs.arxiv.org/html/2305.15543)).
* **OAMP-Net / OAMP-Net2** — H. He, C.-K. Wen, S. Jin, G. Y. Li. *A Model-Driven Deep Learning Network
  for MIMO Detection* (**IEEE GlobalSIP 2018** — the companion survey notes the evidence points to
  GlobalSIP rather than GLOBECOM) and ***Model-Driven Deep Learning for MIMO Detection***,
  **IEEE TSP vol. 68, p. 1702, 2020** ([arXiv 1907.09439](https://ar5iv.labs.arxiv.org/html/1907.09439)).
  Unfolds OAMP; OAMP-Net2 learns a small number of scalars (step sizes / thresholds) rather than full
  weight matrices, and the paper explicitly *"investigate[s] the number of learnable variables in
  different DL-based MIMO detectors"* **[verified snippet]** with a complexity table comparing OAMP-Net2,
  DetNet, DNN-dBP, DNN-MPD, TPG **[verified table header]**. It outputs **posterior-mean symbol
  estimates** `x̂ = E{x_i | r_i, τ_t}`; the TSP paper *does* define per-bit LLR expressions, but
  **calibration is not discussed** and the simulations are **uncoded BER with accurate CSI**.
* **MMNet** — M. Khani, M. Alizadeh, J. Hoydis, P. Fleming, ***Adaptive Neural Signal Detection for
  Massive MIMO***, **IEEE Transactions on Wireless Communications, 2020**
  ([IEEE](https://xplorestaging.ieee.org/document/9103314), [NSF PAR](https://par.nsf.gov/servlets/purl/10169226),
  [DOI 10.1109/TWC.2020.2996144](https://doi.org/10.1109/TWC.2020.2996144)); conference version
  *Exploiting Channel Locality for Adaptive Massive MIMO Signal Detection*, **ICASSP 2020**
  ([PDF](http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf)). Model-driven detector that
  exploits channel locality to cut operations per decoded symbol **[verified snippet: "how channel
  locality can help reduce the total number of operations MMNet needs to decode each received…"]**.
  Evaluated on **3GPP channel matrices**; output is soft information from an unfolded linear+denoiser
  stack, and the companion survey could **not** verify that it emits per-bit LLRs or that it uses an LDPC
  decoder.
* **Unfolded detectors that *do* work in the LLR domain (the bridge to §4):**
  **DUIDD** — *Deep-Unfolded Interleaved Detection and Decoding for MIMO Wireless Systems*
  ([arXiv 2212.07816](https://ar5iv.labs.arxiv.org/html/2212.07816)) unfolds IDD, working explicitly with
  *"the extrinsic LLR values [that] are then fed back to the data detector"* **[verified snippet]**;
  *Deep-Unfolded Iterative Soft-Input Soft-Output SIC Receiver for Coded MIMO Systems*, IEEE 2024
  ([IEEE 10757483](https://ieeexplore.ieee.org/document/10757483/citations?tabFilter=papers),
  [Aalborg record](https://vbn.aau.dk/da/publications/deep-unfolded-iterative-soft-input-soft-output-sic-receiver-for-c/));
  *Soft-Output Deep LAS Detection for Coded MIMO Systems: A Learning-Aided LLR Approximation*, Ullah &
  Choi ([Glasgow ePrints](https://eprints.gla.ac.uk/324456/)).
* **Surveys / entry points:** *Comprehensive Review of Deep Unfolding Techniques for Next-Generation
  Wireless Communication Systems* ([arXiv 2502.05952](https://ar5iv.labs.arxiv.org/html/2502.05952v2),
  Feb 2025 → 2026 revisions) — the most systematic recent survey, with cross-method comparison tables;
  *Model-Based Deep Learning*, Shlezinger, Whang, Eldar, Dimakis, **Proceedings of the IEEE 2023**
  ([arXiv 2012.08405](https://ar5iv.labs.arxiv.org/html/2012.08405)); *Model-Based Machine Learning for
  Communications* ([arXiv 2101.04726](https://ar5iv.labs.arxiv.org/html/2101.04726));
  *Deep Learning-Aided 6G Wireless Networks: A Comprehensive Survey* ([arXiv 2201.03866](https://arxiv.org/html/2201.03866v3));
  *AI-Enhanced Signal Detection and Channel Estimation for Beyond 5G and 6G*
  ([Wiley ETT](https://onlinelibrary.wiley.com/doi/full/10.1002/ett.70306), 2025/26);
  *Deep Learning in Wireless Communication Receivers: A Survey*
  ([Semantic Scholar](https://www.semanticscholar.org/paper/Deep-Learning-in-Wireless-Communication-Receivers%3A-Doha-Abdelhadi/a2254234ac5927813312f843801918122f7e56eb));
  *Foundation Models for Wireless Communications: From PHY Intelligence to Network Autonomy*
  ([arXiv 2606.06239](https://arxiv.org/abs/2606.06239), [HF papers](https://huggingface.co/papers/2606.06239));
  *Toward AI-Native 6G Air Interface: A 3GPP Perspective on Protocol Framework*
  ([arXiv 2606.27466](https://arxiv.org/pdf/2606.27466v2)).

**Bottom line for the team:** the deep-unfolding canon answers "detect symbols well", not "produce
decoder-ready soft bits". If the workstream's interface is LLRs into an NR LDPC decoder, the closest
prior art is IDD-style unfolding (DUIDD and the Sionna IDD material), not DetNet/OAMP-Net/MMNet.

**Full-text-verified corrections and additions** (from
`survey_line_a_b_output_types_llr_calibration_deep_dive.md`; [FT] = read from the paper's full text):

* **DetNet — "Learning to Detect", IEEE TSP vol. 67 no. 10 p. 2554, 2019, DOI 10.1109/TSP.2019.2899805**
  ([arXiv 1805.07631](https://arxiv.org/abs/1805.07631)). [FT] Unfolds projected gradient descent on a
  **one-hot** representation with loss `Σ_l log(l)‖x_oh − x̂_oh,l‖²`. Its §IV "Soft decision output" gives
  approximate **symbol posterior probabilities** `P(x=s|y)` — explicitly **not** per-bit LLRs — and the paper
  says additional soft inputs are left "for future work", i.e. **no decoder feedback**. Reported batch-1
  runtime: **DetNet 0.0045 s vs SDR 0.009 s, AMP 0.005 s, sphere decoding 0.001 s**. A full-text keyword scan
  found **zero** hits for "one-bit", "quantiz", "ADC", "low-resolution" — confirming the correction above.
  Soft-output quality is scored only against M-Best sphere decoding over `δ(P,Q)=Σ_s|P(s)−Q(s)|`, and is
  "comparable only at high SNR". Simulation-only.
* **OAMP-Net — IEEE GlobalSIP 2018** (not GLOBECOM), [arXiv 1809.09336](https://ar5iv.labs.arxiv.org/html/1809.09336).
  Per-layer learned step `r_t = x̂_t + γ_t W_t(y − Hx̂_t)`; 2T parameters; symbol estimates; no LLR/decoder.
* **OAMP-Net2 — IEEE TSP vol. 68 pp. 1702–1715, 2020, DOI 10.1109/TSP.2020.2976585** (not JSTSP).
  [FT] **The citation confusion has a concrete cause: arXiv 1907.09439 v1 was titled "…for Joint MIMO
  Channel Estimation and Signal Detection", and v2 was retitled "…for MIMO Detection".** Output =
  conditional mean `E{x_i|r_i,τ_t}` **plus an LLR read-out, Eq. (29)**, with an explicit soft-in/soft-out
  turbo claim — but the paper states it will "only provide the principle … specific experimental results are
  outside the scope of this paper and will be conducted in the future". **4 trainable parameters per layer
  (4T total)**. Complexity Table I: OAMP-Net2 `O(TN_t³)`, DetNet `O(TN_t²)`, DNN-dBP/DNN-MPD/TPG/LcgNet
  `O(TN_rN_t)`; learnable variables OAMP-Net2 **4T** vs DetNet `(6N_rN_t+2N_r+N_t)T`. Channels: i.i.d.
  Rayleigh, **Kronecker** correlated Rayleigh, and **3GPP 3D MIMO TR 36.873 via QuaDRiGa** (where it loses,
  because it needs unitarily-invariant channels); perfect CSI; simulation-only.
* **MMNet — IEEE TWC vol. 19 no. 8 pp. 5635–5648, 2020, DOI 10.1109/TWC.2020.2996144**
  ([arXiv 1906.04610](https://arxiv.org/abs/1906.04610)). [FT] 10 layers; per layer a learned complex
  linear operator `Θ_t` (`N_t×N_r`) plus a per-symbol noise-variance scaling `θ_t`, then a **model-based
  posterior-mean denoiser**; `2N_t(N_r+1)` parameters (20 K–41 K). **Output = soft symbols internally, then
  hard symbol decisions. A full-text scan found ZERO occurrences of LLR / log-likelihood / LDPC /
  channel-decoder, and the BER count is 0 — it is an SER-only paper.** Reported: *"same error rate as
  OAMPNet at 2.5 dB lower SNR and with at least 10× less computational complexity … 4–8 dB better overall
  than MMSE"*; `O(N_r²)` vs `O(N_r³)`. Channels: i.i.d. Gaussian + **3GPP 3D MIMO (TR 36.873) via
  QuaDRiGa** — **the common claim that MMNet assumes CDL/Kronecker is wrong; Kronecker appears only for
  prior work.**
* **CMDNet is the decisive calibration datapoint for the whole unfolding family** — IEEE TCOM 69(12):
  8214–8227, 2021 ([arXiv 2102.12756](https://ar5iv.labs.arxiv.org/html/2102.12756)). [FT] It measures that
  DetNet's "soft" output is not actually soft: *"the soft output version of DetNet should deliver accurate
  probabilities or LLRs … Indeed, we visualize with an exemplary histogram of LLRs that this is not the
  case … **DetNet mostly provides hard decisions with ∼97 % LLRs being −1 and 1**"*, adding *"In coded
  systems with soft decoders usually employed today, delivering soft information is a strict requirement."*
  CMDNet itself runs a **real 128×64 rate-1/2 LDPC decoder (BP, 10 iterations)** and reports **coded FER**.
* **An empirical LLR-calibration metric for a DetNet-family detector already exists** — Baumgartner et al.,
  [arXiv 2211.06054](https://arxiv.org/abs/2211.06054): a per-|LLR|-bin reliability curve
  `P_emp,k = (# wrong hard decisions in bin k)/(# bits in bin k)`, plus a DetNet-vs-MDetNet parameter table.
  This is the natural template for the team's own calibration study.
* **Survey corrections:** *Comprehensive Review of Deep Unfolding Techniques* ([arXiv 2502.05952](https://ar5iv.labs.arxiv.org/html/2502.05952v2))
  is a **preprint only — no journal venue exists** (v4, Jan 2026), and its open-challenge list does **not**
  mention LLR compatibility. The dedicated survey to cite instead is *Applications for Deep Unfolding
  Techniques in MIMO Wireless Communications Systems*, **IEEE Communications Magazine 2026**,
  DOI 10.1109/MCOM.001.2500444. *Model-Based Deep Learning* is **Proceedings of the IEEE 111(5):465–499, 2023**,
  DOI 10.1109/JPROC.2023.3247480; *Deep Learning-Aided 6G Wireless Networks* is **IEEE OJ-COMS 2022**,
  DOI 10.1109/OJCOMS.2022.3210648.
* **Negative result worth noting:** no 2024–2026 survey frames *decoder-compatible LLR output* as an open
  problem — the only primary-literature evidence that it is one comes from CMDNet and OAMP-Net2.

---

## 4. Receivers that output LLRs, and coding-aware / bit-wise training losses

**The canonical, quotable definition of the exact chain this team is building** (fetched and verified
directly from the Sionna documentation, not via a search snippet):

> "The neural receiver substitutes channel estimation, equalization, and demapping. It takes as input the
> post-DFT received samples, which form the received resource grid, and computes log-likelihood ratios
> (LLRs) on the transmitted coded bits. These LLRs are then fed to the outer decoder to reconstruct the
> transmitted information bits."
> — [Sionna 2.1.0, *Neural Receiver for OFDM SIMO Systems*](https://nvlabs.github.io/sionna/v2.1.0/phy/tutorials/notebooks/Neural_Receiver.html)

That is the reference formulation of "grid → LLRs → outer decoder", and Sionna is the natural baseline
provider for exactly the interface this workstream needs.

**The concrete LLR interface — numbers the team can copy** (full-text verified by the LINE A/B survey):

* **Sionna Research Kit neural demapper**: emits one LLR per coded bit, trained with
  `nn.BCEWithLogitsLoss()` (float16 in), and hands the decoder **int16 LLRs via
  `np.rint(np.ldexp(llrs, 8))` — a 2⁸ scaling**. The tutorial measures a **2.42× scale mismatch vs the
  OpenAirInterface reference** and notes *"the LDPC decoder is implemented as min-sum decoder which is known
  to be robust against mis-scaling of the LLRs"*. `LDPC5GDecoder` has an internal clipping limit
  **`llr_max = 20.0`**, `cn_update ∈ {boxplus-phi (default), boxplus, minsum, offset-minsum}`,
  `vn_update = 'sum'`, 20 iterations, float input. **[This is the single most directly reusable artifact in
  the whole survey for the team's model→LDPC boundary.]**
* **NVIDIA standard-compliant real-time NRX** — **IEEE ICMLCN 2025**, DOI 10.1109/ICMLCN64995.2025.11140048
  ([arXiv 2409.02912](https://arxiv.org/abs/2409.02912)): a CGNN over the resource grid whose
  `ReadoutLLRs` layer *"outputs LLR estimates for each UE's symbol of all REs"*, trained with **BCE between
  LLR estimates and LDPC-coded ground-truth bits** (*"the code rate and coding scheme is transparent to the
  NRX, as the NRX outputs LLRs on coded bits"*). **Weights are float16 with no QAT, and no LLR
  bit-width/saturation value is stated at the LDPC interface.** Budget numbers: 1 ms latency budget on an
  A100; **~350 µs per iteration + 270 µs overhead at 132 PRB / 2 UEs → at most 2 iterations**; the
  real-time variant has **1.4 × 10⁵ weights (2 iterations)** vs **4.4 × 10⁵ (8 iterations)** for the large
  model; **<0.7 dB SNR degradation** vs the non-real-time NRX; approaches LMMSE + K-Best at 10 % TBLER.
* **NVIDIA Aerial `LLRNet`** takes the opposite training approach — *"plugged in the PUSCH receiver chain in
  place of the conventional soft demapper"* with **MSE against target LLRs** rather than BCE — the
  LLR-regression line from **Shental & Hoydis, *"Machine LLRning": Learning to Softly Demodulate*,
  IEEE Globecom Wkshps 2019**, DOI 10.1109/GCWkshps45667.2019.9024433 ([arXiv 1907.01512](https://arxiv.org/abs/1907.01512)).
  `NVlabs/neural_rx` documents the same BCE-on-LLRs loss and an optional MSE channel-estimate loss
  (weight 0.01–0.02).
* **CENTRIC (Horizon Europe PoC)** — [Zenodo 12731570](https://zenodo.org/records/12731570): the neural
  receiver consumes the live IQ stream from the O-RU and *"output[s] the predicted LLR values that are then
  inputted into a standard-compliant LDPC decoder"*, with **BLER after LDPC** as the KPI; **no LLR
  quantisation or clipping is described**, and the headline KPIs are **"<1 dB loss vs LMMSE + K-Best"** and
  **"1 ms latency processing 132 PRBs"** on an A100 (D5.2/D5.3).

**Loss functions actually observed in LLR-output receivers** (evidence from the companion LINE B survey):

* **Bit-wise BCE on coded bits** — e.g. *Deep Learning OFDM Receivers* ([IEEE 10017176](https://ieeexplore.ieee.org/abstract/document/10017176/figures#figures#3)).
* **Soft-BCE whose gradient equals a KL divergence between the MAP posterior and the network posterior**
  ([Lund thesis](https://lup.lub.lu.se/luur/download?func=downloadFile&recordOId=9235508&fileOId=9238393#9#6)).
* **Symbol-posterior cross-entropy** — CMDNet (see below); note this is *not* the same object as bit-wise
  BCE.
* **Variational mutual-information lower bound (Donsker–Varadhan)** — Fritschek et al.
* **Maximum-achievable-rate objective** — *Neural Enhancement of Factor Graphs* ([IEEE 9903445](https://ieeexplore.ieee.org/abstract/document/9903445/figures#figures#4)).
* **Non-BCE bit loss** `L_bit(a,b) = (1−b)^a · b^(1−a)` ([Chalmers](https://research.chalmers.se/publication/513449/file/513449_Fulltext.pdf#2#2)).
* **Bit-wise MSE against true LLRs: no instance found** in the sweep — worth knowing, because it is the
  obvious first idea.

* **LLRNet** — the canonical learned LLR estimator, shipped as a worked example in MathWorks tooling:
  train a network to **estimate LLR values** directly, then check its ability to do so for higher-order
  QAM ([Deep Learning Toolbox docs](https://www.mathworks.com/help/releases/r2024b/pdf_doc/deeplearning/nnet_ug.pdf),
  [Communications Toolbox docs](https://www.mathworks.com/help/releases/r2024b/pdf_doc/comm/comm_tbx.pdf) —
  the latter contains the *"Mean Square LLR = 4.43"* style evaluation of LLR accuracy **[verified
  snippets]**). This is the practical template for "network emits LLRs, measure LLR error".
* **MathWorks / Sionna-style autoencoder with the receiver trained through the whole chain** — the
  documented training loop passes bits through encoder → channel → decoder: *"During training, you must
  first pass bits through the encoder, channel, and decoder to obtain the network output"*
  **[verified snippet, Comm. Toolbox]**, with the LLR-producing receiver trained jointly.
* **Mutual-information-based training objectives** — Fritschek, Schaefer, Wunder, *Deep Learning for
  Channel Coding via Neural Mutual Information Estimation*, **IEEE SPAWC 2019**
  ([arXiv 1903.02865](https://ar5iv.labs.arxiv.org/html/1903.02865)); and the comparison study
  *Neural Mutual Information Estimation for Channel Coding: State-of-the-Art Estimators, Analysis, and
  Performance Comparison* ([arXiv 2006.16015](https://ar5iv.labs.arxiv.org/html/2006.16015)). These are the
  principled alternatives to BCE-on-bits when what you actually care about is the *information* the soft
  values carry into the decoder.
* **Learned decoders (the symmetric half of the problem)** — Nachmani, Be'ery, Burshtein, *Deep Learning
  Methods for Improved Decoding of Linear Codes*, **IEEE JSTSP 2018**
  ([arXiv 1706.07043](https://browse.arxiv.org/abs/1706.07043v2)); Bennatan, Choukroun, Kisilev,
  *Deep Learning for Decoding of Linear Codes — A Syndrome-Based Approach* (ISIT 2018). Weighted BP is
  also available as a first-class Sionna tutorial
  ([Weighted Belief Propagation Decoding](https://nvlabs.github.io/sionna/phy/tutorials/Weighted_BP_Algorithm.html)) —
  relevant because a learnable decoder is the natural counterpart if LLR scaling cannot be fixed on the
  detector side.
* **Bit-wise optimisation of the constellation/demapper** — work on joint bit-labelling and shaping plus
  per-bit losses ([TUHH repository copy](https://tore.tuhh.de/dspace-cris-server/api/core/bitstreams/94edb85f-c09c-42d2-84a0-714b6cbe910b/content)).

**LLR calibration / scaling — what the literature actually says.**

* The field *knows* mismatched LLRs are a first-order problem, and the standard mitigation is discussed
  openly in the Sionna documentation: the BICM tutorial has a dedicated section
  **"Mismatched Demapping and the Advantages of Min-sum Decoding"**
  ([Sionna BICM tutorial](https://nvlabs.github.io/sionna/phy/tutorials/Bit_Interleaved_Coded_Modulation.html)).
  The practical takeaway encoded there is that min-sum decoding is markedly more tolerant of LLR scale
  errors than sum-product — i.e. **a receiver team can buy robustness on the decoder side instead of
  perfectly calibrating the detector**.
* **Quantizer-gain mismatch is studied directly**: *Stress Testing Quantizer-Gain Mismatch Effects on
  Neural Turbo Detection in 1-bit LS-MIMO* ([Zenodo PDF](https://www.zenodo.org/records/21103382/files/Phi_Mismatch_Stress_Tests.pdf)). This is one of very few papers whose *subject* is LLR-scale robustness of a neural detector.
* **Fixed-point representation of learned LLR estimators is a known design axis**: *Run-time Non-uniform
  Quantization for Dynamic Neural Networks in Wireless Communication*, **ASP-DAC 2024**
  ([IEEE 10473894](https://ieeexplore.ieee.org/abstract/document/10473894), [PDF](https://www.aspdac.com/aspdac2024/archive/pdf/9B-3.pdf),
  [TU/e copy](https://pure.tue.nl/ws/files/351244378/Run-time_Non-uniform_Quantization_for_Dynamic_Neural_Networks_in_Wireless_Communication_1_.pdf))
  — it works through *"the fixed point representation for the LLRNet"* and the intermediate
  representations **[verified snippet]**, i.e. exactly the "how many bits do the LLRs get, and where does
  saturation bite" question.
* Saturation/clipping guidance also appears in the SDR/quantisation literature generally (e.g. *"Parameter
  α should be chosen such that the least amount of saturation occurs for a given application, as
  oversaturation…"* **[verified snippet]**, [Polytechnique Montréal thesis](https://publications.polymtl.ca/10295/1/2022_DanielBowenDermont.pdf)).
* **The single best explicit statement found anywhere in this sweep** — *Learning Successive Interference
  Cancellation for Low-Complexity Soft-Output MIMO Detection*, 2026,
  [arXiv 2601.16586](https://arxiv.org/html/2601.16586v1), verbatim:
  > "To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to scaling and
  > clipping, which is a well-known …"

  This is the practice-norm statement the team should treat as the default: **network LLRs get scaled and
  clipped before the decoder**, and "well-calibrated" is a design *goal*, not a property that comes free.
* **LLR refinement / reconstruction as a modular stage** — *Neural Augmentation of MIMO-OFDM Receivers for
  Universal LLR Reconstruction* ([arXiv 2606.29345](https://arxiv.org/html/2606.29345v1)), journal version
  *Learning to Refine LLRs: Modular Neural Augmentation for MIMO-OFDM Receivers*
  ([IEEE 11587582](https://ieeexplore.ieee.org/document/11587582)). Rather than trusting the detector's
  LLRs, a learned module *refines* them — architecturally the most promising pattern for this workstream
  because it keeps the conventional chain as the floor and learns a correction.
* **CMDNet** — Beck, Bockelmann, Dekorsy, *CMDNet: Learning a Probabilistic Relaxation of Discrete
  Variables for Soft Detection With Low Complexity* (2021; [arXiv 2102.12756](https://ar5iv.labs.arxiv.org/html/2102.12756),
  [IEEE 9546779](https://ieeexplore.ieee.org/abstract/document/9546779),
  [software on Zenodo](https://zenodo.org/records/8416528)). A low-complexity soft-output detector whose
  paper contains the sentence *"Indeed, we visualize with an exemplary histogram of LLRs that this is not
  the case"* **[verified snippet]** — the companion survey could **not** recover the antecedent, but the
  sentence plus a dedicated "soft information measure" section in the author's dissertation make it clear
  the paper interrogates whether learned soft outputs match the true posterior. CMDNet also evaluates in
  **coded** systems (interleaved/horizontally coded), i.e. decoder in the loop.
* **Learned LLR quantisation** — *Learning Quantization in LDPC Decoders* ([arXiv 2208.05186](https://arxiv.org/pdf/2208.05186)),
  and adaptive-quantisation/companding maps with bit-width `w` and thresholds `T`
  ([IEEE 10251502](https://ieeexplore.ieee.org/ielx7/6287639/6514899/10251502.pdf)).
* **Classical mismatch machinery worth reusing** — LLR clipping for sphere decoding
  ([arXiv 1011.2113](https://ar5iv.labs.arxiv.org/html/1011.2113)); SNR-mismatch-compensated LLR-histogram
  decoding ([IET Communications](https://onlinelibrary.wiley.com/doi/pdfdirect/10.1049/iet-com.2013.0471));
  adaptive scaling-and-saturation quantisation ([Morero thesis](https://rdu.unc.edu.ar/bitstream/handle/11086/1463/Morero_Tesis.pdf)).
* **Negative evidence (a real gap, not an oversight):** targeted searches for *"temperature scaling"
  neural receiver LLR*, *"overconfident LLRs" neural network*, *"LLR calibration" learned demapper*, and
  *"mismatched LLR" neural demodulator* returned **no** paper that applies temperature scaling or an
  explicit calibration procedure to a learned demapper/receiver's LLRs. LLR calibration for learned
  receivers is therefore **not** an established, methodologically settled area — it is an open engineering
  question with only scattered practice-norm statements.

---

## 5. End-to-end receivers with the channel decoder in the loop — and the problems reported

**Foundational / canonical:**

* O'Shea & Hoydis, *An Introduction to Deep Learning for the Physical Layer* (2017) — the autoencoder
  framing of a communication link.
* O'Shea, Hoydis et al., ***Deep Learning Based Communication Over the Air***, **IEEE JSTSP 2018**
  ([copy](https://github.com/mdelrosa/wireless-ml/blob/master/Papers/2018_Dorner_DLoverAir.pdf)) — one of the
  earliest **over-the-air** deep-learning receiver demonstrations (SDR testbed), i.e. the ancestor of the
  team's plan.
* *Trainable Communication Systems: Concepts and Prototype* ([ar5iv 1911.13055](https://ar5iv.labs.arxiv.org/html/1911.13055)) —
  explicitly extends the bit-wise autoencoder **to IDD receivers** **[verified snippet]**, which is the
  clearest statement that "the decoder belongs inside the training loop".
* Sionna's *Introduction to Iterative Detection and Decoding*
  ([tutorial](https://nvlabs.github.io/sionna/phy/tutorials/Introduction_to_Iterative_Detection_and_Decoding.html))
  including a section *"Discussion — Optimizing IDD with Machine Learning"*, and the
  *Neural Receiver for OFDM SIMO Systems* tutorial
  ([tutorial](https://nvlabs.github.io/sionna/phy/tutorials/Neural_Receiver.html)); the 5G NR PUSCH neural
  receiver tutorial is the standards-shaped version
  ([Sionna 5G NR PUSCH Neural Receiver](https://nvlabs.github.io/sionna/v1.2.2/rk/tutorials/neural_receiver/),
  [NVIDIA pyAerial PUSCH neural receiver notebook](https://docs.nvidia.com/aerial/cuda-accelerated-ran/24-3/content/notebooks/example_neural_receiver.html)).

**Reported practical problems (grouped).** *This section was upgraded after the LINE C survey retrieved
full texts via `curl`: the quotes below are read from the papers, and the earlier "gap" claims have been
replaced with actual results.*

1. **LLR scale mismatch / overconfidence — explicitly documented, with verbatim quotes.**
   * *"Direct comparison of |L_R1| and |L_R3| is **not meaningful because the two LLR streams have different
     magnitude scales by construction**; each stream is therefore normalized by its own median absolute
     value."* — [arXiv 2605.26157](https://arxiv.org/pdf/2605.26157). The same paper notes the hazard for
     *"any subsequent stage that relies on the LLR scale (such as the LDPC decoder's message scheduling)"*.
   * *"the LLR magnitudes do in general not match the underlying probabilities… **Without additional LLR
     correction … 'classical' mismatched demapping can lead to severe performance degradation in forward
     error correction (FEC) decoding**."* — Wiesmayr et al., [arXiv 2409.02912](https://arxiv.org/abs/2409.02912).
   * *"recurSIC produces **raw LLRs that are not explicitly scaled by the noise variance**"*, so the
     clipping level `L_max` is set per channel model and per modulation order — empirically chosen values
     span a **~20× range** (0.12 / 0.3 / 1.7 / 2.4) — and fallback bit-flip LLRs are scaled by `α = 0.2` and
     clipped to `0.1·L_max` *"to **limit overconfident soft information**"* — Fesl & Capar,
     [arXiv 2601.16586](https://arxiv.org/html/2601.16586v1).
   * *"Conventionally trained DNN-based modules are known to produce **poorly calibrated, typically
     overconfident, decisions**"*, and — importantly — end-to-end Bayesian calibration does **not** fix
     internal modules: *"the soft estimates learned by the internal modules may still be overconfident."*
     — Raviv, Park, Simeone, Shlezinger, [arXiv 2302.02436](https://arxiv.org/abs/2302.02436).
   * Also relevant from the search-indexed layer: the Sionna BICM tutorial's section **"Mismatched Demapping
     and the Advantages of Min-sum Decoding"** ([tutorial](https://nvlabs.github.io/sionna/phy/tutorials/Bit_Interleaved_Coded_Modulation.html)) —
     fetched text confirms it analyses *"what happens for mismatched demapping, e.g., if the SNR is unknown
     and show how min-sum decoding can have practical advantages in such cases."*
2. **Calibration drift, and a per-slot fallback — the single most important paper for this testbed.**
   ***When Does a Neural Receiver Help? Calibration-Drift Benchmarking and Detect-and-Rollback for 5G/6G NR
   Uplink*** — Elnashar, [arXiv 2605.26157](https://arxiv.org/pdf/2605.26157) (May 2026). Configuration:
   16 scenarios, LDPC in the loop, MathWorks DeepRx_2M (1.23 M parameters), 26 PRB SIMO 1×2, 3.5 GHz,
   operating point = SNR at 10 % BLER. Results as measured:
   * **3/16 scenarios gain 1.0–2.0 dB** (30 ns delay spread 14.2 → 12.3 dB; 100 ns 12.3 → 10.3 dB; 200 Hz
     Doppler 12.5 → 11.6 dB);
   * **10/16 are statistical ties** (within 0.2 dB of MMSE);
   * **QPSK is ~2 dB worse** (0.4 dB vs 2.3 dB) because only 2 of 4 output channels are used and the model
     was trained 16-QAM-dominant — *"the QPSK output, while functional, is **suboptimally calibrated**"*;
   * **64-QAM is an architectural failure** in that model (4 output channels < 6 bits/symbol) — but this is a
     property of the MathWorks reference model, not of DeepRx generally (original DeepRx uses 8 channels +
     hierarchical bit masking and covers up to 256-QAM, [arXiv 2005.01494](https://arxiv.org/abs/2005.01494));
   * **DMRS AddPos=2 is out-of-distribution: BLER floors at 100 % from 4 dB upward, silently** (normal LLR
     magnitudes, no NaNs);
   * a **confidently-wrong bit fraction plateaus at ≈7 %** at high SNR under a 4:1 LLR magnitude budget, so
     no bounded additive LLR-residual correction can push BER below ~7 %;
   * at 500 Hz Doppler the **conventional** receiver collapses (BLER ≈95 %) while the neural receiver works,
     so a naive disagreement-based rollback fails there (60.5 % mean rollback rate → ≈0.61 BLER). **No single
     scalar detector resolves both failure modes.**
   * Their remedy is architectural: **run neural + conventional in parallel and arbitrate per slot before
     decoding**; the detect-and-rollback path adds **<5 % latency** over the neural receiver alone.
3. **Error floor — the gap is now closed with evidence, and the news is mixed.**
   * DeepRx MIMO (Korpi, Honkala, Huttunen, Starck, **IEEE ICC 2021**,
     [arXiv 2010.16283](https://arxiv.org/abs/2010.16283)): *"it can nearly match the uncoded BER of the
     genie-aided LMMSE up to SNRs of 14 dB, **after which it seems to encounter a BER floor**"*, and the
     conclusion flags *"the **error floor** of the proposed MIMO DeepRx, which hinders its performance at
     very low bit error rates."*
   * Counterexample for balance: [arXiv 2312.02601](https://arxiv.org/abs/2312.02601) reports a
     *significantly lower* error floor for a neural receiver.
   * Decoder-side state of the art: [arXiv 2405.13413](https://arxiv.org/abs/2405.13413) (IEEE JSAC 2025)
     notes 6G xURLLC needs FER < 1e-9 while 5G-NR LDPC has an error floor. **But no paper deliberately plots
     neural-receiver BLER down to 1e-5 as an error-floor study** — that specific measurement is still missing.
4. **Constellation-order generalisation.** Recognised and studied in adjacent work:
   *Joint Demapping of QAM and APSK Constellations Using Machine Learning*
   ([IEEE 10908647](https://ieeexplore.ieee.org/document/10908647/references)), and studies of *"the impact
   of changing the modulation format on the NN equalizer's performance"*
   ([IEEE 9523752](https://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=9523752)). The literature's consensus
   fix is **hierarchical bit masking with output width set to the maximum supported order** (DeepRx) or
   **modulation-index input embeddings** (Sionna) — i.e. do not ship a single-order network.
5. **SNR / channel-distribution generalisation and the simulation-to-field gap.**
   * **Counter-intuitive and important:** Luostari, Korpi, Honkala, Huttunen, **IEEE WCNC 2025**,
     [arXiv 2408.04182](https://arxiv.org/abs/2408.04182) — Nokia SDR over-the-air study: DeepRx models
     trained on **LOS models (TDL-D/E, CDL-D/E) "failed the over-the-air tests despite converging well
     during training"**; models trained on a **randomly mixed TDL/CDL set performed best**, and a model
     trained at simulated speeds of 0–30 m/s **beat** models trained at the actual walking speed.
     Conclusion: **broad randomisation beats parameter matching.**
   * SNR specialisation: *"a separate NN is trained for each SNR value as the trained NNs struggle to
     generalize to other SNR values"* ([arXiv 1812.05929](https://arxiv.org/abs/1812.05929)); finetuning at
     low SNR destroys high-SNR performance and produces a high error floor
     ([arXiv 1707.03384](https://arxiv.org/abs/1707.03384)).
   * **Measured disappointment for decoder-in-the-loop:** Wiesmayr, Baytekin, Dick, Studer,
     *On the Impact of Site-Specific Training for a Real-World 5G NR System*,
     [arXiv 2609.04004](https://arxiv.org/abs/2609.04004) (Sept 2026) — ETH Zurich standard-compliant 5G NR
     testbed with COTS UEs, two campaigns >6 months apart. Their **DUIDD** receiver (deep-unfolded IDD,
     30 tunable parameters, LDPC message-passing decoder in the loop) gained only **0.004 absolute dataset
     BLER** from site-specific finetuning, versus roughly a one-third BLER reduction for the decoder-free
     NRX. Worse for the "decoder in the loop" thesis: **site-specific LMMSE + classical iterative
     detection/decoding had the lowest error rate of every receiver tested.** Putting the decoder in the
     loop *reduced* what site adaptation could buy.
   * **The one strong measured win for decoder-in-the-loop:** Cammerer, Aït Aoudia, Dörner, Stark, Hoydis,
     ten Brink, *Trainable Communication Systems: Concepts and Prototype*, **IEEE TCOM 2020**,
     [arXiv 1911.13055](https://arxiv.org/abs/1911.13055) — fully differentiable neural IDD (demapper +
     802.11n LDPC BP decoder unfolded over **I = 40** iterations) trained end-to-end; over the air on two
     USRP B210s at 2.35 GHz the learned demapper gains **0.6 dB over an AWGN-MAP demapper**, and
     re-optimising the LDPC code on measured OTA EXIT curves adds **0.2 dB (m = 6) / 0.4 dB (m = 8)**.
   * DEFINED's own Rayleigh-trained → Rician-tested study (see §1.1) — the ICL-side counterpart.
   * SoftBank's trial claims ~30 % throughput gain from a transformer AI-RAN architecture
     ([press release](https://www.softbank.jp/en/corp/news/press/sbkk/2025/20250821_02/)) — a rare
     *field* datapoint, but a vendor claim without published methodology. **[headline verified, evidence
     unverified]**
6. **Training instability (solved, but non-trivially).**
   * *"It was experimentally observed that **training the end-to-end system by minimizing [the final-output
     loss] leads to poor performance**"* — the fix was a multi-loss that sums BCE at the demapper output
     over **all 40 iterations** ([arXiv 1911.13055](https://arxiv.org/abs/1911.13055)).
   * Vanishing gradients through unrolled decoders: iteration-by-iteration greedy training
     ([arXiv 2102.03828](https://arxiv.org/abs/2102.03828)); block-wise schedules with tuned block sizes
     (`Δ1 = 5`, `Δ2 = 10`; one-shot training leaves test FER stuck) ([arXiv 2405.13413](https://arxiv.org/abs/2405.13413)).
   * Non-differentiable operations: min-sum check nodes have *"non-differentiable kinks"* → subgradient
     descent ([arXiv 1706.07043](https://arxiv.org/abs/1706.07043)); Viterbi state-equality → surrogate
     cross-entropy loss ([arXiv 2009.02591](https://arxiv.org/abs/2009.02591)).
7. **Evaluation-metric warning.** *"MI alone does not fully predict BLER performance, as the latter is also
   governed by the LDPC code and graph structure"*, and an unaugmented LMMSE can have MI only marginally
   below an augmented receiver yet *"its BLER saturates at an error floor with no waterfall behavior"* —
   Eger & Shlezinger, [arXiv 2606.29345](https://arxiv.org/html/2606.29345v1). Practical rule for the team:
   **use coded BLER after the real LDPC decoder as the primary metric, never uncoded BER and never MI
   alone** — this is also DeepRx's own stated proxy for LLR quality.
8. **A direct negative result on the decoder-in-the-loop thesis.** *A Neural Receiver for 5G NR Multi-user
   MIMO* — **IEEE Globecom Wkshps 2023**, DOI 10.1109/GCWkshps58843.2023.10464486
   ([arXiv 2312.02601](https://arxiv.org/abs/2312.02601)) — uses a differentiable LDPC decoder in the
   training loop and reports that *"we empirically did not observe any gains by doing so."* Combined with the
   ETH 2026 result (site finetuning of a decoder-in-the-loop receiver bought 0.004 absolute BLER), the
   evidence for putting the decoder inside training is now **mixed-to-weak**, even though decoder-in-the-loop
   *evaluation* is essential.
9. **Numbers on the one measured decoder-in-the-loop win, stated with their baselines** — Cammerer et al.,
   **IEEE TCOM 2020**, DOI 10.1109/TCOMM.2020.3002915 ([arXiv 1911.13055](https://arxiv.org/abs/1911.13055)):
   the learned demapper *"outputs LLR and can therefore be smoothly interfaced with a channel decoder"*,
   trained with bit-wise cross-entropy, and over the air (2× USRP B210) gives **+1.3 dB vs 256-QAM with
   802.11n LDPC**; the earlier-cited **+0.6 dB** figure is the same system measured against an AWGN-MAP
   demapper instead. Both are legitimate; they answer different comparison questions.
10. **Two citation corrections in this area.** *"End-to-End Learning for OFDM: From Neural Receivers to
    Hardware Feasibility"* **does not exist** — the real paper is *"…to **Pilotless Communication**"*,
    **IEEE TWC 2021**, DOI 10.1109/TWC.2021.3101364 (≈+7 % throughput at equal BER, simulation only).
    And *Model-Free Training of End-to-End Communication Systems* (IEEE JSAC 2019) and *Towards Hardware
    Implementation of NN-based Communication Algorithms* (IEEE SPAWC 2019) contain **no LLR content**.

---

## 6. Complexity and latency of transformer receivers on real hardware

**What is reported.**

* Analytical complexity: self-attention is quadratic in prompt/sequence length; the transformer-receiver
  papers lean on **parallelisability** as the latency argument (e.g. *"the proposed approach is fully
  parallelizable, leveraging the self-attention mechanism of Transformers"*
  **[verified snippet]**, [ScienceDirect S2949715925000551](https://www.sciencedirect.com/science/article/pii/S2949715925000551)).
* Depth reduction as the latency lever: **CHOOSE** (Chain-of-Thought Enhanced Shallow Transformers) —
  [arXiv 2506.21093](https://ar5iv.labs.arxiv.org/html/2506.21093) — trades depth for a latent reasoning
  loop, explicitly motivated by shallow-model efficiency.
* **Standard-compliant real-time neural receiver for 5G NR** —
  [IEEE 11140048](https://ieeexplore.ieee.org/document/11140048),
  [NVIDIA Research](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr) —
  the strongest published claim of "this runs in real time inside an NR-conformant chain".
* **Inline-PHY AI latency budgets, measured:** the OCUDU dApp papers (see cross-cutting section) are the
  only work I found that reports *measured* interface occupancy for a neural-receiver-to-LLR function
  (**1.47 MB in / up to 4.4 MB out per slot; ≤500 μs occupancy**, [arXiv 2609.07805](https://arxiv.org/pdf/2609.07805v1.pdf))
  and *measured* transport overhead (0.29 μs P99.9 quiet, 5.2 μs with the cell on air,
  [arXiv 2609.07843](https://arxiv.org/pdf/2609.07843v1.pdf)).
* Edge/hardware comparisons exist but are scattered: e.g. a hardware-platform table reporting *average
  inference time (ms/sample)* and throughput across platforms/precisions
  ([EURASIP J. Adv. Signal Process. 2026](https://link.springer.com/content/pdf/10.1186/s13634-026-01368-2_reference.pdf)),
  an FP16-vs-FP32 datapoint of *"(FP16) inference, which reduces latency by 30 % compared with FP32 with
  negligible accuracy loss"* **[verified snippet]**, and a platform/quantisation comparison table in
  [arXiv 2607.18285](https://browse-export.arxiv.org/pdf/2607.18285). **[These are individual datapoints,
  not a coherent benchmark suite.]**
* **Real-time receiver on conventional hardware:** *Design and Real-Time Implementation of an Intelligent
  Wireless Receiver System* ([KCI](https://www.kci.go.kr/kciportal/ci/sereArticleSearch/ciSereArtiView.kci?sereArticleSearchBean.artiId=ART003304335)).

**Measured-on-hardware results (LINE 6 survey; the numbers that actually matter).**

| Work | Hardware | Reported result | Measured? |
|---|---|---|---|
| Wiesmayr/Cammerer/Hoydis et al., *Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR* ([arXiv 2409.02912](https://arxiv.org/abs/2409.02912), [IEEE 11140048](https://ieeexplore.ieee.org/document/11140048)) | NVIDIA **A100**, TensorRT | **<1 ms** inference; MU-MIMO NRX with adaptive MCS and no re-training; the speed-up cost **<0.7 dB** SNR vs the non-real-time NRX; TensorRT source released | ✅ hardware. **Caveat: the abstract contains no parameter or FLOP count** |
| Cammerer et al., *Sionna Research Kit* ([arXiv 2505.15848](https://arxiv.org/abs/2505.15848)) | **Jetson AGX Orin** | Real-time TensorRT neural receiver in a 5G NR network with **commercial UEs** | ✅ hardware (no latency figure in abstract) |
| O'Shea, Pennybacker, Kharchenko, *Real-Time dApps for AI-RAN* ([arXiv 2609.07805](https://arxiv.org/pdf/2609.07805v1.pdf)) | NVIDIA GB10 (DGX Spark) | 39 AI-RAN use cases audited against NR timing; **>half cannot cross the observer boundary**; every carrier meets a **100 µs** control deadline on an idle host, **only in-process paths still do with the DU running**; neural-receiver dApp **1.47 MB in / up to 4.4 MB out per slot, ≤500 µs occupancy** | ✅ measured |
| Same authors, *The OCUDU dApp Platform* ([arXiv 2609.07843](https://arxiv.org/pdf/2609.07843v1.pdf)) | GB10 gNB + attached handsets | Class A/B/C timing contracts; **an out-of-tree neural equalizer ran on a live cell with zero fallbacks**, 7–11 pp lower first-transmission BLER at MCS 11–15; Class B direct call 0.29 µs P99.9 quiet / 5.2 µs with cell on air; receiver kernels 82 µs P50 / 112 µs P99.9; Class A budgets 100 µs (estimation) + 150 µs (completion) | ✅ measured |
| *Six Times to Spare* ([arXiv 2602.04652](https://arxiv.org/abs/2602.04652)) | DGX Spark Grace **CPU vs GB10 GPU** | LDPC decoding on the Grace CPU = **0.71 ms/codeword at 20 iterations — exceeds the 0.5 ms slot**; on the GPU **6–24 % of the slot**; ~6× speed-up; **+10–15 W** over GPU idle; the CPU version consumes ~10 Grace cores | ✅ measured — the cleanest TTI-budget calibration found |
| HELENA, LEO-NTN receiver ([arXiv 2506.13408](https://arxiv.org/abs/2506.13408), [arXiv 2609.14735](https://arxiv.org/abs/2609.14735)) | RTX PRO 4500 vs **10 W Jetson Orin NX** | 0.175 ms vs 0.318 ms ViT baseline, **8× fewer parameters** (0.11 M vs 0.88 M); on the RTX PRO 4500 **0.0595 ms P99 (88.1 % under the 0.5 ms budget)**, but **on the Jetson Orin NX no model meets the P99 0.5 ms budget** | ✅ measured — the sharpest evidence that **tail latency, not mean, is the gate** |
| *Computationally Efficient Neural Receivers via Axial Self-Attention* ([arXiv 2510.12941](https://arxiv.org/abs/2510.12941)) | — | Explicit complexity reduction **O((TF)²) → O(T²F + TF²)**; "a fraction of the parameters" vs CNN baselines; beats global attention/CNN/LS-LMMSE at 10 % BLER on CDL-C | ❌ simulated only |
| FPGA/ASIC accelerator line | Zynq UltraScale+ / RFSoC / 28 nm / 12 nm | Massive-MIMO localization transformer accelerator **0.51–2.11 ms, 1961 pos/s**; SwiftChannel RFSoC **sub-ms, 24× speedup and >33× energy efficiency vs GPU**; DL channel estimation on Zynq **88–90 % lower execution time**; attention-CNN RF recognition **98 µs/frame**; 28 nm DL MIMO detector **5.76 Gb/s, 79.7 pJ/b**; 12 nm transformer accelerator **18.1 TFLOPS/W**; analog NN receiver **21.3 TOPS/W** | ✅ mostly measured |
| Quantisation studies (all Yellapagada/Ollila/Costa) | — | PTQ: 8-bit per-channel ≈ no loss, 4-bit needs work. QAT: 4-bit and 8-bit match FP32 at 10 % BLER, **8× compression**. **FP microformats: 8-bit within 0.05 dB of FP32; INT4 loses 3.3–3.7 dB and falls *below* LS-LMMSE, while FP4 (E2M1) halves the loss to 1.3–1.4 dB and stays ~0.5 dB above LS-LMMSE even after 50 % pruning** | ❌ simulated — but the most actionable design rule found: **format matters more than bit-width; INT4 is the wrong choice, FP4 is viable** |

**Two methodology warnings from this line.**

* **AtlasRAN** ([arXiv 2603.14661](https://arxiv.org/abs/2603.14661)): a reported goodput collapse from 1→12
  users turned out to be **harness time-scale dilation / I/O starvation, not decoder saturation**. Read
  every "the receiver is the bottleneck" claim with that in mind.
* **The only "low-cost transformer receiver" headline in this space is unquantified.** CHOOSE
  ([arXiv 2506.21093](https://arxiv.org/abs/2506.21093)) claims 1–2 layer transformers match much deeper
  ones via latent CoT, but its abstract carries **no parameter count, no FLOPs, no latency, and no
  hardware** — it is simulation-only with qualitative efficiency claims. Do not use it to justify a
  hardware design without reading the full text.

**What is missing.** I found **no** paper that reports a measured per-slot inference latency distribution
(P99/P99.9) of a *transformer-based detector* inside a live 5G NR gNB. The decisive pattern in this line:
**every paper with a real measured in-budget per-slot latency uses a CNN or a compact attention/conv
hybrid, not a full transformer**; where transformers *are* measured, the task is localization or channel
estimation rather than detection. Complexity claims in the transformer-receiver literature remain
overwhelmingly **analytical or desktop-GPU**.

---

## 7. Apple Silicon (CoreML / ANE / Metal) for real-time wireless

**The headline: this intersection is essentially empty in the published literature.** I could not find any
paper that runs a wireless PHY receiver or detector on Apple Silicon's GPU/ANE, let alone one with
measured per-slot latency. That absence is the strongest "novel contribution" argument available to this
workstream — but it also means there is no prior art to lean on for CoreML-specific pitfalls, so they must
be measured locally.

**Adjacent, citable Apple-Silicon signal-processing work (all Apple Silicon, none of it wireless PHY):**

* ***Range, Not Precision: Block-Floating-Point Half-Precision FFT and SAR Imaging on Apple Silicon*** —
  [arXiv 2605.28451](https://export.arxiv.org/pdf/2605.28451). Half-precision FFT + SAR imaging on Apple
  Silicon with block-floating-point to manage fp16's limited mantissa. Directly relevant: **fp16 range vs
  precision is the CoreML/ANE constraint that will hit a receiver's LLRs and correlation sums**, and this
  paper is the closest thing to a methodological precedent.
* ***Bandwidth, Not FLOPS: FFT Kernels, Matrix Units and SAR Imaging on Apple M6*** —
  [arXiv 2609.32237](https://ar5iv.labs.arxiv.org/html/2609.32237v1), [scirate](https://scirate.com/arxiv/2609.32237).
  Argues Apple-Silicon GPU DSP is **memory-bandwidth-bound, not FLOP-bound** — a first-order design fact
  for anyone planning to stream a resource grid through GPU kernels.
* **CyberEther** — a GPU-accelerated signal-processing/SDR framework with a Metal backend on Apple
  Silicon: [GNU Radio Conference talk (2024)](https://luigi.ltd/content/files/2025/12/CyberEther-GNU-Radio-2024.pdf),
  [GRCon 2022 slides](https://events.gnuradio.org/event/18/contributions/241/attachments/97/184/GNU%20Radio%20Conference%202022%20-%20CyberEther.pdf),
  [GNU Radio ↔ CyberEther interoperability proposal](https://lists.gnu.org/archive/html/discuss-gnuradio/2026-03/pdf8fqda6VUgM.pdf),
  [repository](https://github.com/codemasters1/CyberEther). This is the nearest published art for
  "Metal compute for real-time RF DSP on a Mac".
* **Apple-Silicon LLM inference studies** — useful only for methodology and for order-of-magnitude
  expectations: an ACM paper motivated by the observation that *"A lack of systematic understanding of LLM
  inference on Apple Silicon exists — prior work has primarily focused on LLM train[ing]…"*
  ([ACM DL 10.1145/3779314](https://dl.acm.org/doi/pdf/10.1145/3779314)), plus community benchmark
  collections ([vllm-mlx benchmarks](https://github.com/waybarrios/vllm-mlx/blob/HEAD/docs/benchmarks/llm.md),
  [MLX quantisation benchmark discussion](https://huggingface.co/allenai/tmax-9b/discussions/2)).
**Apple-Silicon numbers that decide the architecture** (gathered by a parallel survey pass; the two ANE
papers below were corroborated by independent search hits — [HF papers entry for ANEForge](https://huggingface.co/papers/2606.17090), [press coverage of the ANE study](https://iphonesoft.fr/2026/08/06/neural-engine-apple-plus-secrets-etude-universitaire) — but I did **not** read the PDFs myself):

* **The only published Apple-Silicon timing for a neural receiver that exists** — and it is CPU-only:
  [arXiv 2605.26157](https://arxiv.org/pdf/2605.26157) measures a **Mac Studio M3 Ultra in CPU mode**
  running a 1.23 M-parameter DeepRx_2M forward pass at **72.10 ms/slot**, with LDPC decoding at 9.12 ms and
  the full neural pipeline at **81.23 ms/slot**. On an RTX 6000 GPU the same forward pass is 24.10 ms. The
  paper states this is *"still well above the 1 ms 5G slot budget at 15 kHz SCS"*. Compute ratio: ~1.5
  GFLOPs/slot at 26 PRB ≈ **~30× the FLOPs of MMSE equalisation**. For contrast, NVIDIA reached **<1 ms on
  an A100 with TensorRT** ([arXiv 2409.02912](https://arxiv.org/abs/2409.02912)) — but only after shrinking
  the model, at a cost of **<0.7 dB** SNR. **No CoreML/Metal/ANE number for any neural receiver exists
  publicly.** This is simultaneously the biggest opportunity and the biggest unmeasured risk in the plan:
  the gap between 72 ms (CPU, unoptimised) and a slot budget is exactly what a Metal/CoreML implementation
  would have to close, and nobody has published whether it can.
* **ANE dispatch floor ≈ 0.23 ms per evaluation on M1** — even a ReLU, a 64-element linear layer or a
  small conv costs 0.23–0.26 ms; on an **M5 Pro the floor is ≈ 70 μs** (≈ 90 μs for a small fused
  program), per *Apple Neural Engine: Architecture, Programming, and Performance* (Bryngelson, Jun 2026,
  ~302 pp; [arXiv 2606.22283](https://arxiv.org/abs/2606.22283)) and *ANEForge: Python for direct
  computation on the Apple Neural Engine* ([arXiv 2606.17090](https://arxiv.org/abs/2606.17090)).
  **Consequence: on M1-class hardware a single ANE dispatch eats ~46 % of a 500 μs slot; the ANE is only
  viable as one fused, chunked program — the GPU path (Metal/MPS) is the realistic option, with the ANE
  as an optional offload for a single large fused stage.**
* **ANE working-set cliff ≈ 2 MB (M1) / 4.72 MB (M5)**: above it, throughput falls off the ~12 TFLOP/s
  fp16 compute roof onto the ~85 GB/s bandwidth roof (ridge ≈ 141 FLOP/byte on M1). A 273-PRB × 14-symbol
  × 4-port complex-fp16 grid is only ≈ 0.12 MB, but a transformer over resource elements crosses 2 MB
  quickly → **chunk by symbol/subband**.
* **The OCUDU dApp runtime is CUDA-only today.** [arXiv 2609.07843](https://arxiv.org/pdf/2609.07843v1.pdf)
  declares backends as *"CPU (x86, ARM) or CUDA"*; there is **no Metal/ANE/CoreML backend** in the platform
  or SDK, and the dApp seams are not in this checkout — the platform is a separate preview repo
  (`gitlab.com/ocudu/work_groups/wg2_ai_ran`). It defines three timing contracts: **Class A** = resident on
  the GPU receive chain (zero-copy device tensors; budgets quoted as 100 μs for estimation / 150 μs to
  completion), **Class B** = inside the scheduler's admitted 100 μs deadline, **Class C** = never-blocking
  observer. Measured on an NVIDIA GB10 (DGX Spark, Arm) host: Class B direct call 0.29 μs P50 / 5.2 μs
  P99.9 with the cell on air; 68 KB ring publish 2.3 μs P99.9; **receiver kernels 82 μs P50 / 112 μs
  P99.9**; grid copy 50–100 μs per slot for 273 PRB 4-port. **[Reported to me by the parallel survey pass
  from the paper text; internally consistent with the snippets I verified myself.]**
* **Timing context for a real gNB (3GPP-quoted by the LINE 7 survey):** [TS 38.211](https://www.etsi.org/deliver/etsi_ts/138200_138299/138211/) Table 4.2-1 numerologies — 10 ms frame, 1 ms subframe, 14 symbols/slot, slot = **1 ms (15 kHz), 0.5 ms (30 kHz), 0.25 ms (60 kHz), 0.125 ms (120 kHz)**; OFDM symbol ≈71.4 µs at 15 kHz, ≈35.7 µs at 30 kHz. [TR 38.913](https://www.etsi.org/deliver/etsi_tr/138900_138999/138913/) URLLC = 0.5 ms UL + 0.5 ms DL; [TS 38.213](https://www.etsi.org/deliver/etsi_ts/138200_138299/138213/) §9.2.3 makes `k1` a **slot count** via `dl-DataToUL-ACK` (k1 = 4 at 30 kHz = 2 ms). **Working budget used by the team's own analysis: 500 µs slot − ~100–150 µs conventional processing tail = ~100–150 µs neural allowance ≈ 7–11 µs/symbol amortised — versus a 70–230 µs ANE dispatch floor.** Hence: *one fused ANE dispatch per slot is the only viable ANE shape*; the practical engine is Metal/MPS.
* **Core ML API facts that constrain the design** (LINE 7 survey, verified against Apple's DocC JSON API and `apple.github.io/coremltools`):
  `MLComputeUnits = {cpuOnly, cpuAndGPU, cpuAndNeuralEngine, all}`; Apple's own wording is *"Use `all` to
  allow the OS to select the best processing unit (including the neural engine, if available)"* — there is
  **no ANE-only mode and no runtime API reporting which unit actually ran**. `MLComputePlan` gives per-op
  `deviceUsage()` and `estimatedCost()`, but these are **offline estimates, not guarantees**. Device
  specialisation ("may take a few seconds or even minutes") is **cached keyed to the `mlmodelc` path** —
  move the folder and you pay again (Apple's fix: `CompiledMLModel` / a pinned `.mlmodelc`).
  `EnumeratedShapes` (≤128) is the ANE-blessed flexible-shape path; an unbounded `RangeDim` is **rejected**
  for `mlprogram`; `reshapeFrequency = .infrequent` unlocks ANE for flexible shapes on iOS 17.4+. Apple's
  FAQ warns conversion can **silently** *"introduce dynamic layers not supported on the NE, such as
  converting a static reshape to a fully dynamic reshape"*, and Apple's own Stable-Diffusion conversion
  **pads to a static 77-token envelope** — i.e. pad-and-waste is the sanctioned pattern.
* **Custom Metal kernels and ANE residency are mutually exclusive inside one Core ML model.**
  `MLCustomLayer`/custom ops provide **CPU + GPU implementations only**, and Apple advises using them
  "only as a last resort". Hand-written Metal and ANE cannot coexist in one graph — this is a hard
  architectural fork for the team.
* **Core ML/ANE performance numbers** (LINE 7 survey; sources tagged in
  `docs/line7_apple_silicon_inference_survey_english.md`): whisper.cpp encoder, **ANE vs CoreML vs Metal** —
  tiny 5.7 / 11.2 / 7.3 ms, base 12.2 / 23.0 / 13.5 ms, small 40.3 / 77.2 / 40.9 ms, medium
  117.6 / 236 / 120 ms (direct ANE ≈2× CoreML at every size). Apple-published: **DistilBERT seq-128 batch-1
  at 3.47 ms / 0.454 W on an iPhone 13 ANE**; W8A8 int8 vs fp16 ResNet-50 on A17 Pro **1.38 → 0.77 ms**.
  Cross-runtime on an M2 Ultra: **MLX ≈230 > MLC ≈190 > llama.cpp ≈150 > Ollama 20–40 > PyTorch MPS ≈7–9
  tok/s**. Same-harness Qwen3.5-0.8B on M4 Max: **MLX TTFT 43 ms vs CoreML/ANE 526 ms** (and ANE model load
  13.2 s vs MLX 1.03 s). 100 % ANE residency *is* achievable (op audits 1058/1058 etc.) at ~30 ms/token.
* **The fp16 range problem, which is the top numerical risk for LLRs.** On the ANE, fp16 activations
  outside roughly [1e-4, 1e2] lose precision and tiny values flush to zero; the binding constraint is
  **exponent range, not mantissa**. Apple-Silicon fp16 FFT achieves 56–61 dB SQNR, but a naïve fp16 SAR
  pipeline produced **only NaNs** because intermediates reached 5e6 ≫ 65504 — fixed by a `1/N`
  block-floating-point scale ([arXiv 2605.28451](https://export.arxiv.org/pdf/2605.28451)). Apple GPUs have
  **no native FP8 datapath** (FP8 collapses to 14–20 dB SQNR). **Implication for this workstream
  (unpublished anywhere): clamp/tanh the LLR head inside the model, never let values approach 65504, and
  consider a block-floating-point output scale — and note that nobody has published an LLR-vs-precision
  curve.**
* **Jitter, not mean, decides feasibility — with a hard datapoint.** The DAFx-26 study of real-time neural
  inference inside a real audio callback on an M3 (2.67 ms deadline) found an isolated-RTF-0.211 model hit
  **p99 = 158.5 % of its deadline with 355 xruns**, while an AOT-compiled **BNNSGraph** held **p99 = 14.3 %
  with 0 xruns** under maximum contention; BNNSGraph is RealtimeSanitizer-verified (no allocation, locks or
  syscalls) and supports dynamic shapes without recompiling. **Practical rule: budget and report P99.9 per
  slot, not the mean.**
* **Apple-Silicon SDR prior art is qualitative, not quantitative.** srsRAN 4G v25.10 + GNU Radio 3.10.12 +
  UHD 4.10 do run natively on macOS ARM64 (MacBook Pro M4 + LibreSDR B220) — but only after 21 libosmocore
  patches, a 1150-line SCTP shim and 23 srsRAN patches, and **no latency or sample-rate numbers were
  published**. There is nothing for a 5G gNB on macOS. Metal real-time DSP has one strong datapoint: a
  kernel-fused SAR single-dispatch of 4096² in **370 ms on an M1 GPU (22× over the multi-dispatch
  version)**, at 138 GFLOPS fp32 / 306 GFLOPS fp16 radix-8 FFT — **fusion is where the speed-up lives**.
* **The single closest published wireless-on-ANE item** is a GNSS interference-classification paper
  (ION GNSS+ 2026 / IEEE 11683190) — paywalled during this survey, no number extracted. Everything else in
  "wireless on Apple Silicon" is empty.
* **Open uncertainty that could date this section:** several 2026 community sources describe a **"Core AI"**
  framework as Core ML's successor, but this could not be confirmed against any Apple page. If real, the
  Core ML specifics above may be one generation stale.

---

## 8. Synthesis

### 8.1 What is established

1. **In-context / prompt-based detection is a real research line with theory behind it, and DEFINED is now
   fully characterised.** Pilot pairs as a prompt, adaptation without weight updates, and provable
   in-context estimation are all published ([DEFINED/ICC 2025](https://ieeexplore.ieee.org/document/11161684),
   [ICL gradient-free adaptation](https://ar5iv.labs.arxiv.org/html/2506.15176),
   [AISTATS 2025](https://proceedings.mlr.press/v258/kunde25a.html)). DEFINED itself: a 0.42 M-parameter
   8-layer GPT-2-style classifier, receiver-only, backward compatible with the existing frame structure,
   using previously detected symbols as pseudo-pilot pairs; **+19.3 % to +62.6 % SER reduction from decision
   feedback across BPSK→64QAM, +50.2 % in 2×2 MIMO, +67.8 % under Rayleigh→Rician mismatch**, and 2 pilots
   landing within ~10 % relative SER of a 30-pilot MMSE estimator. **But it is simulation-only, has no
   latency measurement, and its decision-feedback mechanism has no theory yet** (the authors say so).
2. **Model-based DL detectors are mature but symbol-oriented.** DetNet/OAMP-Net2/MMNet output symbols or
   hard bits; the LLR-native variant of this family is IDD-style unfolding (DUIDD, deep-unfolded SISO SIC,
   soft-output deep LAS). Note the corrected citations: DetNet's 2017 venue is **SPAWC**, not SPL, and there
   is **no** "one-bit DetNet, TSP 2019" paper.
3. **LLR mismatch is a named, documented problem with concrete practice norms** — median normalisation,
   per-modulation clipping levels spanning ~20×, scaling factors, learned LLR quantisation, LLR refinement
   modules, and min-sum's documented robustness advantage. What does **not** exist is a principled
   calibration method (temperature scaling or equivalent) for a learned receiver's LLRs.
4. **Inline-PHY AI with a real LLR interface has been engineered and measured on an open 5G stack**: byte
   volumes per slot (1.47 MB in / up to 4.4 MB out), ≤500 µs occupancy, and an **out-of-tree neural
   equalizer running on a live GB10 cell with zero fallbacks**. Standard-compliant real-time neural receivers
   exist (<1 ms on an A100 via TensorRT, at a <0.7 dB SNR cost).
5. **Decoder-in-the-loop has exactly one strong measured win** (Cammerer et al., IEEE TCOM 2020: 0.6 dB over
   an AWGN-MAP demapper over the air) **and one measured disappointment** (ETH Zurich 2026: 0.004 absolute
   BLER from site-specific finetuning, with a classical LMMSE + IDD chain achieving the lowest error rate of
   every receiver tested).
6. **Quantisation guidance is actionable**: 8-bit is essentially free (within 0.05 dB of FP32), INT4 is
   *worse than useless* (−3.3 to −3.7 dB, below LS-LMMSE), and **FP4 (E2M1) is the viable 4-bit format**.

### 8.2 What is contested

1. **Whether learned/ICL receivers beat a well-tuned conventional chain once the decoder is in the loop.**
   Most gains are reported at uncoded BER or 1e-2–1e-3 BLER. The 2026 calibration-drift study found only
   3/16 scenarios with a 1–2 dB gain, 10/16 statistical ties, QPSK ~2 dB *worse*, and a silently broken
   out-of-distribution DMRS configuration; the ETH study found the classical chain winning outright. The
   mainstream deployment answer is now **a per-slot trust/rollback decision against a conventional
   receiver**, not a wholesale replacement.
2. **Generalisation**, across SNR, modulation order, channel model and site — with genuinely conflicting
   evidence: DEFINED is robust to Rayleigh→Rician, Nokia found LOS-trained models *failing over the air*
   while broad randomisation won, and ETH found site-specific finetuning barely helped a decoder-in-the-loop
   receiver.
3. **Transformers vs CNNs/compact hybrids at equal *measured* latency.** Every in-budget measured PHY
   receiver found uses a CNN or attention/conv hybrid; transformer-receiver papers argue parallelisability
   but almost never normalise against a CNN baseline at the same measured latency.
4. **Where the LLRs should be produced** — inside the model (bit-wise losses, calibration burden) or derived
   analytically from a symbol estimate. Related: **MI/uncoded-BER are not valid proxies** — BLER can sit at
   an error floor while MI is nearly optimal.
5. **Whether the ≤1 ms-class real-time results transfer to a full-band, high-order-MIMO NR carrier** — they
   were obtained at small bandwidths (e.g. 26 PRB), with model shrinkage, and in some cases on a single
   architecture.

### 8.3 Where the literature is thin — and what this workstream would add

| Gap | Status in literature | What a hardware-measured OCUDU + Apple-Silicon implementation contributes |
|---|---|---|
| Apple-Silicon GPU/ANE for real-time PHY | **Nothing.** The only published Apple timing for a neural receiver is **CPU-mode M3 Ultra at 72.1 ms/slot** (vs a 1 ms budget); no Metal/ANE/CoreML number exists for any wireless PHY workload, and the OCUDU dApp runtime is **CUDA-only** | First measured per-slot P99 latency/throughput for a grid→LLR model on Metal/CoreML in a live gNB, including the fp16-exponent-range behaviour of LLRs and the fused-kernel vs per-op gap |
| Per-slot latency of a *transformer* detector in a running RAN | Only analytical/desktop-GPU, or CNN hybrids; dApp papers measure plumbing, not the model. No P99/P99.9 distributions anywhere | A deadline-verified **tail**-latency number (P99.9 occupancy per slot) and the latency/accuracy Pareto for shallow vs deep, prompt vs non-prompt |
| ICL/prompt detectors under a **fixed** NR DMRS budget | DEFINED is pilot-fed by design and assumes the pilot set can be used as a prompt; no standard-compliant study found. Its 31-symbol block-fading frame is **not** an NR slot with HARQ | Evidence on whether prompt-based detection survives the DMRS density NR actually grants, with the real LDPC decoder behind it |
| LLR calibration for learned receivers | **Genuine gap**: clipping/normalisation practice exists, but no temperature-scaling or principled calibration study, and no published **LLR-vs-precision** curve | A calibration study (temperature scaling, clipping limits, bit-width, fp16 vs fp32) with decoder-in-the-loop BLER numbers |
| **Decoder-in-the-loop BLER at 1e-5 / error floors** | Partially closed: DeepRx MIMO documents a BER floor at ~14 dB, and a confidently-wrong fraction plateau of ~7 % bounds any bounded LLR correction — but nobody plots neural-receiver BLER to 1e-5 as an error-floor study | Measured BLER/error-floor curves through the real NR LDPC decoder at NR operating points, including HARQ |
| Trust/fallback engineering (drift detection, rollback) | One 2026 paper (calibration drift + detect-and-rollback, <5 % added latency) | Independent reproduction on different hardware/model family; a deployable per-slot fallback design following the OCUDU Class-A contract |
| Modulation/SNR generalisation under a real MCS schedule | DeepRx-style hierarchical bit masking and modulation embeddings exist; the failure modes are documented but not solved | Measured evidence across the actual MCS/modulation schedule a live cell uses, not a fixed-order benchmark |
| Energy/thermals of AI PHY on a workstation-class device | Nothing found for PHY; GPU-idle-to-load delta (+10–15 W) is the only nearby datapoint | Power/thermal measurements — decisive for any "AI on the gNB host" argument |

**The one-line pitch:** the literature already has (a) transformer/ICL detectors that work in simulation,
(b) deep-unfolded detectors that talk to a decoder via extrinsic LLRs, and (c) a single measured
inline-PHY latency study on an open 5G stack. What does **not** exist is a **hardware-measured,
real-time, standard-compliant grid→LLR transformer running on Apple Silicon in an over-the-air gNB with
the real LDPC decoder in the loop and honest calibration/error-floor numbers**. That combination — not the
model architecture — is the contribution.

---

## Appendix: secondary and practitioner sources consulted

* [3GPP TR 38.753 (AI/ML for NR air interface) copy](https://whatthespec.net/friendlyspec/pdf/38753/38753-j00.pdf) · [ETSI TR 121 919](https://www.etsi.org/deliver/etsi_tr/121900_121999/121919/19.00.00_60/tr_121919v190000p.pdf) · [AI/ML life-cycle management for AI-native RAN](https://ar5iv.labs.arxiv.org/html/2507.18538v3)
* [NVIDIA AI Aerial: AI-Native Wireless Communications](https://browse-export.arxiv.org/pdf/2510.01533) · [Aerial cuPHY docs](https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/aerial-cuda-accelerated-ran.pdf)
* [CENTRIC D5.3 testing/benchmarking methodology (5G PUSCH MU-MIMO baseline incl. LMMSE + K-Best)](https://centric-sns.eu/wp-content/uploads/2025/03/d5.3-kpiskvis-testing-methodologies-and-benchmarking.pdf)
* [OCUDU user manual](https://docs.ocudu.org/pdf/ocudu-user-manual.pdf) · [OCUDU tutorials](https://docs.ocudu.org/pdf/ocudu-tutorials.pdf) · [OCUDU knowledge base](https://docs.ocudu.org/pdf/ocudu-knowledge-base.pdf)
* [SpikingRx: From Neural to Spiking Receiver](https://ar5iv.labs.arxiv.org/html/2409.05610) (energy-efficient spiking alternative)
* [Fugumt machine-translation index entries](https://fugumt.com/fugumt/paper_check/2506.15176v2_enmode) (used only as corroboration of titles)

