# LINE B, part 2 — Sionna / NVIDIA neural receivers, 5G NR PUSCH, end-to-end learned receivers, LDPC interfacing

Compiled 2026-10-07. Every claim below is backed by text I actually retrieved (full text of the paper/PDF/HTML page, downloaded directly). Quotes are verbatim. Items I could not confirm are in the final "could not verify" list.

Legend for each entry: **(i)** NN output / how LLRs are produced · **(ii)** loss + label source · **(iii)** LLR calibration/scaling/clipping · **(iv)** LDPC interface + numbers, real-time status.

---

## 1. Sionna

### 1.1 "Sionna: An Open-Source Library for Next-Generation Physical Layer Research"
- **Venue/year**: arXiv preprint, v1 22 Mar 2022, v2 20 Mar 2023. OpenAlex classifies it as `preprint`, `arXiv (Cornell University)`, 2022. **No peer-reviewed venue confirmed.**
- URLs: https://arxiv.org/abs/2203.11854 · https://arxiv.org/html/2203.11854v1 · https://doi.org/10.48550/arXiv.2203.11854

**(i) What it provides for learned demappers/receivers and LLRs**
> "Sionna enables seamless integration of NNs in the physical layer signal processing chain. As most building blocks are differentiable, gradients can be backpropagated through an entire system, which is the key enabler for end-to-end learning of new artificial intelligence (AI) -defined air interfaces."

> "Another design principle of Sionna is that all components are implemented as independent Keras layers [17]. This has the advantages that (i) complex system models can be constructed by simply connecting the desired layers, (ii) components can be easily replaced by NNs, and (iii) gradients are automatically computed by TensorFlow [16] …"

Listing 1 ("Sionna 'Hello, World!' example") shows the LLR chain explicitly:
> `llr = Demapper("app", c)([y, 1/snr])` → `b_hat = LDPC5GDecoder(LDPC5GEncoder(k, n))(llr)`

Listing 2 / Listing 3 give the learned variant:
> `llr = NeuralDemapper()([y, 1/snr])` … `loss = BinaryCrossentropy(from_logits=True)(u, llr)`
> "Listing 2: Components can be easily replaced by NNs or made trainable."

The `NeuralDemapper` Keras layer builds its input as `tf.stack([real(y), imag(y), no], axis=-1)`, runs two dense layers and reshapes to `[batch_size, n]` — i.e. **it outputs one LLR per coded bit**, reshaped to codeword length.

**(ii) Loss**
> "One could then define an adequate loss function (line 7), such as the total binary cross-entropy, and compute the gradient of this loss with respect to all trainable variables."

Labels are the **real LDPC-encoded bits**: Listing 2 is `b = BinarySource()([batch_size, k])` → `u = LDPC5GEncoder(k, n)(b)` → `x = Mapper(constellation=c)(u)` → … → `loss = BinaryCrossentropy(from_logits=True)(u, llr)`. So the BCE target `u` is the output of a real 5G NR LDPC encoder.

**(iii)/(iv)** No LLR quantization/clipping discussion in this paper (see §1.4 for the decoder API).

### 1.2 "Sionna Research Kit: A GPU-Accelerated Research Platform for AI-RAN"
- **Venue/year**: **IEEE ICMLCN 2025** (accepted demo); arXiv 2505.15848 (May 2025). DOI `10.1109/icmlcn64995.2025.11140427`.
- URLs: https://arxiv.org/abs/2505.15848 · https://arxiv.org/html/2505.15848v1 · https://ar5iv.labs.arxiv.org/html/2505.15848

> "To demonstrate the capabilities, we deploy a real-time neural receiver—trained with NVIDIA Sionna and using the NVIDIA TensorRT library for inference—in a 5G NR cellular network using commercial user equipment."

> "We have developed a prototype of a 5G NR standard-compliant neural receiver [4] that replaces parts of the physical layer signal processing with machine-learned components. The architecture has been carefully optimized to ensure real-time inference capabilities."

> "The receiver is trained with NVIDIA Sionna [8] and implemented using the NVIDIA TensorRT inference library."

Section II-B is titled "Case Study: CUDA-accelerated LDPC Decoding":
> "In a second example, we demonstrate GPU offloading in wireless systems by implementing a CUDA-accelerated LDPC decoder which seamlessly integrates into the OAI stack."

> "…tutorials on real-world data acquisition and a TensorRT accelerated neural demapper are provided."

No explicit LLR bit-width statement in this paper (that appears in the tutorials, below).

### 1.3 Sionna "Neural Demapper Training and TensorRT Export" tutorial (Research Kit)
- URL: https://nvlabs.github.io/sionna/v2.1.0/rk/tutorials/neural_demapper/Neural_demapper.html (also v1.2.2 with `#Export-TensorRT-Engine`)

**(i) Output — yes, LLRs, directly from the NN:**
> "From the above equation, we see that the neural demapper requires the received complex-valued symbol \(y\) and the noise variance \(N_0\) as input. **It returns modulation order LLRs.** We can use the Sionna APP demapper as a reference implementation to verify the performance of the neural demapper."

The model docstring states: `llr: [batch_size, num_bits_per_symbol], torch.float32 — LLRs for each bit of a symbol.` Architecture: 3-layer MLP (`Linear(num_inputs,32) → ReLU → Linear(32,32) → ReLU → Linear(32,num_bits_per_symbol)`).

**(ii) Loss — confirmed binary cross-entropy (BCE) on bits:**
> "**As demapping is a binary classification task of the modulation order logits, we use the binary cross entropy (BCE) loss and average over the individual bits.**"
> `# Binary classification problem -> train on BCE loss` / `bce = nn.BCEWithLogitsLoss()`
> `llr = neural_demapper_synthetic([qxr, qxi])` · `loss = bce(-llr, bits.float())  # Negate: OAI convention is flipped vs Sionna`

**Label source caveat — these are *uncoded* random bits, not LDPC output:** "We draw random payload bits, map them to QAM symbol and simulate the transmission over an AWGN channel." (`bits = binary_source([BATCH_SIZE, NUM_BITS_PER_SYMBOL])`).

**(iii) LLR scaling / quantization — explicit, int16 with 2^8 scaling, plus a measured scale mismatch:**
> `def int16_to_float16(symbols_i): return np.ldexp(symbols_i.astype(np.float32), -8).astype(np.float16)`
> `def float16_to_int16(llrs_h): return np.rint(np.ldexp(llrs_h.astype(np.float32), 8)).astype(np.int16)`

Measured LLR ranges and the scaling factor needed to match the OAI demapper:
> `Sionna LLR range: [-143, 172], mean=0.1, std=38.8`
> `OAI LLR range: [-257, 245], mean=0.4, std=94.0`
> `LLR scale factor (OAI/Sionna): 2.42`

**(iv) LDPC interface — min-sum, tolerant of mis-scaling:**
> "As noise variance estimates are not available, we train the neural demapper with only two inputs (real and imaginary part of the received symbol, respectively). This obviously does not allow to reproduce exact APP estimates, but intuitively the demapper learns an average noise variance over all possible input SNRs. **Keep in mind that the LDPC decoder is implemented as min-sum decoder which is know to be robust against mis-scaling of the LLRs.**"

Sign convention explicitly handled: `llr = -1 * demapper_ref(y_t, no) # Flip sign: Sionna uses log(P(1)/P(0)), OAI uses log(P(0)/P(1))`.

