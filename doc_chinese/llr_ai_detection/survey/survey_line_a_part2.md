# LINE A, part 2 — MMNet and other deep-unfolding MIMO detectors, plus recent surveys

**Compiled 2026-10-07.** Method note: `web_fetch` was unusable as instructed, but `bash`+`curl` egress **did** work, so full texts were downloaded and text-extracted locally (pypdf). Quotes below are verbatim from the extracted text of the cited PDF/HTML; OCR/PDF line-break artifacts (stray spaces, hyphenation, broken math) are silently normalized to run as sentences, but wording is unaltered. Where a claim comes only from a search-result title or an API metadata record, that is stated explicitly. Everything else is in the **Could not verify** list at the end.

---

## 1. MMNet — "Adaptive Neural Signal Detection for Massive MIMO"

### 1.1 Identity, versions, URLs

| Item | Value | Evidence |
|---|---|---|
| TWC paper | IEEE Trans. Wireless Commun., **vol. 19, no. 8, pp. 5635–5648, 2020** | Survey ref [75]: `[75] M. Khani, M. Alizadeh, J. Hoydis, and P. Fleming, "Adaptive Neural Signal Detection for Massive MIMO," IEEE Trans. Wireless Commun., vol. 19, no. 8, pp. 5635–5648, 2020.` — https://arxiv.org/pdf/2502.05952 |
| DOI | 10.1109/TWC.2020.2996144 | Unpaywall record returns `TITLE: Adaptive Neural Signal Detection for Massive MIMO / JOURNAL: IEEE Transactions on Wireless Communications / YEAR: 2020 / OA: True`, OA URL `https://arxiv.org/pdf/1906.04610` |
| arXiv | 1906.04610 — **only v1 exists** (checked the abs page: `Submitted on 11 Jun 2019]` / `[v1]`) | https://arxiv.org/abs/1906.04610 |
| Accepted-manuscript copy (published version, IEEE header) | 14 pp., 9.3 MB | https://par.nsf.gov/servlets/purl/10169226 — header reads: `This article has been accepted for publication in a future issue of this journal ... Citation information: DOI 10.1109/TWC.2020.2996144, IEEE Transactions on Wireless Communications` |
| ICASSP 2020 paper | "Exploiting Channel Locality for Adaptive Massive MIMO Signal Detection", DOI 10.1109/ICASSP40776.2020.9052971 | Semantic Scholar record: `"title": "Exploiting Channel Locality for Adaptive Massive MIMO Signal Detection", "venue": "IEEE International Conference on Acoustics, Speech, and Signal Processing", "year": 2020`; Unpaywall: `JOURNAL: IEEE International Conference on Acoustics Speech and Signal Processing | YEAR: 2020 | OA: False`. Landing page https://ieeexplore.ieee.org/document/9052971 |
| ICASSP PDF host | `dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf` | Indexed and quotable via search titles (see §1.9), but direct download is **HTTP 401** — full text not obtained |
| PhD thesis | MIT PhD, EECS, 2023, 163 pp. | https://dspace.mit.edu/handle/1721.1/152866 ; working PDF endpoint: `https://dspace.mit.edu/server/api/core/bitstreams/bcde0905-33f2-4d90-acfd-6a916dbc0250/content` (the `/bitstream/handle/...` and `/bitstreams/.../download` URLs return HTTP 405 + `x-amzn-waf-action: captcha`) |
| Code / dataset | `https://github.com/mehrdadkhani/MMNet` | arXiv v1: `Our implementations and channels dataset are available at https://github.com/mehrdadkhani/MMNet.` |

### 1.2 Architecture — T layers, linear estimator + denoiser

Layer recursion (Eq. 13 of the TWC/arXiv text; identical in the thesis as Eq. 3.13):

> `MMNet: zt = x̂t + Θ(1)t (y − Hx̂t)`
> `x̂t+1 = ηt(zt; σ2t)`
> `where Θ(1)t is an Nt ×Nr complex-valued trainable matrix.`

Noise-variance model (Eq. 14):

> `σ2t = θ(2)t / Nt ( ‖I − AtH‖2F / ‖H‖2F [‖y − Hx̂t‖22 − Nrσ2]+ + ‖At‖2F / ‖H‖2F σ2 )`
> `where the parameter vector θ(2)t of size Nt × 1 scales the noise variance by different amounts for each symbol.`

**What the denoiser is / what is learned.** MMNet does *not* learn a black-box denoiser network. The denoiser is the model-based posterior-mean (conditional-mean) estimator for Gaussian noise, and the *neural* part is the per-layer linear operator plus the per-symbol noise-variance scaling:

> `MMNet uses a flexible linear transformation (which does not need to be linear in H) to construct the intermediate signal zt, but it applies the standard optimal denoiser for Gaussian noise in (5). Further, unlike OAMPNet, MMNet does not require any expensive matrix inverse operation.`

Denoiser definition (Eq. 5, "Optimal denoiser for Gaussian noise"):

> `β g t (z; σ2t) = 1/Z Σ_{xi∈X} xi exp( − ‖z − xi‖2 / σ2t )  where Z = Σ_{xj∈X} exp( − ‖z−xj‖2 / σ2t ).`

and its derivation (Eq. 4):

> `A natural choice for the denoising function is the minimizer of E[‖x̂ − x‖2|zt], which is given by: ηt(zt) = E[x|zt].`

**T / number of layers.**

> `MMNet concatenates T layers of the above form.` … `• MMNet: Our design described in (13). It has 10 layers, and the total number of trainable parameters is 2Nt(Nr + 1) per layer, independent of constellation size. In the systems evaluated, this results in 20K-41K trainable parameters.`

(Thesis, identical wording: `• MMNet: Our design described in (3.13). It has 10 layers, and the total number of trainable parameters is 2Nt(Nr + 1) per layer, independent of constellation size. In the systems evaluated, this results in 20K-41K trainable parameters.`)

**Note a version discrepancy** worth flagging to the team: arXiv **v1** says *total*, not *per layer*:
> `• MMNet: Our design described in (13). It has 10 blocks, and the total number of trainable parameters is 2Nt(Nr + 1) real values, independent of constellation size.`

So the published version (20K–41K params for Nt∈{16,32}, Nr=64, T=10) is 10× the arXiv-v1 figure. Cite the published number.

An "MMNet-iid" ablation also exists: `• MMNet-iid: The simple mode described in (11). This scheme has only 2 scalar parameters per layer and does not require any matrix inversions. We implement this neural network with 10 layers.` — and `must learn only 20 parameters in total, compared to the more than 1M trainable parameters of DetNet`.

### 1.3 ★ OUTPUT TYPE — soft symbol estimates + hard symbol decisions; **no LLRs, no bits**

MMNet's per-layer and final output is a **complex symbol estimate** (posterior mean over the constellation), and the reported detector output is a **hard symbol decision**. There is **no per-bit output and no LLR output anywhere in the TWC paper, the accepted manuscript, or the thesis**.

Verbatim evidence:

