# LINE B — Part 1: Deep-learning receivers that OUTPUT LLRs, trained with channel-coding-aware / bit-wise losses

Method note: `web_fetch` is unusable in this sandbox (confirmed once: `URL hostname "arxiv.org" resolves to a non-public IP address`). Every fact below comes from `web_search` result **titles**, which for PDF/HTML documents are the *verbatim matched passage* from the indexed document (sometimes prefixed by the document title and `" - "`). Each claim carries (a) the URL and (b) the verbatim passage actually seen. Nothing is filled in from memory; unverified items are in §8.

---

## 1. Neural demappers / learned LLR estimation for BICM

### 1.1 Optical communications (canonical neural-demapper line)

- **Experimental Demonstration of Neural Network-based Soft Demapper for Long-haul Optical Transmission** — [ieeexplore 10209891](https://ieeexplore.ieee.org/abstract/document/10209891), [core.ac.uk 149798996](https://core.ac.uk/works/149798996), [TU/e portal](https://research.tue.nl/en/publications/experimental-demonstration-of-neural-network-based-soft-demapper-/). Venue/year/authors NOT VERIFIED.
- **Deep Neural Network-Aided Soft-Demapping in Coherent Optical Systems: Regression Versus Classification** — [ieeexplore 9915441](https://ieeexplore.ieee.org/document/9915441), [Aston OA PDF](https://publications.aston.ac.uk/id/eprint/44286/6/Deep_Neural_Network_Aided_Soft_Demapping_in_Coherent_Optical_Systems_Regression_Versus_Classification.pdf). Verbatim: *"Note that it is known that the regression-based equalization in (1) can be expressed as a classification-based one in (4) with s..."*
- **Neural Network-Based Soft-Demapping for Nonlinear Channels** (Schaedler, Calabrò et al.), OFC 2020 W3D.2 — [OSA](https://proxy.osapublishing.org/abstract.cfm?uri=OFC-2020-W3D.2), [IEEE 9083465](https://xplorestaging.ieee.org/document/9083465).
- **Recurrent Neural Network Soft Demapping for Mitigation of Fiber Nonlinearities and ISI** — [ieeexplore 9489812](https://ieeexplore.ieee.org/document/9489812).
- **Long-Haul Optical-Eigenvalue Transmission Using a Neural Network Demodulator and SD-FEC** — [IEEE PDF](https://ieeexplore.ieee.org/ielx8/49/11006336/10892258.pdf). Verbatim (output is a [0,1] probability, not a raw LLR): *"the frequency of the NN output smoothly changed between 0 and 1 because the data far from the ideal signal points were sufficien..."*

### 1.2 Wireless / general learned demappers

- **LLRSymNet: A Low-Complex Neural Network for LLR Estimation Through Symmetry Exploitation** — [IEEE 10901172](https://ieeexplore.ieee.org/abstract/document/10901172), [TU/e OA PDF](https://research.tue.nl/files/354712198/LLRSymNet_A_Low-Complex_Neural_Network_for_LLR_Estimation_Through_Symmetry_Exploitation_1_.pdf). Verbatim: *"From these mathematical representations, we observe that all patterns of receiver symbols, even those with negative real and/or ..."*
- **Novel Deep Neural OFDM Receiver Architectures for LLR Estimation**, arXiv 2503.20500 — [arXiv PDF](http://www.arxiv.org/pdf/2503.20500), [ADS 2025arXiv250320500K](https://ui.adsabs.harvard.edu/abs/2025arXiv250320500K/abstract).
- **"Machine LLRning": Learning to Softly Demodulate**, arXiv 1907.01512 — [arXiv](http://www.arxiv.org/abs/1907.01512v2), [ADS 2019arXiv190701512S](https://ui.adsabs.harvard.edu/abs/2019arXiv190701512S/exportcitation), [researchr key `ShentalH19`](https://researchr.org/publication/ShentalH19/bibliographies). Method/loss/output/numbers NOT VERIFIED.
- **Convolutional Self-Attention-Based Multi-User MIMO Demapper**, arXiv 2201.11779 — [ar5iv](https://ar5iv.labs.arxiv.org/html/2201.11779). Verbatim: *"Let denote the set of trainable parameters of our learned demapper"*.
- **Low-Complexity Near-Optimum Symbol Detection Based on Neural Enhancement of Factor Graphs** — [IEEE 9903445](https://ieeexplore.ieee.org/abstract/document/9903445/figures). Verbatim (loss = rate/MI-based, not BCE): *"We optimize the parametrization towards a maximum achievable rate between the channel input and the detector output"*.
- **Neural Network-based Information-Theoretic Transceivers for High-Order Modulation Schemes**, arXiv 2506.00368 — [arXiv](https://www.arxiv.org/pdf/2506.00368).
- **Hybrid neural coded modulation: Design and training methods** — [OA PDF](https://scholarworks.bwise.kr/erica/bitstream/2021.sw.erica/111379/1/Hybrid%20neural%20coded%20modulation%20Design%20and%20training%20methods.pdf). Verbatim (output = bit probability): *"where \(p_{i}^{(j)}\in [0,1]\) and we define \(\phi_{\theta}(y^{(j)})\) as a shorthand notation for \(\phi_{\theta}(\psi (y^{(j)}..."*
- **Performance evaluation of polar coded neural demapper based 5G MIMO communication system by varying antenna size** — [Semantic Scholar PDF](https://pdfs.semanticscholar.org/e00a/2e5c6f4b0e2530014e8efce8ca5f9208658b.pdf).
- **Deep Learning for Joint Narrowband Interference Cancellation and Soft Demodulation in OFDM Systems**, arXiv 2607.08717 — [arXiv HTML](https://arxiv-org.ezproxy.obspm.fr/html/2607.08717v1).
- **A Neural Network Aided Approach for LDPC Coded DCO-OFDM with Clipping Distortion**, arXiv 1809.01022 — [arXiv](https://browse.arxiv.org/abs/1809.01022).
- **A Machine Learning-Based Receiver for Mitigating Nonlinear Distortion in OFDM Systems for 5G** — [orbilu.uni.lu PDF](https://orbilu.uni.lu/bitstream/10993/66913/1/A_Machine_Learning-Based_Receiver_for_Mitigating_Nonlinear_Distortion_in_OFDM_Systems_for_5G.pdf). Verbatim: *"OFDM demodulation, Soft demapper, LDPC decoder, and Binary sink blocks: collectively perform inverse operations compared to thei..."*
- **NVIDIA Sionna / Aerial LLRNet** — [Aerial llrnet notebook](https://docs.nvidia.com/aerial/cuda-accelerated-ran/content/notebooks/llrnet-dataset-generation.pdf) (*"The wireless ML design flow using Aerial is depicted in the figure below"*), [MathWorks LLRNet docs](https://www.mathworks.com/help/releases/r2024b/pdf_doc/deeplearning/nnet_ug.pdf) (*"Check if the LLRNet can estimate the LLR values for higher order QAM"*).

### 1.3 Loss functions actually observed (verbatim)

- **BCE on bits, explicit** — Deep Learning OFDM Receivers for Improved Power Efficiency and Coverage, [IEEE 10017176](https://ieeexplore.ieee.org/abstract/document/10017176/figures): *"The training for each receiver is performed using the binary cross entropy (CE) as the loss function, training and optimizing th..."*
- **Soft-BCE gradient = KL(MAP posterior ‖ NN posterior)** — [Lund thesis](https://lup.lub.lu.se/luur/download?func=downloadFile&recordOId=9235508&fileOId=9238393): *"\nabla_{\theta}\mathcal{L}_{\mathrm{soft\_BCE}} = \nabla_{\theta}D_{\mathrm{KL}}(P_{\mathrm{MAP}}\parallel P_{\mathrm{NN}}) + ..."*
- **Non-BCE bit loss** — [Chalmers PDF](https://research.chalmers.se/publication/513449/file/513449_Fulltext.pdf): *"We propose a new loss function, where \(L_{\text{bit}}(a,b)=(1-b)^{a}b^{1-a}\) is used in (9)"*
- **What "CEL" means bit-wise** — [Goethe-Universität Frankfurt thesis](https://publikationen.ub.uni-frankfurt.de/opus4/frontdoor/deliver/index/docId/67559/file/thesis.pdf): *"In the CEL the target bit value is used as an selector whether the logarithm is used on the probability for the bit to be one or..."*
- **BCE-trained binary NN classifier (probability output)** — [arXiv 2508.00587](https://browse-export.arxiv.org/pdf/2508.00587): *"We propose to train a binary NN classifier \(p_{\theta}(y_{i}^{\mathrm{out}}|\mathbf{x}_{i})\) , based on Eq"*

---

## 2. Fritschek / Schaefer / Wunder — neural mutual information estimation

### 2.1 "Deep Learning for Channel Coding via Neural Mutual Information Estimation"
- Authors VERIFIED — [github jElhamm](https://github.com/jElhamm/Deep-Learning-for-Channel-Coding-MI-Estimation): *"Simulations for the paper 'Deep Learning for Channel Coding via Neural Mutual Information Estimation' by Rick Fritschek, Rafael F. Schaefery, and Gerhard Wunder"*
- Venue VERIFIED: **IEEE SPAWC 2019**, **DOI 10.1109/SPAWC.2019.8815464** (DOI seen verbatim in the indexed PDF path string). [ieeexplore 8815464](https://ieeexplore.ieee.org/document/8815464), [ar5iv 1903.02865](https://ar5iv.labs.arxiv.org/html/1903.02865).
- Loss = variational MI lower bound (Donsker–Varadhan style), sampled joint distribution — verbatim [ar5iv §2](https://ar5iv.labs.arxiv.org/html/1903.02865#2): *"where the samples of the joint distribution kk, for the first term in (8), are produced via uniform generation of messages p(xn,..."*
- Code: [Fritschek/Wireless_encoding_with_MI_estimation](https://github.com/Fritschek/Wireless_encoding_with_MI_estimation/).
- Output type, numbers, LLR-calibration discussion: NOT VERIFIED.

### 2.2 "Neural Mutual Information Estimation for Channel Coding: State-of-the-Art Estimators, Analysis, and Performance Comparison"
- Venue/year VERIFIED: **IEEE SPAWC 2020**, Atlanta GA, 2020.5.26–29, **DOI 10.1109/SPAWC48557.2020.9154239** — verbatim indexed path string: *"[IEEE 2020 IEEE 21st International Workshop on Signal Processing Advances in Wireless Communications (SPAWC) - Atlanta, GA, USA (2020.5.26-2020.5.29)] ... Neural Mutual Information Estimation for Channel Coding: State-of-the-Art Estimators, Analysis, and Performance Comparison"*. arXiv [2006.16015](https://ar5iv.labs.arxiv.org/html/2006.16015); SPAWC-2020 talk [resourcecenter.ieee.org](https://resourcecenter.ieee.org/workshops/spawc-2020/spawc20vid117); [TU Dresden FIS](https://fis.tu-dresden.de/portal/en/publications/neural-mutual-information-estimation-for-channel-coding(13aa13d6-e9df-4d9f-98a5-19ee3ffe1409).html).
- Loss VERIFIED (Donsker–Varadhan bound) — [ar5iv 2006.16015 §2](https://ar5iv.labs.arxiv.org/html/2006.16015#2): *"𝔼_ℙ log d𝔾/dℚ = 𝔼_ℙ[f(x,y)] − log 𝔼_ℚ[e^{f(x,y)}] ≤ 𝔼_ℙ log dℙ/dℚ"*
- Which estimators are compared / which wins / numbers: NOT VERIFIED. (Estimator family names `NWJ, DV, InfoNCE, MINE, SMILE-1, SMILE-5` were seen only in a **third-party** NeurIPS 2024 benchmark table, [NeurIPS PDF](https://proceedings.neurips.cc/paper_files/paper/2024/file/525bd8aedafa375564f73bacdef411e5-Paper-Datasets_and_Benchmarks_Track.pdf), not in the Fritschek paper.)

---

## 3. "Bit Error and Block Error Rate Training for ML-Assisted Communication" (arXiv 2210.14103)

- URLs: [arXiv abs](https://browse.arxiv.org/abs/2210.14103v1), [ar5iv HTML](https://ar5iv.labs.arxiv.org/html/2210.14103), [IEEE 10095841](https://ieeexplore.ieee.org/document/10095841), [ETH Research Collection 646432](https://www.research-collection.ethz.ch/handle/20.500.11850/646432), [scirate](https://scirate.com/arxiv/2210.14103), [Semantic Scholar](https://www.semanticscholar.org/paper/Bit-Error-and-Block-Error-Rate-Training-for-Wiesmayr-Marti/cd2c0cd947c64fd66db2f8e3f59d41a070cb0c71).
- Authors partially verified: Semantic Scholar slug `...-for-Wiesmayr-Marti-...`; [arXiv endorser page](http://web3.arxiv.org/auth/show-endorsers/2401.04077) verbatim: *"Gian Marti and Reinhard Wiesmayr are qualified to endorse."*
- Verbatim equation retrieved ([ar5iv §2](https://ar5iv.labs.arxiv.org/html/2210.14103#2)): *"\hat{\mathbf{b}}=\mathrm{dec}(\arg\max_{\mathbf{c}\in \mathcal{C}}p_{\mathbf{c}|\mathbf{y}}(\mathbf{c}|\mathbf{y}))"*
- **Loss functions: NOT VERIFIED. Venue/year: NOT VERIFIED. Numbers: NOT VERIFIED.** (Warning: do not state these without re-checking.)
- Adjacent code-aware-loss work found (not this paper): **An Adaptive Loss Function for the Block Error Rate Optimization in Channel Autoencoders** (Chahine, Jamali) — [IEEE 11432601](https://ieeexplore.ieee.org/document/11432601), [Semantic Scholar](https://www.semanticscholar.org/paper/An-Adaptive-Loss-Function-for-the-Block-Error-Rate-Chahine-Jamali/95d39a8e6510f577921c5067125a94c24ac8a59d); **The Negative BER Loss Function for Deep Learning Decoders** — [AMiner](https://www.aminer.cn/pub/63262f5d90e50fcafdf0ad3d/the-negative-ber-loss-function-for-deep-learning-decoders).

---

## 4. CMDNet (Beck, Bockelmann, Dekorsy)

Title VERIFIED: *"CMDNet: Learning a Probabilistic Relaxation of Discrete Variables for Soft Detection With Low Complexity"*. Year VERIFIED 2021 (archived filename `Beck et al. - 2021 - CMDNet ...`; arXiv ID 2102.12756). Venue NOT VERIFIED (only IEEE doc 9546779 + arXiv preprint).

URLs: [IEEE 9546779](https://ieeexplore.ieee.org/abstract/document/9546779) · [arXiv HTML v3](https://arxiv.org/html/2102.12756v3) · [ar5iv](https://ar5iv.labs.arxiv.org/html/2102.12756) · [author-hosted PDF](https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf) · software [Zenodo 8416528](https://zenodo.org/records/8416528), [Zenodo 10022619](https://zenodo.org/records/10022619), [Zenodo 17987038](https://zenodo.org/records/17987038)

### 4.1 The exact sentence you asked about

Verbatim, from [the author-hosted PDF, page 5, chunk #5](https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf#5#5):

> **"Indeed, we visualize with an exemplary histogram of LLRs that this is not the case"**

**ANTECEDENT: NOT RESOLVED.** ~15 distinct probes (`"true LLR"`, `"not calibrated"`, `"the LLRs are not Gaussian"`, `"do not correspond to the true LLRs"`, `"LLRs at the output of CMDNet"`, `"Kullback-Leibler"`, `"we visualize"`, and variants) never surfaced an adjacent sentence from this document. The only other chunk recovered from that page is a temperature/iteration-schedule fragment ([#5#4](https://www.ant.uni-bremen.de/sixcms/media.php/102/14338/Beck%20et%20al.%20-%202021%20-%20CMDNet%20Learning%20a%20Probabilistic%20Relaxation%20of%20Dis.pdf#5#4)):

> "with \(\tau_{\mathrm{max}} = 1 / (M - 1)\) , \(j \in [0, N_{\mathrm{it}}]\)"

So the sentence sits in the CMDNet architecture / annealing-schedule discussion (τ = relaxation temperature, `N_it` = iterations), not in any passage I could recover containing "calibrated"/"true posterior". **The requester's hypothesis (LLRs badly calibrated / not matching the true posterior) is CONSISTENT with the sentence's grammatical form ("…that this is not the case") but is an inference from form, not a verified claim — I am not asserting it.**

Corroboration that CMDNet does analyse soft-information quality — [Beck dissertation #48#38](https://www.ant.uni-bremen.de/sixcms/media.php/102/15262/dissertation_EdgarBeck_20251118_final.pdf#48#38) verbatim: *"A.3.1 Soft Information MeasureIn the following sections, we draw on results of soft information, i"*

### 4.2 Loss
Verbatim, [Beck dissertation #48#15](https://www.ant.uni-bremen.de/sixcms/media.php/102/15262/dissertation_EdgarBeck_20251118_final.pdf#48#15) — a **symbol-posterior cross-entropy** (M = constellation), i.e. **not** bit-wise BCE on coded bits:

> "\[\begin{array}{l}{\mathcal{H}\left(p,q\right) = -\sum_{n}\sum_{x_{n}\in \mathcal{M}}p(x_{n}|\mathbf{y})\cdot \ln q_{n}(x_{n}|\m..."

Caveat: I saw the expression, **not** a sentence labelling it CMDNet's loss.

### 4.3 Output, coded evaluation, complexity
- Coded-system evaluation exists — [arxiv.org/html/2102.12756v3#6](https://arxiv.org/html/2102.12756v3#6): *"After investigation of detection performance in uncoded systems, we turn to an interleaved and horizontally coded / 32×3232\time..."*
- Simulation-based — [IEEE 9546779 figures](https://ieeexplore.ieee.org/abstract/document/9546779/figures): *"In order to evaluate the performance of the proposed approaches CMD and CMDNet, we present numerical simulation results of appli..."*
- Complexity fragment — [ar5iv #7](https://ar5iv.labs.arxiv.org/html/2102.12756#7): *"… CMDNet | 500500500 | 104superscript10410^{4}-105superscript10510^{5} | 2NL+12subscript𝑁L12N_{\textrm{L}}+1"*
- Training fragment — [dissertation #48#16](https://www.ant.uni-bremen.de/sixcms/media.php/102/15262/dissertation_EdgarBeck_20251118_final.pdf#48#16): *"for \(j \in [0, N_{\mathrm{it}}]\) , a solution \(\theta_{10^5,\mathrm{splin}}\) is learned allowing CMDNet to perform better in..."*
- Calibration discussion: **only the single histogram sentence** — no scaling-factor search, no temperature scaling, no clipping, no over-confidence statement retrieved. Numbers: NOT VERIFIED.

---

## 5. arXiv 2606.29345 — "Neural Augmentation of MIMO-OFDM Receivers for Universal LLR Reconstruction"

URLs: [arxiv.org/html/2606.29345v1](https://arxiv.org/html/2606.29345v1) (also arXiv ezproxy mirror) · [Semantic Scholar reader](https://www.semanticscholar.org/reader/57485766f1cd0fdf5cbcade8b6fbcaa3b6c5a3d1) · [scirate](https://scirate.com/arxiv/2606.29345) · [emergentmind (retitled "Neural LLR Reconstruction for MIMO-OFDM Receivers")](https://www.emergentmind.com/papers/2606.29345) · [catalyzex](https://www.catalyzex.com/paper/neural-augmentation-of-mimo-ofdm-receivers)

**Published journal version, title VERIFIED: "Learning to Refine LLRs: Modular Neural Augmentation for MIMO-OFDM Receivers"** — [IEEE 11587582](https://ieeexplore.ieee.org/document/11587582).

**Output type VERIFIED — per-bit LLR-valued real matrix** — [§3](https://arxiv-org.ezproxy.obspm.fr/html/2606.29345v1#3):

> "𝑳~ki=f𝜽k(𝑿i):=CNN𝜽kC(𝑾k⊙𝑿i)∈ℝB×N,\tilde{{\boldsymbol{L}}}_{k}^{i}=f_{{\boldsymbol{\theta}}_{k}}({\boldsymbol{X}}^{i}):={\rm CNN}..."

i.e. `L̃_k^i = f_θk(X^i) := CNN^C_θk(W_k ⊙ X^i) ∈ R^{B×N}` — B bits × N resources.

**Method VERIFIED in outline — augmented receiver with a primary conventional detector ψ plus an ESCNN post-processor** — [Algorithm 1, PDF p.5](https://arxiv-org.ezproxy.obspm.fr/pdf/2606.29345#5#3): *"Algorithm 1: Augmented Receiver at Time i — Init: Primary detector ψ; ESCNN weights {θk}k=1"*; also [HTML §2](https://arxiv-org.ezproxy.obspm.fr/html/2606.29345v1#2): *"=\prod_{b=1}^{B}\mathcal{CN}\]"*

**Evaluation snippets:** *"We proceed to evaluate modulation scaling"* ([HTML §4](https://arxiv.org/html/2606.29345v1#4)); *"be effectively applied in higher-order modulation settings"* ([SS reader](https://www.semanticscholar.org/reader/57485766f1cd0fdf5cbcade8b6fbcaa3b6c5a3d1)).

**Training loss: NOT VERIFIED** (no passage found — do not assume BCE-on-coded-bits or LLR-MSE). **Numbers/gains: NOT VERIFIED. Calibration discussion: NOT VERIFIED.**

---

## 6. LLR calibration / mismatch / scaling / clipping for learned receivers

### 6.1 Works that DO discuss it (verbatim)

- **STRONGEST HIT — Learning Successive Interference Cancellation for Low-Complexity Soft-Output MIMO Detection, arXiv 2601.16586** — [arXiv HTML](https://arxiv.org/html/2601.16586v1), [ar5iv](https://ar5iv.labs.arxiv.org/html/2601.16586), [SS reader](https://www.semanticscholar.org/reader/3b814f6b51696921e4e5c44a73bedaca263298e7), [papers.cool](https://papers.cool/arxiv/2601.16586):
  > "To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to scaling and clipping, which is a well-known ..."
  Also: *"LLRs obtained via the fallback single bit-flip mechanism, i"* ([PDF](https://export.arxiv.org/pdf/2601.16586)).
- **Complexity Adjusted Soft-Output Sphere Decoding by Adaptive LLR Clipping**, arXiv 1011.2113 — [ar5iv](https://ar5iv.labs.arxiv.org/html/1011.2113).
- **Learning Quantization in LDPC Decoders**, arXiv 2208.05186 — [arXiv PDF](https://arxiv.org/pdf/2208.05186), [ADS 2022arXiv220805186G](https://ui.adsabs.harvard.edu/abs/2022arXiv220805186G/exportcitation). Verbatim learned quantization map from a related IEEE paper ([10251502](https://ieeexplore.ieee.org/ielx7/6287639/6514899/10251502.pdf)): *"LLR_{(w2,T2)} = QCM[w2,T1,T2](..."*
- **Input-Correlated Supervision Noise Limits the Benefits of OTA Training for Learned Receivers**, arXiv 2608.12918 — [arXiv PDF](https://browse-export.arxiv.org/pdf/2608.12918), [SS reader](https://www.semanticscholar.org/reader/f0dd6dcaf31c4c547ea7f3748d28b718aa20bde4).
- **Adaptive LLR quantization (classical)**: *"The adaptive quantization algorithm applies a scaling to the log-likelihood ratios (LLRs) and messages in order to increase the ..."* ([Morero thesis](https://rdu.unc.edu.ar/bitstream/handle/11086/1463/Morero_Tesis.pdf)); *"Parameter α should be chosen such that the least amount of saturation occurs for a given application, as oversaturation..."* ([PolyMtl thesis](https://publications.polymtl.ca/10295/1/2022_DanielBowenDermont.pdf)).
- **LLR reliability caveat** — [JKU thesis](https://epub.jku.at/obvulioa/content/titleinfo/9472866/full.pdf): *"Investigating solely the distribution of the LLRs, however, is only an indicator of their reliability"*
- **Mismatched-LLR mechanism (non-neural)** — [IET Communications](https://onlinelibrary.wiley.com/doi/pdfdirect/10.1049/iet-com.2013.0471): *"In summary, our proposed bit- level LLR histogram- based SNR- mismatch compensated (L-SMCT) decoder applies the consistency con..."*
- **Learned LLR scaling factors — patents, not papers**: [CN120956282A](https://eureka.patsnap.com/patent/CN120956282A) *"Different LDPC decoder variants employ different techniques to select the LLR scaling factor in an attempt to minimize th..."*; [WO2026072512A1](https://eureka.patsnap.com/patent/WO2026072512A1) / [US20260088929A1](https://eureka.patsnap.com/patent/US20260088929A1) *"During inference, the neural network receives the input LLRs, and based on the trained model, the neural network determin..."*; [US10784899](https://patentimages.storage.googleapis.com/13/e3/15/9f288771e385e4/US10784899.pdf) *"In the non- linear approach, the scaling factors may be found using machine learning approaches similar to those used in the lin..."*
- **LLR → decoder interface, stated generically**: *"Following the neural receiver, the estimated LLRs \(\mathbf{LLR}_\theta\) are fed into a standard channel decoder"* ([2602.04728](https://arxiv.org/pdf/2602.04728v3)); *"The corresponding LLRs required by the LDPC decoder are obtained as"* ([2602.11951](https://arxiv-org.ezproxy.obspm.fr/pdf/2602.11951)); *"Unlike hard- decision outputs, soft- output demappers furnish the channel decoder with log- likelihood ratios (LLRs) that quanti..."* ([2609.28852](https://arxiv.org/pdf/2609.28852v1.pdf)); [VERITAS 2501.09761](https://arxiv-org.ezproxy.obspm.fr/html/2501.09761v3) *"The job of the Performance Comparator in VERITAS is to decide if the AI-native receiver (e..."*

### 6.2 Negative evidence
Queries `"temperature scaling neural receiver LLR"`, `"overconfident LLRs neural network"`, `"LLR calibration learned demapper"`, `"mismatched LLR neural demodulator"` returned **no** paper applying temperature scaling to a neural receiver's LLRs, and **no** paper labelled "LLR calibration for learned demappers". The only "overconfident" hits were unrelated domains (ACL 2022 token classification; bioRxiv genomics). This appears to be a genuine gap.

---

## 7. Compact answers to the three explicit questions

**(i) Loss function.** Verified: **BCE on bits** ([10017176](https://ieeexplore.ieee.org/abstract/document/10017176/figures)); **soft-BCE whose gradient equals KL(MAP posterior ‖ NN posterior)** ([Lund thesis](https://lup.lub.lu.se/luur/download?func=downloadFile&recordOId=9235508&fileOId=9238393)); **symbol-posterior cross-entropy** (CMDNet framework, [Beck dissertation](https://www.ant.uni-bremen.de/sixcms/media.php/102/15262/dissertation_EdgarBeck_20251118_final.pdf#48#15)); **Donsker–Varadhan MI bound** (Fritschek, both papers); **maximum-achievable-rate** ([9903445](https://ieeexplore.ieee.org/abstract/document/9903445/figures)); **non-BCE bit loss `L_bit(a,b)=(1-b)^a b^{1-a}`** ([Chalmers](https://research.chalmers.se/publication/513449/file/513449_Fulltext.pdf)). **Bit-wise MSE against true LLRs: zero instances found.**

**(ii) Calibration reporting.** Only [arXiv 2601.16586](https://arxiv.org/html/2601.16586v1) says it outright: *"To ensure stable and well-calibrated soft outputs, the computed LLRs are subject to scaling and clipping…"*. Classical LLR clipping/saturation/scaling literature exists ([1011.2113](https://ar5iv.labs.arxiv.org/html/1011.2113), [Morero](https://rdu.unc.edu.ar/bitstream/handle/11086/1463/Morero_Tesis.pdf), [PolyMtl](https://publications.polymtl.ca/10295/1/2022_DanielBowenDermont.pdf)). For **learned** demappers specifically: **no** temperature-scaling or scaling-factor-search paper found. CMDNet's histogram sentence is the only retrieved soft-information diagnostic for a learned detector, antecedent unresolved.

**(iii) LDPC interface.** No retrieved passage gave a concrete bit-width or saturation limit. Verified: learned receivers feed *"a standard channel decoder"* ([2602.04728](https://arxiv.org/pdf/2602.04728v3)); LLRs are *"required by the LDPC decoder"* ([2602.11951](https://arxiv-org.ezproxy.obspm.fr/pdf/2602.11951)); quantization treated as **learnable** in [2208.05186](https://arxiv.org/pdf/2208.05186). **min-sum vs sum-product: unresolved for every learned-demapper paper here.**

---

## 8. COULD NOT VERIFY

1. **CMDNet — the antecedent of the histogram sentence** (the item the requester most wants). Plausible-but-unconfirmed that it concerns LLRs not matching the true posterior / poor calibration.
2. **CMDNet venue/volume** (only IEEE doc 9546779 + arXiv 2102.12756; year 2021 confirmed).
3. **CMDNet** — whether its CEL is bit-wise or symbol-wise; all quantitative results (dB gains, BLER).
4. **Fritschek et al. 2019/2020** — network output type (probability vs LLR); reported numbers; which estimator wins; any LLR-calibration discussion.
5. **arXiv 2210.14103** — proposed loss functions, venue/year, all numbers, whether the receiver outputs LLRs.
6. **arXiv 2606.29345** — training loss; all numbers/gains; author list; any calibration/quantization discussion. (Verified: title, arXiv ID, `∈ R^{B×N}` LLR-tensor output, ESCNN + primary detector ψ, Algorithm 1, "We proceed to evaluate modulation scaling", journal-version title IEEE 11587582.)
7. **"Machine LLRning"** — authors beyond the ADS bibstem `2019arXiv190701512S` / researchr key `ShentalH19`; venue; method; loss; output type; numbers.
8. **Experimental Demonstration of Neural Network-based Soft Demapper for Long-haul Optical Transmission** — venue/year, authors, training loss, output type, numeric gains, FEC quantization reporting.
9. **Deep Neural Network-Aided Soft-Demapping in Coherent Optical Systems: Regression Versus Classification** — venue/year, per-branch losses, numbers (only the regression↔classification equivalence sentence retrieved).
10. **Neural Network-Based Soft-Demapping for Nonlinear Channels (OFC 2020 W3D.2)** — loss, output type, numbers.
11. **Any paper doing temperature scaling / explicit calibration of learned-demapper LLRs** — none found.
12. **LDPC decoder bit-width, saturation limits, min-sum vs sum-product** for any learned demapper/receiver in this sweep — none found.
13. **Bit-wise MSE-against-true-LLR training** — no instance found.
14. **LLRSymNet, 2503.20500, 4K-QAM soft demapper papers (incl. 10674131), 2201.11779** — titles/URLs verified; methods, losses, numbers, quantization and calibration statements not retrieved.
15. **LLRNet (NVIDIA Sionna/Aerial)** — docs-level existence and purpose only; no paper-level loss/calibration detail.

### Environment limitation affecting re-verification
`web_fetch` cannot reach any host in this sandbox (`URL hostname "arxiv.org" resolves to a non-public IP address`). Several highest-value items (2601.16586, 2606.29345, 2608.12918, 2609.28852) are 2026 arXiv preprints reachable here only as search-index snippets. Getting their losses and numbers verbatim needs a fetch-capable environment or the PDFs themselves.