### 1.4 Sionna "5G NR PUSCH Neural Receiver" tutorial (Research Kit)
- URL: https://nvlabs.github.io/sionna/v2.1.0/rk/tutorials/neural_receiver/

> "The core idea is to replace the conventional physical layer receiver signal processing - specifically channel estimation, equalization, and demapping—with a single neural network. Unlike classical receivers, this architecture operates on an entire 5G NR slot, jointly processing the full OFDM time-frequency resource grid to reconstruct the transmitted bits."

> "**CUDA Acceleration : Custom CUDA kernels handle data pre-processing and post-processing to further minimize latency (e.g., converting input symbols to float16 and output LLRs to int16).**"

> "**Fixed Input Dimensions : To avoid the latency penalties associated with dynamic shape reallocation in TensorRT, we fix the number of Physical Resource Blocks (PRBs) to 24.** If the scheduled number of PRBs is smaller than the configured maximum, the input is padded. For more than 24 PRBs, the receiver operates on tiles of 24 PRBs."

> "Zero-Copy Memory : Data transfer between the CPU (OAI stack) and GPU (Neural Receiver) leverages unified memory to avoid memcopy bottlenecks."

> "Slot-Based Processing : Unlike the default OAI PUSCH processing which operates per OFDM symbol, the neural receiver overrides this behavior to process an entire slot at once."

### 1.5 Sionna `LDPC5GDecoder` API (the concrete LLR interface)
- URL: https://nvlabs.github.io/sionna/phy/api/fec/ldpc/sionna.phy.fec.ldpc.LDPC5GDecoder.html

> `class sionna.phy.fec.ldpc.LDPC5GDecoder(encoder, cn_update='boxplus-phi', vn_update='sum', cn_schedule='flooding', hard_out=True, return_infobits=True, num_iter=20, llr_max=20.0, …)`

> "**llr_max (float | None) – Internal clipping value for all internal messages. If None, no clipping is applied. Filler bits from 5G shortening still use a finite LLR of 1e4.**"

> "cn_update … One of “boxplus-phi” (default), “boxplus”, **“minsum”**, “offset-minsum”, “identity”, or a callable."

> "llr_ch – […, n] or […, num_rvs, n], **torch.float**. Tensor containing the channel logits/llr values."

> "If required the decoder can be made trainable and is differentiable (the training of some check node types may be not supported) following the concept of “weighted BP” [Nachmani]."

> "As decoding input logits \(\operatorname{log}\frac{p(x=1)}{p(x=0)}\) LLRs with definition \(\operatorname{log}\frac{p(x=0)}{p(x=1)}\) are …" (sign-convention note)

**Defaults**: sum-product-ish CN update (`boxplus-phi`), sum VN update, 20 iterations, internal clipping at `llr_max = 20.0`. Channel LLR input is **float** (no fixed-point type in the API).

### 1.6 Sionna "Neural Receiver for OFDM SIMO Systems" tutorial — loss is a BMD-rate/BCE objective
- URL: https://nvlabs.github.io/sionna/phy/tutorials/notebooks/Neural_Receiver.html

> "…the neural receiver substitutes channel estimation, equalization, and demapping. It takes as input the post-DFT (discrete Fourier transform) received samples, which form the received resource grid, and computes log-likelihood ratios (LLRs) on the transmitted coded bits. These LLRs are then fed to the outer decoder…"

> "**Training is done on the bit-metric decoding (BMD) rate which is computed from the transmitted bits and LLRs:**"
> `R = 1 - (1/SNMK) * Σ Σ Σ Σ BCE(B_{s,n,m,k}, LLR_{s,n,m,k})`
> "\(\texttt{BCE}(\cdot,\cdot)\) the binary cross-entropy in log base 2" — i.e. **bit-wise BCE on coded bits, framed as a BMD-rate surrogate**.

---

## 2. "Design of a Standard-Compliant Real-Time Neural Receiver for 5G NR" (central item)

- **Venue/year**: **IEEE ICMLCN 2025**, DOI `10.1109/icmlcn64995.2025.11140048`; arXiv 2409.02912 (4 Sep 2024).
- Authors: Reinhard Wiesmayr (ETH Zurich), Sebastian Cammerer, Fayçal Aït Aoudia, Jakob Hoydis, Jakub Zakrzewski, Alexander Keller (NVIDIA). Funding note: "Work done during an internship at NVIDIA… Grant Agreement 101096379 (CENTRIC)."
- URLs: https://arxiv.org/abs/2409.02912 · https://arxiv.org/html/2409.02912v1 · https://research.nvidia.com/publication/2024-09_design-standard-compliant-real-time-neural-receiver-5g-nr · https://doi.org/10.1109/icmlcn64995.2025.11140048

**(i) Architecture & output — LLRs produced directly by the NN's readout layer**
> "**The NRX depicted in Fig. 1 implements a convolutional and graph neural network (CGNN) for MU-MIMO detection across the OFDM resource grid (RG). To enable 5G NR standard compliance, the CGNN is surrounded by a RG demapper and a transport block decoder (cf. Sionna's 5G NR module).** To enable channel estimation for varying DMRS …, a least squares (LS) channel estimator provides an initial channel estimate to the CGNN for each individual data stream."

> "The CGNN architecture consists of three main components: (i) the state initialization layer (StateInit), (ii) the unrolled iterative CGNN algorithm (CGNNit blocks), and (iii) the read-out layers. While the architecture in [6] only proposes one type of output layer that is used to transform the state variable to LLR estimates (ReadoutLLRs), our new architecture implements an additional read-out layer that outputs a (refined) channel estimate, also from the same state variable."

> "After such CGNNit blocks, readout MLPs are applied to transform the final state vectors into the desired outputs. As in [6], **the ReadoutLLRs layer outputs LLR estimates for each UE's symbol of all REs on the RG.**"

> "**The goal of classical MIMO detectors as well as that of the NRX is to compute log-likelihood ratio (LLR) estimates for each of the UE's transmitted bits from the received signal. We define the LLRs as logits**, i.e., (2) … and feed their estimates to the subsequent channel decoder."

**(ii) Training loss — binary cross-entropy on coded-bit labels produced by a real 5G NR LDPC encoder**
> "**The NRX from [6] is trained by empirical risk minimization with the binary cross-entropy (BCE) loss that is computed between the LLR estimates and ground-truth bit labels.** A training step describes one gradient-based weight update that is computed from the average loss of independent samples. Each of these batch samples represents transmission of one entire OFDM RG. If not stated otherwise, the NRX is trained on synthetic training data sampled from the 3GPP urban microcell (UMi) channel model."

> "The bits transmitted in … originate from random payload bits that are **encoded by 5G NR compliant low-density parity-check (LDPC) channel coding and rate-matching**, which depend on the MCS index and the total number of data-carrying REs."

> "**The code rate and coding scheme is transparent to the NRX, as the NRX outputs LLRs on coded bits.**"

Auxiliary loss (double-readout) and multi-loss:
> "The ReadoutChEst layer can be jointly trained with the ReadoutLLRs layer by adding an MSE loss to the BCE loss. The MSE loss is computed between the output channel estimates and the ground-truth channel realizations, and scaled by a hyperparameter…"
> "To support a variable number of unrolled NRX iterations \(N_{it}\), the NRX is trained with the so-called multi-loss [14]. There, the read-out layers are applied to the state variable after each NRX iteration and the total loss is accumulated from the loss of all model readouts."

