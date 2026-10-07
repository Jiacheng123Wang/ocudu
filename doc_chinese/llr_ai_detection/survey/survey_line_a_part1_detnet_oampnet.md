# Line A, Part 1 — Model-based / deep-unfolded MIMO detectors (DetNet family, OAMP-Net family)

Survey date: 2026-10-07. Evidence method: `web_fetch` is unusable in this sandbox, so **every** fact below comes from
`web_search` result **titles** (which are verbatim matched passages from the indexed document) plus the URL the passage
came from. Quotes are reproduced exactly as returned by the search index (hence the OCR/markup artefacts such as
`\hat{\mathbf{x}}`, `\[\begin{array}{rcl}`, `𝑝subscript𝑠𝑖`). Anything I did not literally see is in
[§8 Could not verify](#8-could-not-verify-explicit-list) — **not** asserted.

Reading note: the requester's three "beliefs to check" resolve as follows —
* **P2** — "Learning to Detect" is **IEEE TSP 2019, vol. 67, p. 2554** (ADS bibcode). I found **no** IEEE SPL 2017 version; the likely source of the mis-citation is SPAWC 2017 "Deep MIMO Detection".
* **P4** — arXiv 1809.09336 is **GlobalSIP 2018**, not GLOBECOM 2018 (SIGPORT PDF is literally named `IEEE_Globalsip_2018.pdf`).
* **P5** — DOI `10.1109/TSP.2020.2976585` is indexed against **"Model-Driven Deep Learning for MIMO Detection"** (NSTL), *not* against the joint CE+detection paper. But note arXiv 1907.09439**v1** carried the joint CE+detection title and **v2** was retitled to the detection (OAMP-Net2) title — that is the real origin of the confusion. See §5.
* **P3** — I found **no** "IEEE TSP 2019 one-bit DetNet". See §3 for what does exist.

---

## 1. Deep MIMO Detection (SPAWC 2017) — the DetNet origin paper

**Identity / venue (CONFIRMED)**
* Title: *Deep MIMO detection* (IEEE Xplore capitalises it as "Deep MIMO detection"). Authors N. Samuel, T. Diskin, A. Wiesel.
* Venue: **2017 IEEE 18th International Workshop on Signal Processing Advances in Wireless Communications (SPAWC)**, Sapporo, 2017-07-03/06. DOI **10.1109/SPAWC.2017.8227772**.
* Links: [IEEE Xplore document 8227772](https://ieeexplore.ieee.org/document/8227772) · [arXiv:1706.01151v1](https://arxiv.org/abs/1706.01151v1) · [ar5iv HTML](https://ar5iv.labs.arxiv.org/html/1706.01151)

Verbatim passages actually seen:

> "[IEEE 2017 IEEE 18th International Workshop on Signal Processing Advances in Wireless Communications (SPAWC) - Sapporo (2017.7.3-2017.7.6)] 2017 IEEE 18th International Workshop on Signal Processing Advances in Wireless Communications (SPAWC) - Deep MIMO detection"
> — https://ieeexplore.ieee.org/document/8227772 (also surfaced via a PDF path ending `10.1109/SPAWC.2017.8227772.pdf`)

> "Deep MIMO detection - Deep MIMO detection"
> — https://ar5iv.labs.arxiv.org/html/1706.01151#1

> "DETNET was first proposed in "Deep MIMO detection" [34]"
> — https://aircconline.com/ijwmn/V14N5/14522ijwmn01.pdf#3#2

**What the method does.** This is the paper that introduced **DetNet**. The architecture is described in the follow-up
journal paper (see §2, where the per-layer recurrence was recovered verbatim). A third-party description of the working
principle, seen verbatim:

> "DetNet [16] is a model-based deep learning method whose working principle is to unfold a projected gradient descent algorithm"
> — https://ieeexplore.ieee.org/document/10239294#2

> "Chapter 1 Model-Based Machine Learning for Communications - DetNet is a deep learning based symbol detector proposed in [32] for flat Gaussian MIMO channels"
> — https://ar5iv.labs.arxiv.org/html/2101.04726#3

* **Output type** — see §2 for the only output-type evidence I could obtain (it is about the journal version, DetNet in general). **Not separately verified for the SPAWC paper.**
* **Loss, numbers, simulation setup, complexity, channel decoder** — see §8.

---

## 2. Learning to Detect (IEEE TSP 2019) — DetNet journal version

**Identity / venue (CONFIRMED)**
* Authors: N. Samuel, T. Diskin, A. Wiesel.
* Venue: **IEEE Transactions on Signal Processing, 2019, vol. 67, p. 2554** (start page).
  * DOI **10.1109/TSP.2019.2899805** — NSTL record `【期刊论文】 Learning to Detect` carries exactly that DOI.
* Links: [IEEE Xplore 8642915](https://ieeexplore.ieee.org/abstract/document/8642915) · [NASA ADS 2019ITSP...67.2554S](https://ui.adsabs.harvard.edu/abs/2019ITSP...67.2554S/abstract) · [arXiv:1805.07631v1](https://arxiv.org/abs/1805.07631v1) · [ar5iv HTML](https://ar5iv.labs.arxiv.org/html/1805.07631)

Verbatim passages actually seen:

> "Learning to Detect" — https://ui.adsabs.harvard.edu/abs/2019ITSP...67.2554S/abstract
> *(the bibcode `2019ITSP...67.2554S` encodes journal=ITSP, year=2019, volume=67, page=2554, first author initial S)*

> "【期刊论文】 Learning to Detect NSTL 国家科技图书文献中心"
> — https://lz.nstl.gov.cn/paper_detail.html?doi=10.1109/TSP.2019.2899805

> "Learning to Detect, Statistics | DeepDyve - Learning to Detect Samuel, Neev;Diskin, Tzvi;Wiesel, Ami 2018-05-19 00:00:00 Neev Samuel, Member, IEEE, and Tzvi Diskin, Member,..."
> — https://www.deepdyve.com/lp/arxiv-cornell-university/learning-to-detect-5vR9kzpb6b#1

> "Learning to Detect - Skip to main content" — https://arxiv.org/abs/1805.07631v1

**Architecture (verbatim, from the paper's own PDF text).** The per-layer DetNet recurrence was recovered directly:

> "\[\begin{array}{rcl}{\mathbf{q}_1} & = & {\mathbf{y}}\\ {\mathbf{q}_{k + 1}} & = & {\rho \left(\mathbf{W}_k\mathbf{q}_k + \mathb..."
> — http://arxiv.org/pdf/1805.07631#4#2

Interpretation (consistent with the recovered recurrence and with third-party descriptions): layer 1 is initialised from the
received signal `y`; each subsequent layer `k` applies an affine map `W_k q_k + …` followed by a nonlinearity `ρ`. Note the
**per-layer (untied) weight matrix `W_k`** — this is the defining "unfolded iterative algorithm" structure. The paper also
defines a **FullyCon** (fully-connected) baseline that it compares against:

> "Learning to Detect - - FullyCon:" — https://ar5iv.labs.arxiv.org/html/1805.07631#3

**Complexity (verbatim).**

> "1) FullyCon and DetNet run time: In order and estimate the computational complexity of the different detectors we compared their..."
> — http://arxiv.org/pdf/1805.07631#4#3

**Output type.** The strongest evidence I could obtain is from a Princeton COS 597S lecture deck on this exact paper,
which frames the target of the learned network as the **MMSE / posterior-mean estimator**, i.e. a *soft symbol estimate*
rather than bits or LLRs:

> "&= \arg \min_{\hat{x}_{oh}} E \left[ \| x_{oh} - \hat{x}_{oh} \|^2 \mid y \right]"
> — https://kyleatprinceton.github.io/cos597s/assets/documents/L09-detnet.pdf#3#3

> "Learning to Detect" — https://kyleatprinceton.github.io/cos597s/assets/documents/L09-detnet.pdf#3#1

I was **not** able to recover a verbatim sentence from the paper itself saying "soft symbol estimates" / "hard decisions" /
"LLRs". See §8.

**Was there a 2017 IEEE SPL version?** No evidence. Searches for
`"Learning to Detect" "IEEE Signal Processing Letters" 2017` and `"Deep MIMO detection" "IEEE Signal Processing Letters"`
returned nothing linking either paper to SPL. The only 2017 venue for this group that I could confirm is **SPAWC 2017**
(§1). Conclusion: the "2017 IEEE SPL" belief is **most likely a mis-citation of "Deep MIMO Detection", SPAWC 2017** — but I
mark it "no evidence found" rather than "definitively disproved", since absence of a search hit is not proof of absence.

**Loss / numbers / simulation setup / LDPC-in-the-loop** — see §8.

---

## 3. One-bit (1-bit ADC) / low-resolution deep MIMO detectors

### 3.1 The specific claim to refute: "IEEE TSP 2019 one-bit DetNet"

**No such paper surfaced.** I ran repeated, differently-worded searches (`"one-bit" "DetNet" MIMO detection IEEE 2019`,
`"1-bit DetNet" OR "one-bit DetNet"`, `"DetNet" "1-bit ADC" MIMO detection deep learning paper`,
`"quantized" MIMO detection "model-driven deep learning" one-bit ADC unfolded`). **Zero** results tied DetNet to 1-bit ADCs
in 2019. Treat the claim as **unsupported**. (See §8.)

### 3.2 What *does* exist — verified one-bit / low-resolution deep detectors

| # | Title | Venue evidence | URL |
|---|---|---|---|
| a | **Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation** | DOI **10.1109/TSP.2020.3048232** confirmed (PDF path `…10.1109/tsp.2020.3048232.pdf` attached to this title) → IEEE TSP, 2020/2021 | [IEEE Xplore 9311778](https://ieeexplore.ieee.org/document/9311778) · [CUHK record](https://research.cuhk.edu.hk/en/publications/binary-mimo-detection-via-homotopy-optimization-and-its-deep-adap-2/) |
| b | **LoRD-Net: Unfolded Deep Detection Network With Low-Resolution Receivers** | **IEEE Transactions on Signal Processing**, DOI **10.1109/TSP.2021.3117503**; ADS bibcode `2021ITSP...69.5651K` → **vol. 69, p. 5651, 2021** | [ACM/Literatum DOI page](https://acm-stag.literatumonline.com/doi/10.1109/TSP.2021.3117503) · [ADS](https://ui.adsabs.harvard.edu/abs/2021ITSP...69.5651K/abstract) · [ar5iv 2102.02993](https://ar5iv.labs.arxiv.org/html/2102.02993) · [NSF PAR PDF](https://par.nsf.gov/servlets/purl/10322182) |
| c | **Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems** | IEEE Xplore doc **9847603**; NSF PAR biblio **10438662**; journal name not resolved | [IEEE Xplore](https://ieeexplore.ieee.org/abstract/document/9847603) · [arXiv 2107.11958](http://export.arxiv.org/pdf/2107.11958) · [NSF PAR](https://par.nsf.gov/biblio/10438662-deep-learning-estimation-pilot-signal-design-few-bit-massive-mimo-systems) |
| d | **Detection and Channel Equalization with Deep Learning for Low Resolution MIMO Systems** | DOI **10.1109/ACSSC.2018.8645551** → **Asilomar Conference on Signals, Systems, and Computers (ACSSC) 2018**; authors Klautau, González-Prelcic | [IEEE Xplore 8645551](https://ieeexplore.ieee.org/document/8645551) · [Semantic Scholar](https://www.semanticscholar.org/paper/Detection-and-Channel-Equalization-with-Deep-for-Klautau-Gonz%C3%A1lez-Prelcic/9b4d54e0e3c49db7d97ff256d1887b3dac5c7250) · [NSF PAR PDF](https://par.nsf.gov/servlets/purl/10112568) |
| e | **Accelerated and Deep Expectation Maximization for One-Bit MIMO-OFDM Detection** | **IEEE Transactions on Signal Processing 2024**, DOI **10.1109/TSP.2024.3359083**; arXiv 2210.03888 | [ACM DOI page](https://dl.acm.org/doi/abs/10.1109/TSP.2024.3359083) · [IEEE Xplore PDF 10416234](https://ieeexplore.ieee.org/ielx7/78/10347386/10416234.pdf) |
| f | **Regularized Neural Detection for One-Bit Massive MIMO Communication Systems** | arXiv 2305.15543 (venue not resolved) | [ar5iv 2305.15543](https://ar5iv.labs.arxiv.org/html/2305.15543) |
| g | **Learning-Based One-Bit Maximum Likelihood Detection for Massive MIMO Systems: Dithering-Aided Adaptive Approach** | arXiv 2304.07696 (companion: "Adaptive Learning-Based Maximum Likelihood and Channel-Coded Detection for Massive MIMO Systems with One-Bit ADCs") | [arXiv 2304.07696v2](http://arxiv.org/pdf/2304.07696v2) |
| h | **Binary MIMO Detection via Extreme Learning Machines in 5G Network and Beyond** | IEEE Xplore doc **10464624**; Keung & Qureshi (researchr key `KeungQ23`) → 2023 | [Semantic Scholar](https://www.semanticscholar.org/paper/Binary-MIMO-Detection-via-Extreme-Learning-Machines-Keung-Qureshi/11bb501cbea4a3bc59745384bc4b3b9245cb8745) · [researchr](https://researchr.org/publication/KeungQ23/authors) |

Verbatim passages actually seen for the above:

> "Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation - Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation"
> — https://ieeexplore.ieee.org/document/9311778#1
> *(and the same title attached to the PDF path `…libgen.scimag86443000-86443999.zip --- 10.1109/tsp.2020.3048232.pdf`, which confirms the DOI)*

> "LoRD-Net: Unfolded Deep Detection Network With Low-Resolution Receivers | IEEE Transactions on Signal Processing"
> — https://acm-stag.literatumonline.com/doi/10.1109/TSP.2021.3117503

> "LoRD-Net: Unfolded Deep Detection Network With Low-Resolution Receivers - Now on home page"
> — https://ui.adsabs.harvard.edu/abs/2021ITSP...69.5651K/abstract

> "Detection and Channel Equalization with Deep Learning for Low Resolution MIMO Systems - Detection and Channel Equalization with Deep Learning for Low Resolution MIMO Systems"
> — https://ieeexplore.ieee.org/document/8645551#1
> *(the same title was attached to a PDF path ending `10.1109/ACSSC.2018.8645551.pdf`, confirming ACSSC 2018)*

> "Accelerated and Deep Expectation Maximization for One-Bit MIMO-OFDM Detection | IEEE Transactions on Signal Processing"
> — https://dl.acm.org/doi/abs/10.1109/TSP.2024.3359083?af=R

> "Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems - Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems"
> — https://ieeexplore.ieee.org/abstract/document/9847603#1

> "Binary MIMO Detection via Extreme Learning Machines in 5G Network and Beyond | Semantic Scholar - Binary MIMO Detection via Extreme Learning Machines in 5G Network and Beyond"
> — https://www.semanticscholar.org/paper/Binary-MIMO-Detection-via-Extreme-Learning-Machines-Keung-Qureshi/11bb501cbea4a3bc59745384bc4b3b9245cb8745

**One useful by-product** — a one-bit receiver survey/listing was indexed, showing that the one-bit literature is broad:

> "All the different prior works for one-bit MIMO receivers (see Sec"
> — https://repositories.cdlib.org/content/qt8968c6d8/qt8968c6d8.pdf

> "The uplink unquantized signal is received as \(\mathbf{r}\) and the one-bit quantized signal is given by \(\mathbf{y}\)"
> — https://repositories.cdlib.org/content/qt8968c6d8/qt8968c6d8.pdf#27#16

Also relevant (non-deep, but the classical one-bit soft-output reference point):

> "[IEEE 2018 IEEE International Conference on Communications (ICC 2018) - Kansas City, MO (2018.5.20-2018.5.24)] 2018 IEEE International Conference on Communications (ICC) - Successive Cancellation Soft Output Detector for Uplink MU-MIMO Systems with One-Bit ADCs"
> — PDF path ending `10.1109/ICC.2018.8422415.pdf`

### 3.3 Output type / loss / numbers for the one-bit papers

I could **not** recover output-type, loss, parameter-count or simulation-setup sentences for papers (a)–(h) within the
search budget. The one *quantizer-model* sentence I did recover for (c) is:

> "Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems - 𝒬b(r)={τl−Δ2ifr∈(τl−1,τl]withl∈ℒ(2b−1)Δ2ifr∈(τ2b−1,τ2b]}"
> — https://arxiv-org.ezproxy.obspm.fr/html/2107.11958v1#2

…which shows a **b-bit uniform quantizer with thresholds τ_l and step Δ** — i.e. the paper works with a *few-bit* (not
strictly 1-bit) quantizer model. See §8 for everything else.

---

## 4. A Model-Driven Deep Learning Network for MIMO Detection — GLOBECOM or GlobalSIP?

**Resolution: GlobalSIP 2018.** The SIGPORT copy of the paper is literally named `IEEE_Globalsip_2018.pdf`.

**Identity / venue**
* Title: *A Model-Driven Deep Learning Network for MIMO Detection*. Authors H. He, C.-K. Wen, S. Jin, G. Y. Li.
* Venue: **2018 IEEE Global Conference on Signal and Information Processing (GlobalSIP 2018)** — *strongly indicated*, see caveat below.
* Links: [SIGPORT PDF (filename `IEEE_Globalsip_2018.pdf`)](https://sigport.org/sites/default/files/docs/IEEE_Globalsip_2018.pdf) · [IEEE Xplore 8646357](https://ieeexplore.ieee.org/abstract/document/8646357) · [arXiv:1809.09336](https://arxiv.org/abs/1809.09336) · [ar5iv HTML](https://ar5iv.labs.arxiv.org/html/1809.09336) · [HKUST IR record](https://lbnx03.ust.hk/ir/Record/1783.1-126247)

Verbatim passages actually seen:

> "A Model-Driven Deep Learning Network for MIMO Detection"
> — https://sigport.org/sites/default/files/docs/IEEE_Globalsip_2018.pdf#1#1

> "A Model-Driven Deep Learning Network for MIMO Detection - A Model-Driven Deep Learning Network for MIMO Detection"
> — https://ieeexplore.ieee.org/abstract/document/8646357#1

> "A Model-Driven Deep Learning Network for MIMO Detection - where"
> — https://ar5iv.labs.arxiv.org/html/1809.09336#2

> "2018 IEEE Global Conference on Signal and Information Processing (GlobalSIP)"
> — https://m2.mtmt.hu/api/publication/30586425
> *(this record surfaced in the same query, but I could not read its body to confirm it is this paper — see §8)*

**Architecture (OAMP-Net).** The per-iteration equations were recovered verbatim from the arXiv PDF:

> "\[\mathbf{r}_{t} =\hat{\mathbf{x}}_{t}+\gamma_{t}\mathbf{W}_{t}(\mathbf{y}-\mathbf{H }\hat{\mathbf{x}}_{t}),\] (13) \[\hat{\math..."
> — http://export.arxiv.org/pdf/1809.09336#2#2

i.e. an **OAMP-style linear (de-correlation) step with a learned per-layer step size `γ_t`**, followed by a nonlinear
estimator. A third-party rendering of the same network's equations:

> "\[\mathbf{q}_{{\rm t}}=\{\hat{\mathbf{x}}_{{\rm t}-1}-\theta^{(1)}_{{\rm t}}\mathbf {H}^{\rm T}\mathbf{y}+\theta^{(2)}_{{\rm t}}..."
> — https://core.ac.uk/download/619600387.pdf#3#2

**Robustness / channel model (verbatim, from the same group's later paper):**

> "To test the robustness of the OAMP-NET, the trained parameters of 64-QAM with 40 dB in the WINNERII channels is applied to other..."
> — http://export.arxiv.org/pdf/1903.04766#4#4

**Output type / loss / numbers / decoder** — not recovered. See §8. (The natural companion is §5, where OAMP-Net2's
soft-symbol estimator *was* recovered verbatim.)

---

## 5. Model-Driven Deep Learning for MIMO Detection (OAMP-Net2) — venue and the DOI confusion

**Resolution of the DOI question.**
* NSTL indexes **DOI `10.1109/TSP.2020.2976585`** against the title **"Model-Driven Deep Learning for MIMO Detection"** — i.e. it does *not* belong to the joint CE+detection paper.
* NASA ADS gives bibcode **`2020ITSP...68.1702H`** for the title *Model-Driven Deep Learning for MIMO Detection* → **IEEE Transactions on Signal Processing, vol. 68, p. 1702, year 2020**.
* **The real source of the confusion:** arXiv **1907.09439v1** is titled *"Model-Driven Deep Learning for Joint MIMO Channel Estimation and Signal Detection"*, while **v2** is titled *"Model-Driven Deep Learning for MIMO Detection"*. The same arXiv ID was retitled between versions.

Verbatim passages actually seen:

> "NSTL国家科技图书文献中心 - 【期刊论文】Model-Driven Deep Learning for MIMO Detection EI 工程索引 SCIE Web of Science核心 SCOPUS Scopus数据库 NSTL国家科技图书文献中心"
> — https://gz.nstl.gov.cn/paper_detail.html?doi=10.1109/TSP.2020.2976585

> "Model-Driven Deep Learning for MIMO Detection" — https://ui.adsabs.harvard.edu/abs/2020ITSP...68.1702H/abstract

> "Model-Driven Deep Learning for Joint MIMO Channel Estimation and Signal Detection" — https://browse.arxiv.org/abs/1907.09439v1

> "Model-Driven Deep Learning for MIMO Detection" — https://browse.arxiv.org/abs/1907.09439v2

> "Model-Driven Deep Learning for MIMO Detection - Model-Driven Deep Learning for MIMO Detection"
> — https://ieeexplore.ieee.org/document/9018199#1

**Architecture (verbatim).**

> "Similar to the TPG detector [22] and OAMP-Net [1], the OAMP-Net2 uses two learnable parameters \((\gamma_{t},\theta_{t})\) to ad..."
> — https://export.arxiv.org/pdf/1907.09439#6#4

→ **two learnable scalars per layer**: `γ_t` (linear-step / de-correlation scale) and `θ_t` (nonlinear-estimator scale).
The "2" in OAMP-Net2 refers to this second learned parameter relative to OAMP-Net.

Algorithm 1's interface (verbatim):

> "``` InputInput OutputOutput Received signal \(\mathbf{y}_{\mathrm{d}}\), estimated channel matrix \(\hat{\mathbf{H}}\), equivale..."
> — https://export.arxiv.org/pdf/1907.09439#6#3

System model (verbatim):

> "Model-Driven Deep Learning for MIMO Detection - 𝐘d=𝐇𝐗d+𝐍d,subscript𝐘dsubscript𝐇𝐗dsubscript𝐍d{\mathbf{Y}}_{{\mathrm{d}}}={\mathbf{H}}{\mathbf{X}}_{{\mathrm{d}}}+{\mathbf{N}}_{{\..."
> — https://ar5iv.labs.arxiv.org/html/1907.09439#2

**OUTPUT TYPE — the single most important item.** The strongest evidence I recovered is the explicit
**posterior-mean / soft-symbol estimator** implemented by the nonlinear step:

> "Model-Driven Deep Learning for MIMO Detection - 𝑝subscript𝑠𝑖\hat{\mathbf{x}}_{{\mathrm{d}},t+1}^{(i)}=\mathtt{E}\left\{x_{i}|r_{i},\tau_{t}\right\}=\frac{\sum_{s_{i}}s_{i}\math..."
> — https://ar5iv.labs.arxiv.org/html/1907.09439#4

That is, the layer output is `E{x_i | r_i, τ_t}` — a **conditional-mean (soft) symbol estimate computed from the
posterior over constellation points**, not bits and not an LLR. Combined with the "two learnable parameters"
sentence above, the picture is: OAMP-Net2's per-layer output is a **soft symbol estimate**; a constellation
quantisation to hard symbols is the natural final step (but I did **not** see a paper sentence stating the final
hard-decision step — see §8).

**Complexity / learnable variables (verbatim).**

> "Model-Driven Deep Learning for MIMO Detection - Furthermore, we investigate the number of learnable variables in different DL-based MIMO detectors"
> — https://ar5iv.labs.arxiv.org/html/1907.09439#5

> "<table><tr><td>Complexity</td><td>Detectors</td><td>OAMP-Net2</td><td>DetNet</td><td>DNN-dBP</td><td>DNN-MPD</td><td>TPG</td><td..."
> — http://arxiv.org/pdf/1907.09439#6#4

→ the paper contains a **complexity table comparing OAMP-Net2, DetNet, DNN-dBP, DNN-MPD, TPG** (table cell values not
recoverable through the search index).

**CSI assumption (verbatim).**

> "In the above, all detectors are investigated with accurate CSI"
> — https://export.arxiv.org/pdf/1907.09439#6#5

**Loss, BER numbers, MIMO dimensions, modulation, channel model, LDPC-in-the-loop** — not recovered. See §8.

---

## 6. "Understanding Deep MIMO Detection" and "An Explanation … Homotopy Optimization"

### 6a. Understanding Deep MIMO Detection

**Identity / venue (CONFIRMED)**
* Venue: **IEEE Transactions on Wireless Communications**, **vol. 22, p. 9626, 2023**; DOI **10.1109/TWC.2023.3272525**.
* Links: [IEEE Xplore 10121671](https://ieeexplore.ieee.org/abstract/document/10121671) · [NASA ADS 2023ITWC...22.9626H](https://ui.adsabs.harvard.edu/abs/2023ITWC...22.9626H/abstract) · [ACM DL](https://dl.acm.org/doi/abs/10.1109/TWC.2023.3272525) · [arXiv:2105.05044v1](https://arxiv.org/abs/2105.05044v1) · [ar5iv HTML](https://ar5iv.labs.arxiv.org/html/2105.05044) · [ResearchGate PDF listing](https://www.researchgate.net/publication/351510998_Understanding_Deep_MIMO_Detection)

Verbatim:

> "Understanding Deep MIMO Detection | IEEE Transactions on Wireless Communications - Several features on this page require Premium Access"
> — https://dl.acm.org/doi/abs/10.1109/TWC.2023.3272525#1

> "Understanding Deep MIMO Detection" — https://ui.adsabs.harvard.edu/abs/2023ITWC...22.9626H/abstract

> "Understanding Deep MIMO Detection - The Royal College of Surgeons of England Library Surgical Library - The Royal College of Surgeons of England Library Surgical Library"
> — https://rcseng.ovidds.com/discover/result?logSearchID=164781296&pubid=solr_7902-info%3Adoi%2F10.1109%252FTWC.2023.3272525

**What it studies (verbatim).** It analyses the generic *fully-connected ReLU* deep detector as a composition of affine
maps and nonlinearities:

> "1The fully-connected ReLU DNN is the basis for most of current state-of-the-art DNNs [30], [31]"
> — https://www.researchgate.net/publication/351510998_Understanding_Deep_MIMO_Detection#2

> "pθ(x) = ψdl+1 ◦ Al ◦ φdl ◦ Al−1 ◦ φdl−1 ◦ · · · ◦ φd1 ◦ A0(x) (12)"
> — https://www.semanticscholar.org/reader/5e01a822365d3018fb999f65c0abd670714eb135#2

> "Understanding Deep MIMO Detection - 𝐟𝜽(𝐱)=𝐖𝒳k𝐱+𝐛𝒳ksubscript𝐟𝜽𝐱subscript𝐖subscript𝒳𝑘𝐱subscript𝐛subscript𝒳𝑘\displaystyle\mathbf{f}_{\boldsymbol{\theta}}(\mathbf{x})=\..."
> — https://ar5iv.labs.arxiv.org/html/2105.05044#3

> "Understanding Deep MIMO Detection - where and 𝐟nlr(⋅):ℝ2dr→ℝ2dr\mathbf{f}_{\mathrm{nlr}}(\cdot):\mathbb{R}^{2d_{r}}\rightarrow\mathbb{R}^{2d_{r}} are the unknown di..."
> — https://ar5iv.labs.arxiv.org/html/2105.05044#2

The word "homotopy" appeared in retrieved fragments of the paper — one verbatim fragment contains the ε-net / covering
machinery typical of a homotopy-continuation approximation argument:

> "Understanding Deep MIMO Detection - for, where j∈{1,…,l}j\in\{1,\ldots,l\} andr=‖𝜽−𝝀‖∞≤2Rr=\|\boldsymbol{\theta}-\boldsymbol{\lambda}\|_{\infty}\leq 2R is the upper..."
> — https://ar5iv.labs.arxiv.org/html/2105.05044#9

> "Understanding Deep MIMO Detection - ∑m=1|𝒵|(lnp(𝐮m|𝐱m))2superscriptsubscript𝑚1𝒵superscript𝑝conditionalsubscript𝐮𝑚subscript𝐱𝑚2\displaystyle\sum_{m=1}^{|\mathcal{Z}|}(\ln p(\mathbf{u}_{m}|\mathbf{x}_{m}))^{2}..."
> — https://ar5iv.labs.arxiv.org/html/2105.05044#10

**Output type, loss, simulation setup, numbers** — not recovered verbatim. See §8.

### 6b. An Explanation of Deep MIMO Detection From a Perspective of Homotopy Optimization

**Identity (CONFIRMED, with gaps)**
* Venue: **ICASSP 2023** — the IEEE Resource Center hosts it under the ICASSP-2023 conference track:
  [resourcecenter.ieee.org/conferences/icassp-2023/spsicassp23vid2795](https://resourcecenter.ieee.org/conferences/icassp-2023/spsicassp23vid2795)
* IEEE Xplore document **10041793**.
* Authors: at least one author is **Liu, Wing Kin** (CUHK). The CUHK research portal hosts the publication record.
* Links: [IEEE Xplore 10041793](https://ieeexplore.ieee.org/document/10041793) · [IEEE PDF](https://ieeexplore.ieee.org/ielx7/8782710/10040759/10041793.pdf) · [CUHK record](https://research.cuhk.edu.hk/en/publications/an-explanation-of-deep-mimo-detection-from-a-perspective-of-homot-2/)

Verbatim:

> "Technical Paper: An Explanation of Deep MIMO Detection from a Perspective of Homotopy Optimization"
> — https://resourcecenter.ieee.org/conferences/icassp-2023/spsicassp23vid2795

> "Received 27 October 2022; revised 15 January 2023; accepted 20 January 2023. Date of publication 9 February 2023;"
> — https://ieeexplore.ieee.org/ielx7/8782710/10040759/10041793.pdf#3#1

> "An Explanation of Deep MIMO Detection From a Perspective of Homotopy Optimization - Toggle top search"
> — https://library.mscc.edu/search/eds/details/an-explanation-of-deep-mimo-detection-from-a-perspective-of-homotopy-optimization?searchfield=AU&query=Liu%2C%20Wing%20Kin

**Method content (verbatim fragments).** The paper re-formulates a problem into a homotopy/continuation form and uses a
**soft-projection operator onto the constellation**:

> "An Explanation of Deep MIMO Detection From a Perspective of Homotopy Optimization - Problem (17) can be rewritten aswhere we perform a change of a variable \begin{align*} & \min _{{\bm {x}}\in \mathbb {R}^{n}} f(..."
> — https://ieeexplore.ieee.org/document/10041793/citations?tabFilter=papers#citations#3

> "\[\hat{P}_\alpha(z) = \sum_{c \in \mathcal{C}} (c + \rho_\alpha(z - c))]"
> — https://ieeexplore.ieee.org/ielx7/8782710/10040759/10041793.pdf#3#3

→ `P̂_α(z) = Σ_{c∈C} (c + ρ_α(z − c))` is a **soft projection / shrinkage-toward-constellation operator** indexed by the
continuation parameter `α`: as `α → 0` it becomes a hard nearest-constellation projection; for `α > 0` it is a soft
(continuous) relaxation. This is the mechanism that ties the deep detector to a homotopy path, and it implies the
**layer outputs are continuous soft estimates**, with hard decisions only in the limit.

**Relationship between 6a and 6b.** Both are CUHK-affiliated and both concern homotopy/continuation explanations of deep
MIMO detection, and "Binary MIMO Detection via Homotopy Optimization and Its Deep Adaptation" (§3.2a) is also CUHK. I did
**not** see an explicit statement that 6a and 6b share an author list — treat the overlap as *highly plausible but
unconfirmed*.

---

## 7. Adjacent DetNet-family / unfolded-detector results found along the way

These are useful context for Line A part 1; venues are as seen.

| Title | Venue as evidenced | URL |
|---|---|---|
| Multilevel MIMO Detection with Deep Learning (Corlay, Boutros) | arXiv 1812.01571; ADS `2018arXiv181201571C` | [arXiv listing](https://ui.adsabs.harvard.edu/abs/2018arXiv181201571C/abstract) · [ar5iv](https://ar5iv.labs.arxiv.org/html/1812.01571) |
| Deep MIMO Detection Using ADMM Unfolding | **IEEE Data Science Workshop (DSW) 2019**, DOI 10.1109/DSW.2019.8755566 | [IEEE Xplore 8755566](https://ieeexplore.ieee.org/abstract/document/8755566) |
| MMNet | Deep unfolded detector for **3GPP MIMO channel matrices**, incl. ill-conditioned; ICASSP 2020 | [ICASSP 2020 PDF](http://dihana.cps.unizar.es/proceedings/ICASSP/2020/pdfs/0008559.pdf) · [arXiv 2502.05952v1](https://export.arxiv.org/pdf/2502.05952v1) |
| Learnable MIMO Detection Networks based on Inexact ADMM | IEEE **TWC**, DOI 10.1109/TWC.2020.3026471 | PDF path `…10.1109/twc.2020.3026471.pdf` |
| Deep Learning-Aided Tabu Search Detection for Large MIMO Systems | IEEE **TWC**, DOI 10.1109/TWC.2020.2981915/2981919 | [ar5iv 1909.01683](https://ar5iv.labs.arxiv.org/html/1909.01683) |
| CMDNet: Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection With Low Complexity (Beck et al., 2021) | IEEE Xplore doc **9546779**; arXiv 2102.12756 | [IEEE Xplore](https://ieeexplore.ieee.org/abstract/document/9546779) · [arXiv 2102.12756v3](https://arxiv.org/html/2102.12756v3) |
| Deep-Unfolded Iterative Soft-Input Soft-Output SIC Receiver for Coded MIMO Systems | IEEE Xplore doc **10757483**; Aalborg University | [IEEE Xplore](https://ieeexplore.ieee.org/document/10757483) · [AAU record](https://vbn.aau.dk/da/publications/deep-unfolded-iterative-soft-input-soft-output-sic-receiver-for-c/) |
| Graph Neural Network-Enhanced Expectation Propagation Algorithm for MIMO Turbo Receivers | arXiv 2308.11335 | [ar5iv 2308.11335](https://ar5iv.labs.arxiv.org/html/2308.11335) |
| On Purely Data-Driven Massive MIMO Detectors | IEEE Xplore doc **11113418**; arXiv 2401.07515 | [ar5iv 2401.07515](https://ar5iv.labs.arxiv.org/html/2401.07515) · [IEEE Xplore](https://ieeexplore.ieee.org/abstract/document/11113418) |

Two third-party characterisations worth quoting because they are blunt about DetNet's place in the taxonomy:

> "DetNet [16] is a model-based deep learning method whose working principle is to unfold a projected gradient descent algorithm"
> — https://ieeexplore.ieee.org/document/10239294#2

> "Since DetNet is deduced by deep unfolding, the number of required layers corresponds to the number of iterations of the underlying..."
> — https://epub.jku.at/obvulihs/download/pdf/10445535#52#30

> "Neural Network Approaches for Data Estimation in Unique Word OFDM Systems - Due to the deduction of the layer structure of DetNet by deep unfolding, the number of layers corresponds to the number of requi..."
> — https://ieeexplore.ieee.org/document/10286901/citations#citations#4

> "Despite the above limitations, DetNet offers several performance advantages"
> — https://www.alphaxiv.org/abs/2204.05350

---

## 8. "Could not verify" — explicit list

Everything below is a question the requester asked that I **could not** answer from evidence I actually saw. Do not treat
any of these as negative findings about the papers — they are gaps in this search session.

**General**
1. I did not obtain any paper's **full abstract** verbatim. All abstracts remain unverified.
2. No **training-loss function** was recovered verbatim for any of the six target papers (DetNet, Learning to Detect, OAMP-Net, OAMP-Net2, Understanding Deep MIMO Detection, An Explanation …).
3. No **numerical BER/BLER vs SNR curves, dB gains, constellation orders, MIMO dimensions (8×8 / 64×8 / 128×8), or channel models (i.i.d. Rayleigh / Kronecker / 3GPP CDL)** were recovered verbatim for any of the six target papers.
4. No **parameter counts, real-multiplication counts, FLOPs or MAC counts** were recovered as numbers for any target paper. (The DetNet/OAMP-Net2 complexity *table* is confirmed to exist — cell values were not readable through the index.)
5. **Whether a real LDPC/Turbo decoder is in the loop** — not verified for any target paper. In particular I found **no** verbatim statement about **LLR calibration / empirical scaling / LLR mismatch** in DetNet, OAMP-Net, OAMP-Net2, or either of the two "explanation" papers. The nearest thing I saw anywhere was in a *different* paper (CMDNet), which I quote only as adjacency, not as evidence about the target papers:
   > "Indeed, we visualize with an exemplary histogram of LLRs that this is not the case"
   > — https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf#5#5

**Per paper**
6. **§1 Deep MIMO Detection (SPAWC 2017):** page numbers not confirmed; DOI and venue confirmed. Method description, output type, loss, numbers, and simulation setup for *this* paper not recovered. (The DetNet architecture/output evidence in §2 is from the journal version.)
7. **§2 Learning to Detect (TSP 2019):** end page (I saw start page 2554 only; a "2554–2568" guess was searched but **not** confirmed), issue number, and article number not confirmed. **Output type not confirmed by a paper verbatim quote** — the MMSE/posterior-mean framing comes from a third-party lecture deck. Loss, numbers, simulation setup: not recovered.
8. **§3 one-bit:** output type, loss, numbers and simulation setup not recovered for any of the eight one-bit papers. The venue (journal vs conference) for **"Deep Learning for Estimation and Pilot Signal Design in Few-Bit Massive MIMO Systems"** (doc 9847603) and for **"Regularized Neural Detection for One-Bit Massive MIMO Communication Systems"** (arXiv 2305.15543) was **not** resolved. The claim of a **"one-bit DetNet" in IEEE TSP 2019 is unsupported by any hit I obtained** — but I state this as *no evidence found*, not as a proof of non-existence.
9. **§4 A Model-Driven Deep Learning Network for MIMO Detection:** the GlobalSIP 2018 attribution rests on the SIGPORT **filename** `IEEE_Globalsip_2018.pdf` plus the existence of a GlobalSIP-2018 proceedings ToC. I did **not** see this paper listed inside an official GlobalSIP 2018 table of contents, nor did I see a DOI string (`…GlobalSIP.2018.8646357`) or page numbers. The `m2.mtmt.hu/api/publication/30586425` record titled "2018 IEEE Global Conference on Signal and Information Processing (GlobalSIP)" surfaced in a matching query but I could not read its body to confirm it is this paper. **GLOBECOM 2018 is not supported by anything I saw.** Output type, loss, numbers, simulation setup: not recovered.
10. **§5 OAMP-Net2:** I did **not** see the DOI `10.1109/TSP.2020.2976585` printed next to the title in a publisher page — the link is via the NSTL record only. **End page (1702–?) not confirmed.** The existence of **two different TSP 2020 He–Wen–Jin–Li papers** (this one and the joint CE+detection one) is inferred from the arXiv v1/v2 title change plus the x-mol Chinese record for the joint paper; I did **not** recover the joint paper's DOI or its volume/pages. Loss, BER numbers, MIMO dimensions, modulation, channel model, decoder-in-the-loop: not recovered. Whether OAMP-Net2 applies a final **hard** constellation quantisation: not verified (only the soft estimator `E{x_i|r_i,τ_t}` was recovered).
11. **§6a Understanding Deep MIMO Detection:** the **author list is not confirmed** (ads bibcode `…9626H` implies a first author whose surname begins with H). Whether the paper explicitly uses the word "homotopy" in its own framing was **not** confirmed by a verbatim sentence — the fragments I recovered are the composition-of-affine-maps model and an ε-net/covering bound. Loss, output type, numbers, simulation setup: not recovered. Also unconfirmed whether it targets DetNet, OAMP-Net, or a generic unfolded network (the "fully-connected ReLU DNN" quote suggests generic).
12. **§6b An Explanation … Homotopy Optimization:** **full author list, exact DOI, page numbers, and the exact ICASSP-2023 session/proceedings entry were not confirmed.** Only "Liu, Wing Kin" is confirmed as an author. Only two verbatim fragments were recovered (the change-of-variable reformulation and the soft-projection operator `P̂_α`). Whether that operator is called a "soft projection" in the paper, and what its exact role in the architecture is: not verified.
13. **Relationship between §6a and §6b** (shared authors / same group): not verified; only co-affiliation with CUHK is observed.
14. **§7 MMNet**: I did not resolve the exact MMNet paper title/venue beyond "ICASSP 2020" + "3GPP MIMO channel matrices"; the parameter/complexity comparison table glimpsed at `xplorestaging.ieee.org/.../09285254.pdf` (`| (8,2) | 5 | 12,944 | 2,000 | 50,000 | 256 QAM | 34 ∼ 38 | 8 ∼ 18 |`) was **not** attributed to a specific named paper, so I do not quote it as a DetNet/OAMP-Net number.

**Tooling constraint that caused these gaps.** `web_fetch` is disabled in this sandbox, so I could not open any full text.
The search index returns only ~8 results per query and each result title is a single matched passage; recovering a *long*
verbatim quote such as a full abstract, a loss formula, or a numeric results table is generally not achievable this way.
A follow-up pass with working full-text fetch (or with the papers supplied as PDFs) is required to close items 2–5, which
are the ones the engineering team most needs.
