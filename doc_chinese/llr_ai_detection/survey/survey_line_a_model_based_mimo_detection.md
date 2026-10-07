# Literature survey — (A) model-based / deep-unfolded MIMO detection, (B) LLR-output neural receivers

Prepared 2026-10-07. **Method / evidence caveat:** in this sandbox `web_fetch` is blocked for essentially every host
("resolves to a non-public IP address"), so no PDF was read end-to-end. Every claim below comes from passages that the
`web_search` back-end matched *inside* indexed full texts (arXiv/ar5iv, IEEE Xplore, university repositories, Zenodo,
OpenReview, vendor docs) and returned as the result snippet. Claims I could not see in such a passage are flagged in
**§4 Uncertainties** rather than asserted. Reported numbers are quoted only where the number itself appeared in a matched
passage.

---

## 0. The KEY QUESTION, answered up front

**What does each detector output, and are the LLRs "calibrated" (usable directly by an LDPC decoder without ad-hoc scaling)?**

| Detector | What it actually outputs | Calibrated per-bit LLRs? | Channel decoder in the loop? |
|---|---|---|---|
| DetNet (SPAWC'17 / TSP'19) | soft **symbol** estimates per layer (`q_k`, `x̂_k`); hard decision for BER | **No** — no LLR head found | **No** evidence of LDPC/Turbo |
| OAMP-Net / OAMP-Net2 (GlobalSIP'18 / TSP'20) | posterior-mean **symbol** estimates (`x̂ = E{xᵢ\|rᵢ,τ_t}`); the TSP'20 paper *does* define per-bit LLR expressions | LLR expressions exist in the paper; **calibration is not discussed** in anything I could retrieve | Not verified for OAMP-Net2 itself |
| MMNet (TWC'20) | unfolded detector "designed for a wide range of realistic channel matrices"; outputs soft information | **Not verified** (see §4) | Not verified |
| Neural demappers / neural receivers (LINE B) | **per-bit LLRs** directly (or bit posteriors → LLR) | Recognised as a real problem: LLR-distribution mismatch / "this is not the case" (CMDNet); explicit *LLR correction* networks; learned LLR quantization; neural LLR reconstruction for mismatched receivers | **Yes** — LDPC is in the loop (Sionna, 5G-NR PUSCH neural receivers, DUIDD, deep-unfolded SISO SIC) |

Bottom line: **in LINE A the canonical unfolded detectors are symbol-estimate detectors**; **LINE B is where LLRs,
LLR calibration and the LDPC interface are actually treated as first-class problems** (LLR correction, learned
quantization, 4-/6-bit LLR studies).

---

## 1. LINE A — model-based / model-driven deep learning for MIMO detection

### A.1 DetNet

**Deep MIMO Detection** — IEEE SPAWC 2017 (Sapporo), DOI 10.1109/SPAWC.2017.8227772 — [arXiv:1706.01151](https://arxiv.org/abs/1706.01151) · [ar5iv](https://ar5iv.labs.arxiv.org/html/1706.01151)
The 2017 paper by N. Samuel, T. Diskin, A. Wiesel, cited ~397 times; the conference forerunner of DetNet. Evidence for the
venue: the indexed proceedings entry *"[IEEE 2017 IEEE 18th International Workshop on Signal Processing Advances in
Wireless Communications (SPAWC) – Sapporo (2017.7.3–2017.7.6)] … Deep MIMO detection"* and the SciSpace record
*"Deep MIMO detection (2017) | Neev Samuel | 397 Citations – Proceedings Article 10.1109/SPAWC.2017.8227772"*.
Reports uncoded BER of a learned detector; simulation-only.

**Learning to Detect** — IEEE Transactions on Signal Processing, vol. 67, no. 10, p. 2554, 2019; DOI 10.1109/TSP.2019.2899805 — [arXiv:1805.07631](https://arxiv.org/abs/1805.07631)
This is the DetNet paper proper. Venue confirmed by the ADS bibcode `2019ITSP...67.2554S` ([ADS record](https://ui.adsabs.harvard.edu/abs/2019ITSP...67.2554S/abstract)) and the NSTL DOI record. Architecture: a fixed number of
unfolded layers, each layer implementing one projected-gradient-style iteration on the real-valued equivalent channel;
the indexed recursion is `q₁ = y`, `q_{k+1} = ρ(W_k q_k + …)`, i.e. a learned linear mixing `W_k` followed by a
piecewise-linear nonlinearity `ρ` (the paper's own text, [arXiv PDF](http://arxiv.org/pdf/1805.07631)). A model-based-DL
textbook chapter describes it flatly as *"DetNet is a deep learning based **symbol detector** proposed in [32] for flat
Gaussian MIMO channels"* ([Chapter 1, Model-Based Machine Learning for Communications, arXiv:2101.04726](https://ar5iv.labs.arxiv.org/html/2101.04726)).
**Output type: per-layer soft symbol estimates** (no LLR head surfaced anywhere); the network is trained end-to-end against
the transmitted symbols and BER is obtained by hard decisions. Later work re-derives it as a homotopy-optimisation scheme
(*"An Explanation of Deep MIMO Detection From a Perspective of Homotopy Optimization"*, IEEE, 2023, [Xplore 10041793](https://ieeexplore.ieee.org/document/10041793))
and analyses it in *"Understanding Deep MIMO Detection"* — IEEE Transactions on Wireless Communications 2023, DOI 10.1109/TWC.2023.3272525 ([arXiv:2105.05044](https://ar5iv.labs.arxiv.org/html/2105.05044)).

### A.2 The "DetNet with one-bit quantization (IEEE TSP 2019)" claim — not substantiated

I could not find any one-bit/quantised DetNet paper, in TSP 2019 or elsewhere, by Samuel–Diskin–Wiesel or by anyone else.
The TSP 2019 entry is *"Learning to Detect"* itself (vol. 67, p. 2554), and I found no 2017 IEEE SPL paper by these authors
either — the 2017 item is the **SPAWC** paper above. The request's "DetNet (2017, IEEE SPL)" plus "DetNet one-bit (TSP 2019)"
therefore looks like a merge of two different records. The real one-bit/low-resolution *model-based* deep detectors I did find:

- **Deep Signal Recovery with One-bit Quantization** — IEEE ICASSP 2019 (Khobahi, Naimpour, Soltanalian, Eldar) — [Stanford SearchWorks record](https://searchworks.stanford.edu/articles/edsair__edsair.doi.dedup.....89aece1a3ab29efba954e41444c7620e)
- **Model-Based Deep Learning for One-Bit Compressive Sensing** — IEEE Transactions on Signal Processing (Khobahi, Soltanalian) — [IEEE Xplore 9187438](https://ieeexplore.ieee.org/document/9187438)
- **LoRD-Net: Unfolded Deep Detection Network With Low-Resolution Receivers** — IEEE Transactions on Signal Processing 2021, DOI 10.1109/TSP.2021.3117503 — [arXiv:2102.02993](https://ar5iv.labs.arxiv.org/html/2102.02993) · [ACM DL entry](https://dl.acm.org/doi/10.1109/TSP.2021.3117503). Unfolded detection explicitly for low-resolution (incl. 1-bit) receivers; also presented as *"Model-Inspired Deep Detection with Low-Resolution Receivers"*, ISIT 2021.
- **Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation** — IEEE Transactions on Signal Processing, DOI 10.1109/TSP.2020.3048232 — [IEEE Xplore 9311778](https://ieeexplore.ieee.org/document/9311778/authors). ML MIMO detection under **one-bit quantized observations with binary symbols**, deep-adapted.
- **Regularized Neural Detection for One-Bit Massive MIMO Communication Systems** — [arXiv:2305.15543](https://ar5iv.labs.arxiv.org/html/2305.15543)
- **Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems** — [arXiv:2107.11958](http://export.arxiv.org/pdf/2107.11958)
- **Learnable binary MIMO detection with negative penalty based on inexact ADMM** — IET Electronics Letters, DOI 10.1049/ell2.13155 — [IET](https://ietresearch.onlinelibrary.wiley.com/doi/pdf/10.1049/ell2.13155)

### A.3 OAMP-Net and OAMP-Net2

**A Model-Driven Deep Learning Network for MIMO Detection** — H. He, C.-K. Wen, S. Jin, G. Y. Li — 2018 — [arXiv:1809.09336](https://ar5iv.labs.arxiv.org/html/1809.09336)
This is OAMP-Net. **Venue correction:** the request says IEEE GLOBECOM 2018, but the indexed PDF of this paper is hosted on
sigport.org as `IEEE_Globalsip_2018.pdf` ([sigport](https://sigport.org/sites/default/files/docs/IEEE_Globalsip_2018.pdf)), i.e.
**IEEE GlobalSIP 2018**; the HKUST IR record and the Mendeley catalogue entry match the same title. OAMP-Net unfolds the
orthogonal approximate message passing iteration and makes the per-iteration shrinkage/step parameters learnable. Output:
symbol estimates (posterior means), not LLRs. I did not retrieve numeric BER values for the conference version.

**Model-Driven Deep Learning for MIMO Detection** — H. He, C.-K. Wen, S. Jin, G. Y. Li — IEEE Transactions on Signal Processing, vol. 68, p. 1702, 2020 — [arXiv:1907.09439](https://ar5iv.labs.arxiv.org/html/1907.09439) · [IEEE Xplore 9018199](https://ieeexplore.ieee.org/document/9018199)
This is the OAMP-Net2 paper (ADS bibcode `2020ITSP...68.1702H`, [ADS](https://ui.adsabs.harvard.edu/abs/2020ITSP...68.1702H/abstract)); note the request's "IEEE JSTSP 2020" attribution
appears wrong — the record is **IEEE TSP 2020**. Key retrieved facts:
- Parameterisation: *"Similar to the TPG detector [22] and OAMP-Net [1], the OAMP-Net2 uses two learnable parameters (γ_t, θ_t) to ad[apt]…"* → only **2 learnable scalars per layer**, i.e. O(T) parameters.
- Output: posterior-mean symbol estimates, e.g. `x̂_{d,t+1}^{(i)} = E{x_i | r_i, τ_t} = Σ_{s_i} s_i p_{s_i}(…)`, plus variance terms (both passages indexed from the paper).
- **LLRs do appear in this paper**: an indexed passage reads *"where b_{j,k} is the k-th bit in the transmitted symbol x_j, and S_j⁺ and S_j⁻ denote the su[bsets]…"* ([arXiv:1907.09439 §VI](http://arxiv.org/pdf/1907.09439v1)), i.e. the paper writes down a per-bit LLR expression for the detector output. Whether those LLRs are used with a real decoder, and whether any scaling is applied, is **not verified**.
- The paper explicitly compares *"the number of learnable variables in different DL-based MIMO detectors"* and contains a complexity table whose header is *"Complexity | Detectors | OAMP-Net2 | DetNet | DNN-dBP | DNN-MPD | TPG | …"* ([arXiv PDF](http://arxiv.org/pdf/1907.09439)) — I could not retrieve the table's numeric cells.
- Robustness study: *"1) Robustness to SNR and Channel Correlation: Fig. 9 presents the BER performance of OAMP and OAMP-Net2 with SNR and correlation …"*, and the simulation section notes *"all detectors are investigated with accurate CSI"* — i.e. **simulation-only, perfect-CSI**.

### A.4 MMNet

**Adaptive Neural Signal Detection for Massive MIMO** — M. Khani, M. Alizadeh, J. Hoydis, P. Fleming — IEEE Transactions on Wireless Communications, 2020, DOI 10.1109/TWC.2020.2996144 — [arXiv:1906.04610](http://arxiv.org/pdf/1906.04610) · [Nokia Bell Labs listing](https://www.nokia.com/bell-labs/publications-and-media/publications/adaptive-neural-signal-detection-for-massive-mimo/)
Architecture (from the author's MIT PhD thesis, [DSpace](https://dspace.mit.edu/bitstream/handle/1721.1/152866/khanishirkoohi-khani-phd-eecs-2023-thesis.pdf)): *"MMNet concatenates T layers of the above form"* — each layer = linear estimator (with a learnable step/denoising-parameterisation, cf. the indexed variance formula `σ_t² = θ_t^{(2)}/N_t · (‖I − A_t H‖_F²/‖H‖_F²)…`) followed by a **neural-network denoiser**; this is model-driven unfolding around OAMP-style iterations. A comparison paper notes *"The noise at the input of the denoiser is assumed to be independent but not identically distributed in MMNet"* ([Deep HyperNetwork-Based MIMO Detection, arXiv:2002.02750](https://ar5iv.labs.arxiv.org/html/2002.02750)).
**Reported numbers:** *"in practical 3GPP channels, MMNet achieves an improvement of 3-dB in SNR compared to OAMP-Nets with less [complexity]"* (indexed from IEEE Access 2021, DOI 10.1109/ACCESS.2021.3087136). An independent review characterises MMNet as *"a deep unfolded detector designed for a wide range of realistic channel matrices, including ill-conditioned…"* ([Comprehensive Review of Deep Unfolding, arXiv:2502.05952](https://export.arxiv.org/pdf/2502.05952v1)). **Output type (bits vs symbols vs LLRs): not verified** — see §4.

**Exploiting Channel Locality for Adaptive Massive MIMO Signal Detection** — IEEE ICASSP 2020 — [PDF](http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf) · [IEEE TV listing](https://origin-stage.ieeetv.ieee.org/ondemand/ieee-icassp-2020-virtual-conference-may-2020/6241)
The conference companion. Indexed passages: *"In this section, we evaluate and compare the performance of MMNet with state-of-the-art schemes on 3GPP MIMO channel matrices…"* and *"In this section, we show how channel locality can help reduce the total number of operations MMNet needs to decode each received…"* — i.e. it is a **complexity/operations-count** paper on 3GPP channels (spatial correlation / CDL-style), simulation-only. The method is a truncation of the network exploiting the banded structure of the channel Gram matrix.

### A.5 Other deep-unfolding detectors

- **Trainable Projected Gradient Detector for Massive Overloaded MIMO Channels: Data-Driven Tuning Approach** (TPG) — [arXiv:1812.10044](https://ar5iv.labs.arxiv.org/html/1812.10044) · [IEEE Xplore 8759948](https://xplorestaging.ieee.org/document/8759948). Deep-unfolds projected gradient descent for **overloaded** MIMO; it is one of the baselines OAMP-Net2 compares against.
- **Deep MIMO Detection Using ADMM Unfolding** — IEEE Data Science Workshop (DSW) 2019, DOI 10.1109/DSW.2019.8755566 — [IEEE Xplore 8755566](https://ieeexplore.ieee.org/document/8755566). Unfolds the ADMM iterations into layers with learnable penalty/step parameters; symbol-level output.
- **Learnable MIMO Detection Networks Based on Inexact ADMM** — IEEE Transactions on Wireless Communications 2020, DOI 10.1109/TWC.2020.3026471 (Kim, Park) — [ACM DL](https://dl.acm.org/doi/10.1109/TWC.2020.3026471). Inexact-ADMM unfolding; reported to keep complexity low by avoiding exact linear solves.
- **SICNN: Soft Interference Cancellation Inspired Neural Network Equalizers** — IEEE (2024), [IEEE Xplore 10471626](https://ieeexplore.ieee.org/document/10471626) · [arXiv:2308.12591](https://arxiv.org/pdf/2308.12591). Learned SIC-style equaliser; the paper's comparison **table** is indexed as *"OAMP-Net2 | MONet2 | 4 892 300 | 4 894 800 | 9 815 100"* — i.e. large multiplication counts for OAMP-Net2-style networks (exact column meaning not verified).
- **Deep Unfolding with Kernel-based Quantization in MIMO Detection** — [arXiv:2505.12736](https://ar5iv.labs.arxiv.org/html/2505.12736) (2025). Unfolded detector combined with kernel-based (quantised) computation.
- **Soft Graph Transformer for MIMO Detection** — [arXiv:2509.12694](https://arxiv.org/html/2509.12694v1) (2025). Transformer/GNN-style detector with a defined multi-stage inference pipeline; a "soft" (probabilistic) output.
- **A Deep-Unfolding-Optimized Coordinate-Descent Data-Detector ASIC for mmWave Massive MIMO** — IEEE Journal on Selected Areas in Communications 2025, DOI 10.1109/JSAC.2025.3531558 — [ACM DL](https://dl.acm.org/doi/10.1109/JSAC.2025.3531558) · [arXiv:2501.14861](https://ar5iv.labs.arxiv.org/html/2501.14861). Hardware evidence that unfolded detectors are reaching ASIC implementations; the indexed text works with *"the soft estimate of the u-th UE's symbol"* → **soft symbol estimates**, not LLRs.
- **On Purely Data-Driven Massive MIMO Detectors** — [arXiv:2401.07515](https://ar5iv.labs.arxiv.org/html/2401.07515) (2024). Positions OAMP among purely data-driven detectors for massive MIMO.
- **Deep learning based approximate message passing for MIMO detection in 5G** — Politecnico di Milano thesis — [Politesi](https://www.politesi.polimi.it/handle/10589/181815) (LAMP-style unfolding for 5G).

### A.6 Unfolded detectors that DO aim at LLRs / coded systems (bridge to LINE B)

- **DUIDD: Deep-Unfolded Interleaved Detection and Decoding for MIMO Wireless Systems** — [arXiv:2212.07816](https://ar5iv.labs.arxiv.org/html/2212.07816). Unfolds **iterative detection and decoding (IDD)**; the indexed text explicitly works with *"the extrinsic LLR values [that] are then fed back to the data detector"* — i.e. LLR-domain, decoder-in-the-loop, simulation.
- **Deep-Unfolded Iterative Soft-Input Soft-Output SIC Receiver for Coded MIMO Systems** — IEEE 2024, [IEEE Xplore 10757483](https://ieeexplore.ieee.org/document/10757483/citations?tabFilter=papers) · [Aalborg University record](https://vbn.aau.dk/da/publications/deep-unfolded-iterative-soft-input-soft-output-sic-receiver-for-c/). Unfolded soft-input soft-output SIC for coded MIMO — LLR-domain output feeding a decoder.
- **Soft-Output Deep LAS Detection for Coded MIMO Systems: A Learning-Aided LLR Approximation** — Ullah & Choi — IEEE — [University of Glasgow ePrints 324456](https://eprints.gla.ac.uk/324456/) · [NSTL record](https://shz.nstl.gov.cn/paper_detail.html?id=ffc5b2868d402949f814d435efc15496). Directly targets **LLR approximation** for a coded MIMO system (deep list/ LAS detector).

### A.7 Recent surveys (2024–2026)

- **Comprehensive Review of Deep Unfolding Techniques for Next-Generation Wireless Communication Systems** — [arXiv:2502.05952](https://ar5iv.labs.arxiv.org/html/2502.05952v2) (Feb 2025, revised through v4 in 2026). The most systematic recent survey: covers unfolding across PHY tasks, notes that each section has *"a cross method comparison table that evaluates prominent unfolded networks across ke[y metrics]"*, and describes MMNet, DetNet-style and ADMM-unfolded detectors. It also flags that *"for overloaded massive MIMO systems, low-complexity detectors are essential"*.
- **Model-Based Deep Learning** — Shlezinger, Whang, Eldar, Dimakis — Proceedings of the IEEE (2023) — [arXiv:2012.08405](https://ar5iv.labs.arxiv.org/html/2012.08405) · [Technion record](https://cris.technion.ac.il/en/publications/model-based-deep-learning). The canonical framing of "learnable parameters inside a known model" that both DetNet-style and OAMP-Net-style detectors instantiate.
- **Model-Based Machine Learning for Communications** (book chapter) — [arXiv:2101.04726](https://ar5iv.labs.arxiv.org/html/2101.04726). Taxonomy of model-based ML receivers; the source of the flat statement that DetNet is "a deep learning based symbol detector … for flat Gaussian MIMO channels".
- **Deep Learning-Aided 6G Wireless Networks: A Comprehensive Survey of Revolutionary PHY Architectures** — [arXiv:2201.03866](https://arxiv.org/html/2201.03866v3). Broad PHY survey that indexes the OAMP-Net/OAMP-Net2 lineage and DetNet among DL detectors.
- **Intelligent Radio Signal Processing: A Survey** — [arXiv:2008.08264](https://ar5iv.labs.arxiv.org/html/2008.08264): *"More recently, OAMP-Net2 was proposed [113] as an improved version of OAMP-Net"*.
- **Leveraging Deep Neural Networks for Massive MIMO Data Detection** — Nguyen & Nguyen — [arXiv:2204.05350](https://ar5iv.labs.arxiv.org/html/2204.05350) · [Oulu repository copy](https://oulurepo.oulu.fi/bitstream/handle/10024/44744/nbnfi-fe202301265946.pdf) · IEEE (Communications Magazine-style overview). Contains the balanced summary *"Despite the above limitations, DetNet offers several performance advantages"*.
- **Deep Learning Techniques For Massive MIMO Detection Algorithms: A Review** — [FJIECE](https://fjiece.atu.edu.iq/index.php/fjiece/article/download/276/143/1156) (2024). Review with a structured table (*Ref | Research methodology | Main Findings | Strength | Limitations*).
- **AI-Enhanced Signal Detection and Channel Estimation for Beyond 5G and 6G Wireless Networks** — [Wiley ETT](https://onlinelibrary.wiley.com/doi/full/10.1002/ett.70306) (2025/2026), includes an **FF-DetNet** extension evaluated in large-scale mMIMO.

---

## 2. LINE B — deep-learning receivers that emit LLRs, with coding-aware losses

### B.1 Sionna / NVIDIA neural receivers for 5G NR

**Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR** — [arXiv:2409.02912](https://arxiv.org/abs/2409.02912) · [IEEE Xplore 11140048](https://ieeexplore.ieee.org/document/11140048) · [NVIDIA Research page](https://research.nvidia.com/index.php/publication/2025-05_design-standard-compliant-real-time-neural-receiver-5g-nr) (2025; work done during an NVIDIA internship, funded by EU CENTRIC, Grant 101096379; arXiv endorsers listed include Reinhard Wiesmayr, Sebastian Cammerer and Alexander Keller).
This is the anchor paper for "does anyone actually build the LLR interface for real": a **standard-compliant, real-time GPU/TensorRT neural receiver for 5G NR**, i.e. the learned part produces the soft bits that a standard LDPC decoder consumes, under fixed-point and latency constraints. Simulation-only claims do not apply — this is an implementation paper. Exact architecture, LLR quantisation/saturation settings and BLER numbers were not retrieved as passages (see §4).

**Sionna** — Hoydis et al., *Sionna: An Open-Source Library for Next-Generation Physical Layer Research* — [arXiv:2203.11854](https://arxiv.org/abs/2203.11854). Provides the reference stack (differentiable 5G-NR PUSCH transmitter/receiver, LDPC, demapper, neural receivers). Relevant artefacts I verified:
- **Neural Demapper Training and TensorRT Export** — Sionna Research Kit tutorial: [nvlabs.github.io/sionna/v1.2.2/rk/tutorials/neural_demapper/Neural_demapper.html](https://nvlabs.github.io/sionna/v1.2.2/rk/tutorials/neural_demapper/Neural_demapper.html); **Integration of a Neural Demapper**: [v2.1.0 tutorial](https://nvlabs.github.io/sionna/v2.1.0/rk/tutorials/neural_demapper/). These document a learned demapper whose output is consumed as LLRs and which is exported for accelerated inference.
- **Neural Receiver for OFDM SIMO Systems** — [Sionna tutorial](https://nvlabs.github.io/sionna/phy/tutorials/notebooks/Neural_Receiver.html): "In this notebook, you will learn how to train a neural receiver that implements OFDM detection"; the notebook's evaluation table fragment `3.5 | 1.1663e-04 | 7.8125e-04 | 2078 | 17817600 | 10 | 12800 | 17.2 | reached max iter` shows BLER-oriented evaluation with iteration counts.
- **Jumpstart into Multi-User MIMO Neural Receiver for 5G NR PUSCH** (NVlabs/neural_rx) — [notebook](https://notebooks.githubusercontent.com/view/ipynb?browser=chrome&bypass_fastly=true&color_mode=auto&commit=b3d57522a0d5c512b9dd77fb41aa8dee0e907a92&device=unknown_device&docs_host=https%3A%2F%2Fdocs.github.com&enc_url=68747470733a2f2f7261772e67697468756275736572636f6e74656e742e636f6d2f4e566c6162732f6e657572616c5f72782f623364353735323261306435633531326239646437376662343161613864656530653930376139322f6e6f7465626f6f6b732f6a756d7073746172745f7475746f7269616c2e6970796e62&logged_in=false&nwo=NVlabs%2Fneural_rx&path=notebooks%2Fjumpstart_tutorial.ipynb&platform=mac&repository_id=844740484&repository_type=Repository&version=123): *"This notebook introduces the training and evaluation pipeline of the neural receiver (NRX)"* for multi-user MIMO 5G-NR PUSCH — the end-to-end pipeline where bits from the LDPC encoder are the labels and BLER is measured after LDPC decoding.
- **Sionna Research Kit: A GPU-Accelerated Research Platform for AI-RAN** — [arXiv:2505.15848](https://ar5iv.labs.arxiv.org/html/2505.15848).

### B.2 Loss functions used for LLR-output receivers

- **Binary cross-entropy on coded bits / bit-wise losses.** The dominant choice: the receiver/demapper is trained with a
  per-bit loss against the *encoder's* output bits (labels come from the LDPC encoder, so the loss is coding-aware even
  though the decoder is not differentiated). This is the mechanism behind the Sionna neural-receiver and neural-demapper
  tutorials and the 5G-NR PUSCH neural receiver notebook above, and behind *"Bit-wise optimization enables joint learning
  of bit labeling of each constellation point, as well as constellation shaping"* (TUHH repository copy of an end-to-end
  learned-modulation paper).
- **Bit-error / block-error-rate training.** **Bit Error and Block Error Rate Training for ML-Assisted Communication** —
  [arXiv:2210.14103](https://arxiv.org/html/2210.14103v1) · [ETH Research Collection 646432](https://www.research-collection.ethz.ch/handle/20.500.11850/646432). Trains directly against BER/BLER-type metrics instead of BCE — motivated by exactly the mismatch between a per-bit surrogate loss and the block-level metric that matters.
- **Mutual-information / neural-MI losses.** **Deep Learning for Channel Coding via Neural Mutual Information
  Estimation** — Fritschek, Schaefer, Wunder — IEEE SPAWC 2019 — [arXiv:1903.02865](https://ar5iv.labs.arxiv.org/html/1903.02865) · [Researchr record](https://researchr.org/publication/FritschekSW19-0/bibtex); follow-up
  **Neural Mutual Information Estimation for Channel Coding: State-of-the-Art Estimators, Analysis, and Performance
  Comparison** — [arXiv:2006.16015](https://ar5iv.labs.arxiv.org/html/2006.16015). Learn an MI estimator over the channel and use it as the training objective, rather than a bit-wise surrogate.
- **Learned decoders (bit-domain, LLR-in/LLR-out).** Nachmani et al., **Deep Learning Methods for Improved Decoding of
  Linear Codes** — IEEE JSTSP 2018, DOI 10.1109/JSTSP.2017.2788405 — [indexed PDF](https://ieeexplore.ieee.org/document/8255793). Learned belief propagation: weights on Tanner-graph edges, **LLR messages in and out**, so the decoder interface is LLR-native by construction.

### B.3 LLR calibration, scaling, over-confidence, saturation — who actually reports it

- **CMDNet — Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection With Low Complexity** — Beck,
  Bockelmann, Dekorsy — IEEE (2021/2022), [IEEE Xplore 9546779](https://ieeexplore.ieee.org/abstract/document/9546779) · [arXiv:2102.12756](https://ar5iv.labs.arxiv.org/html/2102.12756) · [author PDF](https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf).
  This is the clearest "the learned soft output is **not** the true posterior" statement I found: the authors write
  *"Indeed, we visualize with an exemplary histogram of LLRs that this is not the case"* — i.e. they test the assumption
  that the network's LLRs match the true posterior LLR distributional behaviour and show a counterexample. The paper is a
  low-complexity, probabilistic-relaxation soft detector; its indexed comparison row is *"CMDNet | 500 500 500 | 10⁴–10⁵ | 2N_L+1"* (parameter/complexity columns, exact meaning not verified).
- **Deep Learning Aided LLR Correction Improves the Performance of Iterative MIMO Receivers** — Chen & Wang (NYCU), IEEE 2023 — [IEEE Xplore 10246845](https://xplorestaging.ieee.org/document/10246845) · [Southampton ePrints 481845](https://eprints.soton.ac.uk/481845/) · [NYCU scholar](https://scholar.nycu.edu.tw/en/publications/deep-learning-aided-llr-correction-improves-the-performance-of-it). Exact thesis of the paper: run a conventional iterative MIMO receiver, then use a **neural network to correct the LLRs** before/while feeding the decoder. This is an explicit admission that raw detector LLRs are imperfect inputs to the decoder.
- **Neural Augmentation of MIMO-OFDM Receivers for Universal LLR Reconstruction** (listed by aggregators as *"Neural LLR Reconstruction for MIMO-OFDM Receivers"*) — [arXiv:2606.29345](https://arxiv.org/abs/2606.29345) (2026) · [Semantic Scholar reader](https://www.semanticscholar.org/reader/57485766f1cd0fdf5cbcade8b6fbcaa3b6c5a3d1). A neural augmentation stage that **reconstructs/corrects LLRs** for MIMO-OFDM receivers so the soft output stays usable across mismatched conditions; a snippet notes the approach can *"be effectively applied in higher-order modulation settings"*. This is the most on-point 2026 paper for "LLRs that a decoder can actually use".
- **Data-Free Quantization of Neural Receivers: When 4-Bit Succeeds, Why 6-Bit Matters for 6G** — NeurIPS 2025 workshop/track — [NeurIPS virtual](https://neurips.cc/virtual/2025/loc/san-diego/123232) · [OpenReview PDF](https://openreview.net/pdf?id=4NsiKauceB). Quantisation study of neural receivers; indexed result: *"Finally, Figure 3 shows that in Scenario II, the 4-bit quantized neural receiver surpasses LS estimation by approximately 2.5 d[B]"* — i.e. low-bit soft-output receivers are being characterised against classical baselines.
- **Learning Quantization in LDPC Decoders** — [arXiv:2208.05186](https://arxiv.org/pdf/2208.05186). Learned quantisers for LLR messages inside the LDPC decoder — the fixed-point side of the calibration/saturation problem.
- **SNR-mismatch-aware decoding.** A decoder that *"is not only aware of the SNR mismatch, but can also estimate the magnitude of the mismatch"* — IET Communications, DOI 10.1049/iet-com.2013.0471 ([Wiley](https://onlinelibrary.wiley.com/doi/pdfdirect/10.1049/iet-com.2013.0471)) — classical evidence that LLR magnitude/scale errors are a known decoder-side failure mode, which is exactly the risk when a learned detector replaces the analytic demapper.
- **LLR quantisation/clipping background.** Fixed-point LLR messages *"can take values in the range of [−∞ …]"* and must be saturated (e.g. US 8578238 on LLR fixed-point representation); adaptive LLR quantisation scales LLRs and messages before decoding. This is the standard 5G/LDPC practice that any learned LLR source has to respect.

### B.4 Demappers and receivers specifically (LLR-native designs)

- **Convolutional Self-Attention-Based Multi-User MIMO Demapper** — [arXiv:2201.11779](https://ar5iv.labs.arxiv.org/html/2201.11779). Attention-based demapper operating on multi-user MIMO signals (outputs soft information per bit).
- **Performance evaluation of polar coded neural demapper based 5G MIMO communication system by varying antenna size** — [PDF](https://pdfs.semanticscholar.org/e00a/2e5c6f4b0e2530014e8efce8ca5f9208658b.pdf). Neural demapper explicitly evaluated with a **polar code** in the loop over 5G MIMO antenna configurations.
- **Interference Cancellation Based Neural Receiver for Superimposed Pilot in Multi-Layer Transmission** — [arXiv:2406.18993](https://ar5iv.labs.arxiv.org/html/2406.18993) (2024). Neural receiver (with an explicit algorithm box) for multi-layer transmission with superimposed pilots.
- **End-to-end Optimization of Constellation Shaping for Wiener Phase Noise Channels with a Differentiable Blind Phase Search** — [arXiv:2212.03839](https://ar5iv.labs.arxiv.org/html/2212.03839). End-to-end system with a *learned neural demapper* whose decision regions are inspected.
- **A Neural Network Aided Approach for LDPC Coded DCO-OFDM with Clipping Distortion** — [arXiv:1809.01022](https://browse.arxiv.org/abs/1809.01022). NN-aided LLR handling in front of an LDPC decoder under a nonlinear (clipping) channel.
- **Trainable Communication Systems: Concepts and Prototype** — Cammerer, Ait Aoudia, Dörner, Stark, Hoydis, ten Brink — IEEE Transactions on Communications 2020 — [IEEE Xplore 9118963](https://ieeexplore.ieee.org/document/9118963). Hardware/algorithm co-design of trainable receivers (the precursor to the real-time neural-receiver line).
- **CENTRIC D5.2 — Early version of CENTRIC PoC Demonstrator** — [Zenodo 12731570](https://zenodo.org/records/12731570/files/D5.2%20Early%20version%20of%20CENTRIC%20PoC%20Demonstrator.pdf) (Horizon Europe, the project that funded the real-time neural receiver above) — proof-of-concept demonstrator documentation for AI-native 5G receivers.
- **VERITAS: Verifying the Performance of AI-native Transceiver Actions in Base-Stations** — [arXiv:2501.09761](https://ar5iv.labs.arxiv.org/html/2501.09761v3). Framework for deciding whether an AI-native receiver meets the performance of the conventional one — relevant to *deployment* acceptance of learned LLR sources.

---

## 3. Direct answers to the sub-questions

1. **Hard bits vs symbols vs LLRs vs posteriors.** DetNet (both papers) → soft **symbol** estimates. OAMP-Net/OAMP-Net2 →
   posterior-mean **symbol** estimates, with an LLR expression present in the TSP 2020 paper. MMNet → soft information from
   an unfolded linear+denoiser stack (exact form unverified). LINE B systems → **per-bit LLRs** (or bit posteriors converted
   to LLRs) fed to LDPC; several LINE A-adjacent unfolded detectors (DUIDD, deep-unfolded SISO SIC, deep-LAS LLR
   approximation) also work in the LLR domain.
2. **Calibrated LLRs?** I found **no** model-driven MIMO detector paper that claims well-calibrated LLRs out of the box. The
   papers that *do* engage with calibration are all LINE B-ish: CMDNet (shows the learned LLR distribution is *not* the
   posterior), LLR-correction networks (Chen & Wang 2023), neural LLR reconstruction (2026), learned LLR quantisation for
   LDPC, and SNR-mismatch-aware decoding.
3. **Channel decoder in the loop?** Yes for LINE B (Sionna/5G-NR PUSCH neural receiver, real-time standard-compliant neural
   receiver, DUIDD, deep-unfolded SISO SIC, polar-coded neural demapper). Not found for DetNet/OAMP-Net/OAMP-Net2/MMNet,
   which report **uncoded BER** (with an explicit note in OAMP-Net2's simulations that all detectors use *accurate CSI*).
4. **Channels/modulations.** MMNet: **3GPP channel matrices** (its ICASSP companion evaluates on 3GPP MIMO channel
   matrices; a third-party summary reports the 3 dB SNR gain *"in practical 3GPP channels"*); OAMP-Net2: correlated MIMO
   with an explicit *"robustness to SNR and channel correlation"* study and perfect CSI. DetNet: *"flat Gaussian MIMO
   channels"* per the textbook chapter. LINE B: 5G-NR PUSCH with LDPC (Sionna/NVIDIA), i.e. standards-defined channel and
   coding, higher-order modulation (256QAM appears in the neural-receiver/quantisation studies).

---

## 4. Uncertainties (things I could NOT verify from a source)

1. **A "DetNet with one-bit quantization, IEEE TSP 2019" paper.** Not found. Also found no 2017 IEEE **SPL** DetNet paper —
   the 2017 record is SPAWC. Treat both citations as likely mistaken; the real one-bit model-based detectors are listed in §A.2.
2. **DetNet's exact loss function** (I recall a per-layer weighted MSE but did not see the formula in a retrieved passage)
   and **DetNet's numeric BER/BLER results** (MIMO size, modulation, dB gains). Only the recursion and the "symbol detector"
   characterisation were retrieved.
3. **MMNet's output type** — whether the TWC 2020 paper emits per-bit LLRs or symbol estimates, and whether it uses an LDPC
   decoder. I could not retrieve a passage stating it. The 3 dB/3GPP figure comes from a *third-party* summary (IEEE Access
   2021), not from MMNet's own text.
4. **OAMP-Net2 numbers** — the complexity/learnable-variable table exists (header retrieved) but its **numeric cells** were
   not retrieved; likewise no BER values. The `b_{j,k}` LLR expression was retrieved but **not** the sentence saying what the
   LLRs are used for (decoder? hard decision?).
5. **Exact venue of "A Model-Driven Deep Learning Network for MIMO Detection"** — evidence points to **GlobalSIP 2018**
   (sigport file name), not GLOBECOM 2018, but I did not retrieve a formal citation line with pages.
6. **"Model-Driven Deep Learning for MIMO Detection" venue** — ADS gives IEEE **TSP** vol. 68 p. 1702 (2020); the request's
   JSTSP attribution is unconfirmed and probably wrong. End page of the article not verified.
7. **All LINE B implementation details** — LLR quantisation bit-width, saturation/clipping limits, min-sum vs sum-product,
   and BLER/throughput/latency numbers for the Sionna / NVIDIA 5G-NR neural receivers, the real-time neural receiver
   (arXiv:2409.02912), the 4-/6-bit quantisation study and the CMDNet calibration claim: the *existence* and framing of each
   is verified, the numbers are not.
8. **Loss functions for several LINE B papers** — I verified bit-wise BCE-style training and BER/BLER and MI-based losses as
   *families*, but for most individual papers (e.g. the PUSCH neural receiver, the neural demapper tutorials) I did not see
   the explicit loss formula in a retrieved passage.
9. **SICNN table semantics** — the numbers `4 892 300 / 4 894 800 / 9 815 100` are real retrieved tokens but I cannot state
   whether they are multiplications, FLOPs or parameters, nor which detectors the later columns belong to.
10. **Publication venue/year of arXiv:2502.05952 (deep-unfolding survey)** beyond arXiv (v1 Feb 2025, v4 seen) and of
    arXiv:2210.14103 (BER/BLER training) beyond arXiv + ETH repository.