**(iii) LLR calibration / scaling — discussed conceptually, and named as a real hazard for mismatched demapping**
> "Though such mismatched demapping will produce LLRs with the same sign as a matched demapper for the correct constellation, **the LLR magnitudes do in general not match the underlying probabilities, as defined in (2). Without additional LLR correction, such as proposed in [16], “classical” mismatched demapping can lead to severe performance degradation in forward error correction (FEC) decoding.**"

> "The intuition for modulation-specific output layers is to enable the model to learn matched demapping."

**Number precision**: weights are float16, no QAT:
> "**Weights are quantized to float16. However, we did not implement quantization-aware training techniques in the scope of this work. Exploring even lower quantization levels such as float8 or int8 precision is a subject of future research.**"

**(iv) LDPC interface + latency/real-time numbers**
- Latency budget: "We assume a strict computational latency budget of **1 ms** for the NRX using an NVIDIA A100 GPU. Furthermore, we assume inline acceleration, i.e., we ignore any memcopy latencies from host to device and vice versa."
- Real-time engine: "To get a more realistic latency measure, we deploy the trained NRX model using TensorRT as real-time inference engine."
- Measured: "For the given system configuration of **132 PRBs and 2 active UEs**, each iteration requires approximately **350 µs** and we observe a constant initialization (and readout) overhead of **270 µs**."
- Constraint effect: "the strict computational latency constraint of 1 ms restricts the receiver to \(N_{it} \le 2\)."
- Model sizes: "Large NRX … \(N_{it}=8\) CGNN iterations and a total of \(4.4\cdot10^{5}\) weights. A reduced architecture is denoted as Real-time (RT) NRX and consists of only \(N_{it}=2\) CGNN iterations resulting in only \(1.4\cdot10^{5}\) weights." (feature depth \(d_S=56\); "The Var-MCS NRX stores \(0.4\cdot10^{5}\) additional weights per set of IO layers.")
- Headline SNR penalty: "we quantify the resulting signal-to-noise ratio (SNR) degradation as **less than 0.7 dB** when compared to a preliminary non-real-time NRX architecture."
- Baseline: "all NRX architectures are capable of approaching the performance of the linear minimum mean square error (LMMSE) channel estimation baseline with K-Best detection" — "the K-Best detector applies a list size of \(k=64\)".
- Scenario: "We simulate 5G NR compliant OFDM slots with a carrier frequency of 2.14 GHz and a bandwidth of approximately 47.5 MHz, which equals 132 physical resource blocks (PRBs) … 30 kHz" SCS, DMRS type A with one additional DMRS position. TBLER target 10%.
- Site-specific fine-tuning: "The fine-tuned Large NRX with \(N_{FT}=10^{3}\) closely approaches the K-Best baseline with LMMSE channel estimation." Also: "\(N_{FT}=10^{3}\) fine-tuning steps only take about 30 s on an NVIDIA RTX 3090 GPU", and catastrophic forgetting is reported.
- **Real-time/hardware**: yes — TensorRT on an NVIDIA A100 GPU; explicit claim "The resulting NRX is ready for deployment in a real-time 5G NR system and the source code including the TensorRT experiments is available online."

---

## 3. NVIDIA 5G NR PUSCH neural receivers

### 3.1 "A Neural Receiver for 5G NR Multi-user MIMO"
- **Venue/year**: **IEEE Globecom Workshops (GC Wkshps) 2023**, DOI `10.1109/gcwkshps58843.2023.10464486`; arXiv 2312.02601. (arXiv comments: "presented at IEEE Globecom 2023".)
- Authors (per citing notebook): Cammerer, F. Aït Aoudia, J. Hoydis, A. Oeldemann, A. Roessler, T. Mayer, A. Keller.
- URLs: https://arxiv.org/abs/2312.02601 · https://arxiv.org/html/2312.02601v1

**(i) Architecture & output**
> "We introduce a neural network (NN) -based multi-user multiple-input multiple-output (MU-MIMO) receiver with 5G New Radio (5G NR) physical uplink shared channel (PUSCH) compatibility. The NN architecture is based on convolution layers to exploit the time and frequency correlation of the channel and a graph neural network (GNN) to handle multiple users… **The proposed architecture adapts to an arbitrary number of sub-carriers and supports a varying number of MIMO layers and users without the need for any retraining.**"

> "The main stage of the proposed neural receiver consists of an unrolled iterative algorithm with iterations" *(matched passage)*

> "The last stage consists of **computing log-likelihood ratios (LLRs)** \(\boldsymbol{\ell}_{n_F,n_S,n_T}^{(t)}\in\mathbb{R}^{m}\) for the \(m\) bits \(\mathbf{b}_{n_F,n_S,n_T}\) transmitted by the layer over this RE for every layer \(n_T\) and every RE \([n_F,n_S]\)."

**(ii) Loss — BCE; labels are transmitted bits**
> "The proposed neural receiver architecture is fully differentiable and, hence, stochastic gradient descent (SGD) -based training is straightforward. We use the adaptive momentum (ADAM) optimizer with learning rate \(l=10^{-3}\), and the **binary cross-entropy (BCE) loss function**. Thus, the neural receiver can be seen as attempting to solve \(N_F\times N_S\times N_T\times m\) binary classification problems in parallel from the received signal \(\mathbf{Y}\)."

> "As LLRs are binary logits, \(\sigma(\ell_{n_F,n_S,n_T,i}(\mathbf{Y}))\) gives the corresponding probability for the bit to be equal to one given \(\mathbf{Y}\)."

> "Moreover, training on the BCE was shown to be equivalent to minimizing the Kullback–Leibler (KL) divergence between the posterior distribution on the bits approximated by the neural receiver and the true one that would be given by an optimal receiver."

**LDPC decoder in the loop — tried, reported as not useful:**
> "**Further, the LLRs after channel decoding can be used for training if the channel decoder is differentiable (e.g., for belief propagation (BP) decoding [14]). However, we empirically did not observe any gains by doing so.**"

**(iii)** No LLR clipping/quantization discussion found in this paper.

**(iv) LDPC interface + numbers / hardware**
> "Fig. 6 shows the block error rate (BLER) at transport block level including all physical layer effects of an entire slot. **As can be seen, the neural receiver operates close to the baseline consisting of LMMSE channel estimation with K-best detection but at a significantly lower computational complexity.** Further, the neural receiver outperforms the more practical receiver using LS channel estimation and LMMSE-based detection significantly. While the solid green curve is a simulated result, the blue dots indicate the measured TBLER for the actual experiment. The small performance mismatch can be explained by the aggregation of all hardware effects (quantization noise, carrier frequency offset, …) which are not considered in the simulations."

> "Finally, we demonstrate the results of a **hardware-in-the-loop verification based on 3GPP compliant conformance test scenarios**."

Training/eval config: "the number of PRBs is set to 4 (i.e., \(N_F=48\) sub-carriers) and 217 PRBs (i.e., \(N_F=2604\) sub-carriers) for the evaluation"; carrier 2.14 GHz, SCS 30 kHz; user speeds uniform in \([0,34]\) m/s.