1. The denoiser *is* the posterior mean: `ηt(zt) = E[x|zt]` (Eq. 4, quoted above) and `β g t (z; σ2t) = 1/Z Σ xi exp(−‖z − xi‖2/σ2t)` — i.e. a soft symbol estimate, not a bit metric.
2. The definition of what the network state is: `The first step takes as input x̂t, a current estimate of x and the received signal y ... a non-linear "denoiser" is applied to zt to produce x̂t+1, a new estimate of x.`
3. Independent confirmation of the output stage, from the Nokia follow-up paper (HyperMIMO):
   > `MMNet consists of T layers performing (8), and a hard decision as in (5) to predict the final estimate x̂. One could also use rx(T) to predict bit-wise log likelihood ratios (LLRs).`
   — https://arxiv.org/pdf/2002.02750 (this "one could also use … LLRs" is phrased as an unexplored option, not a feature).
4. Independent confirmation that MMNet does **not** provide soft outputs usable by a decoder, from CMDNet (Beck et al., IEEE TCOM 2021), whose "latter approaches" are DetNet / OAMPNet / MMNet:
   > `One major drawback of the latter approaches is that they focus on MIMO detection and do not provide soft outputs.`
   — https://arxiv.org/pdf/2102.12756 (its ref [34] is `M. Khani, M. Alizadeh, J. Hoydis, and P. Fleming, "Adaptive Neural Signal Detection for Massive MIMO," IEEE Trans. Wireless Commun., vol. 19, no. 8, pp. 5635–5648, Aug. 2020.`)
5. **Metric is SER, not BER.** The string `BER` occurs **0 times** in the arXiv v1 text, in the NSF accepted-manuscript text, and in the thesis (checked by exact count). The performance metric is stated as:
   > `We use the SNR required to achieve an SER of 10−3 as the primary performance metric. In practice, most error correcting schemes operate around an SER of 10−3– 10−2, so this is the relevant regime for MIMO detection.`

**Bottom line for the requester: MMNet outputs estimated symbols (soft posterior means internally, hard symbol decisions externally). It is not LLR-output and not bit-output.**

### 1.4 Training loss

> `MMNet concatenates T layers of the above form. We use the the average L2-loss over all T layers in order to train the model, which is given by`
> `L = 1/T Σ_{t=1}^{T} ‖x̂t − x‖22.`  (Eq. 15)

Optimizer/training recipe:
> `To train MMNet, we use the Adam optimizer [32] with a learning rate of 10−3. Each training batch has a size of 500 samples. We train MMNet for 1K iterations on each realization of H in the naive implementation. In § VI-B, we exploit frequency and time domain correlations to reduce the training requirement to 4 iterations per channel matrix.`

Online-training schedule (Algorithm 1): `if f == 1 then numTrainIterations ← 1000 else numTrainIterations ← 3`, and
> `MMNet performs 3.55K iterations of training with batch size 500 in order to learn a detector for all 1024 subcarriers in total at each time interval n. Therefore the cost of online training is less than 4 iterations on average per channel realization` (thesis version says `9 iterations of batch size 500 on average`).

MMNet-iid is trained offline once: `we use 10K iterations with a batch size of 500 samples to train a single MMNet-iid neural network, which we then test on new channel samples.`

### 1.5 LDPC / channel decoder in the loop — **NO**

There is **no LDPC decoder, no channel decoder, and no coded-link simulation** in MMNet. Exact-substring checks on the arXiv v1 text, the NSF accepted-manuscript text, and the thesis return **zero** hits for `LDPC`, zero for `channel decoder`, and zero for `BER`; the only `decoder` hits are the phrase `MMSE: Linear decoder` (describing a baseline) and a bibliography entry (`...code decoder for fading channels`).

The closest thing to a coded-system statement is the *motivation* sentence quoted in §1.3: `In practice, most error correcting schemes operate around an SER of 10−3– 10−2, so this is the relevant regime for MIMO detection.` It is never followed up with an actual coded simulation.

**Modulation used:** QAM4, QAM16, QAM64 (all results figures are for these three). `In our experiments, we compare the following schemes on QAM modulation:` and figure captions `SER vs. SNR of different schemes for three modulations (QAM4, QAM16 and QAM64) and two system sizes (32 and 16 transmitters, 64 receivers) with i.i.d. Gaussian channels` / `... with 3GPP MIMO channels`. Thesis adds that for MMNet-iid, `The DetNet paper describes instantiations of the architecture for BPSK, QAM4 and QAM16`.

### 1.6 Channel models — 3GPP 3D MIMO (TR 36.873 via QuaDRiGa) + i.i.d. Gaussian; **not CDL, not Kronecker**

This corrects the requester's suspicion. MMNet uses (a) i.i.d. Gaussian and (b) the **3GPP 3D MIMO channel model (3GPP TR 36.873)** implemented in **QuaDRiGa**:

> `The channel matrices H are either sampled from an i.i.d. Gaussian distribution (i.e., each column of H is a complex-normal CN(0, (1/Nr)INr)), or they are generated via the realistic channel simulation described below.`
> `For the case of realistic channels, we generate a dataset of channel realizations from the 3GPP 3D MIMO channel model [12], as implemented in the QuaDRiGa channel simulator [13].`
> `[12] 3GPP TR 36.873. 2015. "Study on 3D channel model ..."` (from the reference list)
> `The simulation results were generated using QuaDRiGa Version 2.0.0-664.`

Kronecker appears **only as a description of prior work**, not of MMNet's evaluation:
> `Gaussian and small-sized correlated channel matrices based on the Kronecker model with exponentially-distributed spatial correlations [11]. DetNet and OAMPNet are both trained offline ...`

Corroboration from the survey's Table III MMNet row: `Massive MIMO, QAM modulation, 3GPP 3D channel, Nt = {16, 32}, Nr = 64.` — https://arxiv.org/pdf/2502.05952

The 3GPP TR 36.873 3D channel model is a *predecessor* of the CDL family, so "3GPP CDL" is not literally what MMNet evaluated on. Geometry: `a base station (BS) equipped with a rectangular planar array consisting of 32 dual-polarized antennas installed at a height of 25 m ... 120°-cell sector of radius 500 m ... Nt ∈ {16, 32} single-antenna users ... speed of 1 m/s ... center frequency of 2.53 GHz ... bandwidth of 20 MHz and using 1024 sub-carriers from which only every fourth is kept, resulting in F = 256 effective sub-carriers.`

### 1.7 Reported numbers

**dB gains (headline, abstract of the published version):**
> `On spatially-correlated channels, it achieves the same error rate as the next-best learning scheme (OAMPNet) at 2.5dB lower signal-to-noise ratio (SNR), and with at least 10× less computational complexity. MMNet is also 4–8dB better overall than a classic linear scheme like the minimum mean square error (MMSE) detector.`

(arXiv v1 abstract is the same in substance: `...at 2.5dB lower signal-to-noise ratio (SNR) and with at least 10 × less computational complexity. MMNet is also 4–8dB better overall than a classic linear scheme like the minimum mean square error (MMSE) detector.`)

**Per-figure findings (3GPP channels):**
> `OAMPNet performance improvement slope is faster than MMSE. It shows 2–3dB average improvement in SNR requirement relative to MMSE to achieve the same SER.` … `MMNet outperforms MMSE and OAMNet schemes for both system sizes and in all modulations.`
> `We observe that MMNet can achieve up to 5dB and 8dB improvement, respectively, over OAMPNet and MMSE on more realistic channels.`

