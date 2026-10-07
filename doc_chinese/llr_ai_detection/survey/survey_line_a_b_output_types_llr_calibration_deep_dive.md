# Deep dive: OUTPUT TYPE and LLR CALIBRATION in model-driven MIMO detectors (LINE A) and LLR-output neural receivers (LINE B)

2026-10-07. Companion to `survey_line_a_model_based_mimo_detection.md` and `survey_line_b_llr_output_receivers.md`.
**Evidence rule:** `web_fetch` is blocked in this sandbox (every host → "resolves to a non-public IP address"), so nothing was
read end-to-end. Every claim comes from a passage that `web_search` matched *inside* an indexed full text and returned as the
result snippet; the URL is given. Anything not seen that way is in §5 rather than asserted.

---

## 1. The decisive table

| Detector / receiver | Output object (verified) | Calibrated LLRs? | Channel decoder in loop | Evidence |
|---|---|---|---|---|
| **DetNet** (SPAWC'17 / TSP'19) | per-layer **soft symbol estimate**; "a deep learning based **symbol detector**" | no LLR head found | none found | `q₁=y; q_{k+1}=ρ(W_k q_k+…)`; textbook characterisation |
| **OAMP-Net** (GlobalSIP'18) | **soft symbol estimate** (`r_t = x̂_t + γ_t W_t(y − H x̂_t)`) | not discussed | not found | paper PDF |
| **OAMP-Net2** (TSP'20) | **posterior-mean symbol estimate** `x̂ = E{x_i \| r_i, τ_t}`; the paper *does* write a per-bit LLR expression (bits `b_{j,k}`, subsets `S⁺/S⁻`) | not discussed | not found | paper PDF/ar5iv |
| **MMNet** (TWC'20 / ICASSP'20) | soft information from T unfolded linear+denoiser layers (exact form **not verified**) | not verified | not verified | thesis + third-party summary |
| **Learning SIC for soft-output MIMO** (2026) | **LLRs**, explicitly *"subject to scaling and clipping"* | **yes — states the need** | soft-output (decoder-oriented) | arXiv 2601.16586 |
| **DUIDD / Deep-unfolded SISO SIC** | **extrinsic LLRs** exchanged with a decoder | LLR-domain | **yes** (IDD / coded MIMO) | arXiv 2212.07816; IEEE 10757483 |
| **Neural receivers / demappers** (LINE B) | **per-bit LLRs** (or bit posteriors → LLR) | known problem: LLR correction, learned quantisation, mismatch studies | **yes** (LDPC) | see §3.3 |

**One-sentence answer to the key question:** the classic model-driven MIMO detectors (DetNet, OAMP-Net/OAMP-Net2, MMNet) are
**soft-symbol-estimate detectors evaluated with uncoded BER**; per-bit LLRs, LLR calibration and the LDPC interface only become
first-class objects in (a) the newer *soft-output / coded* unfolded detectors and (b) the LINE-B neural-receiver literature.

---

## 2. LINE A — per-paper detail

**Deep MIMO Detection** — IEEE SPAWC 2017, DOI 10.1109/SPAWC.2017.8227772 — [arXiv:1706.01151](https://arxiv.org/abs/1706.01151) · [ar5iv](https://ar5iv.labs.arxiv.org/html/1706.01151) · [IEEE 8227772](https://ieeexplore.ieee.org/document/8227772)
N. Samuel, T. Diskin, A. Wiesel. The DetNet origin paper. Third-party sources state plainly: *"DETNET was first proposed in 'Deep MIMO detection'"* and *"DetNet is a model-based deep learning method whose working principle is to unfold a projected gradient descent algorithm"*. Output is an estimated symbol vector (posterior/MMSE-style target), not bits or LLRs. Simulation-only; no numerical results recovered.

**Learning to Detect** — IEEE Transactions on Signal Processing, vol. 67, no. 10, p. 2554, 2019, DOI 10.1109/TSP.2019.2899805 — [arXiv:1805.07631](https://arxiv.org/abs/1805.07631) · [ar5iv](https://ar5iv.labs.arxiv.org/html/1805.07631) · [IEEE 8642915](https://ieeexplore.ieee.org/abstract/document/8642915) · [ADS 2019ITSP...67.2554S](https://ui.adsabs.harvard.edu/abs/2019ITSP...67.2554S/abstract)
The DetNet journal paper. Per-layer recurrence recovered verbatim: `q₁ = y`, `q_{k+1} = ρ(W_k q_k + …)` — untied per-layer affine maps plus a piecewise-linear nonlinearity, with a fully-connected (`FullyCon`) baseline. A textbook chapter calls it *"a deep learning based symbol detector … for flat Gaussian MIMO channels"*; a Princeton lecture deck for this exact paper frames the learning target as `argmin E[‖x − x̂‖² | y]`, i.e. a **soft symbol estimate**. The paper measures run time/complexity comparisons between FullyCon and DetNet. **No LLR output, no channel decoder, no calibration discussion** in any retrieved passage. Simulation-only.

**The "DetNet + one-bit quantisation, IEEE TSP 2019" citation is not supported.** Repeated, differently-worded searches found no such paper, and no 2017 IEEE SPL DetNet paper either (the 2017 record is SPAWC). The real one-bit / low-resolution *model-based* deep detectors are:
- **LoRD-Net: Unfolded Deep Detection Network With Low-Resolution Receivers** — IEEE TSP, vol. 69, p. 5651, 2021, DOI 10.1109/TSP.2021.3117503 — [ar5iv 2102.02993](https://ar5iv.labs.arxiv.org/html/2102.02993) · [ADS 2021ITSP...69.5651K](https://ui.adsabs.harvard.edu/abs/2021ITSP...69.5651K/abstract). Unfolded detector for low-resolution (incl. 1-bit) receivers.
- **Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation** — IEEE TSP, DOI 10.1109/TSP.2020.3048232 — [IEEE 9311778](https://ieeexplore.ieee.org/document/9311778). ML MIMO detection under one-bit quantised observations with binary symbols.
- **Detection and Channel Equalization with Deep Learning for Low Resolution MIMO Systems** — Asilomar (ACSSC) 2018, DOI 10.1109/ACSSC.2018.8645551 — [IEEE 8645551](https://ieeexplore.ieee.org/document/8645551).
- **Accelerated and Deep Expectation Maximization for One-Bit MIMO-OFDM Detection** — IEEE TSP 2024, DOI 10.1109/TSP.2024.3359083 — [ACM DL](https://dl.acm.org/doi/abs/10.1109/TSP.2024.3359083) · [arXiv:2210.03888](https://arxiv.org/abs/2210.03888).
- **Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems** — [IEEE 9847603](https://ieeexplore.ieee.org/abstract/document/9847603) · [arXiv:2107.11958](http://export.arxiv.org/pdf/2107.11958). Uses an explicit b-bit uniform quantiser `Q_b(r)` with thresholds `τ_l` and step `Δ`.
- **Regularized Neural Detection for One-Bit Massive MIMO Communication Systems** — [arXiv:2305.15543](https://ar5iv.labs.arxiv.org/html/2305.15543).
- **Learning-Based One-Bit Maximum Likelihood Detection for Massive MIMO: Dithering-Aided Adaptive Approach** — [arXiv:2304.07696](http://arxiv.org/pdf/2304.07696v2).
- **Deep Signal Recovery with One-Bit Quantization** — ICASSP 2019 (Khobahi, Naimipour, Soltanalian, Eldar) — [record](https://searchworks.stanford.edu/articles/edsair__edsair.doi.dedup.....89aece1a3ab29efba954e41444c7620e); **Model-Based Deep Learning for One-Bit Compressive Sensing** — IEEE TSP — [IEEE 9187438](https://ieeexplore.ieee.org/document/9187438). (These two are the most likely source of the mis-remembered "one-bit TSP 2019".)

**A Model-Driven Deep Learning Network for MIMO Detection** — **IEEE GlobalSIP 2018** (not GLOBECOM) — [SIGPORT PDF](https://sigport.org/sites/default/files/docs/IEEE_Globalsip_2018.pdf) · [arXiv:1809.09336](https://ar5iv.labs.arxiv.org/html/1809.09336) · [IEEE 8646357](https://ieeexplore.ieee.org/document/8646357) · [HKUST record](https://lbnx03.ust.hk/ir/Record/1783.1-126247)
He, Wen, Jin, Li — OAMP-Net. Unfolds OAMP with a learned per-layer step size: `r_t = x̂_t + γ_t W_t(y − H x̂_t)`, then a learned nonlinear estimator. **Output: soft symbol estimates**, refined per layer. The same group tested robustness of learned parameters trained at 64-QAM/40 dB and applied them in **WINNER II channels** (evidence from [arXiv:1903.04766](http://export.arxiv.org/pdf/1903.04766)). No LLRs, no decoder, no numbers recovered.

**Model-Driven Deep Learning for MIMO Detection** — IEEE Transactions on Signal Processing, vol. 68, p. 1702, 2020, DOI 10.1109/TSP.2020.2976585 — [arXiv:1907.09439 v2](https://browse.arxiv.org/abs/1907.09439v2) · [ar5iv](https://ar5iv.labs.arxiv.org/html/1907.09439) · [IEEE 9018199](https://ieeexplore.ieee.org/document/9018199) · [ADS 2020ITSP...68.1702H](https://ui.adsabs.harvard.edu/abs/2020ITSP...68.1702H/abstract)
This is **OAMP-Net2**, and it is **IEEE TSP 2020, not IEEE JSTSP 2020**. *(The confusion has a traceable cause: arXiv 1907.09439v1 was titled "Model-Driven Deep Learning for Joint MIMO Channel Estimation and Signal Detection" and v2 was retitled "Model-Driven Deep Learning for MIMO Detection" — same arXiv ID, two titles.)* Architecture: **two learnable parameters (γ_t, θ_t) per layer**, i.e. O(T) parameters, versus the O(N²) per-layer matrices of DetNet. **Output type (verified verbatim):** `x̂_{d,t+1}^{(i)} = E{x_i | r_i, τ_t} = Σ_{s_i} s_i p_{s_i}(…)` — a **conditional-mean / soft symbol estimate** over the constellation posterior. The paper **also writes a per-bit LLR expression** (`b_{j,k}` = k-th bit of symbol x_j; `S_j⁺`, `S_j⁻` the subsets with that bit 0/1) — so an LLR read-out exists in the text, but no retrieved passage shows it being fed to a decoder or rescaled. It contains a complexity table comparing **OAMP-Net2 | DetNet | DNN-dBP | DNN-MPD | TPG** and states *"we investigate the number of learnable variables in different DL-based MIMO detectors"* (cell values not recoverable via search). Robustness study: *"Fig. 9 presents the BER performance of OAMP and OAMP-Net2 with SNR and correlation …"*, with the caveat *"all detectors are investigated with accurate CSI"* → **simulation-only, perfect CSI**. No BER numbers recovered.

**Adaptive Neural Signal Detection for Massive MIMO** — IEEE Transactions on Wireless Communications, 2020, DOI 10.1109/TWC.2020.2996144 — [arXiv:1906.04610](http://arxiv.org/pdf/1906.04610) · [IEEE 9103314](https://xplorestaging.ieee.org/document/9103314) · [Nokia Bell Labs](https://www.nokia.com/bell-labs/publications-and-media/publications/adaptive-neural-signal-detection-for-massive-mimo/)
Khani, Alizadeh, Hoydis, Fleming — MMNet. Unfolds T layers, each a linear estimator followed by a **neural-network denoiser** (the thesis says *"MMNet concatenates T layers of the above form"*); a comparison paper notes *"the noise at the input of the denoiser is assumed to be independent but not identically distributed in MMNet"*. It targets realistic, **ill-conditioned 3GPP channel matrices**. **Numbers:** *"in practical 3GPP channels, MMNet achieves an improvement of 3-dB in SNR compared to OAMP-Nets with less [complexity]"* (third-party summary, IEEE Access 2021, DOI 10.1109/ACCESS.2021.3087136 — not MMNet's own text). **Output type not verified**; no LLR/decoder evidence found.

**Exploiting Channel Locality for Adaptive Massive MIMO Signal Detection** — IEEE ICASSP 2020 — [PDF](http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf) · [IEEE 9052971](https://ieeexplore.ieee.org/abstract/document/9052971)
MMNet's conference companion: exploits channel-matrix locality to cut the *"total number of operations MMNet needs to decode each received"* frame; evaluated *"on 3GPP MIMO channel matrices"*. Complexity-oriented, simulation-only; symbol-level output.

**Other deep-unfolding detectors** (all symbol-level unless stated):
- **Trainable Projected Gradient Detector for Massive Overloaded MIMO Channels: Data-Driven Tuning Approach** (TPG) — [arXiv:1812.10044](https://ar5iv.labs.arxiv.org/html/1812.10044) · [IEEE 8759948](https://xplorestaging.ieee.org/document/8759948).
- **Deep MIMO Detection Using ADMM Unfolding** — IEEE DSW 2019, DOI 10.1109/DSW.2019.8755566 — [IEEE 8755566](https://ieeexplore.ieee.org/document/8755566).
- **Learnable MIMO Detection Networks Based on Inexact ADMM** — IEEE TWC 2020, DOI 10.1109/TWC.2020.3026471 (Kim, Park) — [ACM DL](https://dl.acm.org/doi/10.1109/TWC.2020.3026471); plus **Learnable binary MIMO detection with negative penalty based on inexact ADMM** — IET Electronics Letters, DOI 10.1049/ell2.13155 — [IET](https://ietresearch.onlinelibrary.wiley.com/doi/pdf/10.1049/ell2.13155).
- **Multilevel MIMO Detection with Deep Learning** (Corlay, Boutros) — Asilomar 2018, DOI 10.1109/ACSSC.2018.8645519.
- **Deep HyperNetwork-Based MIMO Detection** — [arXiv:2002.02750](https://ar5iv.labs.arxiv.org/html/2002.02750). Conditions the detector on the channel via a hypernetwork; explicitly contrasts with MMNet's i.n.i.d. denoiser-noise assumption.
- **Graph Neural Network-Enhanced Expectation Propagation Algorithm for MIMO Turbo Receivers** — IEEE TSP 2023, DOI 10.1109/TSP.2023.3310279 — [ar5iv 2308.11335](https://ar5iv.labs.arxiv.org/html/2308.11335) · [ACM DL](https://dl.acm.org/doi/10.1109/TSP.2023.3310279). Learned EP inside a **turbo (iterative detection–decoding) receiver** — an LLR-loop architecture.
- **Deep Unfolding with Kernel-based Quantization in MIMO Detection** — [arXiv:2505.12736](https://ar5iv.labs.arxiv.org/html/2505.12736).
- **Soft Graph Transformer for MIMO Detection** — [arXiv:2509.12694](https://arxiv.org/html/2509.12694v1).
- **Low-Complexity Deep MIMO Detection for Correlated Measurements** — [IEEE 11451696](https://ieeexplore.ieee.org/document/11451696).
- **On Purely Data-Driven Massive MIMO Detectors** — [arXiv:2401.07515](https://ar5iv.labs.arxiv.org/html/2401.07515).
- **A Deep-Unfolding-Optimized Coordinate-Descent Data-Detector ASIC for mmWave Massive MIMO** — IEEE JSAC 2025, DOI 10.1109/JSAC.2025.3531558 — [ACM DL](https://dl.acm.org/doi/10.1109/JSAC.2025.3531558) · [arXiv:2501.14861](https://ar5iv.labs.arxiv.org/html/2501.14861). Explicitly works with *"the soft estimate of the u-th UE's symbol"* → soft symbols, not LLRs.
- **SICNN: Soft Interference Cancellation Inspired Neural Network Equalizers** — [IEEE 10471626](https://ieeexplore.ieee.org/document/10471626) · [arXiv:2308.12591](https://arxiv.org/pdf/2308.12591). Its comparison table row is indexed as *"OAMP-Net2 | MONet2 | 4 892 300 | 4 894 800 | 9 815 100"* (units not verified).

**Unfolded detectors that DO produce LLRs (the bridge to LINE B):**
- **DUIDD: Deep-Unfolded Interleaved Detection and Decoding for MIMO Wireless Systems** — [arXiv:2212.07816](https://ar5iv.labs.arxiv.org/html/2212.07816). IDD receiver; *"the extrinsic LLR values are then fed back to the data detector"* — LLR-domain, decoder-in-the-loop.
- **Deep-Unfolded Iterative Soft-Input Soft-Output SIC Receiver for Coded MIMO Systems** — IEEE 2024 — [IEEE 10757483](https://ieeexplore.ieee.org/document/10757483) · [Aalborg record](https://vbn.aau.dk/da/publications/deep-unfolded-iterative-soft-input-soft-output-sic-receiver-for-c/).
- **Soft-Output Deep LAS Detection for Coded MIMO Systems: A Learning-Aided LLR Approximation** (Ullah, Choi) — [Glasgow ePrints](https://eprints.gla.ac.uk/324456/) · [NSTL](https://shz.nstl.gov.cn/paper_detail.html?id=ffc5b2868d402949f814d435efc15496).
- **Learning Successive Interference Cancellation for Low-Complexity Soft-Output MIMO Detection** — [arXiv:2601.16586](https://arxiv.org/html/2601.16586v1) (2026). **The single most explicit calibration statement found anywhere in LINE A:** *"To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to scaling and clipping, which is a well-known …"*. This is the paper to cite for "unfolded/learned MIMO detectors need LLR scaling + clipping before the decoder".
- **An unidentified 2022 arXiv paper (2211.06054)** compares *"DetNet | MDetNet"* with parameter-like counts (100000 / 31783 for "System I / System II") **and** defines an empirical reliability metric `P_emp,k = (# wrong hard decisions in bin k) / (# bits in bin k)` — i.e. a per-|LLR|-bin calibration curve for a DetNet-family detector. Title/authors could not be recovered; worth fetching directly.

**Recent surveys (2024–2026):**
- **Comprehensive Review of Deep Unfolding Techniques for Next-Generation Wireless Communication Systems** — [arXiv:2502.05952](https://ar5iv.labs.arxiv.org/html/2502.05952v2) (Feb 2025; v4 seen). Cross-method comparison tables over unfolded networks; discusses MMNet/DetNet/ADMM-unfolded detectors and notes low-complexity detectors are essential for overloaded massive MIMO.
- **Model-Based Deep Learning** — Shlezinger, Whang, Eldar, Dimakis — Proceedings of the IEEE (2023) — [arXiv:2012.08405](https://ar5iv.labs.arxiv.org/html/2012.08405) · [Technion](https://cris.technion.ac.il/en/publications/model-based-deep-learning).
- **Model-Based Machine Learning for Communications** — [arXiv:2101.04726](https://ar5iv.labs.arxiv.org/html/2101.04726).
- **Deep Learning-Aided 6G Wireless Networks: A Comprehensive Survey of Revolutionary PHY Architectures** — [arXiv:2201.03866](https://arxiv.org/html/2201.03866v3).
- **Intelligent Radio Signal Processing: A Survey** — [arXiv:2008.08264](https://ar5iv.labs.arxiv.org/html/2008.08264).
- **Leveraging Deep Neural Networks for Massive MIMO Data Detection** (Nguyen & Nguyen) — [arXiv:2204.05350](https://ar5iv.labs.arxiv.org/html/2204.05350) · [Oulu copy](https://oulurepo.oulu.fi/bitstream/handle/10024/44744/nbnfi-fe202301265946.pdf).
- **Deep Learning Techniques For Massive MIMO Detection Algorithms: A Review** — [FJIECE](https://fjiece.atu.edu.iq/index.php/fjiece/article/download/276/143/1156).
- **AI-Enhanced Signal Detection and Channel Estimation for Beyond 5G and 6G Wireless Networks** — [Wiley ETT](https://onlinelibrary.wiley.com/doi/full/10.1002/ett.70306) (includes an FF-DetNet variant for large-scale mMIMO).

---

## 3. LINE B — per-paper detail

### 3.1 Neural demappers (learned LLR estimators)

**Experimental Demonstration of Neural Network-based Soft Demapper for Long-haul Optical Transmission** — IEEE (doc 10209891) — [IEEE](https://ieeexplore.ieee.org/abstract/document/10209891) · [TU/e](https://research.tue.nl/en/publications/experimental-demonstration-of-neural-network-based-soft-demapper-/). A neural soft demapper in a real optical transmission experiment (output feeds an SD-FEC). Loss, venue and numbers not recovered.

**Deep Neural Network-Aided Soft-Demapping in Coherent Optical Systems: Regression Versus Classification** — IEEE (doc 9915441) — [IEEE](https://ieeexplore.ieee.org/document/9915441) · [Aston OA PDF](https://publications.aston.ac.uk/id/eprint/44286/6/Deep_Neural_Network_Aided_Soft_Demapping_in_Coherent_Optical_Systems_Regression_Versus_Classification.pdf). Verbatim framing of the output question: *"it is known that the regression-based equalization … can be expressed as a classification-based one … with s[oft outputs]"* — i.e. the network can regress symbols or classify bits; the classification form yields bit posteriors → LLRs.

**Long-Haul Optical-Eigenvalue Transmission Using a Neural Network Demodulator and SD-FEC** — IEEE — [PDF](https://ieeexplore.ieee.org/ielx8/49/11006336/10892258.pdf). Verbatim: *"the frequency of the NN output smoothly changed between 0 and 1 because the data far from the ideal signal points were sufficien[tly]…"* → the network emits **values in [0,1] (bit probabilities), not raw LLRs**; LLRs are formed afterwards.

**LLRSymNet: A Low-Complex Neural Network for LLR Estimation Through Symmetry Exploitation** — IEEE (doc 10901172) — [IEEE](https://ieeexplore.ieee.org/abstract/document/10901172) · [TU/e OA PDF](https://research.tue.nl/files/354712198/LLRSymNet_A_Low-Complex_Neural_Network_for_LLR_Estimation_Through_Symmetry_Exploitation_1_.pdf). By name and title an **LLR-estimating** network exploiting constellation symmetry; loss not recovered.

**"Machine LLRning": Learning to Softly Demodulate** — [arXiv:1907.01512](http://www.arxiv.org/abs/1907.01512v2) (2019). Learned soft demodulation (LLR estimation) — method/loss/numbers not recovered.

**Novel Deep Neural OFDM Receiver Architectures for LLR Estimation** — [arXiv:2503.20500](http://www.arxiv.org/pdf/2503.20500) (2025). Deep OFDM receivers whose output is explicitly LLRs; loss not recovered.

**Convolutional Self-Attention-Based Multi-User MIMO Demapper** — [arXiv:2201.11779](https://ar5iv.labs.arxiv.org/html/2201.11779). Attention-based MU-MIMO demapper with trainable parameters *"of our learned demapper"*; soft-output by construction.

**Performance evaluation of polar coded neural demapper based 5G MIMO communication system by varying antenna size** — [PDF](https://pdfs.semanticscholar.org/e00a/2e5c6f4b0e2530014e8efce8ca5f9208658b.pdf). Neural demapper evaluated **with a polar code in the loop** across 5G MIMO antenna sizes — an explicit coded-BLER evaluation.

**Efficient Training Scheme for NN Based 4K-QAM Soft Demapper** — [PDF](https://ojs.wiserpub.com/index.php/CM/article/download/6162/3000/58565). Compares learned soft demapping against *"log-MAP and max-log-MAP demappers"* for a coded 4K-QAM system; **the log-MAP vs max-log-MAP comparison is exactly the LLR-fidelity axis**.

**A Neural Network Aided Approach for LDPC Coded DCO-OFDM with Clipping Distortion** — [arXiv:1809.01022](https://browse.arxiv.org/abs/1809.01022). NN-aided LLR handling in front of an **LDPC** decoder under a nonlinear channel.

### 3.2 Loss functions (what is actually used)

- **Binary cross-entropy on coded bits** — explicitly stated in *Deep Learning OFDM Receivers for Improved Power Efficiency and Coverage*: *"The training for each receiver is performed using the binary cross entropy (CE) as the loss function, training and optimizing th…"* ([IEEE 10017176](https://ieeexplore.ieee.org/abstract/document/10017176/figures)).
- **Soft-BCE whose gradient equals a KL divergence between the MAP posterior and the network posterior** — the theoretical bridge between BCE training and LLR calibration: `∇_θ L_soft_BCE = ∇_θ D_KL(P_MAP ‖ P_NN) + …` ([Lund University thesis](https://lup.lub.lu.se/luur/download?func=downloadFile&recordOId=9235508&fileOId=9238393)).
- **Mutual-information / achievable-rate objectives** — *Neural Enhancement of Factor Graphs*: *"We optimize the parametrization towards a maximum achievable rate between the channel input and the detector output"* ([IEEE 9903445](https://ieeexplore.ieee.org/abstract/document/9903445/figures)); and the Donsker–Varadhan MI-bound training of Fritschek et al. (below).
- **Symbol-posterior cross-entropy** — the closest loss evidence for CMDNet (Beck dissertation).
- **A non-BCE bit loss** `L_bit(a,b) = (1−b)^a b^{1−a}` ([Chalmers OA PDF](https://research.chalmers.se/publication/513449/file/513449_Fulltext.pdf)).
- **BER/BLER-metric training** — *Bit Error and Block Error Rate Training for ML-Assisted Communication*, [arXiv:2210.14103](https://arxiv.org/html/2210.14103v1) · [ETH 646432](https://www.research-collection.ethz.ch/handle/20.500.11850/646432) (equation `b̂ = dec(argmax_{c∈C} p_{c|y}(c|y))` retrieved; **its losses, venue and numbers are NOT verified**).
- **Bit-wise MSE against true LLRs: no instance found** in the whole sweep.

### 3.3 Calibration, scaling, saturation, quantisation — who says what

**Learning Successive Interference Cancellation for Low-Complexity Soft-Output MIMO Detection** — [arXiv:2601.16586](https://arxiv.org/html/2601.16586v1) (2026). The clearest statement in either line: *"To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to scaling and clipping, which is a well-known …"*. Treat this as the canonical citation that learned/soft-output MIMO detectors **require an explicit LLR scaling + clipping stage**.

**CMDNet: Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection With Low Complexity** — Beck, Bockelmann, Dekorsy — 2021, [arXiv:2102.12756](https://ar5iv.labs.arxiv.org/html/2102.12756) · [IEEE 9546779](https://ieeexplore.ieee.org/abstract/document/9546779) · [author PDF](https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf). A low-complexity probabilistic-relaxation soft detector whose **soft-information quality is explicitly inspected**: *"Indeed, we visualize with an exemplary histogram of LLRs that this is not the case"*. The antecedent sentence was not recoverable after ~15 targeted probes — the natural reading (the learned LLRs do **not** behave like true posterior LLRs) is plausible but **unconfirmed**. Complexity columns indexed as *"CMDNet | 500 500 500 | 10⁴–10⁵ | 2N_L+1"* (semantics unverified). Coded-system evaluation exists (interleaved/horizontally coded 32×32).

**Neural Augmentation of MIMO-OFDM Receivers for Universal LLR Reconstruction** — [arXiv:2606.29345](https://arxiv.org/abs/2606.29345) (2026); journal version **"Learning to Refine LLRs: Modular Neural Augmentation for MIMO-OFDM Receivers"**, [IEEE 11587582](https://ieeexplore.ieee.org/document/11587582). Output verified as a **per-bit LLR tensor** `L̃_k^i = f_θk(X^i) := CNN^C_θk(W_k ⊙ X^i) ∈ R^{B×N}`; architecture = a primary conventional detector ψ plus an ESCNN post-processor (Algorithm 1). Evaluates *"modulation scaling"* and reports applicability to higher-order modulation. Loss and numbers not recovered.

**Deep Learning Aided LLR Correction Improves the Performance of Iterative MIMO Receivers** — Chen & Wang (NYCU), IEEE 2023 — [IEEE 10246845](https://xplorestaging.ieee.org/document/10246845) · [Southampton ePrints](https://eprints.soton.ac.uk/481845/) · [NYCU](https://scholar.nycu.edu.tw/en/publications/deep-learning-aided-llr-correction-improves-the-performance-of-it). A network **corrects the LLRs** produced by a conventional iterative MIMO receiver before decoding — i.e. raw detector LLRs are treated as systematically imperfect.

**Data-Free Quantization of Neural Receivers: When 4-Bit Succeeds, Why 6-Bit Matters for 6G** — NeurIPS 2025 — [NeurIPS](https://neurips.cc/virtual/2025/loc/san-diego/123232) · [OpenReview PDF](https://openreview.net/pdf?id=4NsiKauceB). Quantisation study of neural receivers; indexed result: *"the 4-bit quantized neural receiver surpasses LS estimation by approximately 2.5 d[B]"*. This is the quantitative anchor for "how many bits does the soft-output path need".

**Learning Quantization in LDPC Decoders** — [arXiv:2208.05186](https://arxiv.org/pdf/2208.05186). Makes the LLR quantiser/companding **learnable**, indexed by bit-width `w` and thresholds `T`: `LLR_(w2,T2) = QCM[w2,T1,T2](…)` ([IEEE 10251502](https://ieeexplore.ieee.org/ielx7/6287639/6514899/10251502.pdf)).

**Complexity Adjusted Soft-Output Sphere Decoding by Adaptive LLR Clipping** — [arXiv:1011.2113](https://ar5iv.labs.arxiv.org/html/1011.2113). The classical "clip the LLRs" reference for soft-output MIMO detection.

**Classical LLR scaling/saturation practice** (the constraints any learned LLR source must satisfy): adaptive quantisation *"applies a scaling to the log-likelihood ratios (LLRs) and messages in order to increase the …"* ([UNC thesis](https://rdu.unc.edu.ar/bitstream/handle/11086/1463/Morero_Tesis.pdf)); *"Parameter α should be chosen such that the least amount of saturation occurs for a given application, as oversaturation…"* ([PolyMtl thesis](https://publications.polymtl.ca/10295/1/2022_DanielBowenDermont.pdf)).

**SNR-mismatch-aware decoding** — the canonical mismatched-LLR mechanism: a decoder that *"is not only aware of the SNR mismatch, but can also estimate the magnitude of the mismatch"* ([IET Communications](https://onlinelibrary.wiley.com/doi/pdfdirect/10.1049/iet-com.2013.0471)); and bit-level LLR-histogram SNR-mismatch compensation (L-SMCT) in the same paper.

**LLR reliability caveat** — *"Investigating solely the distribution of the LLRs, however, is only an indicator of their reliability"* ([JKU thesis](https://epub.jku.at/obvulioa/content/titleinfo/9472866/full.pdf)).

**Negative evidence (important):** searches for "temperature scaling neural receiver LLR", "overconfident LLRs neural network", "LLR calibration learned demapper", "mismatched LLR neural demodulator" returned **no** paper applying temperature scaling to a neural receiver's LLRs and **no** paper labelled "LLR calibration for learned demappers". The `scaling + clipping` statement in arXiv 2601.16586 and the LLR-correction/reconstruction papers are the closest things that exist in the indexed literature.

### 3.4 Neural receivers, Sionna, 5G NR, and the LDPC interface

**Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR** — [arXiv:2409.02912](https://arxiv.org/abs/2409.02912) · [IEEE 11140048](https://ieeexplore.ieee.org/document/11140048) · [NVIDIA Research](https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr) (work done during an NVIDIA internship; EU CENTRIC grant 101096379). The anchor implementation paper: a **standard-compliant, real-time neural receiver for 5G NR**, i.e. the learned component's soft output is consumed by a standard LDPC decoder under real-time constraints. Implementation paper (GPU/TensorRT class), not simulation-only. Exact LLR quantisation/saturation settings and BLER numbers were not recovered from the index.

**Real-Time Inference of 5G NR Multi-user MIMO Neural Receivers (NVlabs/neural_rx)** — [GitHub](https://github.com/NVlabs/neural_rx) · [jumpstart notebook](https://notebooks.githubusercontent.com/view/ipynb?browser=chrome&bypass_fastly=true&color_mode=auto&commit=b3d57522a0d5c512b9dd77fb41aa8dee0e907a92&device=unknown_device&docs_host=https%3A%2F%2Fdocs.github.com&enc_url=68747470733a2f2f7261772e67697468756275736572636f6e74656e742e636f6d2f4e566c6162732f6e657572616c5f72782f623364353735323261306435633531326239646437376662343161613864656530653930376139322f6e6f7465626f6f6b732f6a756d7073746172745f7475746f7269616c2e6970796e62&logged_in=false&nwo=NVlabs%2Fneural_rx&path=notebooks%2Fjumpstart_tutorial.ipynb&platform=mac&repository_id=844740484&repository_type=Repository&version=123). *"This notebook introduces the training and evaluation pipeline of the neural receiver (NRX)"* for multi-user MIMO 5G-NR PUSCH — the end-to-end pipeline (LDPC encoder → bits as labels → neural receiver → LDPC decoder → BLER).

**Sionna: An Open-Source Library for Next-Generation Physical Layer Research** — Hoydis et al. — [arXiv:2203.11854](https://arxiv.org/abs/2203.11854). Supplies the differentiable 5G-NR PUSCH stack, LDPC codes, demappers and neural receivers. Verified artefacts: **Neural Demapper Training and TensorRT Export** ([SRK tutorial](https://nvlabs.github.io/sionna/v1.2.2/rk/tutorials/neural_demapper/Neural_demapper.html)), **Integration of a Neural Demapper** ([v2.1.0](https://nvlabs.github.io/sionna/v2.1.0/rk/tutorials/neural_demapper/)), **Neural Receiver for OFDM SIMO Systems** ([tutorial](https://nvlabs.github.io/sionna/phy/tutorials/notebooks/Neural_Receiver.html)) whose evaluation table fragment is `3.5 | 1.1663e-04 | 7.8125e-04 | 2078 | 17817600 | 10 | 12800 | 17.2 | reached max iter`, and **Sionna Research Kit: A GPU-Accelerated Research Platform for AI-RAN** ([arXiv:2505.15848](https://ar5iv.labs.arxiv.org/html/2505.15848)).

**NVIDIA Aerial LLRNet** — an industrial learned-LLR estimator for 5G NR: [Aerial llrnet-dataset-generation notebook](https://docs.nvidia.com/aerial/cuda-accelerated-ran/content/notebooks/llrnet-dataset-generation.pdf); MathWorks documentation asks *"Check if the LLRNet can estimate the LLR values for higher order QAM"* ([MathWorks](https://www.mathworks.com/help/releases/r2024b/pdf_doc/deeplearning/nnet_ug.pdf)).

**Deep Learning Methods for Improved Decoding of Linear Codes** — Nachmani, Be'ery, Burshtein — IEEE JSTSP 2018, DOI 10.1109/JSTSP.2017.2788405. Learned belief propagation: learnable Tanner-graph edge weights, **LLR messages in and out** — LLR-native by construction (relevant as the "learn to decode" anchor).

**Fritschek / Schaefer / Wunder — neural mutual information estimation:**
**Deep Learning for Channel Coding via Neural Mutual Information Estimation** — IEEE SPAWC 2019, DOI 10.1109/SPAWC.2019.8815464 — [IEEE 8815464](https://ieeexplore.ieee.org/document/8815464) · [arXiv:1903.02865](https://ar5iv.labs.arxiv.org/html/1903.02865) · code: [Fritschek](https://github.com/Fritschek/Wireless_encoding_with_MI_estimation). Trains with a **variational (Donsker–Varadhan) MI lower bound** rather than a bit-wise surrogate. **Neural Mutual Information Estimation for Channel Coding: State-of-the-Art Estimators, Analysis, and Performance Comparison** — IEEE SPAWC 2020, DOI 10.1109/SPAWC48557.2020.9154239 — [arXiv:2006.16015](https://ar5iv.labs.arxiv.org/html/2006.16015). Output type and numbers not recovered for either.

**Interference Cancellation Based Neural Receiver for Superimposed Pilot in Multi-Layer Transmission** — [arXiv:2406.18993](https://ar5iv.labs.arxiv.org/html/2406.18993) (2024). Neural receiver with an explicit algorithm box for multi-layer transmission with superimposed pilots.

**Trainable Communication Systems: Concepts and Prototype** — Cammerer, Ait Aoudia, Dörner, Stark, Hoydis, ten Brink — IEEE Transactions on Communications 2020, DOI 10.1109/TCOMM.2020.3002915 — [arXiv:1911.13055](https://browse.arxiv.org/abs/1911.13055) · [IEEE 9118963](https://ieeexplore.ieee.org/document/9118963). Hardware/algorithm co-design of trainable receivers, the precursor of the real-time neural-receiver line.

**CENTRIC deliverables** (Horizon Europe, funded the real-time neural receiver): [D5.2 PoC demonstrator](https://zenodo.org/records/12731570/files/D5.2%20Early%20version%20of%20CENTRIC%20PoC%20Demonstrator.pdf), [D3.5 final](https://centric-sns.eu/wp-content/uploads/2025/07/d3.5_final.pdf) (*"In terms of block error rate (BLER), the politeless system matched or outperformed the baseline configurations across a wide ran[ge]"*), [D3.6](https://centric-sns.eu/wp-content/uploads/2025/07/d3.6_report_final.pdf) (*"The proposed neural receiver architecture is fully differentiable and, hence, SGD-based training is straightforward"*).

**VERITAS: Verifying the Performance of AI-native Transceiver Actions in Base-Stations** — [arXiv:2501.09761](https://ar5iv.labs.arxiv.org/html/2501.09761v3). Framework for accepting/rejecting an AI-native receiver against the conventional baseline.

**Generic LLR→decoder interface statements in recent work:** *"Following the neural receiver, the estimated LLRs LLR_θ are fed into a standard channel decoder"* ([arXiv:2602.04728](https://arxiv.org/pdf/2602.04728v3)); *"The corresponding LLRs required by the LDPC decoder are obtained as"* ([arXiv:2602.11951](https://arxiv-org.ezproxy.obspm.fr/pdf/2602.11951)); *"Unlike hard-decision outputs, soft-output demappers furnish the channel decoder with log-likelihood ratios (LLRs) that quanti[fy]"* ([arXiv:2609.28852](https://arxiv.org/pdf/2609.28852v1.pdf)).

---

## 4. What this means for the team (short read)

1. If the goal is a **decoder-ready LLR interface**, the LINE-A classics give you no template: DetNet/OAMP-Net/OAMP-Net2/MMNet
   end in **soft symbols** and are scored with uncoded BER under perfect CSI. Do not expect a drop-in LLR contract.
2. The templates that do exist are: (a) **soft-output unfolded detectors with explicit scaling+clipping** (arXiv 2601.16586),
   (b) **LLR-correction / neural-LLR-reconstruction post-processors** (Chen & Wang 2023; arXiv 2606.29345 / IEEE 11587582),
   (c) **neural receivers trained with BCE-on-coded-bits** and handed to a standard LDPC decoder (Sionna, NVlabs `neural_rx`,
   the standard-compliant real-time neural receiver), and (d) **learned LLR quantisation** inside the decoder (arXiv 2208.05186).
3. Nobody in this literature reports a **calibrated** (temperature-scaled / scale-factor-searched) learned LLR for MIMO
   detection — that is a genuine open slot, not something you can copy.
4. Every quantitative claim about BER-vs-SNR numbers for DetNet/OAMP-Net/OAMP-Net2/MMNet needs a full-text pass with working
   fetch or the PDFs; the search index exposed the tables' headers but not their cells.

---

## 5. Uncertainties (explicit, do not treat as findings)

1. **"DetNet one-bit, IEEE TSP 2019"** — no such paper found; no 2017 IEEE SPL DetNet paper found. Stated as absence of
   evidence, not proof of non-existence.
2. **Training losses** for DetNet, OAMP-Net, OAMP-Net2, MMNet and both explanation papers: **none recovered verbatim**.
3. **Numeric results** (BER/BLER vs SNR, dB gains, MIMO sizes, modulation orders, channel models) for those papers:
   **none recovered verbatim**. The only numbers are the third-party MMNet "3 dB in 3GPP channels" and the un-attributable
   complexity table row.
4. **Parameter/complexity counts**: OAMP-Net2's comparison table (vs DetNet, DNN-dBP, DNN-MPD, TPG) exists; cells unreadable.
   SICNN's `4 892 300 / 4 894 800 / 9 815 100` row exists; units and column identity unverified.
5. **MMNet's output type** (LLRs vs symbols) and whether MMNet uses an LDPC decoder: **unverified** after many probes.
6. **GlobalSIP 2018 attribution** for OAMP-Net rests on the SIGPORT filename plus a GlobalSIP-2018 ToC existence — no official
   ToC entry, DOI string or page numbers were seen.
7. **OAMP-Net2's LLR expression** (`b_{j,k}`, `S_j⁺/S_j⁻`): the formula was seen, but **not** the sentence saying what happens
   to those LLRs (decoder? hard decision? scaling?).
8. **arXiv 2211.06054** (DetNet vs "MDetNet" parameter table + empirical per-LLR-bin error probability `P_emp,k`): a
   potentially ideal calibration reference whose **title and authors were not recoverable**; needs a direct fetch.
9. **CMDNet**: the antecedent of the histogram-of-LLRs sentence is unresolved; venue never seen (only IEEE doc 9546779);
   loss labelling and all quantitative results unverified.
10. **arXiv 2210.14103** (BER/BLER training): only one equation and two author names were recovered — **do not quote its
    losses, venue or numbers**.
11. **LDPC interface detail** — concrete quantisation bit-widths, saturation limits, and min-sum vs sum-product for learned
    receivers: **no paper in this sweep gave them in a retrievable passage**. Only "a standard channel decoder" statements.
12. **Real-time neural receiver (arXiv:2409.02912) numbers** — BLER gains, throughput, latency, LLR fixed-point format:
    existence and framing verified, numbers not.
13. Venue/year for: the optical experimental neural demapper, LLRSymNet, "Machine LLRning", arXiv 2606.29345's authors,
    the deep-unfolding survey's final publication venue, and arXiv 2210.14103's venue — all unresolved.
14. **Fritschek et al.** output type (bit probabilities vs LLRs) and all numeric results: unverified.
15. **Bit-wise MSE-against-true-LLR training**: no instance found in the indexed literature (negative evidence, could be a
    search-index limitation rather than a real gap).