### 3.2 NVlabs/neural_rx — "Jumpstart into Multi-User MIMO Neural Receiver for 5G NR PUSCH"
- Repo: https://github.com/NVlabs/neural_rx ("Real-Time Inference of 5G NR Multi-user MIMO Neural Receivers")
- Notebook: https://github.com/NVlabs/neural_rx/blob/main/notebooks/jumpstart_tutorial.ipynb
- Source checked: `utils/e2e_model.py`, `utils/neural_rx.py`, `config/nrx_rt.cfg`

> "This notebook introduces the training and evaluation pipeline of the neural receiver (NRX)" … `config_name = "nrx_rt.cfg" # this config defines the entire training and evaluation pipeline`

**(ii) Loss — BCE on LLRs (+ optional MSE on channel estimates), from the code docstring:**
> "loss_data: tf.float — **Binary cross-entropy loss on LLRs. Computed from active UEs and their selected MCSs.**"
> "loss_chest: tf.float — Mean-squared-error (MSE) loss between channel estimates and ground truth channel CFRs. Only relevant if double-readout is used."
> `"double_readout": [True, True], # use additional MSE loss on h_hat` · `"weighting_double_readout": [0.02, 0.01]} # weighting between MSE & BCE loss`

**(i) Output:**
> `class ReadoutLLRs(Layer): """Network computing LLRs from the state vectors. … The output layer is a dense layer without non-linearity and with num_bits_per_symbol units."""`

**(iv) Training/eval + dtype:** `nrx_dtype = tf.float32`; training via `scripts/train_neural_rx.py`; "during training, random number of active users and SNR values are sampled. Thus, the loss function has a high fluctuation."; "the neural receiver is typically trained for a smaller number of PRBs. However, for the evaluation an arbitrary number can be set without any need for re-training." No LLR clipping/quantization is defined here — quantization to int16 appears only in the OAI/Sionna-RK integration layer.

### 3.3 NVIDIA Aerial / pyAerial notebooks
- "Using pyAerial to evaluate a PUSCH neural receiver": https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/content/notebooks/example_neural_receiver.html

> "the neural network is used to replace channel estimation, noise and interference estimation and channel equalization, and **thus outputs log-likelihood ratios directly**. The model is a variant of what has been proposed in [Cammerer et al., 'A Neural Receiver for 5G NR Multi-user MIMO', IEEE Globecom Workshops (GC Wkshps), Dec. 2023]."

> "The rest of the PUSCH receiver pipeline following the neural receiver, **meaning LDPC decoding chain, is modeled using pyAerial**. Also, the neural receiver takes LS channel estimates as inputs in addition to the received PUSCH slot."

Code comments: `# This is the neural receiver part. # It outputs the LLRs for all symbols.` → `derate_match(input_llrs=[llrs], …)` → `LdpcDecoder.decode(...)` → `CrcChecker.check_crc(...)`.

Measured BLER table (TB-level, with LDPC decode; "ms/TB" is simulation wall-clock):
```
Es/No (dB)   PUSCH Rx BLER   Neural Rx BLER   ms/TB
 -8.00          1.0000           1.0000        113.3
 -6.00          0.9214           0.8929        113.0
 -4.00          0.6684           0.6427        113.0
 -2.00          0.2941           0.2703        112.9
  0.00          0.1056           0.0917        112.9
  2.00          0.0291           0.0254        112.9
  4.00          0.0066           0.0055        112.9
```

- "LLRNet: Model training and testing": https://docs.nvidia.com/aerial/cuda-accelerated-ran/latest/content/notebooks/llrnet_model_training.html

> "**The LLRNet is plugged in the PUSCH receiver chain in place of the conventional soft demapper.**"

> "The LLRNet model follows the original paper … Shental, J. Hoydis, “'Machine LLRning': Learning to Softly Demodulate”, https://arxiv.org/abs/1907.01512"

Model + loss (LLR-target MSE — **not** BCE on bits):
> `class LLRNet(nn.Module): """LLRNet model for soft demapping. … (real and imaginary parts separated) and outputs soft bits (LLRs)."""`
> `def loss_fn(predictions, llr, mod_order): """MSE loss between predicted and target LLRs.""" mse = torch.mean((predictions[:, :mod_order] - llr) ** 2)`
> `epochs = 5`; "Modulation order. LLRNet needs to be trained separately for each modulation order."

---

## 4. F. A. Aït Aoudia / J. Hoydis papers

### 4.1 "Joint Learning of Probabilistic and Geometric Shaping for Coded Modulation Systems"
- **Venue/year**: **IEEE Globecom 2020**, DOI `10.1109/globecom42002.2020.9348032`; arXiv 2004.05062.
- URLs: https://arxiv.org/abs/2004.05062 · https://arxiv.org/html/2004.05062v2

**(i) LLRs — yes, from a differentiable (learned-parameter) demapper:**
> "On the receiver side, **a differentiable demapper computes log-likelihood ratios (LLRs)** \(\tilde{\mathbf{l}}\in\mathbb{R}^{n}\) from the received symbols \(\mathbf{y}\in\mathbb{C}^{q}\). The LLRs are fed to a decoding algorithm that reconstructs the matched information bits."

**(ii) Loss — total binary cross-entropy as a proxy for bit-wise mutual information (BMI); labels are codeword bits**
> "**by optimizing the proposed end-to-end system made of the modulator, channel, and demapper on the bit-wise mutual information (BMI) [5], which is an achievable rate for BICM systems [6]**, joint optimization of geometric shaping, probabilistic shaping, bit labeling, and demapping is performed for a given channel model, code rate, and for a wide range of signal-to-noise ratios (SNRs)."

> "Finding a local solution to (2)-(3) is done by performing stochastic gradient descent (SGD) on a loss function that serves as a proxy for \(R\). **As noticed in [5], the BMI is closely related to the total binary cross-entropy (BCE) defined by** …"

> "Therefore, the KL divergence in (11) is null, and **the loss function \(\mathcal{L}\) equals the BMI \(R\) up to the sign.**"

Labels come from a real channel encoder: "The matched information bits \(\boldsymbol{c_I}\) are fed to a channel encoder that generates a vector of parity bits … the codeword \(\mathbf{c}=[\boldsymbol{c_I}\ \boldsymbol{c_P}]\) is mapped to a vector of channel symbols". (The FEC code itself is treated abstractly in this paper.)

### 4.2 "Model-Free Training of End-to-End Communication Systems"
- **Venue/year**: **IEEE JSAC 2019** (Special Issue on Machine Learning in Wireless Communication), DOI `10.1109/jsac.2019.2933891`; arXiv 1812.05929.
- URLs: https://arxiv.org/abs/1812.05929 · https://arxiv.org/html/1812.05929v3

**(i) Output:** autoencoder with a *differentiable receiver network* trained on the message; **no LLR statement found in the text** (see could-not-verify).

**(ii) Loss — categorical cross-entropy on messages:**
> "where \(\mathbb{E}_m\) is the expectation taken over the messages \(m\), and \(l(\mathbf{p},m)=-\log(p_m)\) is the **categorical cross-entropy (CE)**."
> "The CE serves as loss function for training."

Method: "The key idea is to approximate the loss function gradient with respect to (w.r.t.) the transmitter parameters by relaxing the channel input to a random variable." / "We develop a novel alternating algorithm for end-to-end training without channel model knowledge which iterates between two phases: (i) training of the receiver using the true gradient of the loss and (ii) training of the transmitter based on an approximation of the loss function gradient."