**Gap to ML (SER = 10⁻³):** arXiv v1/TWC: `MMSE has an 8–10dB gap with Maximum-Likelihood on 64×16 channels. OAMPNet reduces this gap to 5–7dB. However, MMNet closes the gap to less than 1.5dB.` Thesis: `OAMPNet reduces this gap to 5dB, and MMNet closes the gap further to less than 2.2dB.`

**i.i.d. findings:** `There is a 2–3dB performance gap between Maximum-Likelihood and MMSE across all modulations for Nt = 32. However, this gap decreases to 1dB for Nt = 16` and `MMNet-iid has O(Nr)× lower computational complexity than OAMPNet because it does not need matrix inversions and must learn only 20 parameters in total, compared to the more than 1M trainable parameters of DetNet.`

**Parameter counts:** `2Nt(Nr + 1) per layer ... In the systems evaluated, this results in 20K-41K trainable parameters.` Baseline counts given in the paper: OAMPNet `10 layers with 2 trainable parameters per layer` (=20); DetNet `on the order of 1–10M, trainable parameters depending on the size of the system and constellation set`.

**Complexity (order + multiplications):**
> `One iteration of training MMNet on a batch of size of b has a complexity of O(bN2r), as detection takes O(N2r) in MMNet.` … `The MMSE scheme has a higher complexity of O(N3r) because it needs to invert a matrix. OAMPNet similarly requires a matrix inversion, resulting in a complexity of O(N3r).`
> `Detection with MMNet, including its online training process, requires fewer multiplication operations than detection with pre-trained DetNet and OAMPNet models.` (Fig. 11 spans 10⁴–10⁷ multiplication ops, Nt = 16 and 32)
> `MMNet with online training and detection operations together, places after AMP with 2–5 × fewer multiplications than pre-trained DetNet.`
> `Consequently, the cost of MMNet with its online training algorithm is 10–15 × less than OAMPNet depending on the system size. MMNet has only 41 × higher computational complexity than a very light iterative approach like AMP`

**Latency:** no numeric latency is reported. The only latency statement is qualitative:
> `the sequential online training algorithm introduced in this paper incurs significant latency which may be traded off with parallel training of multiple sub-carriers at the cost of more training iterations and hence increased complexity. The optimal trade-off depends on the channel coherence time.`

### 1.8 Independent summary of MMNet (for cross-checking)
Survey 2502.05952, §III-A:
> `MMNet [75] unfolds ISTA, alternating linear detection and nonlinear denoising, designed for ill-conditioned and non i.i.d. channels. MMNet balances complexity and flexibility, outperforming DetNet [70], OAMP-Net [10]. The MMNet outperforms the classical LMMSE detector with a performance gain of approximately 4 − 6 dB at a symbol error rate (SER) of 10−3 for different channel conditions.`

6G survey (arXiv 2201.03866, published IEEE OJ-COMS):
> `the authors of [59] focus on a robust DL-based MIMO detector that can optimize itself via online training during the transmission over realistic and spatially correlated channel models. The proposed detector, called MMNet, is built upon iterative soft-thresholding algorithms. Experiments conducted by using a dataset of channel realizations from the 3GPP 3D MIMO channel indicate that MMNet outperforms the classical approaches such as AMP and SDRX, as well as recent DL techniques like DetNet [50], [51], and OAMPNet [52], under the assumption of perfect CSI.`

### 1.9 ICASSP 2020 paper — partial only (PDF not retrievable)
Direct download of `http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf` returns **HTTP 401**; the `https://` variant fails to connect. The paper's identity is nevertheless confirmed (title/venue/year/DOI as in §1.1), and two verbatim passages were recovered from indexed search-result titles pointing at that exact PDF:

- Title/passage: `EXPLOITING CHANNEL LOCALITY FOR ADAPTIVE MASSIVE MIMO SIGNAL DETECTION` — http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf
- `In this section, we evaluate and compare the performance of MMNet with state- of- the- art schemes on 3GPP MIMO channel matrices...` — same URL
- `In this section, we show how channel locality can help reduce the total number of operations MMNet needs to decode each received...` — https://par.nsf.gov/servlets/purl/10169226 (this sentence also appears verbatim in the TWC §VI and thesis §3.7.2, which the ICASSP paper is the short version of)

Content-wise the ICASSP paper is the conference version of TWC §VI (channel locality / online-training acceleration), so the TWC + thesis answers above cover the same material. **The ICASSP paper's own abstract and numeric claims were not read — see Could-not-verify.**

### 1.10 PhD thesis — layer-by-layer description
The thesis (163 pp.) reproduces MMNet chapter 3 essentially verbatim from the TWC paper: Eq. (3.13) `MMNet` recursion, Eq. (3.14) noise variance, Eq. (3.15) `L = 1/T Σ ‖x̂t − x‖22`. Chapter 3 also contains the deeper *analysis* the requester wants, e.g.:
> `We empirically analyze the dynamics of errors across different layers of MMNet ...`
> `We observe a stronger locality in the frequency domain than in the time domain on these channels.`
> `in every channel coherence interval in the 3GPP MIMO model, each algorithm receives ∼100 signals to detect`
> `MMNet reuses the weights it trains (with 9 iterations of batch size 500 on average) for all 100 received signals in a coherence interval.`

---

## 2. Other deep-unfolding detectors

### 2.1 LAMP / Learned AMP (sparse linear inverse problems)
**Borgerding, Schniter, Rangan, "AMP-Inspired Deep Networks for Sparse Linear Inverse Problems," IEEE TSP 2017** (arXiv 1612.01183v2). ACM DL: https://dl.acm.org/doi/abs/10.1109/TSP.2017.2708040 ; https://arxiv.org/abs/1612.01183

Abstract (verbatim):
> `we propose two novel neural-network architectures that decouple prediction errors across layers in the same way that the approximate message passing (AMP) algorithms decouple them across iterations: through Onsager correction. First, we propose a "learned AMP" network that significantly improves upon Gregor and LeCun's "learned ISTA." Second, inspired by the recently proposed "vector AMP" (VAMP) algorithm, we propose a "learned VAMP" network that offers increased robustness to deviations in the measurement matrix from i.i.d. Gaussian. In both cases, we jointly learn the linear transforms and scalar nonlinearities of the network.` … `Finally, we apply our methods to two problems from 5G wireless communications: compressive random access and massive-MIMO channel estimation.`

- **Output type:** linear/sparse signal estimate (`x̂T(y; Θ)`), i.e. symbols/coefficients — **not** LLRs. Text: `and x̂T(y(d); Θ) the output of the T-layer network with input ...`; shrinkage stages constrained to soft-thresholding: `nonlinear stages were constrained to the soft-thresholding`.
- **Loss:** quadratic loss — `minimizing the quadratic loss`; `minimize the loss ΣD ...`
- **MIMO detection?** This paper does **not** apply LAMP to MIMO *detection* (it targets compressive random access and massive-MIMO *channel estimation*).

**LAMP applied to MIMO detection:** Miyoshi/Tsujimoto/Nishimura/Hagiwara et al., **"Parameter-Learned AMP for MIMO Signal Detection," IEEE VTS APWCS 2022**, DOI 10.1109/APWCS55727.2022.9906492, https://ieeexplore.ieee.org/document/9906492
Abstract (via OpenAlex, DOI 10.1109/apwcs55727.2022.9906492):
> `In this paper, two types of the learned AMP (LAMP) is proposed, and those detection performance is evaluated. The numerical evaluation results show that a modification to residual interference power is needed to optimize the hyper-parameters properly for the straightforwardly-implemented LAMP and that the higher performance is obtained by the simple structure LA[MP]...`
Also: `the detection performance severely degrades when two conditions, i.e., the large-system limit and a channel matrix property that each element follows independent and identically distributed complex Gaussian distribution, are not satisfied.` Output = detected symbols (BER metric). No LLR output mentioned.

