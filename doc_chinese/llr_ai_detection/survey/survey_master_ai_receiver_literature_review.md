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

**Identity and versions.** The paper the team asked about has been renamed between versions:

| Version | Title | Where |
|---|---|---|
| v1 (originally posted) | *Decision Feedback In-Context Learning for Wireless Symbol Detection* | [arXiv 2503.16594](https://arxiv.org/abs/2503.16594) |
| v2 (current) | *Transformer-based Wireless Symbol Detection Over Fading Channels* | [arXiv HTML v2](https://arxiv.org/html/2503.16594v2), [ar5iv](https://ar5iv.labs.arxiv.org/html/2503.16594) |
| Preliminary version | *Decision Feedback In-Context Symbol Detection Over Block-Fading Channels* — **IEEE ICC 2025** | [IEEE Xplore 11161684](https://ieeexplore.ieee.org/document/11161684), [arXiv 2411.07600](https://ar5iv.labs.arxiv.org/html/2411.07600), [NSF PAR manuscript](https://par.nsf.gov//servlets/purl/10631659), [SpectrumX](https://www.spectrumx.org/publication/decision-feedback-in-context-symbol-detection-over-block-fading-channels/) |

Authors are at the Charles L. Brown Department of ECE, **University of Virginia** (Li Fan, Jing Yang,
Cong Shen and one further co-author — the paper's footnote lists four UVA e-mail addresses:
`lf2by, zyy5hb, yangjing, cong@virginia.edu`). **Simulation code is public:
`https://github.com/ShenGroup/DEFINED`.** The venue/author/code facts are verified from the paper's own
indexed footnote text; the v1→v2 title change and the ICC 2025 preliminary-version mapping are verified
from the ADS/Semantic Scholar/NSF-PAR records.

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

**Reported practical problems (grouped).**

1. **LLR scale mismatch / overconfidence.** Treated as an expected engineering hazard rather than a
   surprise: see the min-sum-vs-sum-product discussion under "Mismatched Demapping" in the
   [Sionna BICM tutorial](https://nvlabs.github.io/sionna/phy/tutorials/Bit_Interleaved_Coded_Modulation.html),
   the [quantizer-gain mismatch stress tests](https://www.zenodo.org/records/21103382/files/Phi_Mismatch_Stress_Tests.pdf),
   and the fixed-point LLRNet treatment in [ASP-DAC 2024](https://www.aspdac.com/aspdac2024/archive/pdf/9B-3.pdf).
2. **Calibration drift and the need for a fallback.** The most important recent paper for this workstream:
   ***When Does a Neural Receiver Help? Calibration-Drift Benchmarking and Detect-and-Rollback for 5G/6G
   NR Uplink*** ([arXiv 2605.26157](https://arxiv.org/pdf/2605.26157), [scirate](https://scirate.com/arxiv/2605.26157),
   [Semantic Scholar](https://www.semanticscholar.org/paper/2ac76e9257e6a1185d5aa28b9360781ee2461ca9)). It
   benchmarks neural receivers on NR uplink, characterises **calibration drift**, and proposes a
   detect-and-roll-back scheme; the indexed text reports *"The 60.5 % mean rollback rate (Table IV)
   reflects that R5 trusts R1 on the majority of slots — slots on which R1 has collaps[ed]…"*
   **[verified snippet]**. Read this as: *in practice a neural receiver is paired with a classical
   receiver and a trust decision per slot.* That is a design requirement for the team's testbed, not a
   detail.
3. **Error floor / behaviour at low BLER.** I found **no** paper that clearly documents a neural-receiver
   error floor at BLER 1e-5 with an LDPC decoder in the loop; the literature mostly plots BER/BLER down to
   ~1e-3–1e-4. This is a **gap**, and it matters because an NR gNB operates at 10 % BLER targets with HARQ
   and cares about residual error behaviour. **[gap claim — see §0]**
4. **Constellation-order generalisation.** Recognised and studied in adjacent work:
   *Joint Demapping of QAM and APSK Constellations Using Machine Learning*
   ([IEEE 10908647](https://ieeexplore.ieee.org/document/10908647/references)), and studies of *"the impact
   of changing the modulation format on the NN equalizer's performance"*
   ([IEEE 9523752](https://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=9523752)) **[verified snippets]**.
   In NR uplink the MCS/modulation order changes every few slots, so a detector that is only valid for one
   QAM order is not deployable without re-selection logic.
5. **SNR / channel-distribution generalisation and the simulation-to-field gap.**
   * DEFINED's own Rayleigh-trained → Rician-tested study (see §1.1) **[verified]**.
   * Sites-specific training for a real 5G NR system: ***On the Impact of Site-Specific Training for a
     Real-World 5G NR System*** ([arXiv 2609.04004](https://scirate.com/arxiv/2609.04004),
     [PDF](https://export.arxiv.org/pdf/2609.04004)) — a measured, real-system study (NVIDIA-sponsored)
     of how much a neural receiver depends on the deployment site's channel statistics.
   * *AirNet: Neural Network Transmission over the Air* ([ar5iv 2105.11166](https://ar5iv.labs.arxiv.org/html/2105.11166))
     states the SNR-generalisation problem bluntly: *"how to train the network when we do not know the
     channel SNR in advance"* **[verified snippet]**.
   * SoftBank's trial claims ~30 % throughput gain from a transformer AI-RAN architecture
     ([press release](https://www.softbank.jp/en/corp/news/press/sbkk/2025/20250821_02/)) — a rare
     *field* datapoint, but a vendor claim without published methodology. **[headline verified, evidence
     unverified]**

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

**What is missing.** I found **no** paper that reports measured per-slot inference latency of a
*transformer-based detector* inside a live 5G NR gNB — the closest are the dApp interface papers (which
measure the plumbing, not the model) and NVIDIA's standard-compliant receiver. Complexity claims in the
transformer-receiver literature are overwhelmingly **analytical or desktop-GPU** rather than
deadline-verified in a running RAN.

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
* **Timing context for a real gNB:** NR numerology gives a 1 ms subframe / 0.5 ms slot at 15/30 kHz SCS
  and ~71.4 μs per OFDM symbol at 15 kHz; with HARQ feedback the practical per-slot compute budget is a
  fraction of a slot. The OCUDU dApp measurements above (≤500 μs occupancy for a neural-receiver-to-LLR
  function) are the concrete published instance of that budget being exercised. **[The generic numerology
  numbers are standard knowledge; the dApp occupancy figure is verified.]**

---

## 8. Synthesis

### 8.1 What is established

1. **In-context / prompt-based detection works in simulation and is a real research line with theory
   behind it.** Pilot pairs as a prompt, adaptation without weight updates, and provable
   in-context estimation are all published ([DEFINED/ICC 2025](https://ieeexplore.ieee.org/document/11161684),
   [ICL gradient-free adaptation](https://ar5iv.labs.arxiv.org/html/2506.15176),
   [AISTATS 2025](https://proceedings.mlr.press/v258/kunde25a.html)). DEFINED's specific contribution is
   the *decision-feedback* twist plus a pilot-matched comparison (`MMSE-P_k`) and a Rayleigh→Rician
   robustness study.
2. **Model-based DL detectors are mature but symbol-oriented.** DetNet/OAMP-Net2/MMNet output symbols or
   hard bits; the LLR-native variant of this family is IDD-style unfolding (DUIDD).
3. **LLR mismatch is a known, named problem with known mitigations** (min-sum robustness, clipping,
   quantiser gain, fixed-point LLR estimation, learned decoders), and there is at least one paper whose
   subject is precisely quantiser-gain mismatch stress testing.
4. **Inline-PHY AI with a real LLR interface has been engineered and measured** on an open 5G stack —
   byte volumes per slot and ≤500 μs occupancy — and the transport overhead can be microseconds.
   Standard-compliant real-time neural receivers exist.
5. **Real-system evidence of benefit exists** (SoftBank ~30 % AI-RAN throughput; T-Mobile/Ericsson;
   Nokia Bell Labs + R&S 6G receiver demo), but it is vendor-reported rather than peer-reviewed.

### 8.2 What is contested

1. **Whether learned/ICL receivers beat a well-tuned conventional chain once the decoder is in the loop
   and the comparison is honest.** Most gains are at uncoded BER or at 1e-2–1e-3 BLER; the 2026
   calibration-drift paper shows a neural receiver needing a per-slot trust/rollback decision against a
   classical receiver, with a 60.5 % mean rollback rate — hardly a clean win.
2. **Generalisation:** across SNR, modulation order, channel model, and *site*. The literature shows both
   encouraging robustness (DEFINED's Rayleigh→Rician) and clear fragility (site-specific training).
3. **Transformers vs CNNs/GNNs at equal latency budget.** Transformer-receiver papers argue
   parallelisability; almost none normalise against a CNN baseline *at the same measured latency*.
4. **Where the LLRs should be produced** — inside the model (bit-wise loss, calibration burden) or derived
   analytically from a symbol estimate (Gaussian assumption, cleaner calibration).

### 8.3 Where the literature is thin — and what this workstream would add

| Gap | Status in literature | What a hardware-measured OCUDU + Apple-Silicon implementation contributes |
|---|---|---|
| Apple-Silicon GPU/ANE for real-time PHY | **Nothing found** | First measured per-slot latency/throughput for a grid→LLR model on CoreML/Metal in a live gNB; includes the fp16-range problem for LLRs |
| Per-slot latency of a *transformer* detector in a running RAN | Only analytical/desktop-GPU; dApp papers measure plumbing, not the model | A deadline-verified number (occupancy per slot) and the latency/accuracy Pareto for shallow vs deep, prompt vs non-prompt |
| ICL/prompt detectors under a **fixed** NR DMRS budget | DEFINED-style work assumes a pilot-fed prompt; no standard-compliant study found | Evidence on whether prompt-based detection survives the DMRS density the standard actually grants |
| LLR calibration + **decoder-in-the-loop BLER at 1e-5 / error floor** | Thin; most papers stop at 1e-3–1e-4 or report uncoded BER; **no** work found applying temperature scaling or a principled calibration procedure to a learned receiver's LLRs | Measured BLER/error-floor curves through the real NR LDPC decoder, plus a calibration/scaling study (temperature scaling, clipping limits, bit-width) with numbers |
| Trust/fallback engineering (drift detection, rollback) | One 2026 paper (calibration drift + detect-and-rollback) | Independent reproduction on different hardware/model family; a deployable fallback design |
| Energy/thermals of AI PHY on a workstation-class device | Nothing found | Power/thermal measurements — decisive for any "AI on the gNB host" argument |

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