**(iv) Hardware — real SDR prototype:**
> "Moreover, we demonstrate the algorithm's practical viability through **hardware implementation on software defined radios (SDRs)** where it achieves state-of-the-art performance over a coaxial cable and wireless channel."
> "**To the best of our knowledge, this is the first prototype of an autoencoder-based communication system trained over actual channels.**"
> "This prototype was trained using a channel model, and then deployed and evaluated over the actual channel." / "With our setup based on consumer class hardware, training for a few hundred iterations takes only a few minutes."

### 4.3 "Towards Hardware Implementation of Neural Network-based Communication Algorithms"
- **Venue/year**: **IEEE SPAWC 2019**, DOI `10.1109/spawc.2019.8815398`; arXiv 1902.06939.
- URLs: https://arxiv.org/abs/1902.06939 · https://arxiv.org/html/1902.06939v1
- Note the capitalization: the arXiv/IEEE title uses lowercase "based" ("Neural Network-based"), not "Based".

**(i) Output:** the paper is about fixed-point NN implementation for PHY tasks; **no LLR statement found**.

**(ii) Loss:** treated generically — "Let us denote by \(f_{\boldsymbol{\psi}}\) the mapping implemented by an NN with parameters \(\boldsymbol{\psi}\in\mathbb{R}^{P}\), \(L(\boldsymbol{\psi})\) the loss function, \(\mathcal{C}\) the quantization codebook, and \(\widehat{\boldsymbol{\psi}}\in\mathcal{C}^{P}\) the quantized weights." No specific loss (BCE/MSE/MI) is singled out in the passages found.