Related: **AMP-DNN** (low-complexity AMP detection with DNN), Digital Communications and Networks 2022, DOI 10.1016/j.dcan.2022.11.011 — `the number of trainable parameters is only related to that of layers, regardless of modulation scheme, antenna number and matrix calculation`.
Also: **"Learning Stabilization in Deep Unfolding of Generalized Approximate Message Passing"**, IEEE WCNC 2025, DOI 10.1109/WCNC61545.2025.10978405 — learns `the differential (incremental) values of the learnable parameters between two consecutive iterations`; metric is `bit error rate (BER) of massive MIMO detection`.

### 2.2 ADMM-Net (deep ADMM unfolding) for MIMO detection
**M. Kim, D. Park, "Learnable MIMO Detection Networks Based on Inexact ADMM," IEEE Trans. Wireless Commun., vol. 20, no. 1, pp. 565–576, 2021**, DOI 10.1109/TWC.2020.3026471 (survey ref [42]: `[42] M. Kim and D. Park, "Learnable MIMO Detection Networks Based on Inexact ADMM," IEEE Trans. Wireless Commun., vol. 20, no. 1, pp. 565–576, 2021.`), https://ieeexplore.ieee.org/document/ (ACM mirror: https://dl.acm.org/doi/10.1109/TWC.2020.3026471)

Abstract (via OpenAlex, DOI 10.1109/twc.2020.3026471):
> `In this article, we present a new iterative MIMO detection algorithm based on inexact alternating direction method of multipliers. Each iteration is considered as a neural network layer with learnable parameters, which are optimized by the stochastic gradient descent algorithm with a training data set of the received vectors and the ground truth transmitted signals. Numerical results show that the proposed algorithm outperforms the existing learnable detection network and it achieves near-optimal performance close to the sphere decoder in the case of a large number of receive antennas.`