**(iii)/(iv) Quantization (the paper's core contribution):**
> "However, most work on this topic is simulation based and implementation on specialized hardware for fast inference, such as field-programmable gate arrays (FPGAs), is widely ignored."
> "**We demonstrate that it is possible to implement NN-based algorithms in fixed-point arithmetic with quantized weights at negligible performance loss and with hardware complexity compatible with practical systems, such as FPGAs and application-specific integrated circuits (ASICs).**"
> "Fixed-point compute units are faster and consume less hardware resources and energy than conventional floating-point units."
> "As the weights are assumed to be fixed after deployment, the bit shifts can be 'hardwired' in the hardware implementation, removing the need for storing the weights in memory, as well as programmable bit shifters."
> "Hardware acceleration is needed to achieve reasonable inference time, and most of previous contributions leverage graphics processing units (GPUs), which come at high monetary and energy cost not viable for communication systems."

### 4.4 "End-to-End Learning for OFDM: From Neural Receivers to Pilotless Communication"
- **Venue/year**: **IEEE Transactions on Wireless Communications** (DOI `10.1109/twc.2021.3101364`; ADS record `2022ITWC...21.1049A`); arXiv 2009.05261.
- URLs: https://arxiv.org/abs/2009.05261 · https://arxiv.org/html/2009.05261v2
- ⚠️ **The title variant "…From Neural Receivers to Hardware Feasibility" does NOT exist.** An OpenAlex title search for "End-to-End Learning for OFDM" returns only "From Neural Receivers to Pilotless Communication" (2021, IEEE TWC) plus unrelated works. Treat the "Hardware Feasibility" title as a mis-citation.

**(i) LLRs — the *baseline* demapper produces LLRs in closed form; the neural receiver's output feeds the same soft pipeline:**
> "The log-likelihood ratio (LLR) for the \(i^{th}\) bit … is computed as follows:" (Gaussian/APP demapping after LMMSE channel estimation)
> "Whenever \(k\) corresponds to the index of a pilot symbol, no LLR value is computed. After deinterleaving, **the LLRs are fed to a channel decoding algorithm (e.g., belief propagation)** which computes predictions of the transmitted bits."

**(ii) Loss — total binary cross-entropy per bit per frame:**
> "In our setting, **training aims at minimizing the total binary cross-entropy \(\mathrm{bit}\,\mathrm{frame}^{-1}\) (BCE)**, defined as"
> \(\mathcal{L}\triangleq-\sum_{k\in\mathcal{N}_D}\sum_{i=1}^{m}\mathbb{E}_{b_{k,i},\mathbf{y}}\{\log_2(Q_{k,i}(b_{k,i}|\mathbf{y}))\}\) … estimated through Monte Carlo sampling.

**(iii)** No LLR clipping/saturation discussion found.

**(iv) Headline results (no hardware; simulation + analysis):**
> "Both schemes achieve the same BER as the pilot-based baseline with **7 % higher throughput**."
> "conventional baselines are less robust to higher speeds compared to the neural receiver." / "At high speeds, the iterative baseline achieves the lowest BERs, closely followed by the neural receiver…"
> Baselines: "two strong baseline receivers which rely on linear minimum mean square error (LMMSE) channel estimation with perfect tempo-spectral covariance matrix knowledge and **iterative estimation, demapping, and decoding (IEDD)**".

### 4.5 "Trainable Communication Systems: Concepts and Prototype" (Cammerer, Aït Aoudia, Dörner, Stark, Hoydis, ten Brink)
- **Venue/year**: **IEEE Transactions on Communications 2020**, DOI `10.1109/tcomm.2020.3002915`; arXiv 1911.13055 (comments: "submitted to IEEE TCOM").
- URLs: https://arxiv.org/abs/1911.13055 · https://ar5iv.labs.arxiv.org/html/1911.13055

**(i) Output — LLRs, explicitly, for seamless decoder interfacing:**
> "**On the receiver side, the learned demapper outputs LLR and can therefore be smoothly interfaced with a channel decoder, e.g., based on BP (BP).** Since the learned constellations benefit from IDD [9], we present a differentiable neural IDD architecture which unlocks significant gains on the AWGN channel, considering a standard 802.11n LDPC code."

> "we define the logits such that \(\widetilde{p}_{\boldsymbol{\theta_D}}(b_j=0|y)=\sigma(l_j)\) instead of the more common definition \(\widetilde{p}_{\boldsymbol{\theta_D}}(b_j=1|y)=\sigma(l_j)\). However, this only impacts the sign but **allows to use logits and LLR equally throughout this paper**"

**(ii) Loss — bit-wise binary cross-entropy; the encoder/decoder are NOT needed at training time:**
> The loss is estimated by the bit-wise CE:
> \(\mathcal{L}(\boldsymbol{\theta_M},\boldsymbol{\theta_D})\approx-\frac{1}{|\mathcal{B}|}\sum_{\mathbf{b}\in\mathcal{B}}\sum_{j=1}^{m}\left(b_j\log(\widetilde{p}(b_j|y))+(1-b_j)\log(1-\widetilde{p}(b_j|y))\right)\)
> "**Note that, at training, the channel encoder and decoder are not needed, as the loss function (6) is based on the output of the demapper.** At evaluation, the number of iterations performed by the BP decoder was set to 40."

For the IDD extension, the naive codeword-level CE fails and a per-iteration sum of binary CE is used:
> "It was experimentally observed that training the end-to-end system by minimizing (12) leads to poor performance. Therefore, the following loss is used, **which is obtained by summing the binary CE computed at the output of the demapper over all iterations**" (eq. 13).
> "Also lower-complexity BP variants, such as min-sum decoding, can be used as long as they are differentiable."

**(iv) Prototype / hardware:**
> "Lastly, we show the viability of the proposed system through **implementation on SDR and training of the end-to-end system on the actual wireless channel**."
> "**A wireless communication system consisting of two USRP B210 from Ettus Research with carrier-frequency of 2.35 GHz and an effective bandwidth of 15.94 MHz was trained in a static indoor office environment.**"
> "The trained system **outperforms a 256-QAM baseline with an 802.11n LDPC code** in a realistic setup by approximately **1.3 dB** while maintaining the same communication rate."

---

## 5. "Interference Cancellation Based Neural Receiver for Superimposed Pilot in Multi-Layer Transmission"
- **Venue/year**: **China Communications 2025**, DOI `10.23919/jcc.ja.2024-0220`; arXiv 2406.18993 (2024).
- URLs: https://arxiv.org/abs/2406.18993 · https://arxiv.org/html/2406.18993v1

**(i) Output — LLR tensors, fed to the channel decoder:**
> "The model output of **LLR tensor** \(\widehat{\mathbf{V}}_{l,i}\in\mathbb{R}^{S\times T\times M}\) can be further obtained, where \(M\) is the number of bits per symbol according to the modulation order indicated by the configured MCS index \(m\)."
> "After performing feature extraction by NN, the model can proceed a intermediate redundant feature map \(\mathbf{V}_{l,i}\in\mathbb{R}^{S\times T\times M_{\max}}\). By cropping \(\mathbf{V}_{l,i}\) in the third dimension according to \(M\), final output of LLR tensor \(\widehat{\mathbf{V}}_{l,i}\) can be obtained … **After collecting all LLR tensors of \(L\) layers, it can be fed to the following channel decoder.**"

**(ii) Loss — BCE + MSE, on encoded bits and ideal channel:**
> "where \(\mathcal{L}_{\mathrm{bce}}\) and \(\mathcal{L}_{\mathrm{mse}}\) denote the binary crossentropy and mean square error loss function, respectively, \(\tau\) denotes the weights of loss functions. \(\widetilde{\mathbf{B}}\in\{0,1\}^{S\times T\times L\times M}\) and \(\mathbf{H}\) represent the **original encoded bits** and ideal channel…"
> General E2E formulation: "…\(f(\mathbf{Y};\Theta)\) represents the recovered bits or corresponding **log-likelihood ratio (LLR)**… and \(\mathcal{L}_{\mathrm{bce}}\) denotes the binary crossentropy loss function."

**(iv) LDPC + numbers** (simulation only; "LDPC" appears in the simulation-parameter table):
> "The link-level BLER performance comparison in low speed scenario of CDL-C channel is presented in Fig. 4 … **proposed framework outperforms the Baseline II of trainable SIP** … Proposed F-SIP with \(\alpha=0.05\) is comparable with the Baseline I of traditional orthogonal pilot with \(N_p=1\)."
> "Taking SNR = 25 dB as an example, **gains of 19.98% and 24.45% can be obtained in scenarios of 300 km/h and 900 km/h**, respectively" (throughput, vs. orthogonal-pilot baseline).

---

## 6. 2025/2026 neural-receiver papers with LDPC BLER (spot-checked)

- **"Site-Specific Finetuning of Neural Receivers with Real-World 5G NR Measurements"** (arXiv 2603.09644, 2026; https://arxiv.org/html/2603.09644v1) — over-the-air 5G NR PUSCH:
  > "the NRX output log-likelihood ratios (LLRs) are trained with the **binary cross-entropy (BCE) loss against ground-truth bit labels** using the Adam optimizer with \(10^{5}\) training batches."
  > "After data detection, **all receivers utilize a low-density parity-check (LDPC) decoder** and operate on an entire slot spanning all 273 PRBs."
  > "**Site-specific finetuning yields an SNR gain of 1.26 dB with the Shallow NRX (2 iter.) and 1.05 dB with the Deep NRX (8 iter.)** … The finetuned Shallow NRX even outperforms the pretrained Deep NRX at less than \(1/3\) of the inference latency (**0.7 ms vs. 2.2 ms on an NVIDIA GH200**)."
  > "**Finetuning the NRX reduces the measured dataset BLER by more than one half compared to the pretrained counterpart**; these improvements carry over to different deployment scenarios, radio units, and user equipment types."
  > Ground-truth labels come from a HARQ-based extraction method: "we proposed a HARQ-based method to extract ground-truth bit labels for failed transmissions from over-the-air measurements."

- **"Efficient Quantization-Aware Neural Receivers: Beyond Post-Training Quantization"** (arXiv 2509.13786v2; https://arxiv.org/html/2509.13786v2):
  > "**4-bit and 8-bit QAT models achieve BLERs comparable to FP32 models at a 10% target BLER.** Moreover, QAT models succeed in NLoS scenarios where PTQ models fail to reach the 10% BLER target, while also yielding an \(8\times\) compression."
  > "A principled QAT formulation with **learnable clipping and per-channel scales**, demonstrating that 4-bit QAT preserves deployment efficiency while maintaining accuracy."
  > Trained/evaluated over 3GPP TR 38.901 CDL-B/D channels, UE speeds to 40 m/s.

- **"Floating-Point Microformat Quantization and Pruning for Efficient MU-MIMO Neural Receivers"** (arXiv 2609.31177v1; https://arxiv.org/html/2609.31177v1):
  > "Quantization-Aware Training (QAT) and, separately, \(50\%\) magnitude pruning, comparing INT8/INT4 and FP8 (E4M3)/FP4 (E2M1) weights with INT8 post-Rectified Linear Unit (ReLU) activations … evaluated at \(10\%\) and \(1\%\) Block Error Rate (BLER). **At 4 bits, uniform INT4 loses** … **halves this loss (\(1.3\)–\(1.4\) dB)** and still outperforms it by about …" (sentence truncated at the figure boundary in the source I retrieved).

- **"Novel Deep Neural OFDM Receiver Architectures for LLR Estimation"** (arXiv 2503.20500; https://arxiv.org/html/2503.20500v3) — note this same arXiv ID appears under an earlier/later title "Design and Evaluation of Neural Network-Based Receiver Architectures for Reliable Communication" (ar5iv v3, scilit), so the title changed across versions:
  > "we propose two novel neural network–based orthogonal frequency division multiplexing (OFDM) receivers performing channel estimation and equalization tasks and **directly predicting log-likelihood ratios (LLRs) from the received in-phase and quadrature-phase (IQ) signals**."
  > "Finally, **the LLRs are fed to the low-density parity-check (LDPC) decoder**, which decodes data and reconstructs the transmitted bits."
  > "In addition to the received signal samples, noise information in dB is appended as an auxiliary input, enabling the network to leverage the signal-to-noise ratio (SNR) context when producing LLRs."

- **"'Machine LLRning': Learning to Softly Demodulate"** (Shental & Hoydis, **IEEE GC Wkshps 2019**, DOI `10.1109/gcwkshps45667.2019.9024433`, arXiv 1907.01512) — the origin of the LLR-MSE training objective used by LLRNet (see §3.3).

---

## 7. Horizon Europe CENTRIC deliverables

### 7.1 D5.2 "Early version of CENTRIC PoC demonstrator" (zenodo 12731570) — the requested item
- URL: https://zenodo.org/records/12731570 · PDF: https://zenodo.org/records/12731570/files/D5.2%20Early%20version%20of%20CENTRIC%20PoC%20Demonstrator.pdf?download=1
- Metadata (from the PDF and the Zenodo record): "Contractual Delivery Date: 30th June 2024 / Actual Delivery Date: 30th June 2024", "Editor: Carles Navarro Manchón, Keysight Technologies", "Version: 1.0", project "Horizon Europe project no. 101096379", "D5.2 Early version of CENTRIC PoC demonstrator".

**This is the clearest real-time neural-receiver + LDPC-interface statement in the whole survey:**
> "In this context NVIDIA has developed what they coined a 'Neural Receiver' which performs the operations of channel estimation, MIMO equalization and symbol demapping. **Except for LDPC decoding and synchronization, the Neural Receiver performs all the baseband operations needed in a real base-station.**"

> "Inside Claude there are two versions of NVIDIA's Neural Receiver: a. Neural Receiver trained with 3GPP models i.e. CDL channels b. Neural Receiver finetuned with the CIRs coming out of the SionnaRT. **This two Neural Receivers consume the live IQ stream coming out of the O-RU and output the predicted LLR values that are then inputted into a standard-compliant LDPC decoder. The bit outputs of the LDPC decoder are used to check the CRC values of each transport block. A failed CRC check will flag the transport block as an error and use it to compute Block Error Rate (BLER) in a live fashion.**"

> "To assess the performance of the Neural Receiver, the selected KPI is the **BLER after the LDPC decoder**. **PUSCH throughput measured in slots per second (slots/s)** will help measure the speed at which the AI server can process slots compared to real-time processing."

> "The results Figure 21 and Figure 22 show the BLER vs SNR of a 3GPP-compliant standard receiver and the two different versions of the Neural Receiver in setups with 3 and 4 antennas, respectively."
> "**the Neural Receiver performs exceptionally better than the baseline receiver even when consuming IQ samples from a real-world O-RU.**"
> "the site-specific version of the Neural Receiver shows better performance than the version trained with 3GPP stochastic models. The finetune effort was miniscule compared to a normal training process."

Hardware chain (verbatim): Keysight VXG generates two 5G waveforms → Keysight PROPSIM F8800A real-time RF channel emulator (CIRs from Sionna RT, "Fira de Barcelona" trajectories) → commercial O-RU → S5040B → "Claude" AI server running the NRX → standard-compliant LDPC decoder. Disseminated at MWC 2024 (3 Rx) and EuCNC 2024 (4 Rx).
**No loss-function, no LLR bit-width, no saturation/clipping value is stated in D5.2.**

### 7.2 D5.3 "KPIs/KVIs testing methodologies and benchmarking" — quantified KPI targets & achieved values
- URL: https://centric-sns.eu/wp-content/uploads/2025/03/d5.3-kpiskvis-testing-methodologies-and-benchmarking.pdf

> "3.2.8 Multi-user MIMO Neural Receiver … the multi-user MIMO neural receiver developed by NVIDIA in the context of WP3. This is one of the enablers in which the validation work has achieved a higher level of maturity."

Table 13 (Validation Summary for Multi-user MIMO Neural Receiver), verbatim cell values:
> "KPI#1: BLER for fixed computational complexity — Target: **BLER close to B#2 with lower computational complexity.** — Achieved Result: **<1 dB loss with respect to B#2**"
> "KPI#2: Inference latency — Target: **<1ms inference latency on NVIDIA A100 GPU** — Achieved Result: **1ms latency processing 132 PRBs**"

Baselines, verbatim:
> "**B#1: the first baseline consists of a 5G PUSCH MU-MIMO receiver with LS-based channel estimation and LMMSE MIMO detector**, as available in open-source implementation in Sionna [15]. This can be considered as a state-of-art commercial receiver for 5G."
> "**B#2: the second baseline is a 5G PUSCH MU-MIMO receiver with LMMSE-based channel estimation and K-Best MIMO detector**, with implementation also available in Sionna [15]. This receiver can be considered very close to optimal, but has an infeasible computational complexity for current base stations."

(D5.3 also covers a separate WP2 enabler "Nullhop: Neural Receiver Acceleration" (Synthara) that accelerates the Sionna neural receiver: target 1 ms inference, **achieved 70 ms**; target 30% complexity reduction, **achieved 60% sparsity increase** — i.e. far from target at time of writing.)

### 7.3 D5.4 "Final CENTRIC PoC demonstrator"
- URL: https://zenodo.org/records/15356248 · https://centric-sns.eu/wp-content/uploads/2025/05/d5.4-final-centric-poc-demonstrator-1.pdf
- §3.2 repeats the D5.2 PoC2 description verbatim, including:
> "**This two Neural Receivers consume the live IQ stream coming out of the O-RU and output the predicted LLR values that are then inputted into a standard-compliant LDPC decoder.**"
> "To assess the performance of the Neural Receiver, the selected KPI is the BLER after the LDPC decoder."

### 7.4 D3.6 "Report final" — training statement + pilotless NRX
- URL: https://centric-sns.eu/wp-content/uploads/2025/07/d3.6_report_final.pdf
> "**The proposed neural receiver architecture is fully differentiable and, hence, SGD-based training is straightforward**"

### 7.5 D3.5 (final WP3 report) — pilotless NRX with trainable constellation
- URL: https://centric-sns.eu/wp-content/uploads/2025/07/d3.5_final.pdf
> "The key innovation is the joint training of a neural receiver and a trainable transmitter using …" / "receiver (NRX) with a trainable custom constellation"
> "ℓ𝑛𝐹,𝑛𝑆,𝑖(𝒀) denotes the LLR computed by the detector from the received signal 𝒀 for the 𝑖th bit of layer 𝑛T transmitted over the RE [𝑛F, 𝑛S]."
> "the NRX slightly outperforms the LMMSE+K-best baseline while benefiting …" and "In terms of block error rate (BLER), the pilotless system matched or outperformed the baseline"

---

## Cross-cutting summary table

| Work | NN output | LLR source | Loss | LLR clipping/quantization stated | LDPC interface | Real-time/HW |
|---|---|---|---|---|---|---|
| Sionna lib (2203.11854) | `llr` per coded bit | `NeuralDemapper` layer directly | `BinaryCrossentropy(from_logits=True)` on LDPC-encoded `u` | not stated | `LDPC5GDecoder(LDPC5GEncoder(k,n))(llr)` | GPU/differentiable; simulation |
| Sionna RK neural demapper tutorial | LLR per bit | 3-layer MLP directly | **BCEWithLogitsLoss** on random (uncoded) bits | float16↔int16 with `ldexp(·,±8)`; measured 2.42× scale mismatch vs OAI | min-sum decoder, "robust against mis-scaling" | TensorRT export; OAI testbed |
| Sionna RK PUSCH NRX tutorial | LLRs | NN replaces CE+EQ+demap | (see neural_rx: BCE+MSE) | **output LLRs → int16**, inputs → float16, CUDA kernels | OAI LDPC chain | TensorRT, OAI, 24-PRB fixed tiles |
| Sionna `LDPC5GDecoder` | — | — | differentiable ("weighted BP") | **`llr_max=20.0` internal clipping; float input** | sum-product default CN `boxplus-phi`, `vn_update='sum'`, `num_iter=20` | GPU |
| Wiesmayr et al. 2409.02912 | LLRs (`ReadoutLLRs`) | CGNN readout directly | **BCE** vs ground-truth coded-bit labels (+ weighted MSE for ChEst) | **weights float16, no QAT**; mismatched-demapper LLR-magnitude hazard discussed | NRX → channel decoder; code rate transparent | **TensorRT on A100, 1 ms budget, 350 µs/iter + 270 µs overhead, 132 PRBs, 2 UEs** |
| Cammerer et al. 2312.02601 | LLRs (last stage) | conv+GNN directly | **BCE**, Adam 1e-3 | not stated | decoder-in-the-loop trainable BP **tried; no gains** | hardware-in-the-loop, 3GPP conformance |
| neural_rx (NVlabs) | LLRs (`ReadoutLLRs`) | MLP readout | **BCE on LLRs** (+ MSE on h_hat, weight 0.01–0.02) | not in repo (int16 only at OAI boundary) | external LDPC chain | TensorRT, real-time inference |
| pyAerial NRX notebook | LLRs "directly" | NN replaces CE/noise-est/EQ | (variant of 2312.02601) | not stated | **pyAerial LDPC derate-match + decode + CRC** | TensorRT; BLER table given |
| LLRNet (Aerial) | soft bits = LLRs | MLP replacing soft demapper | **MSE between predicted and target LLRs** (not BCE) | not stated | in-chain LDPC | Aerial cuPHY |
| Aït Aoudia & Hoydis, Globecom 2020 | LLRs | differentiable demapper | **total BCE ≈ BMI** | not stated | "decoding algorithm" (abstract) | simulation |
| Model-free training, JSAC 2019 | messages (no LLR found) | receiver NN | **categorical CE** | — | — | **USRP SDR prototype** |
| Hardware impl., SPAWC 2019 | — | — | generic | **fixed-point weights/arithmetic for FPGA/ASIC** | — | FPGA/ASIC target |
| OFDM pilotless, TWC 2021 | — | baseline Gaussian demapper LLRs | **total BCE** | not stated | BP decoding after deinterleaving | simulation |
| Trainable Comm. Systems, TCOM 2020 | **LLRs** | learned demapper | **bit-wise binary CE**; encoder/decoder not needed at training | — | BP (40 iters) / IDD; min-sum OK if differentiable | **2× USRP B210 @2.35 GHz**, +1.3 dB over 256-QAM + 802.11n LDPC |
| SIP interference cancellation, China Commun. 2025 | LLR tensors | NN directly | **BCE + weighted MSE** | not stated | LLR tensors → channel decoder | simulation (19.98%/24.45% throughput gain @25 dB) |
| CENTRIC D5.2/D5.4 PoC2 | **predicted LLR values** | NRX (CE+EQ+demap) | not stated | **not stated** | **"standard-compliant LDPC decoder"; BLER after LDPC decoder; slots/s throughput KPI** | **live O-RU + PROPSIM + AI server** |
| CENTRIC D5.3 | — | — | — | — | <1 dB loss vs LMMSE+K-Best; **1 ms for 132 PRBs on A100** | HW-in-the-loop |
| 2603.09644 (2026) | LLRs | NRX | **BCE vs ground-truth bit labels** | — | **LDPC decoder on full 273-PRB slot**; BLER halved; 0.7 ms vs 2.2 ms on GH200 | over-the-air real 5G |
| 2509.13786 (QAT) | — | — | QAT | **learnable clipping, per-channel scales, 4/8-bit** | 10% BLER target met at 4-bit | edge/6G |

---

## "Could not verify" list

1. **A peer-reviewed venue for Sionna (arXiv 2203.11854)** — OpenAlex lists it only as a 2022 arXiv preprint; no journal/conference DOI found.
2. **An LLR bit-width, saturation value, or clipping rule at the *neural receiver → LDPC decoder* interface in Wiesmayr et al. (2409.02912).** The paper states only that *weights* are quantized to float16 (no QAT); no int8/int16 LLR saturation limit is given. The nearest concrete numbers are the Sionna-RK/OAI integration (float16 in, **int16** LLRs out, `ldexp(·, 8)` scaling) and Sionna's `LDPC5GDecoder(llr_max=20.0)`.
3. **The LDPC decoding algorithm (min-sum vs sum-product) actually used inside the 2409.02912 / 2312.02601 / neural_rx results.** I verified min-sum only for (a) the Sionna neural-demapper tutorial's OAI integration and (b) the `LDPC5GDecoder` option list. The neural-receiver papers do not state which decoder variant produced their BLER curves.
4. **Whether real LDPC-encoded bits are the BCE labels in the Sionna *Neural Demapper tutorial*.** There they are **uncoded** random bits (`binary_source`). LDPC-encoded labels are confirmed only for the Sionna library paper listing, 2409.02912, 2406.18993 and 2603.09644.
5. **Any loss/LLR statement in "Model-Free Training of End-to-End Communication Systems" (1812.05929) and "Towards Hardware Implementation of Neural Network-based Communication Algorithms" (1902.06939).** I grepped the full text of both: no "LLR"/"log-likelihood ratio" occurrence was found, and 1902.06939 gives only a generic loss \(L(\psi)\).
6. **The title "End-to-End Learning for OFDM: From Neural Receivers to Hardware Feasibility" does not exist** in OpenAlex or arXiv. The real paper is "…From Neural Receivers to Pilotless Communication" (IEEE TWC, 2021).
7. **"Hardware feasibility" content for the OFDM paper** — I found no FPGA/ASIC prototype in it; the paper is simulation/analytical.
8. **CENTRIC D5.2/D5.4/D5.3/D3.6 contain no LLR quantization, clipping or saturation description for the PoC neural receiver** despite being the most hardware-real item. The KPI is BLER after the LDPC decoder and slots/s throughput, but no numeric slots/s value is quoted in the text I extracted.
9. **The CENTRIC D2.1 "Evaluation of GPU Implementation of ML-based Receiver" (zenodo 18745043) and D5.5 "CENTRIC Dataset #2: AI-Based Receiver Processing" (zenodo 15856694)** — I located the records but did not retrieve/verify their contents.
10. **Quantitative BLER-vs-SNR values from 2312.02601** — the numbers live in figures that are not present in the arXiv HTML text I retrieved; only the qualitative claim ("operates close to the baseline consisting of LMMSE channel estimation with K-best detection") is verified.
11. **EqDeepRx (arXiv 2602.11834)**, **"When Does a Neural Receiver Help?" (2605.26157)**, **"Data-Free Quantization of Neural Receivers: When 4-Bit Succeeds, Why 6-Bit Matters for 6G" (NeurIPS 2025, OpenReview 4NsiKauceB)**, **"A Universal Neural Receiver that Learns at the Speed of Wireless" (2602.15458)**, **"On the Impact of Site-Specific Training for a Real-World 5G NR System" (2609.04004)** — seen only as web-search result titles/passages, not read in full; treat any claim from them as unconfirmed.
12. **The exact 1.3–1.4 dB FP4-vs-INT4 comparison sentence** (2609.31177) — the retrieved passage is truncated at a figure reference ("At 4 bits, uniform INT4 loses … halves this loss (1.3–1.4 dB)"); the full sentence was not recovered.