- **Output type:** transmitted **symbols** (`ground truth transmitted signals` as training target) — **no LLR output claimed**.
- **Numbers (survey Table III):** `ADMM-Net [42] | ADMM: O(M3) | O(M2) | 5T | MIMO Rayleigh fading channel, BPSK, 4-QAM and 16-QAM, Nt = Nr = 50. | 30 iterations | < 20 Layers` — https://arxiv.org/pdf/2502.05952. Survey prose: `Compared to DetNet [70], the proposed network [42] requires fewer trainable parameters, resulting in improved performance in terms of symbol error.`
- Related ADMM unfoldings: **RADMMNet / LCRADMMNet** for imperfect CSI (arXiv 2307.12575, https://arxiv.org/abs/2307.12575); **"Learnable binary MIMO detection with negative penalty based on inexact ADMM,"** Electronics Letters 2024, DOI 10.1049/ell2.13155 (one-bit ADCs).
- Also, from the 6G survey: `By unfolding, another conventional iterative technique, the alternate direction method of multipliers (ADMM), is mapped to a DL framework in [114]. The authors provide ADMM-Net for large-scale mmWave communications in which the DL-based SLP selects the optimum subset of RF chains...` (that one is beam-selection, not symbol detection).

### 2.3 VAMP-Net / unfolded VAMP for MIMO
**No "VAMP-Net for MIMO detection" paper was found.** What exists:
- **Learned VAMP** as an architecture, in the LAMP paper (sparse linear inverse problems; §2.1 above).
- **Unfolded VAMP in wireless, but for grant-free random access (joint activity detection + channel estimation), not symbol detection:** `By exploiting structured sparsity and cluster sparsity, we unfold the vector approximate message propagation (VAMP) algorithm with Bernoulli Gaussian mixed distribution into a network and introduce a parameter estimation module to adapt different active ratios and noise variance based on the expectation maximization (EM) algorithm.` — "Model-Based Deep Learning for Massive Access in mmWave Cell-Free Massive MIMO System," IEEE ICC Workshops 2024, DOI 10.1109/ICCWorkshops59551.2024.10615768.
- VAMP is also mentioned in the SICNN bibliography lineage but not as a MIMO detector.
→ Treat "VAMP-Net for MIMO detection" as **not verified to exist**; the nearest real artefact is learned VAMP for sparse recovery.

### 2.4 OAMP variants
- **OAMP-Net:** He, Jin, Wen, Gao, Li, Xu, "Model-Driven Deep Learning for Physical Layer Communications," IEEE Wireless Communications, vol. 26, no. 5, pp. 77–83, 2019 (as cited in SICNN ref [7]/[19]).
- **OAMP-Net2:** He, Wen, Jin, Li, **"Model-Driven Deep Learning for MIMO Detection," IEEE Trans. Signal Process., vol. 68, pp. 1702–1715, 2020**, arXiv 1907.09439, https://arxiv.org/abs/1907.09439. This one **does claim LLR output**, with an important caveat:

  > `As many modern digital communication systems need to produce a probabilistic estimation of the transmitted data given the observations to probabilistic channel decoder. A significant issue is whether the MIMO detector can use the soft-information from the decoder and produce the soft-output. Different from the DetNet in [19] that only can provide the soft-output, our proposed detector are the soft-input and soft-output receiver and therefore can achieve the turbo equalization. We only provide the principle of the OAMP-Net2-based turbo receiver and the specific experimental results are outside the scope of this paper and will be conducted in the future.`

  and Eq. (29): `The marginal posterior probability is used to produce the soft output log-likelihood ratios (LLR), which is given by LA(bj,k) = log( Σ_{S+j} P(xj|rt,τt) / Σ_{S−j} P(xj|rt,τt) )`, followed by `After interleaved and delivered to the channel encoder, extrinsic LLR can be computed and given to the OAMP-Net2 detector as updated prior information. The detector and decoder iteratively exchange information until convergence.`

  → **So: LLR output is formulated, but the turbo/decoder-in-the-loop experiments are explicitly deferred ("will be conducted in the future").** This is a concrete instance of the open-problem theme.
- **Parameter counts / comparison table (OAMP-Net2 paper, Table I — verbatim row labels and values):**
  `Computational complexity: OAMP-Net2 O(T N3t) | DetNet O(T N2t) | DNN-dBP O(T NrNt) | DNN-MPD O(T NrNt) | TPG O(T NrNt) | LcgNet O(T NrNt)`
  `Learnable variables: OAMP-Net2 4T | DetNet (6NrNt + 2Nr + Nt)T | DNN-dBP T(Nr + Nt)T | DNN-MPD 2T | TPG 4NtT | LcgNet ...` (row is run-together in the PDF extraction; treat the per-column alignment as approximate except for OAMP-Net2 = 4T, which is also stated in prose)
  Prose: `the total number of trainable variables is equal to 4T since each layer of the OAMP-Net2 contains only four trainable variables Ωt = (γt, φt, ξt, θt). By contrast, 2T trainable variables (γt, θt) are required to train in OAMP-Net.` and `the number of trainable variables of the OAMP-Net2 and OAMP-Net are independent of the number of antennas Nr and Nt, and only determined by the number of layers T.`
  Setup (survey Table III): `MIMO Rayleigh fading channel, QPSK and 16-QAM, Nt = Nr = 4 and Nt = Nr = 8. | 1000 iterations | 4 Layers`.
- **OAMP-Net2 variants for correlated measurements / "Bayes-optimal" unfolding —** see §2.7.

### 2.5 SICNN — "Soft Interference Cancellation Inspired Neural Network Equalizers"
S. Baumgartner, O. Lang, M. Huemer, **IEEE Transactions on Machine Learning in Communications and Networking (TMLCN), 2024**, DOI 10.1109/TMLCN.2024.3377174, https://ieeexplore.ieee.org/document/10471626 , arXiv 2308.12591.

- **What it is:** deep unfolding of an iterative soft interference cancellation (SIC) *equalizer*, for SC-FDE and UW-OFDM — **it is an equalizer, not a MIMO symbol detector**, and the requester should note that scope difference.
  Abstract: `SICNN is designed by deep unfolding a model-based iterative soft interference cancellation (SIC) method. It eliminates the main disadvantages of its model-based counterpart, which suffers from high computational complexity and performance degradation due to required approximations. ... SICNNv1 is specifically tailored to single carrier frequency domain equalization (SC-FDE) systems ... SICNNv2 is more universal ...`
- **Output type (verbatim):**
  > `the SIC operation from the model-based method is preserved in the developed NNs, which is expected to help SICNNv1 and SICNNv2 to provide reliable soft estimates (required, e.g., to compute log-likelihood ratios in case of channel coded transmission), and allows to obtain interpretable intermediate quantities / variables inside the NN-based equalizers.`
  and
  > `every stage of SICNNv1/SICNNv2 has to provide estimates of the posterior data symbol probabilities, given the estimates of the previous stage and the received vector, and thus should work with the same set of learnable parameters.`
  → **output = posterior data-symbol probabilities (soft estimates); LLRs are named as a downstream consumer, not produced directly.**
- **Loss (verbatim):** `We optimize the parameters of SICNNv1 by employing a custom loss function based on the cross entropy loss` — a weighted sum of per-stage (per-"layer") partial cross-entropies, `inspired by the loss function employed for training DetNet [18] and by the auxiliary classifiers of GoogLeNet [41]`.
- **Complexity:** measured in `real-valued multiplications required for the equalization of a received vector` (Sec. V-E); closed-form count for SICNNv1 given as `MSICNNv1 = QNd MSICNNv1,kq + 4NdN + 4N + 1`; the paper also presents reduced-parameter variants (`we additionally present a version with a reduced number of learnable parameters`).

### 2.6 Deep HyperNetwork-Based MIMO Detection (HyperMIMO)
M. Goutay, F. Ait Aoudia, J. Hoydis, arXiv **2002.02750** (v2), https://arxiv.org/abs/2002.02750 ; published version per survey ref [107]: `J. Zhang, C.-K. Wen, and S. Jin, "Adaptive MIMO Detector Based on Hypernetwork: Design, Simulation, and Experimental Test," IEEE J. Sel. Areas Commun.` — *note: that reference is a different (JSAC) paper on the same hypernetwork idea; see Could-not-verify.*

- **What it does:** `we address both issues by training an additional neural network (NN), referred to as the hypernetwork, which takes as input the channel matrix and generates the weights of the neural NN-based detector.` It is built on a **parameter-reduced MMNet**, taking `RA` from a QR decomposition plus the noise std `σ`.
  `To reduce the number of parameters of MMNet, we leverage ...` and `The number of layers of MMNet in the HyperMIMO detector was set to T = 5. The hypernetwork was made of 3 dense layers ... the second layer 75 units ...`
- **Output type (verbatim):** `MMNet consists of T layers performing (8), and a hard decision as in (5) to predict the final estimate x̂. One could also use rx(T) to predict bit-wise log likelihood ratios (LLRs).`
- **Loss (verbatim):** `HyperMIMO, which comprises the hypernetwork and MMNet, is trained by minimizing the MSE L = Ex,H,n[‖rxT − x‖22]. Note that this loss differs from the one of [8], which is 1/T ΣTt=1 Ex,H,n[‖rxt − x‖22].`
- **Numbers (verbatim):** `to achieve a SER of 10−3, HyperMIMO exhibits a loss of 0.65dB compared to MMNet, but a gain of 1.85 dB over OAMPNet and 2.85 dB over LMMSE.` Setup: `QPSK constellation`, `6 users`, `SNRs from the range [0,10] dB`, Adam `batch size of 500 and a learning rate decaying from 10−3 to 10−4`; `our scheme only has 10× more parameters than MMNet as proposed in [8]`, and `HyperMIMO was trained with fixed channel statistics, i.e., fixed user positions`. Metric = SER (Fig. 5).
- **No LDPC decoder** in the loop; no LLR output.

### 2.7 "Low-Complexity Deep MIMO Detection for Correlated Measurements" — and Bayes-optimal unfolding
**Zhang, Luo et al., ICVISP 2025**, DOI **10.1109/ICVISP68610.2025.11451696**, https://ieeexplore.ieee.org/document/11451696 (IEEE Xplore returns HTTP 202/0 bytes to automated clients; abstract obtained via OpenAlex, DOI 10.1109/icvisp68610.2025.11451696).

Abstract (verbatim, OpenAlex):
> `This paper proposes a model-driven deep multiple-input multiple-output (MIMO) detector that augments Orthogonal Approximate Message Passing Networks 2 (OAMP-Net2) to achieve robust recovery under correlated channels while reducing reliance on the large-system assumption. In conventional OAMP-Net, the Bayes-optimal MMSE updates depend on approximate error orthogonality that holds for unitarily invariant channel ensembles. Under strongly correlated channels these conditions weaken, so the residual is not well approximated as independent Gaussian and the nonlinear updates can become suboptimal, which may degrade detection accuracy. Moreover, heightened noise sensitivity hampers reliable estimation. To address these issues, we introduce an annealed discrete decision (ADD) module that schedules the nonlinearity across layers via a learnable inverse temperature. By dynamically tuning the denoising strength, ADD suppresses interference accumulation and limits error propagation. Simulation results demonstrate that the proposed network significantly improves detector performance across a range of practical settings and remains robust at low SNR.`

This is the closest artefact found to **"Bayes-optimal unfolding for MIMO detection"**: it explicitly targets the breakdown of the Bayes-optimal MMSE update assumption in OAMP-Net under strong correlation. Output type not stated in the abstract (numeric results are framed as detection performance / robustness) → see Could-not-verify.

### 2.8 Deep Unfolding with Kernel-based Quantization in MIMO Detection
arXiv **2505.12736**, https://arxiv.org/abs/2505.12736 ; presented at **ICML 2025 ML4Wireless workshop** — arXiv comments field: `submitted to ICML ML4Wireless workshop`; workshop page https://icml.cc/virtual/2025/47048.

- **What it does:** a kernel-based adaptive quantization (KAQ) QAT framework for **deep unfolding networks such as PGD-Net and ADMM-Net** in MIMO detection: `Deploying deep unfolding models such as PGD-Nets and ADMM-Nets into resource-constrained edge devices using quantization methods is challenging. ... this paper proposes a novel kernel-based adaptive quantization (KAQ) framework for deep unfolding networks. By utilizing a joint kernel density estimation (KDE) and maximum mean discrepancy (MMD) approach to align activation distributions between full-precision and quantized models, the need for prior distribution assumptions is eliminated.`
- **Output type:** detected **symbols**; metric = `bit error rate (BER) versus SNR of PGD-Net under different precisions`. No LLR.
- **Loss (verbatim):** `The total loss combines the mean squared error (MSE) and the MMD loss, optimizing the detection accuracy and capturing the matching distribution of the activations at the same time`.
- **Numbers (verbatim):** `Both PGD-Net and ADMM-Net architectures are unfolded into K = 5 trainable layers, initialized with uniform parameters ηk = 0.1 and λk = 0.05`; `a dataset of 5×104 samples`; Adam `learning rate of 10−3 ... total of 50 training epochs ... Mini-batches of size 128`; `the inference latency of the quantized models using KAQ is reduced in 20% compared to the full precision version.` Claims: `allows the KAQ dynamic quantization method to perform nearly as well as the FP16-precision PGD-Net baseline`.

### 2.9 Other unfolded detectors that *do* target soft/LLR output (relevant to the theme)
- **CMDNet** — Beck, Bockelmann, Dekorsy, "CMDNet: Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection with Low Complexity," **IEEE Trans. Commun., vol. 69, no. 12, pp. 8214–8227, 2021**, arXiv 2102.12756, https://arxiv.org/abs/2102.12756. Explicitly a *soft-output* unfolded detector, and the one that **runs an actual LDPC decoder**: `we use a 128 64 LDPC code with rate RC = 1/2 from [39] and at receiver side a belief propagation decoder with 10 iterations`, reporting `Coded Frame Error Rate (CFER)`. Also reports soft output for `32 32 QPSK` with `LDPC`, and states `In coded systems with soft decoders usually employed today, delivering soft information is a strict requirement.`
- **Soft-Output Deep LAS** — A. Ullah, W. Choi, T. M. Berhane, Y. Sambo, M. A. Imran, "Soft-Output Deep LAS Detection for Coded MIMO Systems: A Learning-Aided LLR Approximation," **IEEE Trans. Veh. Technol., 2024** (survey ref [106]). Survey text: `Ullah et al. [106] unfolded a likelihood ascent search algorithm for joint detection and soft-output estimation.`
- **Tan et al.**, "Improving Massive MIMO Message Passing Detectors With Deep Neural Network," IEEE TVT 69(2) 1267–1280, 2020 (survey ref [86]): unfolded MPD with `LLR rescaling` among the learned correction factors.

---

## 3. Recent (2024–2026) surveys

### 3.1 ★ "Comprehensive Review of Deep Unfolding Techniques for Next-Generation Wireless Communication Systems"
Sukanya Deka, Kuntal Deka, Nhan Thanh Nguyen, Sanjeev Sharma, Vimal Bhatia, Nandana Rajatheva.
- **arXiv 2502.05952** (downloaded copy is **v4, `arXiv:2502.05952v4 [eess.SP] 14 Jan 2026`**), https://arxiv.org/abs/2502.05952
- **Venue: NOT established.** No journal-ref or DOI is present in the PDF or on the arXiv metadata; OpenAlex lists it as `YEAR: 2025 | VENUE: arXiv (Cornell University) | TYPE: preprint`. So: **preprint, no venue verified** (do not claim a journal).
- **Scope (abstract):** `We then explore the application of deep unfolding in key areas, including signal detection, channel estimation, beamforming design, decoding for error-correcting codes, integrated sensing and communication, power allocation, and physical layer security.` Sections: III Signal Detection, IV Channel Estimation, V Precoder Design, VI Sensing & Communication, VII Decoding in ECC, VIII Power Allocation, IX Physical Layer Security, X Challenges, XI Conclusions.
- **Comparison tables** — the survey's own framing: `In addition, each section concludes with a cross method comparison table that evaluates prominent unfolded networks across ke[y metrics]` (search-title hit on https://browse-export.arxiv.org/pdf/2502.05952). The signal-detection table is **TABLE III: Comparison of Classical vs Unfolded methods for Signal Detection**, columns = `Unfolded Model | Computational Complexity (Classical | Unfolded) | Number of Parameters (Unfolded) | Convergence speed (Number of layers) (Classical | Unfolded) | Simulation Setup`. Verbatim rows:
  - `DetNet [70] | Sphere Decoder: O(M Nt) | O(T N2t) | (6Nr Nt + 2 Nr + Nr)T | QPSK, 8-PSK, 16-QAM, 0.55-Toeplitz channel, Nt = {15, 20, 30}, Nr = {20, 25, 30}. | ∼ 1000 iterations | 30 Layers`
  - `LcgNet [57] | CGD: O(I(8N2t + 14Nt + 8)) | O(T(4N2t + 6Nt)) | (6Nt + 4N2t + 4T) | MIMO Nr = 32, Nt = 64, Rayleigh fading channel. | ∼ 40 iterations | 7 Layers`
  - `OAMP-Net2 [11] | OAMP: O(N3t) | O(T N3t) | 4T | MIMO Rayleigh fading channel, QPSK and 16-QAM, Nt = Nr = 4 and Nt = Nr = 8. | 1000 iterations | 4 Layers`
  - `MMNet [75] | MMSE: O(N3r) | O(N3r) | 2Nt(Nr + 1) | Massive MIMO, QAM modulation, 3GPP 3D channel, Nt = {16, 32}, Nr = 64. | 50 iterations | 10 Layers`
  - `ADMM-Net [42] | ADMM: O(M3) | O(M2) | 5T | MIMO Rayleigh fading channel, BPSK, 4-QAM and 16-QAM, Nt = Nr = 50. | 30 iterations | < 20 Layers`
  **⚠️ Discrepancy to flag:** the survey lists MMNet's *unfolded* complexity as `O(N3r)`, which contradicts MMNet's own claim of `O(N2r)` detection with `no expensive matrix inverse operation`. The survey's MMNet parameter count `2Nt(Nr+1)` also matches the arXiv-v1 "total", not the published "per layer" figure. Treat the survey's MMNet row as partly inaccurate.
- **Memory/latency remark:** `methods like MMNet [75] and LoRD-Net [80] introduce additional latency and training complexity due to their reliance on online training and periodic retraining, making them less suitable for fast-varying channel environments.`
- **★ Does it make the "outputs are symbols not LLRs / LLR is an open problem" claim?** — **No, not as a general statement.** Its only output-type remarks are per-paper (see §4). Its declared open challenges are **C1–C5**: methodological (nested solvers), `Unfolding principle algorithms` (`Complex operations such as matrix decompositions or convex solver calls ... are difficult to represent in differentiable NN layers`; `the fixed number of layers: iterative algorithms run until convergence, whereas unfolding models require the number of layers to be fixed a priori`), `Handling design constraints and objectives`, `Sophisticated mappings`, and `Real-world applications` (`unfolding designs are not naturally aligned with area-efficient or parallelizable architectures`). **LLR/decoder-compatible output is not listed among them.**

### 3.2 "Model-Based Deep Learning" — Shlezinger, Whang, Eldar, Dimakis
- **Venue verified: Proceedings of the IEEE, vol. 111, no. 5, pp. 465–499, May 2023, DOI 10.1109/JPROC.2023.3247480.** (Crossref record: `TITLE: ['Model-Based Deep Learning'] CONTAINER: ['Proceedings of the IEEE'] VOL: 111 ISSUE: 5 PAGE: 465-499 YEAR: [[2023, 5]] AUTH: ['Shlezinger','Whang','Eldar','Dimakis']`.) ✅ **Yes — it is Proceedings of the IEEE 2023.**
- arXiv: **2012.08405** (v3), https://arxiv.org/abs/2012.08405 — note the arXiv metadata has **no** journal-ref/DOI (`JOURNAL_REF: none | DOI: none`), so the venue must be cited from Crossref/the publisher, not arXiv. Also available: https://www.weizmann.ac.il/math/yonina/sites/math.yonina/files/Model-Based_Deep_Learning_0.pdf
- Scope: `In this article we present the leading approaches for studying and designing model-based deep learning systems.` Contains a dedicated **`A. Deep Unfolding`** subsection:
  > `Deep unfolding [56], also referred to as deep unrolling ... each layer to resemble a single iteration.`
  > `Example 1: Deep Unfolded Projected Gradient Descent:` … `the DetNet system of [18] which unfolds projected gradient descent optimization`
  > `It is also noted in [18] that the unfolded network requires an [intermediate normalization] ...` (DetNet discussion)
- Also from the same group: Shlezinger, Whang, Eldar, Dimakis, "Model-Based Deep Learning: Key Approaches and Design Guidelines," IEEE DSLW 2021, DOI 10.1109/DSLW51110.2021.9523403. (Different paper, same authors — don't conflate.)
- Related: `A. Balatsoukas-Stimming and C. Studer, "Deep Unfolding for Communications Systems: A Survey and Some New Directions," in 2019 IEEE International Workshop on Signal Processing Systems (SiPS), 2019, pp. 266–271` (SICNN ref [8]) — an earlier, pre-2024 survey.

### 3.3 "Deep Learning-Aided 6G Wireless Networks: A Comprehensive Survey of Revolutionary PHY Architectures"
- **arXiv 2201.03866**, https://arxiv.org/abs/2201.03866 — **published in IEEE Open Journal of the Communications Society**, `DOI: https://doi.org/10.1109/OJCOMS.2022.3210648` (from the arXiv abs page: `COMMENTS: Published in IEEE OJ-COMS`, `JOURNAL_REF: none`). Note the requester's short title is missing the suffix "**of Revolutionary PHY Architectures**".
- Scope (abstract): `we have focused our attention on four promising PHY concepts foreseen to dominate next-generation communications, namely massive multiple-input multiple-output (MIMO) systems, sophisticated multi-carrier (MC) waveform designs, reconfigurable intelligent surface (RIS)-empowered communications, and PHY security.`
- MMNet coverage: quote in §1.8; also covers OAMP-Net2: `the work in [88] proposes a model-driven DL-based joint channel estimation and symbol detection network, OAMP-Net2, by unfolding the existing iterative algorithm OAMP ... Furthermore, the authors examine OAMP-Net2 under the practical Quadriga 3GPP 3D MIMO channel model [84], [85].` And the HyperMIMO follow-up: `The work in [60] introduces an additional NN, a HyperNetwork, that eliminates the need for online retraining on top of a modified version of MMNet to create the HyperMIMO detector.`
- Comparison tables: the survey has summary tables with explicit `Unfolding` rows (e.g. around lines 989–1108 of the extracted text) and a MIMO-detection table listing `MMNet` / `Modified MMNet` against `MMSE, MLD, OAMPNet, MMNet`.
- It does **not** make the "unfolded detectors output symbols, LLRs are open" claim; its model-driven discussion is about `deep unfolded networks, where DNN layers replicate iterations of an existing iterative algorithm, and hybrid networks, where DNNs help conventional models and enhance efficiency [26].`

### 3.4 "Deep Learning Techniques For Massive MIMO Detection Algorithms: A Review"
Saja Abdul Karim Anwar, Marwa Al-Sultani (University of Diyala, Iraq).
- **Al-Furat Journal of Innovations in Electronics and Computer Engineering (FJIECE), Vol. 5 No. 1 (2026), published 2026-03-31**, eISSN 2708-3985, https://fjiece.atu.edu.iq/index.php/fjiece/article/view/276
- Scope (abstract, verbatim): `This study provides a systematic review of Deep neural network-based detection techniques for massive MIMO systems. A systematic literature review was conducted, drawing on recent studies selected from major scientific databases and research published between 2016 and 2025, with a focus on widely adopted detection frameworks. A unified comparison was performed using key criteria, including bit error rate (BER) performance, computational complexity and practical feasibility. The reviewed methods were categorized as data-driven, model-driven, and hybrid approaches ... The results indicate that model-driven and hybrid techniques enhanced trade-off between detection accuracy and computational complexity, while purely data-driven methods require intensive training and exhibit limited generalize ability under varying channel conditions.`
- **Caveat:** this is a low-visibility, non-indexed venue; only the landing page was read. Its comparison tables were **not** inspected.

### 3.5 ★ "The Applications for Deep Unfolding Techniques in MIMO Wireless Communications Systems"
- **IEEE Communications Magazine, 2026**, DOI **10.1109/MCOM.001.2500444**, https://ieeexplore.ieee.org/document/11474797 — this is the 2026 survey specifically on deep unfolding for MIMO that the requester asked to look for.
- Abstract (OpenAlex, DOI 10.1109/mcom.001.2500444): `we present a comprehensive overview of deep unfolding background, systematically reviewing its structure. Next, we discuss the implementations of deep unfolding in MIMO communication systems, including detection, beamforming, and estimation, while highlighting their respective advantages. Furthermore, we investigate the performance analysis of systems designed using deep unfolding in reconfigurable intelligent surface (RIS)-assisted MIMO-orthogonal frequency division multiplexing (OFDM) environments, and MIMO radar beam pat[terning]...`
- Full text not obtained (IEEE Xplore blocks automated access: HTTP 202, 0 bytes) → tables not inspected.

### 3.6 Other surveys surfaced (title/venue from indexed records)
- `A Variational Bayes-Driven Deep Unfolding Network for Efficient MIMO Detection` (ResearchGate-indexed; venue/year not verified — see Could-not-verify).
- A survey whose text includes: `Various learning-based massive MIMO detection methods, such as OAMPNet [145] and MMNet [100], [146], have been developed by unf[olding]` — hosted at https://recil.ulusofona.pt/bitstreams/9b3e6119-da11-4227-8cf2-2779a67788dd/download (PDF downloaded but text extraction failed; title not verified).

---

## 4. ★ The theme: do unfolded MIMO detectors output symbols rather than LLRs, and is LLR output an open problem?

**What the surveys actually say: the surveyed 2024–2026 reviews do NOT state this as a general finding or as an open problem.** The claim is instead supported by the *primary* papers, several of which say it explicitly. Verbatim evidence, strongest first:

1. **CMDNet (IEEE TCOM 2021)** — direct statement that MMNet/OAMPNet/DetNet do not provide soft outputs:
   > `One major drawback of the latter approaches is that they focus on MIMO detection and do not provide soft outputs.`
   > `This allows us to account for subsequent decoding, e.g., in MIMO systems, in contrast to literature [28], [34].` ([34] = MMNet, [28] = DetNet)
   > `we show superiority to other recently proposed ML-based approaches and demonstrate with simulations in coded systems CMDNet's soft outputs to be reliable for decoders as opposed to [28].`
   > `In coded systems with soft decoders usually employed today, delivering soft information is a strict requirement.`
   — https://arxiv.org/pdf/2102.12756

2. **CMDNet's LLR-histogram experiment** shows the leading unfolded detector's "soft" output is effectively hard:
   > `Actually, the soft output version of DetNet should deliver accurate probabilities or Log Likelihood Ratios (LLRs) according to [28] after optimization. Indeed, we visualize with an exemplary histogram of LLRs that this is not the case. ... Furthermore, it can be clearly seen that DetNet mostly provides hard decisions with ∼ 97% LLRs being −1 and 1, respectively. Only a few values are close to 0. In contrast, CMDNet provides meaningful soft information resembling a mixture of Gaussians ... These results strongly indicate that the difference of soft output quality originates from different underlying optimization strategies ... whereas the one-hot representation in DetNet is optimized w.r.t. MSE.`
   — https://arxiv.org/pdf/2102.12756

3. **OAMP-Net2 (IEEE TSP 2020)** — formulates LLR output but defers decoder-in-the-loop experiments:
   > `Different from the DetNet in [19] that only can provide the soft-output, our proposed detector are the soft-input and soft-output receiver and therefore can achieve the turbo equalization. We only provide the principle of the OAMP-Net2-based turbo receiver and the specific experimental results are outside the scope of this paper and will be conducted in the future.`
   — https://arxiv.org/pdf/1907.09439

4. **HyperMIMO (arXiv 2002.02750)** — treats LLR output as an unimplemented option on top of MMNet:
   > `MMNet consists of T layers performing (8), and a hard decision as in (5) to predict the final estimate x̂. One could also use rx(T) to predict bit-wise log likelihood ratios (LLRs).`

5. **SICNN (IEEE TMLCN 2024)** — soft symbol probabilities produced; LLRs named only as a downstream use:
   > `provide reliable soft estimates (required, e.g., to compute log-likelihood ratios in case of channel coded transmission)`

6. **Survey 2502.05952** — only per-paper output-type mentions, no general claim:
   > `It generates soft outputs for integration with soft-input decoders and outperforms DetNet [70], OAMP-Net2 [11], and MMNet [75] in BER under correlated channels.` (about CMDNet/`CONCRETE MAP`)
   > `Ullah et al. [106] unfolded a likelihood ascent search algorithm for joint detection and soft-output estimation.`
   Its own open-challenge list (C1–C5) does **not** include LLR/soft-output compatibility.

**Net assessment the team can use:** across the unfolded MIMO detectors checked, the *default* output is a symbol estimate (soft posterior mean internally, hard symbol decision externally), the de-facto metric is SER/BER over uncoded links, and reliable decoder-grade LLR output is claimed by only a minority (OAMP-Net2 in principle, CMDNet in practice with a real LDPC BP decoder). No 2024–2026 survey inspected explicitly frames "LLR-based / decoder-compatible output" as an open problem — the closest framing is CMDNet's and OAMP-Net2's own statements, plus the ICVISP-2025 paper's critique that the *Bayes-optimal* MMSE update inside OAMP-Net breaks down under strong correlation.

---

## 5. Could not verify

1. **ICASSP 2020 paper full text / abstract / its own numbers.** PDF at `dihana.cps.unizar.es/.../0008559.pdf` returns HTTP 401; IEEE Xplore blocks automated fetches; Unpaywall reports `OA: False`. Only the title, venue/year/DOI, and two indexed verbatim sentences (given in §1.9) were obtained.
2. **Whether the arXiv 1906.04610 v1 text is byte-identical to the published TWC version.** Only v1 exists on arXiv; the NSF accepted-manuscript copy matches v1 on every passage checked, but I did not do an exhaustive diff. Note the one confirmed divergence: parameter count wording ("total ... 2Nt(Nr + 1) real values" in arXiv v1 vs "2Nt(Nr + 1) per layer ... 20K-41K" in the published/thesis text).
3. **"3GPP CDL" and "spatially correlated CDL" for MMNet — NOT confirmed.** MMNet uses the 3GPP **3D MIMO** model (TR 36.873) via QuaDRiGa plus i.i.d. Gaussian. I found **no** CDL or Kronecker evaluation in MMNet. The Kronecker mention in the paper refers to prior work (DetNet/OAMPNet).
4. **Any MMNet BER curve, LDPC code, code rate, or channel decoder.** Zero occurrences of `LDPC`, `channel decoder`, `BER` in all three MMNet sources. MMNet is an uncoded SER study only.
5. **Any MMNet latency number (ms/µs).** Only the qualitative sentence about the sequential online-training algorithm incurring significant latency.
6. **A "VAMP-Net for MIMO detection" paper.** Not found; learned VAMP exists in Borgerding et al. (sparse linear inverse problems) and unfolded VAMP appears in wireless only for grant-free random-access JADCE (ICC Workshops 2024).
7. **Venue of arXiv 2502.05952 ("Comprehensive Review of Deep Unfolding Techniques...").** No journal-ref/DOI on arXiv or in the PDF; OpenAlex lists it as an arXiv preprint (2025). Do not cite a journal.
8. **Comparison-table contents of:** the FJIECE 2026 review (landing page only), the IEEE Communications Magazine 2026 deep-unfolding-for-MIMO survey (Xplore blocked), and the Lusofona-hosted survey (PDF text extraction failed; title unknown).
9. **Full text of "Low-Complexity Deep MIMO Detection for Correlated Measurements" (IEEE Xplore 11451696).** Only the OpenAlex abstract (quoted in §2.7); its output type, loss, parameter count, and complexity numbers are unverified.
10. **Full text of "Learnable MIMO Detection Networks Based on Inexact ADMM" (Kim & Park).** Only the OpenAlex abstract plus the survey's Table III row; its exact output type (symbol vs bit) and its loss function are unverified beyond `ground truth transmitted signals`.
11. **Full text of "Parameter-Learned AMP for MIMO Signal Detection" (APWCS 2022).** OpenAlex abstract only; the abstract is truncated mid-word (`the higher performance is obtained by the simple structure LA`), so the LAMP-variant naming is incomplete.
12. **SICNN's exact BER dB gains** vs. named baselines, and its total learnable-parameter counts. The complexity section gives closed-form real-multiplication counts that I did not fully transcribe; no headline dB-gain sentence was extracted.
13. **The IEEE JSAC paper "Adaptive MIMO Detector Based on Hypernetwork: Design, Simulation, and Experimental Test" (Zhang, Wen, Jin)** — cited as survey ref [107]; I did not confirm whether it is the journal version of arXiv 2002.02750 (different author list suggests it is a separate work). Unverified.
14. **Whether any 2024–2026 survey explicitly frames LLR/decoder-compatible output as an open problem.** I found none among those inspected; this is a negative result over the sources listed, not a proof of absence.
15. **"Deep Learning Techniques For Massive MIMO Detection Algorithms: A Review" comparison tables and whether it discusses output types (bits/symbols/LLRs).** Landing page/abstract only.
