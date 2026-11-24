#!/usr/bin/env python3
"""Train the depth-3 network: the contract's plane in, LLRs and the reporting heads out.

What this trains, per `model_network_design.md` section 0.2 and `design` section 3.2:

* the main head is trained with per-bit BCE against the SCRAMBLED coded bits, whose labels are
  `1 - b` because the chain reads a positive LLR as bit 0. The logit IS the LLR, so nothing is
  converted at inference;
* the channel-estimate head is trained with NMSE against the channel divided by the reference
  amplitude, on the allocated resource elements only: everywhere else the contract does not
  constrain it;
* the post-equalization noise head is trained in `log10` against the chain's own zero-forcing
  form, and its targets outside the representable window are excluded rather than clipped
  (`depth3_loss.noise_shape_target`).

The inputs are built by the same functions the dataset builder and the seam use (`depth3`), so
the trainer cannot drift from the contract by having its own idea of the layout.

The revision-version note: the labels are the SCRAMBLED bits and therefore in the scrambling
domain, which is where the contract's LLRs live. Descrambling happens after this unit, as
`memo_01` section 3 records.

Usage
-----
    train_depth3.py --train <npz> --val <npz> --out <dir> [--epochs 150] [--max-train N]
                    [--blocks 18] [--modulation 256QAM] [--no-aux]

Platforms
---------
The same command line runs on Ubuntu (CUDA) and on macOS (Apple Silicon), and the training arithmetic is
the same on both: the loss, the gradients and the LLR scaling are float32, `--dtype` means what it always
meant, `ChannelGate` keeps its fp32 product, and every custom layer serialises exactly as before. Only
three decisions are taken per platform, and none of them can change a number the model computes:

* `--jit` still defaults to on. On Linux the resolved value is the requested one, because XLA there is the
  measured 1.3x-2x of the epoch time. On macOS XLA is off unless `--jit` is written out: `tensorflow-metal`
  registers no XLA device, so a compiled step is either rejected or run on the CPU with copies around it.
  An explicit `--jit` is honoured, self-tested before the first batch, and degraded with a printed reason
  if the compiled graph still cannot run.
* `--workers 0` keeps the serial batch loop on Linux, which is what was measured there. On macOS it selects
  a small thread count (a quarter of the cores, capped at three) instead, because the per-sample work is
  NumPy and releases the GIL while the accelerator waits; more threads than that measured SLOWER, since only
  one can hold the GIL. An explicit `--workers N` is respected on both platforms.
* the Apple Neural Engine does NOT train anything. It has no backward-pass kernels and this loop never
  schedules onto it: the step runs on the Apple GPU (through `tensorflow-metal`) and on the CPU. The ANE is
  the INFERENCE target of `export_deployment()` / `export_buckets()`, whose fp16 `mlprogram` CoreML
  schedules on it -- the folded gains, the avoided extra operators and the static-width buckets exist for
  that graph, not for training.
* the Apple GPU is used only when `tensorflow-metal` is installed AND `--no-gpu` is not given, and on this
  project's measurements it is usually the WRONG choice for the deployment network. On an M4 Pro with
  tensorflow-metal 1.2.0 and TensorFlow 2.18.1: 4.93 s/epoch on the Metal GPU against 2.63 s/epoch on the
  CPU at c_hid 32 and batch 8, closing to 1.16x at batch 128, and only overtaking the CPU at c_hid 128 with
  batch 32 (1.37x). The reason is arithmetic intensity: eighteen 3x3 depthwise blocks over 612x14 elements
  are a long chain of small kernels, and Metal's per-kernel command overhead dominates. The startup banner
  therefore prints the PLACEMENT, so every run says which device it actually got. `--dtype float16` is
  refused on macOS: `tensorflow-metal` segfaults in its half-precision binary op (upstream bug, the
  unmodified script crashes the same way), and training in float32 costs nothing because the exported
  bundle is fp16 regardless.
"""

# The shared DSP utilities (dmrs, synth, labels, LDPC) still live in the AI-CE training tree; this is
# the single place that knows where, so the import below is not a hidden path dependency.
import depth3_paths
depth3_paths.add_ce_shared()
import argparse
import collections
import json
import math
import operator
import os
import platform
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import tensorflow as tf

import dd_label as DL              # noqa: E402
import depth3 as D3
import fast_label as FL                # noqa: E402
import pusch_uci as UCI            # noqa: E402
import depth3_loss as L            # noqa: E402
import ldpc_decode as LD           # noqa: E402
import synth as SN                 # noqa: E402
from coded_bler import CodedBler # noqa: E402  (decoder side, no TensorFlow)
# The canonical LayerScale for this workstream lives next to the refiner that needed it first.
from train_refiner import Scale    # noqa: E402

NOF_SYMBOLS = D3.NOF_SYMBOLS
# THE DECODER THRESHOLDS, MEASURED RATHER THAN ASSUMED (2026-11-07). The 50% block-error crossing of the classical chain on
# the NOMINAL signal-to-noise ratio, computed over every AMC leg of the s2 corpus: 1,114,092 unique receptions joined from the
# mother sets to their dump indices by capture id, thirty-five legs, with the per-leg spread on 256QAM running 17.8 to 23.5 dB,
# which is why one leg is not enough. Adding the twenty-five fixed-MCS legs (776,514 more receptions, all QPSK) moves QPSK by
# 0.06 dB and the other three by nothing. The values are on `snr_nominal` -- the same variable the datasets carry -- and the
# measured-SINR variant of the same table is 9 dB away on QPSK, so mixing the two scales would be a silent error.
# Recompute with `work_tmp/measurement/measure_thresholds.py` when more captures land.
THR_QM = {"QPSK": -3.16, "16QAM": 6.61, "64QAM": 13.38, "256QAM": 20.91}

DILATIONS = (1, 2, 4, 8)

# THE ONE PLATFORM FLAG. Every decision that may differ between the Ubuntu/CUDA reference box and an Apple
# Silicon machine reads this, and every one of those decisions is outside the numbers the model computes.
IS_DARWIN = sys.platform == "darwin"

# The reverse of `D3.BITS_PER_SYMBOL`, built once. `Batches.modulation_of` used to rebuild it on every call,
# and it is called once per sample by the loader and once per row by the coded-BLER path.
_MODULATION_OF_QM = {bits: name for name, bits in D3.BITS_PER_SYMBOL.items()}

# HOW MANY PER-BATCH LOSSES ARE ACCUMULATED BEFORE ONE DEVICE-TO-HOST TRANSFER (see the epoch loop).
# ONE FLUSH PER SIXTY-FOUR BATCHES, RATHER THAN ONE PER BATCH AND RATHER THAN ONE PER EPOCH, because the two
# extremes both cost something real: `float(total)` per batch is a device synchronisation that stops the host
# from running ahead into the next batch (the original form), and one transfer per epoch either keeps one
# device tensor per batch alive or -- as a `tf.Variable` slice write, which measured 60 us a batch against
# 0.3 us for `float()` on an idle CPU -- pays an eager assignment per batch. At 64 the host still pipelines
# 64 steps ahead, at most 64 scalar tensors are alive, and the read-back is one `tf.stack` plus one copy.
# Measured on the CPU backend: 1.1 us a batch, against 0.3 for the per-batch `float()` and 60 for the
# variable; on the GPU backends this replaces a full synchronisation per batch, which is the point.
LOSS_ACCUMULATION = 64


def parse_dilations(text):
    """`--dilations "1,2,4,8,16,32"` as a tuple, or the default cycle when the flag is empty.

    THE MAXIMUM DILATION WAS NEVER JUSTIFIED AGAINST A MEASUREMENT. The chain stops at 8 because that
    was the cycle the first draft picked, which reaches a 127-subcarrier receptive field; the corpus
    later measured the channel's coherence length at 150-300 subcarriers (`model_network_design` 30.3,
    |rho| = 0.97 at lag 20), so the network cannot see one full coherence interval. Dilation costs
    neither parameters nor multiply-adds (`model_network_design` 27.3: 372 -> 369 us for RF 37 -> 127),
    which is why extending it is the cheapest lever left -- but the value has to be measurable, so it
    is a knob now and the receptive field is printed with every run.
    """
    if not text.strip():
        return DILATIONS
    values = tuple(int(part) for part in text.split(",") if part.strip())
    if not values or min(values) < 1:
        raise SystemExit(f"--dilations {text}: every entry must be a positive integer")
    return values


def receptive_field(blocks, dilations, ksize=3):
    """Subcarriers one output position can see: 1 + sum((k-1)*d) over the block cycle.

    The kernel side matters: a k=5 mixing layer reaches two subcarriers further each side per unit of dilation,
    so the same dilation cycle buys twice the field. The report would otherwise understate a k=5 run.
    """
    return 1 + (ksize - 1) * sum(dilations[i % len(dilations)] for i in range(blocks))


def _flag_was_given(argv, *flags):
    """Whether one of `flags` was written on the command line, in the `--flag` or the `--flag=v` form.

    argparse cannot answer this: `--jit` is a `BooleanOptionalAction` whose default is `True`, so the parsed
    value is `True` whether the user asked for XLA or said nothing at all. macOS has to tell those apart --
    silence means "take the safe default here", an explicit `--jit` means "I want to measure it" -- and the
    presence of the token in argv is the only place that distinction still exists.
    """
    for token in argv:
        for flag in flags:
            if token == flag or token.startswith(flag + "="):
                return True
    return False


def resolve_xla(requested, argv):
    """The `jit_compile` state this run will use, plus the reason when it differs from the request.

    LINUX IS UNCHANGED: the request is the answer. XLA there is the measured 1.3x-2x of the epoch time and
    the CUDA build's XLA produced those numbers, so this function must not be able to touch it.
    MACOS RESOLVES INSTEAD. `tensorflow-metal` registers no XLA device, so `jit_compile=True` compiles the
    step for whatever backend XLA can find (the CPU) and then copies every tensor across for the Metal ops;
    a graph this wide can also fail to compile outright. The project has already measured what XLA-on-a-CPU
    costs: 1.43-1.48x on the CUDA box and 6.2x SLOWER on the laptop CPU (README section 3, design section
    19.219). That was measured again on an M4 Pro with TF 2.18.1: compiling the c32 step with XLA and running
    it on the CPU took 89.5 ms a sample against 12.1 ms without, 7.4x slower, so leaving it off is not merely
    the safe choice there -- it is the fast one. XLA is therefore off unless `--jit` is written out, and an
    explicit request is honoured -- `xla_self_test` then proves the backend can compile at all before the
    first batch is paid for.
    """
    if not IS_DARWIN:
        return bool(requested), None
    if _flag_was_given(argv, "--jit", "--no-jit"):
        return bool(requested), None
    return False, ("off by default on macOS: tensorflow-metal exposes no XLA device, so a compiled step "
                   "runs on the CPU with copies around it; pass --jit to force it")


def xla_self_test():
    """Compile one trivial function with XLA, and report whether this backend can do it at all.

    Called only when XLA was asked for ON MACOS. Without it, a run whose step cannot be compiled fails at
    the first batch, after the dataset is mapped and the model built; the same failure is available here in
    a few milliseconds, where it degrades the run instead of ending it.
    """
    try:
        @tf.function(jit_compile=True)
        def probe(value):
            return tf.reduce_sum(value * value)
        probe(tf.ones((2, 2), dtype=tf.float32)).numpy()
        return True, ""
    except Exception as err:  # noqa: BLE001 - a compile failure must degrade, never kill the run
        return False, f"{type(err).__name__}: {err}"


def configure_macos_runtime():
    """The backend settings an Apple Silicon run wants, and a description of what was found.

    Called on macOS only, before anything is built or run. Nothing here changes a number: it asks the Metal
    plugin for growth-based allocation (so a long run does not claim the whole unified-memory pool up front)
    and records which devices accepted it. Both calls are guarded -- a trainer that refuses to start because
    a memory hint is unsupported on this plugin version would be worse than one that trains.
    """
    facts = {"machine": platform.machine(), "tensorflow": tf.__version__, "numpy": np.__version__,
             "gpus": [], "growth": []}
    try:
        gpus = tf.config.list_physical_devices("GPU")
    except Exception:  # noqa: BLE001 - a device query must never decide whether the run happens
        gpus = []
    facts["gpus"] = [device.name for device in gpus]
    for device in gpus:
        try:
            tf.config.experimental.set_memory_growth(device, True)
            facts["growth"].append(device.name)
        except Exception:  # noqa: BLE001 - not every plugin implements it, and that is not an error
            pass
    return facts


def pipeline_defaults(args):
    """The `(workers, prefetch)` the batch loader will run with.

    LINUX IS THE COMMAND LINE, UNCHANGED: `--workers 0` keeps the serial loop and `--prefetch 0` means twice
    the worker count, exactly as the call sites used to spell it.
    MACOS SUBSTITUTES A DEFAULT FOR `--workers 0`, AND IT IS A SMALL ONE. The per-sample work is NumPy and
    releases the GIL, so a second thread genuinely overlaps the first one's C time with its Python time -- but
    only one thread can hold the GIL, and past the point where that is the limiter every extra thread adds
    context switches and competes for the same unified-memory bandwidth. Measured on a 14-core Apple Silicon
    host, 200 samples, `Batches.epoch` at batch 8: 0.650 ms/sample serially, 0.430 at two threads, 0.412 at
    three, 0.419 at four and 0.552 at six (51-PRB samples), with the same shape on a mixed-allocation set
    (0.401 / 0.308 / 0.360 / 0.408 / 0.516). Six threads is therefore WORSE than no threads, which is the trap
    this default exists to avoid: a quarter of the cores, capped at three. An explicit `--workers N` is
    respected exactly as on Linux -- including `--workers 1`, which is how a serial loader is asked for on
    macOS, since 0 there selects this default.
    """
    workers = int(args.workers)
    if IS_DARWIN and workers == 0:
        workers = max(1, min(3, (os.cpu_count() or 1) // 4))
    prefetch = int(args.prefetch) or (2 * workers)
    return workers, prefetch


# True when `--no-gpu` successfully hid the GPUs; the banner says so rather than pretending the device
# list is empty, because the Metal plugin keeps listing its GPU after it has been made invisible.
_GPU_HIDDEN = False


def hide_gpus():
    """Make every GPU device invisible to this process, before the runtime is initialised.

    THE GPU IS NOT AUTOMATICALLY THE FASTER DEVICE FOR THIS NETWORK. Measured on an M4 Pro with
    tensorflow-metal 1.2.0 and TensorFlow 2.18.1 at c_hid 32, batch 8: 4.93 s/epoch on the Metal GPU
    against 2.63 s/epoch on the CPU; the GPU only overtakes at c_hid 128 with batch 32. Without this
    switch the only way to get the faster path on a machine that has the plugin installed is to uninstall
    it. `set_visible_devices` only works before the first op, which is why `main` calls this immediately
    after parsing, ahead of the memory-growth request and the XLA self-test.
    """
    global _GPU_HIDDEN
    try:
        tf.config.set_visible_devices([], "GPU")
        _GPU_HIDDEN = True
        return True
    except Exception as err:  # noqa: BLE001 - a failed switch must not end the run
        print(f"gpus: --no-gpu could not hide the GPU ({type(err).__name__}: {err}); TensorFlow will "
              f"place ops wherever it decides", flush=True)
        return False


def allow_metal_fp16():
    """Whether the user has explicitly accepted the macOS fp16 crash path (see the guard in `main`)."""
    return os.environ.get("DEPTH3_ALLOW_METAL_FP16") == "1"


def probe_compute_device():
    """The device a matrix multiply is actually placed on, asked of TensorFlow instead of assumed.

    THIS SCRIPT CANNOT TURN THE GPU ON. `tensorflow-metal` is a separate pluggable device: a plain `tensorflow`
    wheel on macOS has no GPU device at all, and every op then runs on the CPU, several times slower per epoch,
    without a single line in the log saying so. That is how a CPU-only "macOS run" gets recorded as if it were a
    GPU measurement. `Tensor.device` reports the placement TensorFlow chose for a 64x64 matmul -- `GPU:0` when
    the Metal plugin is loaded, `CPU:0` when it is not -- and that placement is what the training step gets too:
    the step is made of the same convolutions and matrix multiplies.
    """
    try:
        product = tf.linalg.matmul(tf.ones((64, 64)), tf.ones((64, 64)))
        return str(product.device).rsplit("device:", 1)[-1]
    except Exception as err:  # noqa: BLE001 - a diagnostic must never end a run
        return f"unknown ({type(err).__name__})"


def report_macos_runtime(facts, workers, prefetch, jit):
    """Say what this process is running on, in one block, ON MACOS ONLY.

    The Ubuntu log lines are the record of recorded runs and gain nothing from a platform banner, so the
    banner exists only where the platform is the new variable. These lines answer "why is this run different"
    before the model is built, and they state the one thing about the Apple Neural Engine that has to be
    unambiguous: it does not run a backward pass, so nothing in this loop schedules onto it.
    """
    if not IS_DARWIN:
        return
    gpu = facts["gpus"][0] if facts["gpus"] else "none (CPU only)"
    if _GPU_HIDDEN:
        # The plugin still lists the device, so the list alone would claim a GPU this run cannot use.
        gpu = f"{gpu} (hidden by --no-gpu)"
    growth = ", memory growth on" if facts["growth"] else ""
    # THE PLACEMENT, NOT THE INTENTION. Without `tensorflow-metal` there is no GPU device to place anything on,
    # and a GPU device with a matmul still placed on the CPU is a plugin problem -- both are said here rather
    # than left to be inferred from a slow epoch (see `probe_compute_device`).
    placement = probe_compute_device()
    if _GPU_HIDDEN:
        carries = "hidden by --no-gpu, so the CPU carries the step"
    elif placement.startswith("GPU"):
        carries = "the Apple GPU carries the step"
    else:
        carries = "the CPU carries the step -- install tensorflow-metal to use the Apple GPU"
    print(f"platform: macOS ({facts['machine']}), tensorflow {facts['tensorflow']}, numpy {facts['numpy']}")
    print(f"platform: Apple GPU: {gpu}{growth}")
    print(f"platform: compute placement: a 64x64 matmul lands on {placement} ({carries})")
    print(f"platform: training runs on the {'Apple GPU and the CPU' if placement.startswith('GPU') else 'CPU'}, "
          f"xla {'on' if jit else 'off'}; the Apple Neural Engine does NOT train -- it has no "
          f"backward-pass kernels")
    print("platform: the ANE serves INFERENCE only: CoreML schedules the mlprogram written by "
          "export_deployment()/--export-buckets")
    print(f"platform: batch pipeline {workers} thread(s), prefetch {prefetch} batch(es); the per-sample "
          f"NumPy work releases the GIL")



@tf.keras.utils.register_keras_serializable(package="ocudu")
class ChannelGate(tf.keras.layers.Layer):
    """Scale a feature map by a per-channel gate.

    WHY A LAYER AND NOT A MULTIPLY. A broadcast `Multiply` of the fp16 map by the fp16 gate converts to a coreml
    `mul` whose gate operand comes out as float32, and the conversion fails with "the named input y must have the
    same data type as the named input x". Casting here keeps both operands in the map's dtype all the way through.
    """

    def call(self, inputs):
        # THE PRODUCT IS TAKEN IN FP32 AND CAST BACK. Matching the gate to the map's dtype is not enough: the
        # converter lowers the sigmoid path to fp32 on its own, so whichever operand is cast, the `mul` still ends up
        # with one fp32 and one fp16 input and conversion refuses it. Promoting both operands makes the multiplication
        # unambiguous, and the single cast back to the map's dtype is exact for a value in [0, 1].
        #
        # THIS IS ALSO WHAT MAKES A MIXED-PRECISION RUN SAFE, ON EITHER PLATFORM. `--dtype float16` under
        # `--se` keeps the gate's arithmetic in fp32 and the map's dtype at the output, so the macOS work
        # (which does not touch this call) cannot make the exported graph's `mul` operands disagree, and the
        # gate's gradient cannot be taken in a half precision that underflows.
        x, gate = inputs
        scaled = tf.cast(x, tf.float32) * tf.cast(gate, tf.float32)
        return tf.cast(scaled, x.dtype)

    def get_config(self):
        # Keras refuses to serialize a Layer subclass that has no get_config, and a .keras file that cannot be
        # written is a run whose checkpoints cannot be averaged or exported.
        return super().get_config()


class MaskedSmooth(tf.keras.layers.Layer):
    """A dense local average over one coherence bandwidth, taken only where the allocation actually is.

    WHY THIS AND NOT A BIGGER RECEPTIVE FIELD. The trunk sees 127 subcarriers, but through dilations: its long-range
    taps are sparse, so it cannot compute the dense average that an MMSE estimator performs inside one coherence
    bandwidth (measured 150-300 subcarriers, improvement plan section 8). This layer adds exactly that operation, and
    it is masked: averaging a fixed window at the allocation edge pulls in zeros and would attenuate the estimate,
    which is the artefact that made a naive smoothing test look harmful (section 106). Dividing the masked sum by the
    masked window's own coverage removes it.

    The kernel starts as a boxcar of 1/k -- the physical prior, not noise -- and is then learned. It is one shared
    vector over all channels, so the layer adds k parameters and nothing else.
    """

    def __init__(self, taps, dtype=None, **kwargs):
        super().__init__(dtype=dtype, **kwargs)
        self.taps = int(taps)

    def get_config(self):
        # Without this the first snapshot save fails: Keras 3 refuses to serialise a custom layer that cannot
        # describe its constructor, and the training dies at the end of epoch 1 having produced no checkpoint.
        config = super().get_config()
        config.update({"taps": self.taps})
        return config

    def build(self, input_shape):
        self.kernel = self.add_weight(name="k", shape=(self.taps, 1, 1),
                                      initializer=tf.keras.initializers.Constant(1.0 / self.taps),
                                      trainable=True)

    def call(self, x, **kwargs):
        # The allocation mask is a channel of the spatial plane itself, so the layer reads it from its own input: a
        # Lambda slice would make every checkpoint unserialisable (Keras refuses to deserialise Python lambdas), and
        # the whole measurement pipeline loads these files.
        mask = x[..., D3.SP_ALLOCATION:D3.SP_ALLOCATION + 1]
        channels = tf.shape(x)[-1]
        depth_kernel = tf.tile(self.kernel[:, :, None, :], [1, 1, channels, 1])
        masked = x * mask
        num = tf.nn.depthwise_conv2d(masked, depth_kernel, strides=[1, 1, 1, 1], padding="SAME")
        den = tf.nn.conv2d(mask, self.kernel[:, :, :, None], strides=[1, 1, 1, 1], padding="SAME")
        return num / (den + 1e-6), den


@tf.keras.utils.register_keras_serializable(package="ocudu")
class FoldedScaleConv(tf.keras.layers.Layer):
    """A 1x1 convolution whose output channels carry a trainable positive gain.

    `gamma * relu(W x + b)` equals `relu((gamma W) x + gamma b)` while `gamma` stays positive, so
    the LayerScale the residual blocks need costs no operator of its own: the gain folds into the
    convolution's weights. A separate multiply doubles the operator count on the ANE, which
    section 45.8 measured as about 200 us across 18 blocks.

    The gain is parameterised through softplus, which keeps it strictly positive without a
    constraint, and the bias is scaled with it because the scale sits after the affine map.
    """

    def __init__(self, filters, initial_gain=0.1, rank=0, **kwargs):
        super().__init__(**kwargs)
        self.filters = int(filters)
        self.initial_gain = float(initial_gain)
        # LOW-RANK POINTWISE (design section 48, lever D). A `channels x filters` 1x1 costs one
        # multiply-add per output channel per input channel per RE; splitting it into
        # `channels x rank` then `rank x filters` costs 2*rank instead. At c=32 and rank=8 that is
        # 512 of the 1024 multiply-adds removed per block, and the depthwise 3x3's 288 are untouched,
        # so the trunk's per-RE work falls from 1312 to 800 -- 1.64x, not the 2x the raw ratio suggests.
        self.rank = int(rank)

    def get_config(self):
        return {**super().get_config(), "filters": self.filters,
                "initial_gain": self.initial_gain, "rank": self.rank}

    def build(self, input_shape):
        channels = int(input_shape[-1])
        inner = self.rank if self.rank > 0 else channels
        if self.rank > 0:
            # No bias on the factor's first half: the second half carries the affine map, and a bias
            # in between would be absorbed by it anyway (it would only cost an operator on the ANE).
            self.kernel_in = self.add_weight(name="kernel_in", shape=(1, 1, channels, inner),
                                             initializer="glorot_uniform", trainable=True)
        self.kernel = self.add_weight(name="kernel", shape=(1, 1, inner, self.filters),
                                      initializer="glorot_uniform", trainable=True)
        self.bias = self.add_weight(name="bias", shape=(self.filters,), initializer="zeros",
                                    trainable=True)
        # softplus(x) = initial_gain, so training starts where a plain LayerScale would.
        inverse = float(np.log(np.expm1(self.initial_gain)))
        self.gain = self.add_weight(name="gain", shape=(self.filters,),
                                    initializer=tf.keras.initializers.Constant(inverse),
                                    trainable=True)

    def call(self, inputs):
        gamma = tf.nn.softplus(self.gain)
        kernel = self.kernel * gamma[None, None, None, :]
        if self.rank > 0:
            inputs = tf.nn.conv2d(inputs, self.kernel_in, strides=1, padding="SAME")
        return tf.nn.conv2d(inputs, kernel, strides=1, padding="SAME") + gamma * self.bias


# THE CUSTOM OBJECTS EVERY DESERIALISATION SITE MUST PASS. There are three of them -- the resume path, the
# `--export-only` path and every probe script that loads these checkpoints -- and they did not agree: resume
# passed all three classes, `--export-only` passed two and then failed to load any model trained with
# `--smooth-k` ("Could not locate class 'MaskedSmooth'") after training had already been paid for. One dict, so
# a fourth custom layer cannot be added to one site and forgotten at the other. The names are unchanged, which is
# what keeps `depth3_best.keras`, `depth3_last.keras` and `snapshots/*.keras` loadable exactly as before.
CUSTOM_OBJECTS = {"FoldedScaleConv": FoldedScaleConv, "MaskedSmooth": MaskedSmooth,
                  "ChannelGate": ChannelGate}


def init_dense_skip_as_sum(model):
    """Make every dense-connection projection start as a plain sum, so epoch zero IS the residual baseline.

    WHY THIS IS A CURRICULUM AND NOT A CHANGE OF MODEL. The dense connection replaces `y + h` with a learned
    `W [y; h] + b`, whose default start is a random mix. Initialising `W = [I; I]`, `b = 0` and leaving the layer's gain
    at one makes the first step compute exactly the baseline's residual stream; training then decides whether to leave
    that solution. The parameter count, the graph and the export are identical, which is what makes a comparison against
    the random-start run a statement about the start rather than about the architecture.
    """
    touched = 0
    for layer in model.layers:
        if not layer.name.startswith("dproj"):
            continue
        weights = layer.get_weights()
        if not weights:
            continue
        kernel = weights[0]
        if kernel.ndim != 4:
            raise SystemExit(f"{layer.name}: kernel {kernel.shape} is not a 1x1 projection")
        channels = kernel.shape[3]
        inputs = kernel.shape[2]
        if inputs % channels != 0 or inputs // channels < 2:
            raise SystemExit(f"{layer.name}: kernel {kernel.shape} does not start with at least two C-blocks")
        # The visible list is [y, block] followed by optional extras (recent blocks, the entry plane); the first two
        # blocks are the ones a plain residual adds, so they get the identity and everything else starts at zero --
        # at step zero the block then computes exactly `y + h`.
        fresh = np.zeros_like(kernel)
        eye = np.eye(channels, dtype=kernel.dtype)
        fresh[0, 0, :channels, :] = eye
        fresh[0, 0, channels:2 * channels, :] = eye
        rest = list(weights[1:])
        if rest:
            rest[0] = np.zeros_like(rest[0])
        layer.set_weights([fresh] + rest)
        touched += 1
    if not touched:
        raise SystemExit("--dense-skip-init sum found no dproj layers; is --dense-skip or --dense-skip-entry set?")
    print(f"dense-skip init: {touched} projection(s) set to the plain sum (first two C-blocks identity, rest zero, b=0)", flush=True)
    return model


def build(width=D3.NOF_SUBCARRIERS, blocks=18, qm=8, aux_heads=True, c_hid=64, residual=True,
          reporting_heads=True, low_rank=0, smooth_k=0,
          dtype="float32", fold_scale=True, scale_init=1.0, llr_linear=False, dense=False,
          dense_bottleneck=0, fuse_relu=False,
          layout="folded", in_channels=D3.NOF_CHANNELS_SPATIAL, cond=False,
          dilations=DILATIONS, ksize=3, groups=1, noise_to_llr=False, ce_aux=False, llr_post=False,
          cst_head=False, sym_aux=False, mlp_expand=1, se=False, se_ratio=8, dense_skip=False, dense_skip_entry=False, dense_skip_depth=1):
    """The contract's graph: the plane in, the reporting heads out.

    `residual` and `dtype` are the two lessons the refiner paid for (`model_network_design`
    sections 38.3 and 42):

    * a plain `y = block(y)` stack collapses geometrically with depth (0.044 -> 0.005 -> 0.0003
      over the first four blocks), every gradient past the collapse is exactly zero, and training
      stalls at "no better than predicting nothing". Measured again on THIS task by the smoke run
      in the design document's section 12.9: the loss fell to 1.277, the mean LLR to 1.1 and the
      uncoded BER stayed at 0.498, which is chance. A residual block with a LayerScale avoids the
      collapse, and BatchNorm would too but costs far more on the ANE;
    * training in fp16 stalls; the deployed artifact is fp16 either way, because the conversion
      chooses that, so the graph dtype here is a training decision only.
    """
    keras_dtype = tf.float32 if dtype == "float32" else tf.float16

    def finish(graph):
        """Marks the CONTRACT the graph follows, for the export's metadata declaration (design section 19.102).

        A conditioned bundle and a per-modulation 256QAM bundle have the same main-head width, so the bundle has
        to SAY which one it is; an attribute here is what lets the export write that down.
        """
        graph._depth3_cond = bool(cond)
        return graph

    # The MODULATION-CONDITIONED model (design section 19.102): one bundle for all four modulations, with the
    # Qm as an INPUT (four one-hot channels appended to the plane) and a main head whose stride is the widest
    # modulation, so a reception reads `symbol * COND_STRIDE + bit` for its own bits and ignores the rest.
    # The conditioning channels are APPENDED BY THE DATASET (see Batches.sample), because only the sample knows
    # its own Qm -- so `in_channels` here is already the data's channel count and must not be widened again. The
    # build()/data check in main() is what caught the first version of this: it refused 64 against 68.
    head_qm = D3.COND_STRIDE if cond else qm
    if layout != "folded":
        # Time is a spatial axis here, so the 3x3 kernel mixes frequency AND symbols. `in_channels`
        # is the packing the data carries: 12 for the contract's spatial plane, 8 for the R4.4
        # diagnostic arm that isolated the locality.
        inp = tf.keras.Input(shape=(width, NOF_SYMBOLS, in_channels), batch_size=None,
                             dtype=keras_dtype, name="x")
        # A DENSE LOCAL AVERAGE OVER ONE COHERENCE BANDWIDTH. The trunk's receptive field is built from dilations,
        # so its long-range taps are SPARSE: it can see 127 subcarriers but it cannot compute a dense 63-tap average,
        # which is the operation an MMSE channel estimator performs inside one coherence bandwidth (measured 150-300
        # subcarriers, section 8). This branch adds exactly that: one depthwise convolution along frequency whose
        # kernel starts as a boxcar of the coherence-bandwidth scale and is then learned. It is initialised to
        # 1/k so the model begins at the physical prior rather than at noise, and it stays inside the network --
        # nothing classical is bolted on.
        # The model's input stays the keras Input; the branch only feeds a WIDER tensor to the entry projection.
        # Reassigning `inp` instead made Keras wrap the concatenation as the model input and drop the branch from the
        # graph (the model then had a 32-channel input and no smoothing layer at all) -- it looked like the flag was
        # ignored.
        src = inp
        if smooth_k > 0:
            # The allocation mask is the spatial plane's own channel; the smoothing must not average across its edge.
            sm, _ = MaskedSmooth(smooth_k, dtype=keras_dtype, name="smooth_ms")(src)
            src = tf.keras.layers.Concatenate(dtype=keras_dtype, name="smooth_cat")([src, sm])
            in_channels = 2 * in_channels
        y = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name="entry_1x1")(src)
        y = tf.keras.layers.ReLU(dtype=keras_dtype, name="entry_relu")(y)
        # ★ THE RAW FEATURES MUST BE KEPT IF ANY BLOCK IS MEANT TO SEE THEM AGAIN. `y` is overwritten eighteen
        # times; without this handle the entry plane (the received signal plus the Qm conditioning, before any
        # nonlinearity) would be reachable only through the compression of every projection in between.
        y_entry = y
        # ★ VISIBILITY DEPTH (architecture_levers section 47). `y` carries the whole history, but compressed by every
        # projection on the way; keeping the last few raw block outputs gives them an undiluted path into the next
        # blocks. `--dense-skip-depth 1` is the plain C (state + own output), 2 adds the previous block's output, and
        # so on. The list is bounded by the depth, so memory does not grow with the block count.
        recent = []
        for i in range(blocks):
            dilation = dilations[i % len(dilations)]
            # THE BLOCK TYPE HAS TO BE HONOURED ON THIS PATH TOO. The spatial layout has its own trunk
            # builder, and it used to hard-code a depthwise 3x3: `--dense-blocks` and
            # `--dense-bottleneck` were silently ignored whenever `--layout spatial` was in force,
            # which is the layout every deployment bundle uses. Two runs that differed only in that
            # flag therefore trained the same architecture, and the difference between their scores
            # was the trainer's own version drift rather than the block (measured 2026-11-06:
            # 26,185 parameters whether or not the flag was set, against 190,750 expected for dense).
            if dense_bottleneck > 0:
                narrow = max(8, c_hid // dense_bottleneck)
                block = tf.keras.layers.Conv2D(narrow, 1, dtype=keras_dtype,
                                               name=f"sq{i}_1x1")(y)
                block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}a")(block)
                block = tf.keras.layers.Conv2D(narrow, ksize, padding="same",
                                               dilation_rate=(dilation, 1), dtype=keras_dtype,
                                               activation="relu" if fuse_relu else None,
                                               name=f"cv{i}_{ksize}x{ksize}_d{dilation}")(block)
            elif dense:
                # THE KERNEL IS A LEVER, NOT A CONSTRAINT (architecture_levers L2): k=5 widens the
                # spatial mixing reach from 3 to 5 at 25/9 the weights of a k=3 depthwise, and on the
                # ANE a k=5 depthwise measured 219-229 us. Depthwise uses it too, so the two recipes
                # can be compared with everything else held fixed.
                # GROUPS IS THE DIAL BETWEEN DEPTHWISE AND DENSE (architecture_levers L3): g channels per
                # group means g=C is depthwise and g=1 is the dense 3x3 that bought +18.6%. The kernel side
                # stays a separate knob so the two levers can be measured independently.
                block = tf.keras.layers.Conv2D(c_hid, ksize, padding="same", groups=groups,
                                               dilation_rate=(dilation, 1), dtype=keras_dtype,
                                               activation="relu" if fuse_relu else None,
                                               name=f"cv{i}_{ksize}x{ksize}_d{dilation}")(y)
            else:
                block = tf.keras.layers.DepthwiseConv2D(
                    ksize, padding="same", dilation_rate=(dilation, 1), dtype=keras_dtype,
                    activation="relu" if fuse_relu else None,
                    name=f"dw{i}_{ksize}x{ksize}_d{dilation}")(y)
            if not fuse_relu:
                block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
            if mlp_expand != 1:
                # ★ THE DENSE LEVER, PUSHED FURTHER: an expand-project MLP in place of the single 1x1.
                # The one lever with a large measured effect is full channel mixing (+18.6% for dense over
                # depthwise), and the block currently mixes ONCE per residual. A 1x1 to ratio*C, a ReLU, and a
                # 1x1 back mix twice, at the same position, so the spatial receptive field is untouched --
                # which is the part that every dilation/kernel experiment already showed does not help.
                # A RATIO BELOW 1 IS A BOTTLENECK, NOT AN ERROR. `> 1` silently ignored `--mlp-expand 0.25`, which is
                # the one setting that separates "more channels" from "one more nonlinearity": a C -> C/4 -> C pair
                # uses FEWER parameters than the plain 1x1 while still adding a second linear layer and its
                # activation. The width is floored at 4 so a small ratio stays a real layer.
                wide = max(4, int(round(c_hid * mlp_expand)))
                block = FoldedScaleConv(wide, initial_gain=scale_init, rank=low_rank,
                                        dtype=keras_dtype, name=f"pw{i}a_1x1")(block)
                block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"pw{i}a_relu")(block)
                block = FoldedScaleConv(c_hid, initial_gain=scale_init, rank=low_rank,
                                        dtype=keras_dtype, name=f"pw{i}b_1x1")(block)
            else:
                block = FoldedScaleConv(c_hid, initial_gain=scale_init, rank=low_rank,
                                        dtype=keras_dtype, name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            if se:
                # ★ LEVER B: CHANNEL ATTENTION, NOT MORE MIXING. Lever A mixes the channels twice per block; this
                # instead lets the block re-weight its own channels from a global summary. It is orthogonal to A
                # (which is why they can stack), and it costs about two hundred parameters and no extra MACs per
                # element, so it cannot move the 500 us budget. The squeeze is a global average over the allocation
                # plane, the excite is a two-layer bottleneck, and the gate multiplies the block's output.
                z = tf.keras.layers.GlobalAveragePooling2D(dtype=keras_dtype, name=f"se{i}_gap")(block)
                z = tf.keras.layers.Dense(max(4, c_hid // se_ratio), activation="relu",
                                          dtype=keras_dtype, name=f"se{i}_fc1")(z)
                z = tf.keras.layers.Dense(c_hid, activation="sigmoid", dtype=keras_dtype, name=f"se{i}_fc2")(z)
                z = tf.keras.layers.Reshape((1, 1, c_hid), dtype=keras_dtype, name=f"se{i}_gate")(z)
                block = ChannelGate(dtype=keras_dtype, name=f"se{i}_scale")([block, z])
            if dense_skip or dense_skip_entry:
                # ★ LEVER C: A DENSE CROSS-BLOCK CONNECTION INSTEAD OF A RESIDUAL. The block's output is
                # concatenated with the running state and a 1x1 projects the pair back to c_hid, so every later
                # block sees the earlier features directly rather than through a sum. That is the DenseNet idea
                # without unbounded growth: the projection keeps the trunk at c_hid channels, at the cost of one
                # 1x1 of 2*c_hid -> c_hid per block. Levers A and B change what a block computes; this changes
                # what a block can see.
                # ★ LEVER C, WITH THE ENTRY PLANE IN THE SET OF THINGS A BLOCK CAN SEE. Three tensors instead of
                # two: the running state, the block's own output, and the entry features. The projection is 1.0-gain
                # and full rank, so how much of each is used is learned, not fixed -- the measurement on the plain
                # C model showed the model putting 38% of the weight norm on the running state.
                visible = [y, block] + recent
                if dense_skip_entry:
                    visible = visible + [y_entry]
                pair = tf.keras.layers.Concatenate(dtype=keras_dtype, name=f"dcat{i}")(visible)
                # THE PROJECTION MUST NOT SHRINK THE STATE. With the trunk's residual gain (0.03) it would scale
                # every block's output by thirty and the state would decay to zero over eighteen blocks -- the
                # first smoke run sat at chance loss (0.693) exactly because of that. Unit gain keeps the
                # magnitude, which is what a path-through projection has to do.
                y = FoldedScaleConv(c_hid, initial_gain=1.0, rank=low_rank,
                                    dtype=keras_dtype, name=f"dproj{i}_1x1")(pair)
                recent = (recent + [block])[-(max(1, int(dense_skip_depth)) - 1):] if dense_skip_depth > 1 else []
            else:
                y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        # The heads keep the CONTRACT's channel counts (14*Qm, 14, 28, 1) whatever the trunk's
        # internal layout is. The seam binds them by name and the engine checks them by channel
        # count, so a layout that renamed them would be a contract change rather than a choice
        # inside the network. Only the spatial axes differ, and every one of them is flattened back
        # to the contract's `[width, channels]` before the head leaves the model.
        # ★ L6: THE NOISE HEAD BECOMES AN INPUT OF THE DEMAPPER INSTEAD OF ONLY A SIDE OUTPUT.
        # The screen of 2026-11-15 showed the head separates errors at FIXED |LLR| (8.2% -> 30.2% across its
        # quintiles inside the low-confidence band), so it carries per-RE information the LLR magnitude does not.
        # Feeding it back costs one input channel (+head_qm weights) and no structure: the demapper can then
        # sharpen exactly the REs it knows are noisy.
        llr_src = y
        if noise_to_llr:
            nz = tf.keras.layers.Activation("tanh", dtype=keras_dtype, name="post_eq_noise")(
                tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="noise_1x1")(y))
            llr_src = tf.keras.layers.Concatenate(dtype=keras_dtype, name="llr_with_noise")([y, nz])
        if cst_head:
            # ★ LEVER A: A CONSTELLATION-AWARE HEAD, IN ITS HONEST FORM.
            # The exact max-log demapper needs the symbol posterior of the modulation IN USE, and a single conditioned
            # graph cannot express that: the contract's channel order for 256QAM is (I0..I3,Q0..Q3) while QPSK's two
            # bits are (I,Q), i.e. channels 0 and 4 of the same head. A static graph cannot permute channels per
            # sample, so an exact posterior head would need one graph per modulation or a contract change.
            #
            # What is expressible, and what this builds, is the ANALYTIC LINEAR TERM of the demapper:
            #     LLR_k ~= 4 * s_dim / sigma^2      (s_dim = the soft symbol's I or Q component)
            # followed by a learned residual that supplies everything the linear term cannot (the per-bit offsets of
            # the inner PAM levels, and the per-bit sign patterns). The prior is what the classical chain gets for
            # free; the residual keeps the capacity the current head already has.
            s_i = tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="cst_si")(llr_src)
            s_q = tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="cst_sq")(llr_src)
            log_s2 = tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="cst_ls2",
                                            kernel_initializer="zeros", bias_initializer="zeros")(llr_src)
            # THE EXPONENT IS BOUNDED BY THE ACTIVATION, NOT BY A CLIP. A Lambda cannot be deserialized at export
            # time and a Rescaling layer puts a float32 constant next to a float16 tensor (coremltools refuses the
            # mixed dtype), so tanh bounds the log-variance and the analytic term divides by its exponential. The
            # head starts at exactly sigma^2 = 1 (`cst_ls2` is zero-initialised), i.e. as the plain linear demapper,
            # and moves away only if the data asks for it.
            log_s2 = tf.keras.layers.Activation("tanh", dtype=keras_dtype, name="cst_clip")(log_s2)
            sigma2 = tf.keras.layers.Activation("exponential", dtype=keras_dtype, name="cst_s2")(log_s2)
            si_llr = 4.0 * s_i / sigma2
            sq_llr = 4.0 * s_q / sigma2
            half = head_qm // 2
            analytic = tf.keras.layers.Concatenate(dtype=keras_dtype, name="cst_base")(
                [si_llr] * half + [sq_llr] * (head_qm - half))
            residual = tf.keras.layers.Conv2D(head_qm, 1, dtype=keras_dtype, name="cst_res")(llr_src)
            llr = tf.keras.layers.Add(dtype=keras_dtype, name="cst_add")([analytic, residual])
        else:
            llr = tf.keras.layers.Conv2D(head_qm, 1, dtype=keras_dtype, name="llr_1x1")(llr_src)
        if llr_post:
            # ★ LEVER B: A RESIDUAL FILTER ON THE LLR PLANE. Everything tried so far acted on the trunk's features;
            # this acts on the ANSWER -- a depthwise 3-tap along the subcarrier axis plus a 1x1 that mixes the bits of
            # one symbol position, added back to the logits. ~13 k parameters, and it is deliberately applied BEFORE
            # the bounding activation so the deployed contract's range still holds.
            #
            # WHY IT COULD HELP. The residual encoder and the demapper see the channel through the trunk, but the
            # LLR field itself is spatially correlated (a faded subcarrier takes its neighbours with it) and nothing
            # in the current head can exploit that: each LLR is computed from its own position only.
            res = llr
            post = tf.keras.layers.DepthwiseConv2D((3, 1), padding="same", dtype=keras_dtype,
                                                   name="llrp_dw")(res)
            post = tf.keras.layers.Conv2D(head_qm, 1, dtype=keras_dtype, name="llrp_pw")(post)
            llr = tf.keras.layers.Add(dtype=keras_dtype, name="llrp_add")([res, post])
        llr = tf.keras.layers.Activation("linear" if llr_linear else "tanh", dtype=keras_dtype,
                                         name="llr")(llr)
        # [batch, width, symbols, qm] -> [batch, width, symbols * qm]: the contract's channel order,
        # symbol major and bit fastest, which is what the label plane already holds.
        outputs = {"llr": tf.keras.layers.Reshape((width, NOF_SYMBOLS * head_qm), name="llr_flat")(llr)}
        if aux_heads:
            if noise_to_llr:
                # Already built above and wired into the demapper; here it only needs its output head.
                noise = nz
            else:
                noise = tf.keras.layers.Activation("tanh", dtype=keras_dtype, name="post_eq_noise")(
                    tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="noise_1x1")(y))
            outputs["post_eq_noise"] = tf.keras.layers.Reshape((width, NOF_SYMBOLS),
                                                               name="noise_flat")(noise)
            # THE REPORTING HEADS ARE OPTIONAL (design section 22.2). There are TWO head-construction sites in
            # this function, one per layout, and guarding only the spatial one left the flag silently inert --
            # which is how a run asked for two heads produced a bundle with four.
            if ce_aux and not reporting_heads:
                # ★ TRAINING-ONLY CHANNEL HEAD (lever: CE refinement). It predicts the channel itself (Re/Im) and is
                # supervised by the corpus's true H, which forces the trunk to carry an explicit channel estimate.
                # It is deliberately named `ce_res_*` rather than `ch_est*`: the exporter decides what to ship by
                # looking for `ch_est`/`dmrs_noise` LAYER names, so this head stays out of the deployed graph while
                # the trainer still finds it under the `ch_est` output key and applies the nmse term.
                ce = tf.keras.layers.Activation("linear", dtype=keras_dtype, name="ce_res")(
                    tf.keras.layers.Conv2D(2, 1, dtype=keras_dtype, name="ce_res_1x1")(y))
                outputs["ch_est"] = tf.keras.layers.Reshape((width, 2 * NOF_SYMBOLS),
                                                            name="ce_res_flat")(ce)
            if sym_aux:
                # ★ LEVER D: A SYMBOL-LEVEL AUXILIARY TASK, training only. The head regresses the transmitted QAM
                # symbol (I/Q) at each data RE while the trunk is also learning the bit LLRs. The point is the
                # objective, not the head: predicting symbols forces the trunk to keep a per-RE constellation
                # estimate, which the bit-wise BCE alone does not require. The head is named `sym_aux_*` so the
                # exporter drops it (it is not one of the contract's heads) and the bundle is unchanged.
                sym = tf.keras.layers.Activation("linear", dtype=keras_dtype, name="sym_aux")(
                    tf.keras.layers.Conv2D(2, 1, dtype=keras_dtype, name="sym_aux_1x1")(y))
                outputs["sym_est"] = tf.keras.layers.Reshape((width, 2 * NOF_SYMBOLS),
                                                             name="sym_aux_flat")(sym)
            if reporting_heads:
                # Linear for the channel: it must reach deep fades as well as peaks.
                ch_est = tf.keras.layers.Activation("linear", dtype=keras_dtype, name="ch_est")(
                    tf.keras.layers.Conv2D(2, 1, dtype=keras_dtype, name="ch_est_1x1")(y))
                outputs["ch_est"] = tf.keras.layers.Reshape((width, 2 * NOF_SYMBOLS),
                                                            name="ch_est_flat")(ch_est)
                pooled = tf.keras.layers.GlobalAveragePooling2D(keepdims=True, dtype=keras_dtype,
                                                                name="dmrs_pool")(y)
                outputs["dmrs_noise"] = tf.keras.layers.Activation(
                    "tanh", dtype=keras_dtype, name="dmrs_noise")(
                        tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="dmrs_noise_1x1")(pooled))
        return finish(tf.keras.Model(inp, outputs))

    inp = tf.keras.Input(shape=(width, 1, in_channels), batch_size=None, dtype=keras_dtype,
                         name="x")
    y = inp
    if int(inp.shape[-1]) != c_hid:
        # The residual adds need matching widths, and the contract's plane has 64 channels whatever
        # the trunk uses. One projection at the entry is cheaper than a skip projection per block.
        y = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name="entry_1x1")(y)
        y = tf.keras.layers.ReLU(dtype=keras_dtype, name="entry_relu")(y)
    for i in range(blocks):
        dilation = dilations[i % len(dilations)]
        # A DEPTHWISE 3x3 mixes frequency within each channel; a dense one mixes channels and
        # frequency together, which is far more expressive per layer. `model_network_design` 25.3
        # measured that a dense 3x3 computes 5.7 times the FLOPs for the SAME latency on this
        # engine, so the depthwise choice was buying an arithmetic-intensity saving the ANE did not
        # need, at the cost of exactly the per-position capacity the diagnosis points at.
        if dense_bottleneck > 0:
            # BOTTLENECK DENSE. A full dense 3x3 mixes all channels with all channels at 9*C*C
            # weights per block, which is 6 to 7 times the depthwise trunk for a c32 model and 7
            # times for a c128 one. The squeeze below runs the spatial operator on c_hid/narrow
            # channels and the block's existing 1x1 expands back, so the joint space-and-channel
            # mixing that depthwise cannot express is kept while the weights grow only as C*C/narrow.
            # At narrow=2 this is about 2.5 times the depthwise block; at narrow=4 it is cheaper
            # than it, because the depthwise 3x3 carries 9*C against the squeeze's C*C/narrow.
            narrow = max(8, c_hid // dense_bottleneck)
            block = tf.keras.layers.Conv2D(narrow, 1, dtype=keras_dtype,
                                           name=f"sq{i}_1x1")(y)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}a")(block)
            block = tf.keras.layers.Conv2D(narrow, ksize, padding="same", dilation_rate=(dilation, 1),
                                           dtype=keras_dtype, name=f"cv{i}_{ksize}x{ksize}_d{dilation}")(block)
        elif dense:
            block = tf.keras.layers.Conv2D(c_hid, ksize, padding="same", dilation_rate=(dilation, 1),
                                           dtype=keras_dtype, name=f"cv{i}_{ksize}x{ksize}_d{dilation}")(y)
        else:
            block = tf.keras.layers.DepthwiseConv2D(
                3, padding="same", dilation_rate=(dilation, 1), dtype=keras_dtype,
                activation="relu" if fuse_relu else None,
                name=f"dw{i}_3x3_d{dilation}")(y)
        if not fuse_relu:
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}")(block)
        if residual and fold_scale:
            block = FoldedScaleConv(c_hid, initial_gain=scale_init, rank=low_rank,
                                    dtype=keras_dtype, name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
        else:
            block = tf.keras.layers.Conv2D(c_hid, 1, dtype=keras_dtype, name=f"pw{i}_1x1")(block)
            block = tf.keras.layers.ReLU(dtype=keras_dtype, name=f"relu{i}b")(block)
            if residual:
                block = Scale(initial=scale_init, name=f"scale{i}", dtype=keras_dtype)(block)
                y = tf.keras.layers.Add(name=f"add{i}", dtype=keras_dtype)([y, block])
            else:
                y = block

    # The main head's bound. `tanh` keeps the output inside +-1 with no clipping branch, but its
    # gradient vanishes exactly where a confident bit lives: at 42 dB the correct LLR is far beyond
    # the scale, so every well-classified bit sits in the flat region and stops teaching the trunk.
    # Section 4.2 lists a clamp as the second allowed form of the same bound, and a linear head with
    # an explicit clamp keeps the gradient everywhere the output is not already clipped.
    llr_head = tf.keras.layers.Conv2D(NOF_SYMBOLS * head_qm, 1, dtype=keras_dtype, name="llr_1x1")(y)
    outputs = {"llr": tf.keras.layers.Activation(
        "linear" if llr_linear else "tanh", dtype=keras_dtype, name="llr")(llr_head)}
    if aux_heads:
        outputs["post_eq_noise"] = tf.keras.layers.Activation(
            "tanh", dtype=keras_dtype, name="post_eq_noise")(
                tf.keras.layers.Conv2D(NOF_SYMBOLS, 1, dtype=keras_dtype, name="noise_1x1")(y))
        # THE TWO REPORTING HEADS ARE OPTIONAL (design section 22.2): once the shared measurement layer reports
        # RSRP, EPRE and the SINR, a bundle that also carries them spends capacity on a second job and trades one
        # against the other -- the trade that cost the r7 bundle its mid-band accuracy (section 19.264). The noise
        # head above is NOT optional: it is what scales the logits into the LLRs the chain quantises.
        if reporting_heads:
            # Linear for the channel: it must reach deep fades as well as peaks.
            outputs["ch_est"] = tf.keras.layers.Activation(
                "linear", dtype=keras_dtype, name="ch_est")(
                    tf.keras.layers.Conv2D(2 * NOF_SYMBOLS, 1, dtype=keras_dtype, name="ch_est_1x1")(y))
            # The fourth head is one scalar for the whole slot, so the trunk is averaged over frequency
            # first. The contract lists it as its own output because `memo_07` R4 needs the DM-RS domain
            # noise variance, which the classical kernel can no longer measure once the estimate is the
            # network's own: the residual it would measure has no noise left in it.
            pooled = tf.keras.layers.GlobalAveragePooling2D(keepdims=True, dtype=keras_dtype,
                                                            name="dmrs_pool")(y)
            outputs["dmrs_noise"] = tf.keras.layers.Activation(
                "tanh", dtype=keras_dtype, name="dmrs_noise")(
                    tf.keras.layers.Conv2D(1, 1, dtype=keras_dtype, name="dmrs_noise_1x1")(pooled))
    return finish(tf.keras.Model(inp, outputs))


class _GeometryCache:
    """The bounded, thread-safe home of the per-allocation constants (see `Batches`).

    RETIRE ONE ENTRY AT A TIME; NEVER CLEAR THE TABLE. The previous form called `clear()` on reaching the cap,
    so the sample that overflowed it discarded every other geometry as well and the next samples all missed --
    a burst of misses exactly when the loader could least afford one, and on a corpus with more allocations
    than the cap (fifty-one widths times three DM-RS patterns is already a hundred and fifty-three) it could
    repeat indefinitely. Retirement is FIFO, which needs no bookkeeping per hit and therefore no lock on the hot
    path; the cap is a count, so the worst-case footprint (about 340 KB an entry) is the same as before.

    READS ARE LOCK-FREE, MISSES ARE SERIALISED. `dict.get` is atomic under the GIL, so `sample()` on six loader
    threads never blocks on a hit. On a miss the lock covers the build as well as the insert: two threads that
    want the same new geometry then do the work once instead of twice, and the eviction decision cannot see a
    table that another thread is inserting into. The records are immutable and their arrays are marked read-only
    before publication, so a reader that receives one cannot be affected by a later eviction.
    """

    def __init__(self, max_entries):
        self._entries = {}
        self._max_entries = max(1, int(max_entries))
        self._lock = threading.Lock()

    def __len__(self):
        return len(self._entries)

    def get_or_build(self, key, build):
        """The record for `key`, building it under the lock on a miss."""
        hit = self._entries.get(key)
        if hit is not None:
            return hit
        with self._lock:
            hit = self._entries.get(key)
            if hit is not None:
                return hit
            value = build()
            while len(self._entries) >= self._max_entries:
                # The oldest insertion is the first key a plain dict yields; popping it is O(1).
                self._entries.pop(next(iter(self._entries)))
            self._entries[key] = value
            return value


class Batches:
    """Builds training tensors from a dataset file, on the fly.

    The label planes are derived per sample from the stored coded bits and masks instead of being
    stored, so the file stays small and the mapping from bits to channels has exactly one
    implementation (`depth3.label_plane`).

    WHAT IS CACHED AND WHAT IS NOT. Everything a sample allocates falls into two classes. The PER-ALLOCATION
    constants -- the data-RE index walk, the BCE validity mask, the channel-target mask, the symbol-target
    mask -- depend only on (allocation, DM-RS pattern, modulation, control layout), of which a whole corpus
    holds a handful, and they were being rebuilt for every sample: the 256QAM validity mask alone is
    612 x 112 floats, so a 51-PRB sample paid a 274 KB zero-fill and a 14-slice copy for a mask that never
    changed. Those are cached (`_geometry`). The LABEL PLANE is not, because it is a function of the coded
    bits and therefore genuinely per sample.

    THE CACHE IS SHARED BY THE LOADER THREADS (`--workers N`) AND OWNS ITS OWN LOCK (`_GeometryCache`). Hits are
    lock-free (`dict.get` is atomic under the GIL) and misses build under the lock, so two threads never pay for
    the same geometry twice and eviction cannot race an insert. Eviction retires the OLDEST entry instead of
    clearing the table, and every array is marked read-only before it is published, so a caller that tries to
    write through a shared mask fails loudly instead of corrupting every later batch. The one piece of state that
    is *not* shared is the shuffle order: `self.rng` is only touched by `epoch()` on the consumer's thread.
    """

    ARRAYS = ("X", "H", "bits", "nof_bits", "alloc_first_prb", "alloc_nof_prb", "dmrs_bitmap",
              "qm", "sigma2_over_r2", "snr_nominal", "tb_bits", "nof_tb_bits", "rnti", "n_id",
              "rv")
    # The control payload a reception multiplexed on the shared channel, if the file carries it.
    # A dataset built before those fields existed has no control information in its element map,
    # and `uci_of` then returns the empty layout, which is the same map.
    UCI_ARRAYS = ("uci_nof_harq_ack", "uci_ack_re", "uci_ack_rvd_re", "uci_csi1_re")
    # The geometry cache holds one entry per distinct (allocation, DM-RS pattern, modulation, control
    # layout), each about 340 KB because the BCE mask is the whole 612 x 112 plane. Sixty-four is ~22 MB, and
    # it is a COUNT, not a byte budget, precisely because the entries are all the same size. Past the cap the
    # OLDEST entry is retired (`_GeometryCache`), never the table: dropping it all at once turned one overflow
    # into a burst of misses across every geometry the run had just been using.
    GEOMETRY_ENTRIES = 64
    # THE PER-ALLOCATION CONSTANTS, as one record. Kept as plain attributes so a reader can see exactly what
    # the cache holds: the walk, the three masks, and the lists the contract functions take.
    _Geometry = collections.namedtuple(
        "_Geometry", "alloc dmrs modulation uci sub_re sym_re llr_weights ch_weights sym_weights "
                     "allocated")

    def __init__(self, path, modulation, max_samples=0, seed=0, snr_min=None, snr_max=None, cond=False,
                 target="coded", layout="folded"):
        # TWO FORMS, ONE MEANING. A path that is a directory holds one `.npy` per array and is opened
        # memory-mapped, so the dataset is limited by disk rather than by RAM; `convert_npz_to_mmap.py`
        # writes that form. A path that is a file is the original compressed archive.
        #
        # WHY THE ARCHIVE IS READ EAGERLY. `.npz` is a ZIP container: nothing inside it can be
        # memory-mapped even when the members are stored uncompressed, because `np.load` goes through
        # `zipfile` and needs the whole member in memory. Reading it once and keeping it is cheaper
        # than decompressing a 200 MB array on every sample, which is what per-sample access would do.
        if os.path.isdir(path):
            def load(name):
                return np.load(os.path.join(path, name + ".npy"), mmap_mode="r")
            self.data = {name: load(name) for name in self.ARRAYS}
            uci_path = os.path.join(path, self.UCI_ARRAYS[0] + ".npy")
            self.uci = ({name: load(name) for name in self.UCI_ARRAYS}
                        if os.path.exists(uci_path) else None)
            self.mmap = True
        else:
            archive = np.load(path, allow_pickle=True)
            self.data = {name: archive[name] for name in self.ARRAYS}
            self.uci = ({name: archive[name] for name in self.UCI_ARRAYS}
                        if self.UCI_ARRAYS[0] in archive.files else None)
            self.mmap = False
        self.modulation = modulation
        self.qm = D3.BITS_PER_SYMBOL[modulation]
        # THE METADATA COLUMNS ARE LOADED, THE PLANES STAY MAPPED. `alloc_first_prb`, `dmrs_bitmap`, `qm`,
        # `nof_bits`, `sigma2_over_r2` and the rest are one number per sample: a few hundred kilobytes for a
        # corpus of a hundred thousand. Read through the mapping they cost a page fault each, at a random
        # offset, several times per sample and twice more per decoded sample; read once into RAM they cost
        # nothing. `X`, `H`, `bits` and `tb_bits` stay mapped -- those are the arrays whose size is the reason
        # the mapping exists. The values are the same bytes either way.
        self._load_small_columns()
        # A CONDITIONED dataset feeds one model that serves every modulation: the samples carry their own Qm (the
        # file already does), the label plane uses the stride-8 layout, and the Qm is appended as a one-hot.
        self.cond = cond
        # "coded" trains against the transmitted bits, which is the contract's task. "genie" trains
        # against the hard decisions a receiver with the TRUE channel would make, which needs only a
        # per-resource-element division and a constellation decision and no interpolation across
        # frequency. The difference between the two isolates where the network stalls.
        self.target = target
        self.layout = layout
        self.n = len(self.data["X"])
        # Keep only the requested SNR range. The label of a low-SNR resource element is decided by
        # the noise, not by the input, so a model cannot fit it however long it trains; restricting
        # the range is how the pipeline is checked against an answer that IS reachable.
        self.order = np.arange(self.n)
        nominal = self.data["snr_nominal"]
        keep = np.ones(self.n, dtype=bool)
        if snr_min is not None:
            keep &= nominal >= snr_min
        if snr_max is not None:
            keep &= nominal <= snr_max
        self.order = self.order[keep]
        self.n = int(self.order.size)
        if max_samples:
            self.n = min(self.n, max_samples)
        self.order = self.order[:self.n]
        self.rng = np.random.default_rng(seed)
        # The per-allocation constants (see `_geometry`). Per instance, because `cond` changes what the
        # validity mask is and the training and validation sets are separate files.
        self._geometry_cache = _GeometryCache(self.GEOMETRY_ENTRIES)

    def _load_small_columns(self):
        """Copy every one-dimensional column into RAM, leaving the per-sample planes mapped.

        Only the memory-mapped form needs this, and only for the columns whose whole point is to be read at a
        random index: `X` is 612 x 64 floats a sample and stays on disk. The size guard is there so a dataset
        with an unexpectedly long one-dimensional array cannot be silently pulled into memory.
        """
        if not self.mmap:
            return
        for table in (self.data, self.uci or {}):
            for name, column in list(table.items()):
                if getattr(column, "ndim", 0) == 1 and column.nbytes <= 32 * 1024 * 1024:
                    table[name] = np.array(column)

    def allocation_of(self, index):
        first_prb = int(self.data["alloc_first_prb"][index])
        return list(range(first_prb, first_prb + int(self.data["alloc_nof_prb"][index])))

    def dmrs_symbols_of(self, index):
        bitmap = int(self.data["dmrs_bitmap"][index])
        return [s for s in range(NOF_SYMBOLS) if (bitmap >> s) & 1]

    def modulation_of(self, index):
        return _MODULATION_OF_QM[int(self.data["qm"][index])]

    def uci_of(self, index):
        """The control payload of one sample, or None when its element map holds no control."""
        if self.uci is None:
            return None
        layout = UCI.UciLayout(int(self.uci["uci_nof_harq_ack"][index]),
                               int(self.uci["uci_ack_re"][index]),
                               int(self.uci["uci_ack_rvd_re"][index]),
                               int(self.uci["uci_csi1_re"][index]))
        return None if layout.is_empty else layout

    def plane_channels(self):
        """Input channels `sample()` puts in `x`, WITHOUT building a sample.

        `main` needs this one number to check that `--layout` agrees with the file, and it used to read it off
        `train.sample(0)`: unpack the bits, build the label plane, the masks and the noise target, take the last
        dimension, throw all of it away. The first epoch then built the same sample again. The number is a
        property of the LAYOUT and of the file's own plane width, so it is read from the shapes instead, and it
        is the same number the old call returned for every layout: the folded plane keeps the file's channel
        count, `respaciate` always produces `NOF_CHANNELS_SPATIAL`, and `unfold_plane` always produces eight.
        """
        if self.layout == "probe8":
            return 8
        if self.layout == "spatial":
            return D3.NOF_CHANNELS_SPATIAL
        return int(self.data["X"].shape[-1])

    def _geometry_key(self, index):
        """A hashable summary of the inputs every cached constant depends on.

        (allocation, DM-RS pattern, modulation, control layout) and nothing else. The bit sequence is
        deliberately absent: the label plane depends on it, which is exactly why the label plane is the one
        array that is not cached.
        """
        key = (int(self.data["alloc_first_prb"][index]), int(self.data["alloc_nof_prb"][index]),
               int(self.data["dmrs_bitmap"][index]), int(self.data["qm"][index]))
        if self.uci is None:
            return key + (None,)
        return key + (int(self.uci["uci_nof_harq_ack"][index]), int(self.uci["uci_ack_re"][index]),
                      int(self.uci["uci_ack_rvd_re"][index]), int(self.uci["uci_csi1_re"][index]))

    def _geometry(self, index):
        """The per-allocation constants for `index`, built once per combination (see the class docstring)."""
        return self._geometry_cache.get_or_build(self._geometry_key(index),
                                                 lambda: self._build_geometry(index))

    def _build_geometry(self, index):
        """Build (and freeze) one `_Geometry` record. Called by `_GeometryCache` on a miss, under its lock."""
        alloc = self.allocation_of(index)
        dmrs_symbols = self.dmrs_symbols_of(index)
        modulation = self.modulation_of(index)
        uci = self.uci_of(index)
        sub_re, sym_re = D3.data_re_indices(alloc, dmrs_symbols, uci)
        llr_weights = (D3.loss_weights_cond(alloc, dmrs_symbols, modulation, uci) if self.cond
                       else D3.loss_weights(alloc, dmrs_symbols, modulation, uci))
        allocated = D3.allocated_subcarriers(alloc)
        ch_weights = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=np.float32)
        ch_weights[np.flatnonzero(allocated), :] = 1.0
        sym_weights = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS), dtype=np.float32)
        sym_weights[sub_re, sym_re] = 1.0
        record = self._Geometry(alloc, dmrs_symbols, modulation, uci, sub_re, sym_re, llr_weights,
                                ch_weights, sym_weights, allocated)
        # FROZEN BEFORE PUBLICATION. A reader can hold this record while another thread evicts the key, and a
        # write through a shared mask would then be invisible to the next sample: refusing the write keeps the
        # failure local and loud instead.
        for array in (sub_re, sym_re, llr_weights, ch_weights, sym_weights, allocated):
            array.flags.writeable = False
        return record

    def sample(self, index):
        data = self.data
        geometry = self._geometry(index)
        alloc = geometry.alloc
        dmrs_symbols = geometry.dmrs
        modulation = geometry.modulation
        uci = geometry.uci
        # `count=` INSTEAD OF UNPACK-AND-SLICE. The stored column is as wide as the widest reception in the
        # file, so a narrow allocation was unpacking up to tens of kilobytes of padding and then slicing it
        # off -- on the files whose bit plane is a fixed width, that is most of the work. The first `nof_bits`
        # bits are the same bits either way.
        bits = np.unpackbits(data["bits"][index], count=int(data["nof_bits"][index]))

        # NO COPY OF A PLANE THAT IS ONLY READ. `asarray(dtype=float32)` returns the mapped array itself when
        # the file is already float32, and every consumer (`respaciate`, `unfold_plane`, `unpack_input`,
        # `np.concatenate`) only reads it; the old `astype` copied 39 KB per sample to guarantee that.
        plane = np.asarray(data["X"][index], dtype=np.float32)
        # THE SAME EXPRESSION THE ORIGINAL USED, WITHOUT ITS TWO REDUNDANT COPIES: `1j * H[..., 1]` is already
        # the complex64 array the sum needs, so `asarray` on the two components is enough to pin the dtype.
        h = data["H"][index]
        channel = (np.asarray(h[:, :, 0], dtype=np.float32)
                   + 1j * np.asarray(h[:, :, 1], dtype=np.float32))
        # THE SYMBOL LABEL. `bits` are the transmitted bits in data-RE order, so `modulate` turns them into the
        # transmitted QAM symbol per RE; both come from the same CRC-OK transport block the bit plane uses.
        sub_re, sym_re = geometry.sub_re, geometry.sym_re
        # THE SAME SYMBOLS, COMPUTED OVER THE WHOLE BLOCK. `DL.modulate` loops over every symbol in Python -- 52% of
        # the per-sample time, and it holds the GIL, which is why six loader threads measured slower than one. The
        # NumPy version is verified equal to the reference (fast_label.verify_agreement).
        sym = FL.modulate(bits, D3.BITS_PER_SYMBOL[modulation])
        sym_plane = np.zeros((D3.NOF_SUBCARRIERS, NOF_SYMBOLS, 2), dtype=np.float32)
        sym_plane[sub_re, sym_re, 0] = sym.real
        sym_plane[sub_re, sym_re, 1] = sym.imag
        sym_weights = geometry.sym_weights
        label = (D3.label_plane_cond(bits, alloc, dmrs_symbols, modulation, uci) if self.cond
                 else D3.label_plane(bits, alloc, dmrs_symbols, modulation, uci))
        if self.target == "genie":
            label = genie_labels(plane, channel, alloc, dmrs_symbols, modulation, cond=self.cond,
                                 uci=uci)
        llr_weights = geometry.llr_weights
        # The channel head is only constrained where the allocation is, and the target is already
        # divided by the reference amplitude, which is what the head produces.
        ch_weights = geometry.ch_weights

        sigma2_norm = float(data["sigma2_over_r2"][index])
        with np.errstate(divide="ignore", invalid="ignore"):
            noise_target = L.noise_shape_target(sigma2_norm / (np.abs(channel) ** 2 + 1e-30))
        noise_weights = (ch_weights > 0) & noise_target[1]

        if self.layout == "spatial":
            # The contract's current input: the symbol axis is spatial, so the 3x3 mixes time.
            plane_in = D3.respaciate(plane)
        elif self.layout == "probe8":
            plane_in = unfold_plane(plane, alloc, dmrs_symbols)
        else:
            plane_in = plane[:, None, :]
        if self.cond:
            # The Qm CONDITIONS the model as four constant channels: the trunk and the reporting heads are
            # shared across modulations, so the network has to be told which one this reception is (design
            # section 19.102). Appending them here keeps every existing dataset usable unchanged.
            one_hot = D3.qm_one_hot(D3.BITS_PER_SYMBOL[modulation])
            plane_in = np.concatenate(
                [plane_in, np.broadcast_to(one_hot, plane_in.shape[:-1] + (one_hot.size,))], axis=-1)
        return {
            "index": index,
            "x": plane_in,
            "llr_shape": label,
            "llr_weights": llr_weights,
            "ch": np.stack([channel.real, channel.imag], axis=-1),
            "ch_weights": ch_weights,
            "noise": noise_target[0],
            "noise_weights": noise_weights.astype(np.float32),
            "sym": sym_plane,
            "sym_weights": sym_weights,
            "sigma2_over_r2": np.float32(sigma2_norm),
            # The DM-RS domain noise variance is the same scalar the post-equalization head is
            # derived from, so its target is the same `log10` shape of it, computed on the scalar
            # rather than per resource element.
            "dmrs_noise": np.float32(L.noise_shape_target(np.asarray([sigma2_norm]))[0][0]),
            "snr": float(data["snr_nominal"][index]),
            # THE ALLOCATION WIDTH, for the second axis of the loss weight. One small int per sample; the array it comes
            # from is already resident (it is one of the per-sample metadata columns loaded once), so this costs a cast.
            "alloc_nof_prb": int(data["alloc_nof_prb"][index]),
            # The modulation NAME, so the loss can look up that modulation's measured decoder threshold. It is the same
            # variable the one-hot conditioning and the LLR scale were built from two lines above.
            "modulation": modulation,
            # The scale is model-external per the contract and is PER MODULATION (design section 19.102): a
            # conditioned batch holds several modulations at once, so the loss cannot use one number for all of
            # them -- QPSK's limit is 24 and the others' is 20, and a batch scaled with the wrong one trains the
            # head against the wrong quantiser.
            "scale": np.float32(D3.RANGE_LIMIT_FLOAT[modulation]),
        }

    def epoch(self, batch_size, workers=0, prefetch=0):
        """Yield batches for one epoch, reshuffling first.

        `workers` builds the next `prefetch` batches in a thread pool while the accelerator consumes the current one.
        The per-sample work -- unpacking the bit sequence, building the label plane, the loss weights and the noise
        target -- is numpy and therefore releases the GIL, so threads genuinely overlap it with the device. Measured
        on the RTX 3060 box, the card sat at 47 percent utilisation with this loop serial, which is what this exists
        to fix (design section 19.367(d)).

        THE ORDER OF THE BATCHES IS THE SERIAL ORDER, WHATEVER THE WORKERS DO. The pool only looks ahead; the
        generator still yields the batches in the order `self.order` was shuffled into, so a run with workers
        computes exactly the sequence of updates it would have computed without them, and the epoch's numbers
        cannot depend on the thread count.
        """
        self.rng.shuffle(self.order)
        # THE INDICES LEAVE NUMPY HERE, ONCE PER BATCH. `self.order` is an int64 array, so the old form paid an
        # `int()` conversion per sample and handed `sample()` a numpy scalar; `tolist()` does the same widening in
        # C for the whole slice. The values are identical (they are the same integers), and `sample()`'s contract
        # is unchanged -- it takes whatever a Python list of indices contains.
        def build(start):
            return [self.sample(i) for i in self.order[start:start + batch_size].tolist()]

        if workers <= 0:
            for start in range(0, self.n, batch_size):
                yield build(start)
            return

        starts = list(range(0, self.n, batch_size))
        # `ahead` IS AT LEAST THE WORKER COUNT. A `--prefetch 1` next to six workers used to keep exactly one
        # batch in flight, so five of the six threads were never given anything to do; the window cannot be
        # narrower than the pool that fills it.
        ahead = max(prefetch, workers, 1)
        with ThreadPoolExecutor(max_workers=min(workers, max(len(starts), 1))) as pool:
            pending = collections.deque()
            for start in starts[:ahead]:
                pending.append(pool.submit(build, start))
            for index, start in enumerate(starts):
                batch = pending.popleft().result()
                nxt = index + ahead
                if nxt < len(starts):
                    pending.append(pool.submit(build, starts[nxt]))
                yield batch




def stack(samples, key):
    """One batch's `key` as a float32 array.

    IT IS ONE ALLOCATION AND ONE PASS WHEN THE DTYPE ALREADY MATCHES. Every array a sample builds is float32, and
    `astype` copies by default, so the old form stacked and then allocated again to convert a dtype that was
    already correct -- nineteen megabytes a batch at batch 16 for the seven targets this recipe reads. The dtype
    is now checked once on the stacked result instead of being converted blindly, and the per-sample fetch is
    `operator.itemgetter`, which does the dictionary lookup in C instead of running a Python `LOAD_SUBSCR` per
    element. Both are pure host-side savings; the returned array is the same array either way.
    """
    stacked = np.stack(list(map(operator.itemgetter(key), samples)))
    if stacked.dtype == np.float32:
        return stacked
    return stacked.astype(np.float32, copy=False)


def threshold_weights(samples, weight, sigma, center=None, width_weight=0.0, width_max=8):
    """The per-sample factor of `--snr-weight`: large near the decoder threshold, one everywhere else.

    A reception is delivered or lost as a whole block around that threshold, so the samples whose distance to it is
    small are the ones that switch delivered bits; the ones far from it teach the same lesson whatever the model
    does. The window is in `snr_nominal`, the variable the datasets carry, and the threshold is that modulation's
    own measured 50% crossing (`THR_QM`). `center` overrides the centre for 256QAM only, as its flag documents.

    VECTORISED, AND ARITHMETICALLY THE SAME AS THE LOOP IT REPLACED. That loop looked up a dict and called `np.exp`
    once per sample, in Python floats; this evaluates the same expression over the batch in float64 and casts into
    the same float32 output, so the values are identical. The batch is eight samples, so this is not where an epoch
    is won -- but it is host time sitting between two device steps, and removing it is free.
    """
    if weight <= 0.0 and width_weight <= 0.0:
        return np.ones(len(samples), dtype=np.float32)
    sigma = max(1e-3, float(sigma))
    modulations = np.asarray([str(sample["modulation"]) for sample in samples])
    snr = np.asarray([float(sample["snr"]) for sample in samples], dtype=np.float64)
    threshold = np.asarray([THR_QM.get(str(mod), np.nan) for mod in modulations], dtype=np.float64)
    if center is not None:
        threshold = np.where(modulations == "256QAM", float(center), threshold)
    out = np.ones(len(samples), dtype=np.float32)
    if weight <= 0.0:
        # A width-only run must not build the SNR window: its factor is exactly one, and skipping the per-sample
        # modulation and SNR gather keeps the arithmetic identical to the unweighted case.
        return _width_factor(samples, width_weight, width_max, out)
    known = np.isfinite(threshold)
    if known.any():
        # An unknown modulation keeps its weight of one: the distance to a threshold that does not exist is not
        # something to scale a reception by, which is what the original's `continue` said.
        distance = (snr[known] - threshold[known]) / sigma
        out[known] = 1.0 + weight * np.exp(-distance * distance)
    return _width_factor(samples, width_weight, width_max, out)


def _width_factor(samples, width_weight, width_max, base):
    """The second axis of the loss weight: how NARROW the allocation is.

    WHY WIDTH AND NOT ANOTHER SNR TERM. The whole-leg attribution puts the model's remaining losses on the narrowest
    allocations: every one of the 118 units it lost in the census was 256QAM at three or eight PRBs (section 49), and a
    narrow allocation is where the channel estimate has the fewest subcarriers to average over. Raising the weight of
    those samples asks the optimiser for accuracy where the delivered bits are actually being lost, instead of on the
    wide allocations that already decode.

    A STEP RATHER THAN A RAMP, because the census is a step: the losses do not fade out between eight and nine PRBs, the
    bins are `<= 3`, `<= 8`, `<= 24` and `> 24` and the first two carry them. `width_max` is therefore a bin edge, not a
    centre. The factor multiplies the SNR factor when both are on, so the two selections intersect rather than average.
    """
    if width_weight <= 0.0:
        return base
    width = np.asarray([float(sample["alloc_nof_prb"]) for sample in samples], dtype=np.float64)
    return base * (1.0 + width_weight * (width <= float(width_max))).astype(np.float32)


def decode_metrics(llr, labels, weights):
    """Uncoded bit error rate and mean |LLR| on the labelled entries.

    The uncoded rate is the honest early signal that the network learned the mapping: it tends to
    zero as the LLRs become right, and it sits at 0.5 while they are noise. Coded BLER through the
    real LDPC decoder is the acceptance criterion, and it needs the chain, not this metric.
    """
    hard = np.sign(llr)
    labelled = weights > 0
    errors = np.mean((hard[labelled] > 0) != (labels[labelled] > 0.5)) if labelled.any() else np.nan
    return float(errors), float(np.mean(np.abs(llr[labelled]))) if labelled.any() else np.nan


def unfold_plane(plane, alloc, dmrs_symbols):
    """Rearrange the contract's plane into a layout whose TIME axis is a spatial axis.

    The contract folds the fourteen symbols into the channel axis, so a 3x3 kernel can only mix
    frequency; every per-symbol relation has to be learned by 1x1 channel mixing. This layout gives
    each symbol its own spatial slice instead, with eight channels each:

        [Re(y), Im(y), DM-RS mask, allocation, DC, Re(LS), Im(LS), reserved]

    The allocation and DC channels are properties of the subcarrier and are broadcast over symbols,
    and the metadata scalars are dropped: they are derivable from the masks, and this arm exists to
    test the locality, not to be a deployment candidate.
    """
    unfolded = np.zeros((plane.shape[0], NOF_SYMBOLS, 8), dtype=np.float32)
    for symbol in range(NOF_SYMBOLS):
        unfolded[:, symbol, 0] = plane[:, 2 * symbol]
        unfolded[:, symbol, 1] = plane[:, 2 * symbol + 1]
        unfolded[:, symbol, 2] = plane[:, 28 + symbol]
        unfolded[:, symbol, 3] = plane[:, 50]
        unfolded[:, symbol, 4] = plane[:, 51]
    for slot, symbol in enumerate(sorted(dmrs_symbols)[:D3.NOF_LS_SLOTS]):
        unfolded[:, symbol, 5] = plane[:, 42 + 2 * slot]
        unfolded[:, symbol, 6] = plane[:, 43 + 2 * slot]
    return unfolded


_CONSTELLATIONS = {}
_CONSTELLATION_LOCK = threading.Lock()


def _constellation(qm):
    """`(points, labels)` for one modulation, built once per process and frozen before publication.

    `eval_depth3` imports THIS module, so the import has to stay inside the call rather than at module level;
    the cache therefore lives here, keyed by the modulation, and is guarded because the loader may call
    `genie_labels` from several threads. The two arrays are shared by every thread that labels a genie batch, so
    they are marked read-only exactly as the geometry records are: `genie_labels` only reads them, and a stray
    write would otherwise retarget every later sample's labels.
    """
    hit = _CONSTELLATIONS.get(int(qm))
    if hit is not None:
        return hit
    import eval_depth3 as E
    with _CONSTELLATION_LOCK:
        hit = _CONSTELLATIONS.get(int(qm))
        if hit is None:
            points, labels = E.constellation(int(qm))
            points.flags.writeable = False
            labels.flags.writeable = False
            hit = (points, labels)
            _CONSTELLATIONS[int(qm)] = hit
    return hit


def genie_labels(plane, channel, alloc, dmrs_symbols, modulation, cond=False, uci=None):
    """The bits a receiver with the TRUE channel would decide, in the contract's label layout.

    Equalize each data resource element by the true channel, take the nearest constellation point
    and read its bits off. Nothing here needs information from another resource element, so a model
    that cannot fit THIS is blocked by its per-position capacity rather than by interpolation across
    frequency; a model that fits it but not the coded bits is blocked by the estimation step.
    """
    qm = D3.BITS_PER_SYMBOL[modulation]
    # THE CONSTELLATION IS CACHED PER MODULATION, UNDER A LOCK. The previous form cached it in a function
    # attribute and checked only `hasattr`, which is wrong twice over: a conditioned dataset mixes the four
    # modulations in one batch, so after the first sample every QPSK reception would have been labelled against
    # the 256QAM constellation (a wrong target, silently), and the lazy write is not safe under the loader's
    # thread pool. `E.constellation` is pure, so the lock is held only around a dictionary lookup and a build.
    points, labels = _constellation(qm)
    grid = D3.unpack_input(plane, alloc, dmrs_symbols, 1)["grid"]
    subcarriers, symbols = D3.data_re_indices(alloc, dmrs_symbols, uci)
    equalized = grid[symbols, subcarriers] / channel[subcarriers, symbols]
    nearest = np.argmin(np.abs(equalized[:, None] - points[None, :]), axis=1)
    bits = labels[nearest].reshape(-1)
    # The layout follows the MODEL's, not the modulation's: a conditioned head strides by COND_STRIDE for every
    # reception, so a genie target scattered with the per-modulation stride would put the bits in channels the
    # head does not label -- and the loss would quietly train on a plane whose non-zero entries moved.
    stride = D3.COND_STRIDE if cond else qm
    plane_out = np.zeros((D3.NOF_SUBCARRIERS, D3.NOF_SYMBOLS * stride), dtype=np.float32)
    resource_element = np.arange(bits.size) // qm
    bit_index = np.arange(bits.size) % qm
    plane_out[subcarriers[resource_element], symbols[resource_element] * stride + bit_index] = 1.0 - bits
    return plane_out


def rename_outputs(mlmodel, qm, keras_names):
    """Rename a converted model's outputs to the contract's names.

    The TensorFlow frontend names the outputs `Identity`, `Identity_1`, ... and loses the Keras
    layer names, while the contract binds them BY NAME. Renaming the spec is what makes the
    artifact self-describing, and it is checked downstream: `depth3_engine_check` refuses a bundle
    whose heads are not named `llr`, `post_eq_noise`, `ch_est` and `dmrs_noise`.

    The heads are identified by the ORDER OF THE KERAS MODEL'S OWN OUTPUT NAMES, which the caller
    passes in, and each one's shape is then CHECKED against the contract. Two simpler rules were
    tried and both are wrong. Matching by channel count alone assumes the counts are unique: the LLR
    head carries `14 * Qm` channels and the channel head `2 * 14`, so at QPSK both are 28 and the
    map collapses from four entries to three -- which surfaced as "the model has 4 outputs, the
    contract has 3" and refused the export, while at 16QAM (56), 64QAM (84) and 256QAM (112) the
    counts differ and the bug stayed hidden. Matching by a hardcoded order assumes the frontend
    emits the heads in the sequence they were built, and it does not: `Identity_1` turned out to
    carry one channel, which is the DM-RS noise head. Taking the names from the model removes both
    assumptions, and checking every shape keeps a mismatch detectable rather than silently bound to
    the wrong head -- binding the channel estimate to the LLR head would produce a receiver that
    decodes garbage from an artifact that looks correct.
    """
    import coremltools as ct
    spec = mlmodel.get_spec()
    # The Keras names the heads are known by, and the shape each must have in the contract. `llr` is
    # per resource element (14 * Qm), the channel estimate is complex (2 * 14), and the two noise
    # heads carry one value per symbol and one for the whole slot.
    contract = {"llr_flat": ("llr", NOF_SYMBOLS * qm), "noise_flat": ("post_eq_noise", NOF_SYMBOLS),
                "ch_est_flat": ("ch_est", 2 * NOF_SYMBOLS), "dmrs_noise": ("dmrs_noise", 1)}
    actual = list(spec.description.output)
    # A SUBSET OF THE CONTRACT'S HEADS IS A VALID ARTIFACT (design section 22.2). The engine binds whatever the model
    # declares, and a bundle trained without the two reporting heads is exactly what the shared measurement layer made
    # possible; requiring all four here refused the export of the very artifact depth4 was built for.
    if len(actual) != len(keras_names):
        raise ValueError(f"the model has {len(actual)} outputs and the caller named {len(keras_names)}")
    if len(actual) > len(contract):
        raise ValueError(f"the model has {len(actual)} outputs, the contract has at most {len(contract)}")
    unknown = [k for k in keras_names if k not in contract]
    if unknown:
        raise ValueError(f"the model's outputs {unknown} are not one of the contract's heads")
    missing_required = [n for k, (n, _c) in contract.items() if k not in keras_names and n == "llr"]
    if missing_required:
        raise ValueError("the model has no llr head: without it there is nothing to decode")
    for output, keras_name in zip(actual, keras_names):
        name, channels = contract.get(keras_name, (None, None))
        if name is None:
            raise ValueError(f"the model's output {keras_name} is not one of the contract's heads")
        got = output.type.multiArrayType.shape[-1]
        if got != channels:
            raise ValueError(f"output {output.name} ({keras_name}) carries {got} channels where the "
                             f"contract's {name} head has {channels}")
        ct.utils.rename_feature(spec, output.name, name)
    return ct.models.MLModel(spec, weights_dir=mlmodel.weights_dir)


def export_deployment(model, qm, path, cond=False, snr_range=None, sinr_cal=None):
    """Convert a TRAINED model to the fp16 deployment graph, with the contract's output names.

    The conversion decides the deployment precision, which is why training runs in fp32: the
    artifact is fp16 either way. The heads are renamed because the TensorFlow frontend emits
    `Identity`, `Identity_1`, ... and drops the Keras layer names, while the contract binds them BY
    NAME; `depth3_engine_check` refuses a bundle whose heads are not named `llr`, `post_eq_noise`,
    `ch_est` and `dmrs_noise`, and a bundle exported before training would carry random weights and
    still pass every shape check.
    """
    import coremltools as ct
    # ★ TRAINING-ONLY HEADS ARE CUT OFF HERE. A head that exists to shape the trunk (the CE refinement head) has
    # no contract slot, so it must not reach the bundle: the conversion is given a graph whose outputs are exactly
    # the deployed ones, and the dropped heads' weights simply do not exist in the artifact.
    keep = {"llr_flat", "noise_flat", "ch_est_flat", "dmrs_noise"}
    extra = [n for n in model.output_names if n not in keep]
    if extra:
        print(f"export drops training-only output(s): {', '.join(extra)}")
        def produced_by(tensor):
            """The layer that produced this output tensor (Keras 3 keeps it in `_keras_history`)."""
            try:
                return tensor._keras_history[0].name          # noqa: SLF001 - Keras exposes no public route
            except Exception:                                  # noqa: BLE001 - fall back to "keep it"
                return ""
        kept = {k: v for k, v in model.output.items() if produced_by(v) not in extra}
        model = tf.keras.Model(model.inputs, kept)
    # The input shape is taken from the model rather than from the folded contract: the spatialised
    # trunk has a different one, and a hardcoded shape produces a bundle that converts cleanly and
    # then fails at the binding point, which is the one place the check has to hold.
    shape = [int(d) for d in model.inputs[0].shape[1:]]
    converted = ct.convert(
        model,
        inputs=[ct.TensorType(name="x", shape=(1, *shape), dtype=np.float16)],
        compute_precision=ct.precision.FLOAT16,
        minimum_deployment_target=ct.target.iOS17,
        convert_to="mlprogram")
    # THE MAIN HEAD'S WIDTH IS THE CONDITIONING STRIDE WHEN THE MODEL IS CONDITIONED, NOT THE MODULATION's qm.
    # A conditioned trunk writes COND_STRIDE values per symbol (8), so its llr head is NOF_SYMBOLS * 8 channels;
    # the contract check used qm, which is right only when the caller happens to pass 256QAM, whose bits per
    # symbol equal the stride. Exporting a QPSK-nominal conditioned model with `--modulation QPSK` asked for
    # 14 * 2 = 28 channels against the graph's 112 and refused a valid checkpoint (2026-11-05).
    head_qm = D3.COND_STRIDE if cond else qm
    converted = rename_outputs(converted, head_qm, list(model.output_names))
    # EVERY metadata write goes through a SPEC and is REBUILT into a model, because `get_spec()` hands back a COPY:
    # mutating the returned spec and then saving the MODEL silently drops the write -- which is exactly what
    # happened on the first export of a conditioned bundle (the metadata came out holding only coremltools' own
    # keys, and the engine would have refused the bundle it declares). `rename_outputs` above already uses this
    # pattern for the head names.
    # `cond` is a PARAMETER and not the model's attribute: the export-only path loads the `.keras` from disk, and
    # an attribute set at build time does not survive that -- measured 2026-10-16, when an export-only smoke run
    # wrote a conditioned bundle with no declaration at all, which the engine would then have refused.
    # The SNR RANGE the reporting heads were trained on, declared by the bundle itself.
    #
    # WHY IT IS HARD-WIRED AND NOT MEASURED AT RUN TIME: a network does not extrapolate. Measured 2026-10-16 on a
    # model trained on a 20-32 dB corpus, replayed over prescribed-SNR grids: its published SINR read 26.4 dB at
    # 0 dB, 6 dB and 24 dB alike, because every one of those inputs is outside what it ever saw -- while the
    # classical estimator tracked the prescribed SNR to within 0.4 dB. The chain must therefore be able to ask the
    # bundle where its measurements MEAN something, and the only honest source for that answer is the training set.
    if snr_range is not None:
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_snr_range"] = f"{snr_range[0]:.2f},{snr_range[1]:.2f}"
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    if sinr_cal:
        # THE PUBLISHED SINR'S CALIBRATION, declared by the artifact that needs it (design section 19.172). The arm's
        # noise head estimates a post-equalisation noise whose scale does not coincide with the classical chain's, so
        # the scheduler was reading a SINR 1.2-3.4 dB low -- in the DEFAULT reporting mode as well, because the arm
        # publishes its own post-equalisation noise in either mode. The correction is affine IN DB
        # (`slope * published + offset`) and per modulation, which is what a slope error needs; it is measured
        # against the classical chain's publication on the corpus and validated on held-out captures, and it never
        # touches the LLR path (the main head is scaled by S_llr, not by this map).
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_sinr_cal"] = ";".join(
            f"{qm_}:{sinr_cal[qm_][0]:.4f},{sinr_cal[qm_][1]:.4f}" for qm_ in sorted(sinr_cal))
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    if cond or getattr(model, "_depth3_cond", False):
        spec = converted.get_spec()
        spec.description.metadata.userDefined["depth3_layout"] = "modcond"
        spec.description.metadata.userDefined["depth3_qm_set"] = ",".join(str(qm_) for qm_ in D3.COND_QM_ORDER)
        spec.description.metadata.userDefined["depth3_head_stride"] = str(D3.COND_STRIDE)
        # The ranges are written through a spec each time because `get_spec()` copies (see the note above): two
        # writes to two different copies would keep only the last one.
        if snr_range is not None:
            spec.description.metadata.userDefined["depth3_snr_range"] = f"{snr_range[0]:.2f},{snr_range[1]:.2f}"
        converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    # A conditioned bundle and a per-modulation 256QAM bundle have the same main-head width (14 * 8 = 112
    # channels), so a shape-based check cannot tell them apart -- and they are not interchangeable: one serves
    # every modulation, the other one. The bundle therefore SAYS which contract it follows (written above, through
    # the spec), and the engine reads the declaration rather than inferring a layout from a shared width.
    # THE GEOMETRY, declared by the bundle for the same reason the layout is: this arm assembles a FIXED-WIDTH plane
    # (612 subcarriers), so a bundle exported for another width cannot serve this build -- the runtime would reject
    # its input on every single reception, which reads as "the model is broken" rather than "the model is for another
    # cell". Declared FROM the exported shape, so the declaration cannot drift from the graph (design section 19.162).
    spec = converted.get_spec()
    spec.description.metadata.userDefined["depth3_nsc"] = str(shape[0])
    converted = ct.models.MLModel(spec, weights_dir=converted.weights_dir)
    converted.save(path)
    print(f"wrote the fp16 deployment graph to {path} (input {[1, *shape]}, depth3_nsc={shape[0]})")


def cli_sinr_cal(args):
    """{"bits_per_symbol": (slope, offset)} from `--sinr-cal`, or None."""
    if not args.sinr_cal:
        return None
    out = {}
    for entry in args.sinr_cal.split(";"):
        if not entry.strip():
            continue
        qm_text, _, cal_text = entry.partition(":")
        slope_text, _, offset_text = cal_text.partition(",")
        qm = int(qm_text)
        if qm not in D3.COND_QM_ORDER:
            raise SystemExit(f"--sinr-cal: {qm} is not one of the declared modulations {D3.COND_QM_ORDER}")
        slope, offset = float(slope_text), float(offset_text)
        # A SANITY BAND, because this string is applied to the PUBLISHED SINR (a positive number in dB) and a
        # coefficient pair fitted in the NOISE space (negative dB) has its offset negated on the way here. Measured
        # 2026-10-18: declaring the noise-space offset as-is turned a reception published at 17.07 dB into 20.83
        # where it belonged at 18.20; the scheduler believed it, granted 64QAM for the UE's signalling, and attach
        # failed on every leg that carried the declaration. The band cannot tell right from wrong by itself, so the
        # transform is also PRINTED example by example below.
        if not (0.0 < slope <= 3.0):
            raise SystemExit(f"--sinr-cal: slope {slope} for Qm={qm} is outside (0, 3]; a slope is a dB-for-dB "
                             f"correction and cannot be zero, negative or large")
        if abs(offset) > 20.0:
            raise SystemExit(f"--sinr-cal: offset {offset:+.3f} dB for Qm={qm} is larger than 20 dB; the published "
                             f"SINR would move into a range nothing validated")
        out[qm] = (slope, offset)
        print(f"sinr-cal: the published SINR is corrected as {slope:.4f}*x {offset:+.4f}, i.e. "
              f"10 dB -> {slope * 10.0 + offset:.2f} dB, 20 dB -> {slope * 20.0 + offset:.2f} dB, "
              f"25 dB -> {slope * 25.0 + offset:.2f} dB (Qm={qm}; it multiplies the SINR, not the noise)")
    return out


def cli_snr_range(args):
    """The `--snr-range` knob as a pair, or None. Shared by the two export paths so they cannot disagree."""
    if not args.snr_range:
        return None
    low, high = (float(value) for value in args.snr_range.split(","))
    if high <= low:
        raise SystemExit(f"--snr-range {args.snr_range}: the upper bound is not above the lower one")
    return (low, high)


def export_buckets(args, parser, source=None):
    """Exports the same weights as several narrow-band artefacts (model_network_design section 49).

    Cropping the plane to the allocation plus a four-PRB margin reproduces the full-band outputs bit for bit -- measured,
    not assumed -- and the trunk is fully convolutional, so one weight set builds at any input width. The buckets exist
    only because the ANE needs a static shape; the runtime picks the smallest one that covers `12 * prb + 96`
    subcarriers. Nothing here retrains anything.
    """
    if not args.export_buckets:
        return
    if not args.export_probe:
        parser.error("--export-buckets derives its file names from --export-probe")
    weights = args.export_only or os.path.join(args.out, "depth3_best.keras")
    # TWO INDEPENDENT FLAGS, NOT ONE. `aux_heads` also builds the per-RE noise head, which a two-head bundle keeps --
    # only the two REPORTING heads go away. Mirroring the single flag gave the bucket graph 94 weights against the
    # source's 96 and the export refused, which is the check doing its job.
    aux = not args.no_aux
    reporting = not args.no_reporting_heads
    # THE SHAPE COMES FROM THE GRAPH, NOT FROM THE COMMAND LINE. `--in-channels` defaults to 0 because the layouts
    # disagree about it, and `--c-hid` defaults to 64, so a bucket built from the flags alone produced a 64-channel
    # graph with a zero-channel entry conv and refused to load. The source model knows both.
    if source is not None:
        entry = source.get_layer("entry_1x1").kernel.shape
        c_hid, in_channels = int(entry[3]), int(entry[2])
        # THE BLOCK TYPE COMES FROM THE SOURCE TOO, AND THE DEPTH COUNT HAS TO FOLLOW IT. A dense trunk names its 3x3
        # layers `cv{i}_3x3` rather than `dw{i}_3x3`, so counting only `dw` gave zero blocks and the bucket graph then
        # had six weights against the source's ninety-six -- which is how this was found (2026-11-07). `sq{i}_1x1`
        # marks the bottleneck variant, whose 3x3 layers are also `cv`.
        dense = any(layer.name.endswith("_3x3_d1") and layer.name.startswith("cv") for layer in source.layers)
        # THE FACTOR, NOT A FLAG. `dense_bottleneck` is an integer divisor, so reading the presence of the squeeze
        # layer as a boolean rebuilt the bucket at c/1 -- the full width -- and the copy then refused on shape. The
        # divisor is recoverable from the squeeze itself: its output channels are c_hid over the factor.
        bottleneck = 0
        if any(layer.name.startswith("sq") for layer in source.layers):
            bottleneck = max(2, c_hid // int(source.get_layer("sq0_1x1").kernel.shape[3]))
        blocks = sum(1 for layer in source.layers
                     if layer.name.startswith(("dw", "cv")) and "_3x3_" in layer.name)
        # THE HEADS COME FROM THE SOURCE TOO, for the same reason the width and the depth do: a source trained with
        # `--no-reporting-heads` has two heads fewer than the default graph, and building the bucket with the default
        # gives it four extra weights that the copy then refuses (observed as "the bucket graph has 100 weights, the
        # source 96"). Reading the source makes the export follow the model instead of the command line.
        names = {layer.name for layer in source.layers}
        reporting = any("ch_est" in name or "dmrs_noise" in name for name in names)
        aux = any("post_eq_noise" in name for name in names)
    else:
        c_hid, in_channels, blocks = args.c_hid, 8, args.blocks
        dense, bottleneck = args.dense_blocks, args.dense_bottleneck
    for width in [int(w) for w in args.export_buckets.split(",") if w.strip()]:
        narrow = build(width=width, blocks=blocks, qm=D3.BITS_PER_SYMBOL[args.modulation], smooth_k=args.smooth_k,
                       aux_heads=aux, c_hid=c_hid, dtype=args.dtype,
                       layout=args.layout, in_channels=in_channels, cond=args.modcond,
                       reporting_heads=reporting, dense=dense, dense_bottleneck=bottleneck,
                       dilations=args.dilations, ksize=args.kernel_size, groups=args.groups,
                       noise_to_llr=args.noise_to_llr, ce_aux=args.ce_head, llr_post=args.llr_post, cst_head=args.cst_head, sym_aux=args.sym_aux, mlp_expand=args.mlp_expand, se=args.se,
                    se_ratio=args.se_ratio, dense_skip=args.dense_skip)
        # WEIGHTS ARE COPIED, NOT RELOADED. A `.keras` archive carries the width the graph was built with, and loading
        # it into a graph built for another width fails on every layer past the divergence (41 objects in the first
        # attempt). The tensor shapes do not depend on the width -- they are 1x1 and depthwise convolutions -- so
        # assigning them in order is both correct and explicit about why.
        if source is not None:
            # TRAINING-ONLY HEADS ARE DROPPED HERE. A head that exists to shape the trunk (the CE refinement head)
            # has no place in the deployed graph, so the source carries a few weights the bucket does not. They are
            # filtered out by layer name and reported, rather than failing the whole export on a count mismatch.
            dst_layers = {w.path.rsplit("/", 1)[0] for w in narrow.weights}
            kept = [w for w in source.weights if w.path.rsplit("/", 1)[0] in dst_layers]
            dropped = sorted({w.path.rsplit("/", 1)[0] for w in source.weights
                              if w.path.rsplit("/", 1)[0] not in dst_layers})
            if dropped:
                print(f"export drops training-only layer(s): {', '.join(dropped)}")
            if len(kept) != len(narrow.weights):
                raise ValueError(f"the bucket graph has {len(narrow.weights)} weights, the source {len(kept)} "
                                 f"after dropping {dropped}")
            for w_src, w_dst in zip(kept, narrow.weights):
                if tuple(w_src.shape) != tuple(w_dst.shape):
                    raise ValueError(f"weight shape mismatch: {w_src.path} {tuple(w_src.shape)} against "
                                     f"{w_dst.path} {tuple(w_dst.shape)}")
                w_dst.assign(w_src)
        else:
            narrow.load_weights(weights)
        path = f"{args.export_probe.rsplit('.', 1)[0]}_w{width}.mlpackage"
        export_deployment(narrow, D3.BITS_PER_SYMBOL[args.modulation], path, cond=args.modcond,
                          snr_range=cli_snr_range(args), sinr_cal=cli_sinr_cal(args))
        print(f"bucket width {width}: {path}")


def load_snr_nominal(path):
    """`snr_nominal` from either form of dataset path.

    The trainer accepts a directory of `.npy` arrays (memory-mapped, which is how a dataset larger than RAM is
    used) or the original compressed archive. Two places downstream of the batch loader read this one array
    directly, and they were the last to know about only the archive -- a directory reached them as
    `IsADirectoryError` after three epochs had already been trained.
    """
    if os.path.isdir(path):
        return np.load(os.path.join(path, "snr_nominal.npy"), mmap_mode="r")
    return np.load(path, allow_pickle=True)["snr_nominal"]


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--train", default="", help="training dataset; not needed with --export-only")
    parser.add_argument("--val", default="", help="validation dataset; not needed with --export-only")
    parser.add_argument("--out", default="", help="run directory; not needed with --export-only")
    parser.add_argument("--modulation", default="256QAM")
    parser.add_argument("--modcond", action="store_true",
                        help="train the MODULATION-CONDITIONED model (design section 19.102): one bundle for "
                             "every modulation, with the Qm as four one-hot input channels and a main head "
                             "whose stride is COND_STRIDE. The dataset must carry mixed modulations (its `qm` "
                             "column already does) and --modulation only picks the sample's DEFAULT where the "
                             "file has none.")
    parser.add_argument("--snr-range", default="",
                        help="the SNR range (dB, 'min,max') the reporting heads were trained on, DECLARED in the "
                             "bundle as depth3_snr_range so the chain can tell a measurement the model is "
                             "entitled to make from one it extrapolated. Empty means: derive it from the training "
                             "set's own snr_nominal percentiles (1st to 99th) when training, and declare nothing "
                             "with --export-only -- a bundle that declares no range cannot be gated, and the "
                             "receiver says so once per arm.")
    parser.add_argument("--sinr-cal", default="",
                        help="affine calibration of the PUBLISHED SINR, per modulation, in dB: "
                             "'2:slope,offset;4:...;6:...;8:...'. Measured against the classical chain's own "
                             "publication (design section 19.172); it corrects the reporting path only.")
    parser.add_argument("--blocks", type=int, default=18)
    # THE FREE ARCHITECTURE KNOB. Dilation changes neither the parameters nor the multiply-adds, only
    # how far each block looks, and the default cycle reaches 127 subcarriers against a measured
    # coherence length of 150-300. Empty means the default cycle, so old command lines reproduce.
    parser.add_argument("--dilations", default="",
                        help="comma-separated dilation cycle along the subcarrier axis "
                             "(default: 1,2,4,8 from the module constant)")
    # THE KERNEL SIDE IS A LEVER (architecture_levers_beyond_width L2): k=5 gives each mixing layer a
    # 5-subcarrier reach instead of 3 at 25/9 the weights of a k=3 depthwise, and a k=5 ANE kernel was
    # measured at 219-229 us. Default 3 keeps every old command line and checkpoint unchanged.
    parser.add_argument("--kernel-size", type=int, choices=(3, 5, 7), default=3, metavar="K",
                        help="spatial kernel side of the trunk's mixing layers (default 3)")
    # g=1 IS THE DENSE 3x3 that bought +18.6% and g=c-hid is depthwise; the doc lists 2 and 4 as the useful
    # middle. Kept separate from --kernel-size so the two levers are measured one at a time.
    parser.add_argument("--cst-head", action="store_true",
                        help="constellation-aware head: analytic 4*s/sigma^2 linear term plus a learned residual "
                             "(lever A; the exact max-log posterior needs one graph per modulation)")
    parser.add_argument("--dense-skip-init", choices=("random", "sum"), default="random",
                        help="how the dense connection's 2C->C projection starts. `sum` sets its kernel to [I; I] and "
                             "its bias to zero, so at step zero the block reproduces the baseline's residual y + h -- a "
                             "curriculum start rather than a change of graph")
    parser.add_argument("--dense-skip-depth", type=int, default=1, metavar="K",
                        help="how many earlier block outputs the dense connection also exposes (1 = current C: state "
                             "plus the block's own output; 2 also shows the previous block's output). Each extra "
                             "level costs one more C->C slice in the projection")
    parser.add_argument("--dense-skip-entry", action="store_true",
                        help="with the dense skip, also feed each block the ENTRY features (the received signal plus "
                             "the Qm conditioning, before any nonlinearity). Implies --dense-skip; costs one more "
                             "C->C 1x1 per block")
    parser.add_argument("--dense-skip", action="store_true",
                        help="replace each residual sum with a dense cross-block connection: concatenate the block "
                             "output with the running state and project the pair back to c_hid with a 1x1. Later "
                             "blocks then see earlier features directly (lever C)")
    parser.add_argument("--se", action="store_true",
                        help="add squeeze-excite channel attention to every block (lever B). It re-weights the "
                             "channels from a global average instead of mixing them more, so it stacks with "
                             "--mlp-expand; about two hundred parameters and no extra per-element MACs")
    parser.add_argument("--se-ratio", type=int, default=8, metavar="R",
                        help="bottleneck ratio of the squeeze-excite gate (channels // R); default 8")
    parser.add_argument("--mlp-expand", type=float, default=1.0, metavar="R",
                        help="expansion ratio of the pointwise block (1 = the current single 1x1; 2 doubles the "
                             "channel width inside the residual, adding a second 1x1 and a ReLU per block)")
    parser.add_argument("--sym-aux", action="store_true",
                        help="add a TRAINING-ONLY symbol-regression head (lever D); the loss weight is --w-sym and "
                             "the head is dropped at export, so the bundle is unchanged")
    parser.add_argument("--w-sym", type=float, default=0.1,
                        help="loss weight of the symbol head (only used with --sym-aux)")
    parser.add_argument("--llr-post", action="store_true",
                        help="add a residual 3-tap filter on the LLR plane (lever B; ~13 k parameters, applied on the "
                             "logits so the contract's bound is unchanged)")
    parser.add_argument("--ce-head", action="store_true",
                        help="add a TRAINING-ONLY channel-estimation head (supervised by the true H, weight --w-ch); "
                             "it is dropped at export, so the bundle is unchanged")
    parser.add_argument("--noise-to-llr", action="store_true",
                        help="feed the post_eq_noise head back as an extra input channel of the LLR demapper "
                             "(lever L6; the head separates errors at fixed |LLR|, see architecture_levers section 11)")
    parser.add_argument("--groups", type=int, default=1, metavar="G",
                        help="groups of the trunk's mixing conv (1 = dense, c-hid = depthwise); "
                             "must divide the channel count")
    parser.add_argument("--layout", choices=("folded", "spatial", "probe8", "unfolded"),
                        default="spatial",
                        help="'folded' is the superseded 64-channel contract, kept so the recorded "
                             "comparisons stay reproducible. 'spatial' is the contract now: the "
                             "symbol axis is a spatial axis, 12 channels per symbol. 'probe8' is "
                             "the R4.4 diagnostic arm that isolated the locality (8 channels); "
                             "'unfolded' is its old name and still works")
    parser.add_argument("--dense-blocks", action="store_true",
                        help="dense 3x3 instead of depthwise. Section 25.3 measured the same "
                             "latency for 5.7x the FLOPs, and depthwise is what caps the "
                             "per-position capacity the diagnosis points at")
    parser.add_argument("--fuse-relu", action="store_true",
                        help="carry the trunk's first activation inside the 3x3 convolution instead of as a "
                             "separate ReLU. Every layer boundary on the ANE is a tensor the runtime has to "
                             "materialise, and the c32 bucket graph is 101 compute operations of which 37 are "
                             "exactly these ReLUs. No weights change, so an existing snapshot exports to the "
                             "fused graph without retraining")
    parser.add_argument("--dense-bottleneck", type=int, default=0, metavar="N",
                        help="dense 3x3 on c_hid/N channels, squeezed into and expanded out of that "
                             "width inside the block. Keeps the joint space-and-channel mixing that "
                             "depthwise cannot express while the weights grow as C*C/N instead of "
                             "9*C*C: about 2.5x the depthwise block at N=2, cheaper than it at N=4. "
                             "Implies --dense-blocks; 0 keeps the previous architectures")
    parser.add_argument("--in-channels", type=int, default=0,
                        help="input channels of the unfolded trunk. 0 takes it from the batches, "
                             "which is the only value that cannot disagree with the data; the "
                             "export-only path has no batches and passes it explicitly")
    parser.add_argument("--c-hid", type=int, default=64,
                        help="trunk width. The contract freezes the tensor shapes, not the width "
                             "inside the network, and a per-position decision over a 256QAM "
                             "constellation needs more capacity than 64 channels gave it")
    parser.add_argument("--epochs", type=int, default=150)
    parser.add_argument("--batch", type=int, default=8)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--lr-decay", choices=("none", "cosine"), default="none",
                        help="cosine: decay the learning rate from --lr to --lr-min across this run's epochs; the "
                             "schedule is computed from the epoch number, so a resumed run continues it correctly")
    parser.add_argument("--lr-min", type=float, default=None,
                        help="final learning rate for --lr-decay cosine (default: --lr / 10)")
    parser.add_argument("--lr-decay-from", type=int, default=1,
                        help="epoch at which the decay starts at --lr; use the first epoch of a continuation so the "
                             "whole continuation is decayed instead of inheriting the position of the original run")
    parser.add_argument("--max-train", type=int, default=0, help="cap the training samples")
    parser.add_argument("--max-val", type=int, default=64)
    # XLA IS ON BY DEFAULT, AND `--no-jit` TURNS IT OFF. It changes only how the step is compiled, never the
    # exported bundle, and it is worth 1.3x to 2x of the epoch time. Leaving it opt-in meant every hand-written
    # command that forgot the flag trained at half speed with no visible symptom -- one arm of a comparison was
    # measured that way (the cliff run), which is exactly the confound a default is supposed to prevent.
    #
    # THE DEFAULT IS RESOLVED PER PLATFORM, NOT CHANGED: on Linux it is exactly "on", as it always was; on macOS it
    # is off unless this flag is written out, because tensorflow-metal has no XLA device (`resolve_xla`). Writing
    # `--jit` there still compiles, after a self-test and with a one-shot fallback.
    parser.add_argument("--jit", action=argparse.BooleanOptionalAction, default=True,
                        help="compile the training step with XLA (default: on; use --no-jit to disable). "
                             "On macOS the resolved default is off, because tensorflow-metal exposes no XLA "
                             "device; an explicit --jit there is honoured and self-tested. "
                             "TRAINING ONLY -- the bundle is exported from the Keras layers and the weights, so "
                             "this cannot change what the ANE runs (design section 19.218)")
    # THE DEVICE IS A CHOICE BECAUSE THE FASTER ONE IS NOT ALWAYS THE GPU. Measured on an M4 Pro with
    # tensorflow-metal 1.2.0 / TensorFlow 2.18.1, c_hid 32 and batch 8: 4.93 s/epoch on the Metal GPU against
    # 2.63 s/epoch on the CPU. Nothing about the graph changes here -- only which device the runtime may use.
    parser.add_argument("--no-gpu", action="store_true",
                        help="hide every GPU device and run on the CPU. OFF by default on every platform, so an "
                             "existing Ubuntu/CUDA command line keeps selecting its GPU exactly as before; "
                             "when it IS given the devices are hidden on Linux too. On macOS it is the faster "
                             "path for this network at the deployment width -- the Metal plugin measured 1.87x "
                             "SLOWER than the CPU at c_hid 32, batch 8, and only wins at c_hid 128 with "
                             "batch 32 -- and the plugin still lists its GPU after being hidden, which is why "
                             "the startup banner prints the placement rather than the device list")
    parser.add_argument("--target", choices=("coded", "genie"), default="coded",
                        help="'coded' is the contract's task. 'genie' replaces the label with the "
                             "hard decisions a receiver with the true channel would make, which "
                             "isolates per-position capacity from the estimation step")
    parser.add_argument("--snr-min", type=float, default=None,
                        help="keep only samples at or above this nominal SNR. A low-SNR label is "
                             "decided by the noise, so training on it cannot converge to a low "
                             "error rate; the high-SNR slice is the one with a reachable answer")
    parser.add_argument("--snr-max", type=float, default=None)
    # HOW THE LLR TERM IS AVERAGED. "bits" is the historical objective: a sample's pull on the gradient is
    # proportional to its coded-bit count, which leaves narrow allocations with 2.4% of the weight although they are
    # 20% of the samples (improvement plan section 100). "sample" gives every reception one vote; "sqrt" is between.
    # DENSE frequency smoothing over one coherence bandwidth, added as a parallel branch of the input (0 = off).
    parser.add_argument("--smooth-k", type=int, default=0, metavar="K",
                        help="depthwise frequency kernel of K taps added as an input branch (0 disables)")
    parser.add_argument("--loss-norm", choices=sorted(L.LOSS_NORMS), default="bits",
                        help="average the LLR term per bit (bits), per reception (sample), or in between (sqrt)")
    parser.add_argument("--w-llr", type=float, default=L.DEFAULT_WEIGHTS["llr"])
    parser.add_argument("--w-ch", type=float, default=L.DEFAULT_WEIGHTS["ch_est"])
    parser.add_argument("--w-noise", type=float, default=L.DEFAULT_WEIGHTS["post_eq_noise"])
    parser.add_argument("--no-aux", action="store_true", help="LLR head only, for the R2.4 ablation")
    parser.add_argument("--no-val", action="store_true",
                        help="skip the per-epoch validation pass. Selection is then meaningless and the run relies on "
                             "the tail weight average at the end; `--monitor-every` still gives a periodic health check")
    parser.add_argument("--monitor-every", type=int, default=0, metavar="N",
                        help="with --no-val: run the validation/BLER pass every N epochs (0 = never). This is the "
                             "health check that replaces the per-epoch number")
    parser.add_argument("--select-on", choices=("val", "band", "none"), default="val",
                        help="which epoch is the best one: the validation loss, or the BER of the low-SNR band")
    parser.add_argument("--resume", action="store_true",
                        help="continue from <out>/depth3_last.keras if it exists, instead of starting over")
    parser.add_argument("--patience", type=int, default=0,
                        help="SNAPSHOT when the validation loss has not improved for this many epochs, then reset the "
                             "counter and keep training (it no longer terminates -- see --stop-on-early); 0 disables "
                             "the trigger")
    parser.add_argument("--min-delta", type=float, default=0.0,
                        help="improvement smaller than this does not count. 0 keeps the historical behaviour; raising "
                             "it makes the trigger fire sooner, and it is one of the two variables --control can change "
                             "while a run is going")
    parser.add_argument("--stop-on-early", action="store_true",
                        help="the OLD behaviour: end the run when the trigger fires instead of snapshotting and going "
                             "on. Off by default because a trigger is a fact about the curve, and a run that stops on it "
                             "cannot report what the next epochs would have done")
    parser.add_argument("--control", default="",
                        help="JSON re-read every epoch: {\"patience\": n, \"min_delta\": x}. Lets the trigger be "
                             "tightened or loosened without restarting the run; values present override the command "
                             "line for that epoch and are recorded in history")
    parser.add_argument("--snapshot-every", type=int, default=0,
                        help="write a full snapshot every this many epochs regardless of the trigger. These are the "
                             "points a training recipe can be compared at: same weights, same optimizer state, same "
                             "history up to that epoch")
    parser.add_argument("--workers", type=int, default=0,
                        help="threads that build batches while the accelerator runs; 0 keeps the loop serial on "
                             "Linux and selects a quarter of the cores (capped at three) on macOS, where the "
                             "loader is what the GPU waits on and more threads measured slower; an explicit N is "
                             "used as given on both")
    parser.add_argument("--prefetch", type=int, default=0,
                        help="batches to build ahead when --workers is set (defaults to 2x the workers)")
    parser.add_argument("--low-rank", type=int, default=0,
                        help="factorise the pointwise 1x1 into (c x rank) then (rank x c); 0 keeps the plain 1x1, "
                             "8 removes 512 of the 1024 multiply-adds per block at c_hid 32")
    parser.add_argument("--no-reporting-heads", action="store_true",
                        help="DROP the two reporting heads (ch_est, dmrs_noise) and keep post_eq_noise, which is "
                             "what scales the logits into LLRs. The publication then comes from the shared "
                             "measurement layer (design sections 22.2 and 19.263).")
    parser.add_argument("--llr-linear", action="store_true",
                        help="make the LLR head linear and bound it by clamping in the loss, "
                             "instead of a tanh whose gradient vanishes where confident bits live")
    parser.add_argument("--scale-init", type=float, default=1.0,
                        help="initial gain of the residual LayerScale. The refiner uses 0.1 because "
                             "it learns a correction on top of its input; this network learns the "
                             "whole mapping from scratch, and at 0.1 the eighteen blocks each shrink "
                             "their branch tenfold, so training stalls")
    parser.add_argument("--no-fold-scale", action="store_true",
                        help="keep the LayerScale as a separate multiply. Costs about 200 us on "
                             "the ANE (section 45.8); the default folds it into the convolution")
    parser.add_argument("--init", default="",
                        help="start from these weights instead of a fresh initialisation. A run "
                             "whose curve has not flattened can then be EXTENDED rather than "
                             "guessed at, which is what makes the epoch count a measurement")
    parser.add_argument("--export-only", default="",
                        help="export this trained .keras to the fp16 deployment graph and exit")
    parser.add_argument("--export-probe", default="",
                        help="also write the graph as a CoreML mlpackage, so the ANE probe "
                             "measures the artifact this script trains rather than a copy of it")
    parser.add_argument("--export-buckets", default="",
                        help="comma-separated subcarrier widths to ALSO export, one artefact each, from the same "
                             "weights (model_network_design section 49). The trunk is fully convolutional, so cropping "
                             "the plane to the allocation plus four PRB is bit-exact and needs no retraining; the "
                             "buckets exist only because the ANE wants a static shape. Ignored without --export-probe, "
                             "which names the base artefact the bucket names are derived from.")
    parser.add_argument("--probe-graph", default="",
                        help="build the graph from the architecture flags, export it and EXIT "
                             "without training, so ANE placement and latency can be measured "
                             "before a training run is paid for. The weights are random: the "
                             "artifact is for measurement and must never receive")
    parser.add_argument("--no-residual", action="store_true",
                        help="plain block stack. It collapses (section 12.9); kept so the "
                             "failure can be reproduced rather than remembered")
    parser.add_argument("--dtype", choices=("float32", "float16"), default="float32",
                        help="training dtype. The exported artifact is fp16 regardless")
    parser.add_argument("--bler-every", type=int, default=5,
                        help="evaluate coded BLER every N epochs. Decoding dominates the "
                             "validation cost, so it is sampled rather than run every epoch")
    parser.add_argument("--bler-check", action="store_true",
                        help="decode PERFECT LLRs on a few validation samples and exit. The rate "
                             "must be zero, which is what proves the metric can reach zero rather "
                             "than merely being small")
    parser.add_argument("--snr-weight", type=float, default=0.0, metavar="LAMBDA",
                        help="weight each TRAINING sample by 1 + LAMBDA * exp(-((snr - THR_QM[modulation]) / sigma)^2), "
                             "with sigma from --snr-sigma and the thresholds measured on the corpus. The delivered-bits "
                             "metric is decided by the receptions near the decoder threshold -- measured 2026-11-07, fifteen "
                             "256QAM receptions at 25 dB carried the whole six percent gap between two epochs -- while the "
                             "global loss averages them away. Validation always reports the unweighted loss so runs stay "
                             "comparable. 0 disables the weighting")
    # THE WINDOW CAN BE MOVED OFF THE MEASURED THRESHOLD. --snr-weight centres on each modulation's own 50% crossing,
    # which for 256QAM is 20.91 dB; the whole-leg attribution puts 96% of the delivered-bit deficit at 20-26 dB, i.e.
    # ABOVE that centre, where the Gaussian has already decayed to about 1.1x. This knob moves the 256QAM centre only;
    # every other modulation keeps its measured threshold.
    parser.add_argument("--snr-center", type=float, default=None, metavar="DB",
                        help="override the SNR-weight centre for 256QAM only (default: that modulation's measured "
                             "threshold, 20.91 dB)")
    parser.add_argument("--width-weight", type=float, default=0.0, metavar="LAMBDA_W",
                        help="the SECOND axis of the loss weight: multiply each TRAINING sample's weight by "
                             "1 + LAMBDA_W for allocations of at most --width-max PRBs. The census puts every unit "
                             "the model loses on the narrowest allocations (256QAM at 3 or 8 PRBs), where the channel "
                             "estimate has the fewest subcarriers to average. Multiplies --snr-weight when both are "
                             "on. Validation stays unweighted. 0 disables it")
    parser.add_argument("--width-max", type=int, default=8, metavar="PRB",
                        help="the allocation width, in PRBs, at or below which --width-weight applies (default 8, "
                             "which is the census bin edge that carries the losses)")
    parser.add_argument("--snr-sigma", type=float, default=3.0, metavar="DB",
                        help="width of the threshold window for --snr-weight, in dB (default 3.0)")
    parser.add_argument("--group-report", type=int, default=5, metavar="N",
                        help="print the N worst (modulation x SNR band x transport-block size) GROUPS by validation "
                             "bit-cross-entropy, with how many samples and what share of the bits each carries. The "
                             "global validation loss cannot rank models whose difference lives in one group: measured "
                             "2026-11-07, two epochs eight tenths of a percent apart on the global loss were six percent "
                             "apart on delivered bits, and the whole difference was fifteen 256QAM receptions at 25 dB "
                             "carrying 4992-bit blocks. 0 disables the report")
    parser.add_argument("--bler-samples", type=int, default=24,
                        help="validation samples to decode for the coded BLER. Decoding is far "
                             "more expensive than a forward pass, so this is a subset; 0 skips it")
    parser.add_argument("--seed", type=int, default=20261013)
    args = parser.parse_args()
    args.dilations = parse_dilations(args.dilations)
    # WHAT THE PROCESS WAS LAUNCHED WITH. The log is the only lasting record of a multi-hour run, and a
    # restart is where flags drift: on 2026-11-03 a restart dropped `--bler-samples 0`, the default 24
    # took over, and the first decode killed a run that had been healthy for fourteen epochs. The old
    # per-modulation logs cannot say what their batch was for exactly this reason.
    print("argv: " + " ".join(sys.argv[1:]), flush=True)
    # THE LOGGER IS QUIET BEFORE THE FIRST TENSOR IS BUILT. It used to be set below, after the two banner lines;
    # it moves up by two statements so the macOS self-test below cannot print framework chatter into a log that
    # is the record of a multi-hour run. Nothing between the old and the new position produces output.
    tf.get_logger().setLevel("ERROR")
    # ★ THE DEVICE CHOICE COMES FIRST, BEFORE ANY OP. `set_visible_devices` is only honoured before the
    # runtime is initialised, so `--no-gpu` cannot wait until the model is built (see `hide_gpus`).
    if args.no_gpu and hide_gpus():
        print("gpus: all GPU devices hidden (--no-gpu); the step will run on the CPU", flush=True)
    # ★ THE MACOS BACKEND IS ASKED FOR BEFORE ANY OP RUNS, AND ITS BANNER IS PRINTED LATER. Growth-based allocation
    # has to be requested before the runtime is initialised, and the XLA self-test a few lines down builds tensors
    # -- taking the setting here is what keeps a `--jit` run from having its memory-growth request silently
    # refused. What was found is reported below, once the XLA and pipeline decisions it describes are known.
    runtime = configure_macos_runtime() if IS_DARWIN else None
    # ★ THE XLA DECISION, BEFORE ANYTHING IS BUILT. On Linux this resolves to the flag's value, unchanged. macOS
    # takes it here because the graph it affects is the training step and the reason for a change has to be in
    # the log before the first epoch, not in a traceback at the first batch (see `resolve_xla`).
    args.jit, xla_note = resolve_xla(args.jit, sys.argv[1:])
    if IS_DARWIN and args.jit:
        xla_ok, xla_why = xla_self_test()
        if not xla_ok:
            args.jit = False
            xla_note = f"off -- the requested XLA step could not be compiled on this backend ({xla_why})"
    print(f"xla: {'on' if args.jit else 'off'}", flush=True)
    if xla_note:
        print(f"xla: {xla_note}", flush=True)
    # ★ THE BATCH-PIPELINE CONCURRENCY, RESOLVED ONCE. On Linux this is exactly the `args.workers` /
    # `args.prefetch or 2 * args.workers` pair the epoch loop used to spell out; on macOS it supplies the
    # default thread count that stops the Apple GPU waiting for batches (see `pipeline_defaults`).
    workers, prefetch = pipeline_defaults(args)
    tf.random.set_seed(args.seed)
    if IS_DARWIN:
        report_macos_runtime(runtime, workers, prefetch, args.jit)
        if args.dtype != "float32":
            # NOT A WARNING ABOUT A CHANGE: the arithmetic is the Ubuntu arithmetic. It says so, because a reader of a
            # macOS log has to be able to tell that `--dtype float16` here means the same graph and the same loss
            # precision as it does there, and that build()'s warning about fp16 stalling still applies.
            print(f"platform: --dtype {args.dtype}: the loss and the LLR scaling are still computed in float32 and "
                  f"ChannelGate still multiplies in float32 before casting back, so this run's numbers are the "
                  f"Ubuntu run's; build() still documents that fp16 training stalls", flush=True)
    if args.layout == "unfolded":
        # The name the R4.4 runs were recorded under. Kept so their command lines still run.
        print("--layout unfolded is the old name of probe8")
        args.layout = "probe8"

    if args.export_only:
        exported = tf.keras.models.load_model(
            args.export_only, compile=False, custom_objects=CUSTOM_OBJECTS)
        if not args.export_probe:
            parser.error("--export-only needs --export-probe")
        # The range has to come from the knob on this path: --export-only loads a `.keras` and no dataset, so
        # there are no `snr_nominal` percentiles to derive it from (see the note where training derives it).
        export_deployment(exported, D3.BITS_PER_SYMBOL[args.modulation], args.export_probe,
                          cond=args.modcond, snr_range=cli_snr_range(args), sinr_cal=cli_sinr_cal(args))
        export_buckets(args, parser, source=exported)
        return 0

    qm = D3.BITS_PER_SYMBOL[args.modulation]
    # The channel count follows the packing the layout names, so the trunk cannot be built for a
    # layout the batches do not produce. The batches are then checked against it rather than
    # believed: a `--layout` that disagrees with the data is the kind of error that trains happily
    # and deploys into a shape the seam cannot fill.
    in_channels = args.in_channels or {"folded": D3.NOF_CHANNELS, "probe8": 8}.get(
        args.layout, D3.NOF_CHANNELS_SPATIAL)
    if args.modcond:
        # The conditioning is FOUR EXTRA INPUT CHANNELS, appended by the dataset at sample time (it is the sample
        # that knows its Qm). The model therefore takes the data's channel count, and this is the one place that
        # adds them -- build() must not widen again, and the check below compares the two numbers.
        in_channels += len(D3.COND_QM_ORDER)

    if args.probe_graph:
        # Architecture -> ANE latency, before paying for a training run. The weights are random and
        # the artifact must never reach a receiver: it exists to be measured. `--in-channels` and
        # the architecture flags are the whole input, so no dataset is needed.
        print(f"probe graph: layout {args.layout}, {in_channels} input channels, "
              f"c_hid {args.c_hid}, {args.blocks} blocks -- RANDOM WEIGHTS, measure only")
        probe_model = build(blocks=args.blocks, c_hid=args.c_hid, qm=qm, aux_heads=not args.no_aux, low_rank=args.low_rank,
                            reporting_heads=not args.no_reporting_heads,
                            residual=not args.no_residual, dtype=args.dtype,
                            fold_scale=not args.no_fold_scale, scale_init=args.scale_init,
                            llr_linear=args.llr_linear, dense=args.dense_blocks,
                            dense_bottleneck=args.dense_bottleneck, fuse_relu=args.fuse_relu,
                            layout=args.layout, in_channels=in_channels, cond=args.modcond,
                            dilations=args.dilations, ksize=args.kernel_size, groups=args.groups,
                            noise_to_llr=args.noise_to_llr, ce_aux=args.ce_head, llr_post=args.llr_post, cst_head=args.cst_head, sym_aux=args.sym_aux, mlp_expand=args.mlp_expand, se=args.se, se_ratio=args.se_ratio, dense_skip=args.dense_skip or args.dense_skip_entry or args.dense_skip_depth > 1,
                    dense_skip_entry=args.dense_skip_entry,
                    dense_skip_depth=args.dense_skip_depth,
                            smooth_k=args.smooth_k)
        export_deployment(probe_model, qm, args.probe_graph, cond=args.modcond)
        return 0

    # ★ THE ONE MACOS CONFIGURATION THAT CANNOT RUN. `--dtype float16` takes the Metal plugin into its
    # half-precision MPSGraph path, which SEGFAULTS on the first training step (EXC_BAD_ACCESS inside
    # `metal_plugin::MPSBinaryOpBase<Eigen::half>::Compute`, i.e. an fp16 multiply) -- reproduced with the
    # UNMODIFIED script, so it is an upstream plugin bug and not something this trainer can work around.
    # A segfault leaves no traceback, only a macOS crash dialog, which is how five of them were produced while
    # measuring this; refusing here turns that into one line with the reason. Training in float32 costs
    # nothing at deployment: `export_deployment` converts the bundle to fp16 regardless.
    if IS_DARWIN and args.dtype != "float32" and not allow_metal_fp16():
        parser.error(
            f"--dtype {args.dtype} on macOS: tensorflow-metal segfaults on the first training step "
            f"(EXC_BAD_ACCESS in libmetal_plugin's half-precision binary op; the unmodified script crashes "
            f"the same way, so it is an upstream bug). Train in float32 -- the exported bundle is fp16 "
            f"either way -- or set DEPTH3_ALLOW_METAL_FP16=1 to try it anyway and expect a crash report "
            f"rather than a traceback")

    if not args.train or not args.val or not args.out:
        parser.error("--train, --val and --out are required unless --export-only or --probe-graph "
                     "is given")
    train = Batches(args.train, args.modulation, args.max_train, seed=args.seed,
                    snr_min=args.snr_min, snr_max=args.snr_max, target=args.target,
                    layout=args.layout, cond=args.modcond)
    val = Batches(args.val, args.modulation, args.max_val, seed=args.seed + 1,
                  snr_min=args.snr_min, snr_max=args.snr_max, target=args.target,
                  layout=args.layout, cond=args.modcond)
    qm = train.qm
    if args.modcond:
        # The head serves every modulation, so its stride is the widest one; the per-sample Qm travels as the
        # conditioning channels and as the label mask. This is the whole difference from a per-modulation run.
        print(f"train {train.n} samples, val {val.n} samples, MODULATION-CONDITIONED "
              f"(head stride {D3.COND_STRIDE}, {len(D3.COND_QM_ORDER)} conditioning channels, "
              f"modulations from the dataset)")
    else:
        print(f"train {train.n} samples, val {val.n} samples, {args.modulation} (Qm={qm})")

    # ★ THE DATA'S CHANNEL COUNT, WITHOUT BUILDING A SAMPLE. The old form called `train.sample(0)` for this one
    # integer and threw the rest away -- the bit unpack, the label plane, the masks and the noise target, which is
    # most of a sample's cost -- and the first epoch then rebuilt the same sample. `plane_channels()` reads the
    # layout and the file's own width, and returns the same number for every layout.
    from_data = int(train.plane_channels())
    if args.modcond:
        # The sample appends the four conditioning channels itself, so the model's input width is the data's plus
        # those four. Kept here rather than inside `plane_channels` so the two widths stay visibly different.
        from_data += len(D3.COND_QM_ORDER)
    if from_data != in_channels:
        parser.error(f"--layout {args.layout} builds {in_channels} input channels but the data "
                     f"carries {from_data}")
    print(f"layout {args.layout}, {in_channels} input channels")
    model = build(blocks=args.blocks, c_hid=args.c_hid, qm=qm, aux_heads=not args.no_aux, low_rank=args.low_rank,
                  reporting_heads=not args.no_reporting_heads,
                  residual=not args.no_residual, dtype=args.dtype,
                  fold_scale=not args.no_fold_scale, scale_init=args.scale_init,
                  llr_linear=args.llr_linear, dense=args.dense_blocks,
                  dense_bottleneck=args.dense_bottleneck, layout=args.layout, fuse_relu=args.fuse_relu,
                  in_channels=in_channels, cond=args.modcond, dilations=args.dilations, ksize=args.kernel_size, groups=args.groups,
                  noise_to_llr=args.noise_to_llr, ce_aux=args.ce_head, llr_post=args.llr_post, cst_head=args.cst_head, sym_aux=args.sym_aux, mlp_expand=args.mlp_expand, se=args.se, se_ratio=args.se_ratio, dense_skip=args.dense_skip or args.dense_skip_entry or args.dense_skip_depth > 1,
                    dense_skip_entry=args.dense_skip_entry,
                    dense_skip_depth=args.dense_skip_depth,
                  smooth_k=args.smooth_k)
    if args.dense_skip_init == "sum":
        model = init_dense_skip_as_sum(model)
    parameters = int(sum(np.prod(w.shape) for w in model.weights))
    print(f"parameters {parameters}  heads {sorted(model.output.keys())}")
    # THE RECEPTIVE FIELD IS A DESIGN PARAMETER, SO IT IS PRINTED WITH THE PARAMETER COUNT. It is the
    # one architecture number that costs nothing (dilation adds no weights and no multiply-adds), and
    # it was the only one that had no record in the run logs: the cycle lived in a module constant, so
    # two runs could differ in it and look identical in the log.
    print(f"receptive field {receptive_field(args.blocks, args.dilations, args.kernel_size)} subcarriers "
          f"(cycle {','.join(str(d) for d in args.dilations)} x {args.blocks} blocks, kernel "
          f"{args.kernel_size}x{args.kernel_size})")
    if args.init:
        model.load_weights(args.init)
        print(f"started from {args.init}")

    # The scale is model-external per the contract, so the loss sees the LLR the quantizer sees. It is carried
    # PER SAMPLE (see Batches.sample): a conditioned batch mixes modulations whose limits differ.
    scale = D3.RANGE_LIMIT_FLOAT[args.modulation]
    # The width of the main head follows the CONTRACT in use, not the default modulation: a conditioned model
    # always carries COND_STRIDE per symbol (design section 19.102).
    head_channels = NOF_SYMBOLS * (D3.COND_STRIDE if args.modcond else qm)
    optimizer = tf.keras.optimizers.Adam(args.lr)
    weights = {"llr": args.w_llr, "ch_est": args.w_ch, "post_eq_noise": args.w_noise,
               "dmrs_noise": args.w_noise, "sym_est": args.w_sym}

    # XLA is a TRAINING choice and cannot reach the deployed graph: the bundle is exported from the Keras
    # layer structure and the trained weights, so compiling the step changes nothing about what the ANE
    # runs (design section 19.218). It is here because the step is latency-bound on small ops, which is
    # exactly what a fused compilation can cut.
    #
    # IT IS A FACTORY AND NOT A DECORATOR BECAUSE MACOS MAY HAVE TO BUILD IT TWICE. `--jit` there is honoured
    # only after `xla_self_test` has compiled something, and if the real graph is still rejected the first call
    # rebuilds the body without XLA instead of ending a run whose dataset and weights are already in memory.
    # Both forms come from the same body, so they cannot disagree about what the loss is -- which is why there
    # is no second, eager implementation to keep in step. On Linux `compile_step` is called once, with the
    # resolved flag, and the function below is that compiled step.
    def compile_step(jit):
        @tf.function(jit_compile=bool(jit))
        def compiled(x, targets, training):
            with tf.GradientTape() as tape:
                outputs = model(x, training=training)
                # Every head leaves the model with the contract's `[width, channels]` tail, so one
                # reshape reads each of them and the loss cannot drift with the trunk's layout: the
                # spatial axes are an implementation detail that stops at the head.
                # THE LOSS IS FLOAT32 WHATEVER THE MODEL'S DTYPE. The heads are cast before anything is
                # computed, so `--dtype float16` narrows the graph and not the objective; on macOS, where the
                # Metal plugin's fp16 path is the one with the most caveats, this keeps the number in the log
                # comparable with the Ubuntu run of the same command line.
                llr = tf.reshape(tf.cast(outputs["llr"], tf.float32),
                                 (-1, D3.NOF_SUBCARRIERS, head_channels)) * targets["scale"][:, None, None]
                if args.llr_linear:
                    # The bound the contract asks for, applied where it is differentiable, per sample.
                    llr = tf.clip_by_value(llr, -targets["scale"][:, None, None], targets["scale"][:, None, None])
                llr_mask = targets["llr_weights"]
                if training and (args.snr_weight > 0.0 or args.width_weight > 0.0):
                    # THE WEIGHTING APPLIES TO TRAINING ONLY. Validation reports the unweighted loss, so a weighted run stays
                    # comparable with every earlier one and the number in the log still means the same thing.
                    llr_mask = llr_mask * targets["w_snr"][:, None, None]
                terms = {"llr": L.tf_bce_loss(llr, targets["llr_shape"], llr_mask, norm=args.loss_norm)}
                if not args.no_aux:
                    # THE NOISE HEAD IS ALWAYS IN WHEN aux_heads IS (design section 22.2): it is what scales the logits
                    # into the LLRs the chain quantises, so it is trained even in the bundle that dropped the reporting
                    # heads. The channel and DM-RS heads are read only when the graph carries them.
                    if "ch_est" in outputs:
                        ch = tf.reshape(tf.cast(outputs["ch_est"], tf.float32),
                                        (-1, D3.NOF_SUBCARRIERS, 2 * NOF_SYMBOLS))
                        terms["ch_est"] = L.tf_nmse(
                            tf.complex(ch[:, :, 0::2], ch[:, :, 1::2]),
                            tf.complex(targets["ch"][:, :, :, 0], targets["ch"][:, :, :, 1]),
                            targets["ch_weights"])
                    if "sym_est" in outputs:
                        se = tf.reshape(tf.cast(outputs["sym_est"], tf.float32),
                                        (-1, D3.NOF_SUBCARRIERS, NOF_SYMBOLS, 2))
                        err = tf.reduce_sum((se - targets["sym"]) ** 2, axis=-1)
                        terms["sym_est"] = (tf.reduce_sum(err * targets["sym_weights"])
                                            / tf.maximum(tf.reduce_sum(targets["sym_weights"]), 1.0))
                    noise = tf.reshape(tf.cast(outputs["post_eq_noise"], tf.float32),
                                       (-1, D3.NOF_SUBCARRIERS, NOF_SYMBOLS))
                    error = (noise - targets["noise"]) ** 2
                    terms["post_eq_noise"] = (tf.reduce_sum(error * targets["noise_weights"])
                                              / tf.maximum(tf.reduce_sum(targets["noise_weights"]), 1.0))
                    if "dmrs_noise" in outputs:
                        dmrs = tf.reshape(tf.cast(outputs["dmrs_noise"], tf.float32), (-1,))
                        terms["dmrs_noise"] = tf.reduce_mean((dmrs - targets["dmrs_noise"]) ** 2)
                total = sum(weights[name] * terms[name] for name in terms)
            if training:
                gradients = tape.gradient(total, model.trainable_variables)
                optimizer.apply_gradients(zip(gradients, model.trainable_variables))
            return total, terms, llr
        return compiled

    _step_state = {"fn": compile_step(args.jit), "jit": bool(args.jit), "ran": False}

    def step(x, targets, training):
        """One training or evaluation step, degrading an unusable XLA compilation ONCE.

        THE DEGRADATION IS MACOS-ONLY AND HAPPENS BEFORE ANY STEP HAS SUCCEEDED, so there is no optimizer state
        that could have been half-applied when it happens. On Linux the first call is the compiled function and a
        failure there raises exactly as it always did -- that path is not allowed to change how a CUDA run fails.
        """
        try:
            result = _step_state["fn"](x, targets, training)
        except Exception as first:  # noqa: BLE001 - an XLA compile failure surfaces in many forms
            if _step_state["ran"] or not (IS_DARWIN and _step_state["jit"]):
                raise
            print(f"platform: the XLA step failed on its first call ({type(first).__name__}: {first}); "
                  f"rebuilding it without XLA", flush=True)
            _step_state["fn"] = compile_step(False)
            _step_state["jit"] = False
            try:
                result = _step_state["fn"](x, targets, training)
            except Exception as second:  # noqa: BLE001 - report both: the first is the cause
                raise second from first
        _step_state["ran"] = True
        return result

    # The gather rule follows the MODEL: a conditioned head carries every modulation in one stride-8 layout, so
    # the LLRs it produces must be gathered with that layout rather than with the reception's own Qm.
    GATHER = D3.llr_gather_cond if getattr(val, "cond", False) else D3.llr_gather

    def snr_weights(samples):
        """The per-sample weight of `--snr-weight` for one batch (see `threshold_weights`).

        Kept as a closure because it is the one place that knows which flag values apply; the arithmetic and its
        explanation live in the module-level function, so it can be checked without building a model.
        """
        return threshold_weights(samples, args.snr_weight, args.snr_sigma, args.snr_center,
                                 args.width_weight, args.width_max)

    # ★ WHICH TARGETS THE GRAPH ACTUALLY READS. `step` reads a target only when the model carries the head that
    # consumes it, so a run without `--sym-aux` was stacking and copying a 612 x 14 x 2 symbol plane for every
    # batch, and a run with `--no-reporting-heads` was copying the channel plane, both of them tensors the loss
    # never looked at. The keys are the MODEL's own output dict keys, which is exactly what `step` tests. If the
    # model is not returning a dict -- it always is here, both layouts build one -- every target is provided, so a
    # future graph cannot silently lose one.
    head_keys = set(model.output) if isinstance(model.output, dict) else None
    read_ch = (head_keys is None) or ("ch_est" in head_keys)
    read_sym = (head_keys is None) or ("sym_est" in head_keys)
    read_dmrs = (head_keys is None) or ("dmrs_noise" in head_keys)
    read_noise = not args.no_aux

    def tensors(samples):
        """One batch's device tensors: the input plane, and exactly the targets the step reads.

        MEASURED BEFORE DECIDING TO LEAVE IT SYNCHRONOUS. On an M4 Pro, batch 16, the c32 recipe, the seven
        targets below cost 0.37 ms to stack and another 0.37 ms to convert -- 0.74 ms against a 199 ms step,
        0.37% -- because the payload is 19 MB and the machine moves that in a fraction of a millisecond. A
        background packer would therefore buy well under one percent while adding a second thread that touches
        TensorFlow (which is where the Metal plugin has been observed to be fragile) and a queue whose failure
        modes would have to be got exactly right. It is not worth that, so the packing stays on the caller's
        thread and this docstring records why. Moving the numpy half of it into the loader threads was measured
        at 0.37 ms and rejected for the same reason.

        `stack` already avoids the redundant float32 copy. The conditional keys avoid a second kind of waste: a
        host-side stack plus `tf.constant` plus a device copy for every plane whose head this graph does not have.
        """
        stacked = tf.constant(stack(samples, "x"))
        targets = {
            "llr_shape": tf.constant(stack(samples, "llr_shape")),
            "llr_weights": tf.constant(stack(samples, "llr_weights")),
            "scale": tf.constant(stack(samples, "scale")),
        }
        if read_ch:
            targets["ch"] = tf.constant(stack(samples, "ch"))
            targets["ch_weights"] = tf.constant(stack(samples, "ch_weights"))
        if read_sym:
            targets["sym"] = tf.constant(stack(samples, "sym"))
            targets["sym_weights"] = tf.constant(stack(samples, "sym_weights"))
        if read_noise:
            targets["noise"] = tf.constant(stack(samples, "noise"))
            targets["noise_weights"] = tf.constant(stack(samples, "noise_weights"))
        if read_dmrs:
            targets["dmrs_noise"] = tf.constant(stack(samples, "dmrs_noise"))
        if args.snr_weight > 0.0 or args.width_weight > 0.0:
            # THE THRESHOLD WEIGHT, one number per sample. Only the training trace reads it, and only when the
            # weighting is on, so an unweighted run no longer builds a vector of ones per batch either. A weighted
            # run gets exactly the vector it always got.
            targets["w_snr"] = tf.constant(np.asarray(snr_weights(samples), dtype=np.float32))
        return stacked, targets

    def perfect_llr(sample):
        """The LLRs a perfect model would emit, from the contract's label plane.

        The label is `1 - b` and a positive LLR means bit 0, so `(2 * label - 1) * large` is the
        ideal LLR on every channel that carries a coded bit.
        """
        return (2.0 * sample["llr_shape"] - 1.0) * LD.KNOWN_LLR

    def coded_rate(samples, use_perfect):
        """The coded BLER of one batch of samples, capped by `--bler-samples`."""
        bler = CodedBler(val)
        for row, sample in enumerate(samples):
            index = int(sample["index"])
            plane = perfect_llr(sample) if use_perfect else sample["llr_prediction"]
            bler.add(index, GATHER(plane, val.allocation_of(index),
                                          val.dmrs_symbols_of(index), val.modulation_of(index),
                                          val.uci_of(index)))
        return bler.rate()

    if args.bler_check:
        bler = CodedBler(val)
        checked = 0
        for samples in val.epoch(args.batch):
            for index_sample in range(len(samples)):
                if checked >= max(1, min(args.bler_samples, 8)):
                    break
                index = int(samples[index_sample]["index"])
                bler.add(index, GATHER(perfect_llr(samples[index_sample]),
                                              val.allocation_of(index), val.dmrs_symbols_of(index),
                                              val.modulation_of(index), val.uci_of(index)))
                checked += 1
            break
        print(f"coded BLER on perfect LLRs: {bler.summary()}")
        assert bler.blocks > 0, "no decodable block in the validation set"
        assert bler.rate() == 0.0, (
            "the metric cannot reach zero, so it would never accept a good model")
        return 0

    # THE GROUPING THE GLOBAL LOSS CANNOT SEE. A reception is delivered or lost as a whole transport block, near a decoder
    # threshold that depends on its modulation and block size; averaging bit-cross-entropy over every sample hides both the
    # threshold and the size. These bands are the ones the probe already uses to report delivered bits, so a group here is
    # directly comparable with a row there.
    def snr_band(snr):
        for edge, name in ((10.0, "<=10"), (16.0, "10-16"), (20.0, "16-20"),
                           (24.0, "20-24"), (27.0, "24-27")):
            if snr < edge:
                return name
        return ">27"

    def tb_band(bits):
        for edge, name in ((512, "<512b"), (2048, "512-2k"), (4096, "2k-4k")):
            if bits < edge:
                return name
        return ">=4k"

    def group_of(index):
        return (val.modulation_of(index), snr_band(float(val.data["snr_nominal"][index])),
                tb_band(int(val.data["nof_tb_bits"][index])))

    # THE VALIDATION BATCHES COME FROM THE SAME PIPELINE, ON MACOS. `evaluate` is bound by host work once the decode
    # is switched off (a numpy cross-entropy over every element of the batch), and building its batches serially
    # leaves the device idle for exactly as long as the training loop's serial loop did. On Linux the loader stays
    # serial here, as it always was: this pass is not where the reference box was measured.
    val_workers, val_prefetch = (workers, prefetch) if IS_DARWIN else (0, 0)
    # THE LOW-SNR BAND IS ONLY MEASURED WHEN IT CAN DECIDE SOMETHING. `val_band` exists for
    # `--select-on band`; every other selection reads the validation loss and never looks at it, so the per-row
    # `decode_metrics` calls that build it were pure cost -- one `np.sign` plus two boolean masks over a
    # 612 x 112 slice, per ROW, per batch. With `--select-on val` (the default), `use_bler=False` and
    # `--group-report 0`, `evaluate` now reads each tensor exactly once and slices nothing.
    want_band = (args.select_on == "band")

    def evaluate(use_bler):
        totals, errors, magnitudes, band_ber = [], [], [], []
        groups = {}          # (modulation, snr band, tb band) -> [sum bce, sum bits, samples]
        bler = CodedBler(val) if use_bler else None
        decoded = 0
        for samples in val.epoch(args.batch, val_workers, val_prefetch):
            x, targets = tensors(samples)
            total, _, llr = step(x, targets, training=False)
            # ★ ONE DEVICE-TO-HOST COPY PER TENSOR PER BATCH. This loop used to call `llr.numpy()` up to four times a
            # batch and `targets[...].numpy()` once per ROW, so a batch of eight paid some thirty synchronisations to
            # read the same six arrays. The three below are those arrays, read once; every metric that follows
            # consumes them, and `decode_metrics` is called with slices of them, which are views.
            llr_host = llr.numpy()
            labels_host = targets["llr_shape"].numpy()
            weights_host = targets["llr_weights"].numpy()
            ber, mag = decode_metrics(llr_host, labels_host, weights_host)
            # The synchronisation here is not extra work: `llr_host` above already drained the device, so reading the
            # scalar costs nothing. It is kept in float() because the epoch's validation loss is the mean of these
            # exact float32 values, and it has to keep being that.
            totals.append(float(total))
            errors.append(ber)
            magnitudes.append(mag)
            # PER-ROW, SO THE LOW-SNR BAND CAN BE MEASURED (design section 19.438). The gates that decide whether a bundle is
            # adopted are per-bin BERs on the low-SNR bands, and the validation loss is a different objective that can peak at a
            # different epoch. Measuring the band during training lets the checkpoint be chosen on the acceptance metric itself.
            if args.group_report > 0:
                scale = targets["scale"].numpy()
                # THE SCALE IS APPLIED A SECOND TIME HERE, AND THAT IS KEPT. `step` already returns the LLRs
                # multiplied by `scale` (that is what the loss sees), so these group cross-entropies are on a
                # squared scale while `decode_metrics` above uses the once-scaled values. It is reported verbatim
                # because the group report is part of recorded runs' logs: correcting it here would silently make
                # two runs' reports incomparable, which is a decision for whoever reads those logs, not for this
                # rewrite. Flagged, not altered.
                scaled = llr_host * scale.reshape((-1,) + (1,) * (llr_host.ndim - 1))
                # The same stable form `depth3_loss.bce_loss` uses, kept per row so the sum and the bit count can be
                # accumulated per group instead of collapsed into one number.
                per_entry = (np.maximum(scaled, 0.0) - scaled * labels_host
                             + np.log1p(np.exp(-np.abs(scaled))))
                for row in range(llr_host.shape[0]):
                    w = weights_host[row]
                    total_w = float(w.sum())
                    if total_w <= 0.0:
                        continue
                    key = group_of(int(samples[row]["index"]))
                    slot = groups.setdefault(key, [0.0, 0.0, 0])
                    slot[0] += float((per_entry[row] * w).sum())
                    slot[1] += total_w
                    slot[2] += 1
            if want_band:
                for row in range(llr_host.shape[0]):
                    row_ber, _ = decode_metrics(llr_host[row:row + 1], labels_host[row:row + 1],
                                                weights_host[row:row + 1])
                    index = int(samples[row]["index"])
                    band_ber.append((float(val.data["snr_nominal"][index]), row_ber))
            if bler is not None and decoded < args.bler_samples:
                for row in range(llr_host.shape[0]):
                    if decoded >= args.bler_samples:
                        break
                    index = int(samples[row]["index"])
                    # THE SAME UCI THE LABEL USED. `label_plane` excludes the elements uplink control
                    # occupies, which is why a UCI-bearing sample's recorded `nof_bits` is smaller than
                    # the allocation's element count -- 6880 against 6912, exactly the difference that
                    # crashed a sixty-epoch run at its first decode. The training path has always passed
                    # `uci` here; this evaluation path did not, so it gathered the control elements as if
                    # they carried data and then compared the result against a count that excluded them.
                    gathered = GATHER(llr_host[row], val.allocation_of(index),
                                             val.dmrs_symbols_of(index), val.modulation_of(index),
                                             val.uci_of(index))
                    bler.add(index, gathered)
                    decoded += 1
        low = [b for snr, b in band_ber if np.isfinite(snr) and snr <= 10.0]
        # `band_ber` is empty when the band was not asked for, which yields the same nan the old code produced
        # when the validation set had no row at or below 10 dB. `chosen` only reads it under `--select-on band`.
        band = float(np.mean(low)) if low else float("nan")
        return (float(np.mean(totals)), float(np.mean(errors)), float(np.mean(magnitudes)),
                bler.rate() if bler is not None else float("nan"),
                bler.skipped if bler is not None else 0, band, groups)

    os.makedirs(args.out, exist_ok=True)
    history = []
    best = np.inf
    # WHETHER A BEST CHECKPOINT EVER REACHED DISK. `--select-on none` (and a `--select-on band` whose band is nan)
    # leaves `best` at inf and never saves one, so the closing line has to know the difference between "nothing was
    # written" and "wrote it" -- measured 2026-11-24 on a `--select-on none` run whose directory held no
    # `depth3_best.keras` while the log claimed it did.
    best_written = False
    epochs_without_improvement = 0
    start_epoch = 1
    # RESUME, BECAUSE A MULTI-DAY RUN MUST NOT RESTART FROM ZERO. The whole model is saved every epoch, in the format that
    # carries the optimizer state, so continuing is not a warm start: the learning-rate schedule position, the best value
    # and the patience counter come back too. Without it an eleven-day run costs eleven days again after a power cut,
    # which is the one failure a long training on a machine that is not a data centre will actually meet.
    if args.resume:
        last_path = os.path.join(args.out, "depth3_last.keras")
        state_path = os.path.join(args.out, "resume.json")
        if os.path.exists(last_path) and os.path.exists(state_path):
            # A RESUME MUST BE ABLE TO LOAD ITS OWN CHECKPOINT. A bare `load_model` failed with "Could not locate
            # class 'MaskedSmooth'"/'FoldedScaleConv' on the runs that use them, and the resume died before the
            # epoch that would have continued it. The three classes are named once, in `CUSTOM_OBJECTS`, which the
            # `--export-only` path uses too, so the two loaders cannot disagree about what a checkpoint contains.
            model = tf.keras.models.load_model(last_path, compile=False,
                                               custom_objects=CUSTOM_OBJECTS)
            state = json.load(open(state_path))
            start_epoch = int(state.get("epoch", 1)) + 1
            best = float(state.get("best", best))
            epochs_without_improvement = int(state.get("since", 0))
            # Older checkpoints predate the history being carried here, so fall back to the finished
            # history.json when one exists -- a resumed run must never quietly lose the epochs it already ran.
            history = list(state.get("history", []))
            if not history:
                done_path = os.path.join(args.out, "history.json")
                if os.path.exists(done_path):
                    try:
                        history = list(json.load(open(done_path)).get("history", []))
                    except (OSError, ValueError, json.JSONDecodeError):
                        history = []
            # THE HISTORY IS THE RECORD, SO IT SETTLES AN INTERRUPTED STATE. Until 2026-11-03 the state was
            # written before the best-model decision, so `best` and `since` lag one decision; loading that
            # blindly would let the next epoch treat a worse value as an improvement and overwrite the best
            # checkpoint. Both numbers are recomputable exactly from the history: the best is its minimum and
            # the counter is the distance to the epoch that produced it.
            measured = [(position, float(entry["val"])) for position, entry in enumerate(history)
                        if np.isfinite(float(entry["val"]))]
            if measured:
                position, value = min(measured, key=lambda pair: pair[1])
                if value != best:
                    print(f"  resume state said best {best:.5f}; the history's minimum is {value:.5f} "
                          f"at epoch {history[position]['epoch']} -- using the history")
                best = value
                epochs_without_improvement = len(history) - 1 - position
            print(f"resumed from {last_path}: next epoch {start_epoch}, best {best:.5f}, "
                  f"{epochs_without_improvement} epoch(s) without improvement, "
                  f"{len(history)} earlier epoch(s) of history carried over")
        else:
            print("no checkpoint to resume from; starting fresh")
    started = time.time()
    # Seeded before the loop so the state written at the END of an epoch always has a value to record,
    # even if the loop is entered with a control file that cannot be read: the variables are re-read
    # every epoch, but the write must not depend on that read having happened.
    patience_now, min_delta_now, control_src = float(args.patience), float(args.min_delta), "argv"
    # ★ A DECAYED LEARNING RATE MAKES THE END OF TRAINING TIGHT. With a constant rate the model keeps wandering in a
    # ball, and consecutive checkpoints differ by several percentage points of delivered bits (measured: 4.0 pp within
    # one run). A cosine decay shrinks that ball towards the end, so the final epochs are close to each other -- which
    # is exactly the condition the tail weight-averaging (SWA) needs to work.
    #
    # It is computed FROM THE EPOCH NUMBER rather than from a step counter, so a resumed run picks the schedule up
    # where it left off instead of restarting the decay from the top.
    lr_min = args.lr_min if args.lr_min is not None else args.lr / 10.0
    def learning_rate_for(ep):
        if args.lr_decay == "none":
            return args.lr
        first = max(1, args.lr_decay_from)
        span = max(1, args.epochs - first)
        frac = min(1.0, max(0.0, (ep - first) / span))
        return lr_min + 0.5 * (args.lr - lr_min) * (1.0 + math.cos(math.pi * frac))

    for epoch in range(start_epoch, args.epochs + 1):
        if args.lr_decay != "none":
            optimizer.learning_rate.assign(learning_rate_for(epoch))
            # The rate is part of the run's identity: a reader of the log must be able to tell a decayed epoch from a
            # constant one without reproducing the schedule by hand.
            if epoch == start_epoch or epoch % max(1, args.epochs // 20) == 0 or epoch == args.epochs:
                print(f"  lr {float(optimizer.learning_rate):.6g} (cosine, from epoch {args.lr_decay_from})", flush=True)
        # ★ NO HOST SYNCHRONISATION PER BATCH. `float(total)` is a device-to-host copy AND a full synchronisation:
        # it waits until every op the device has queued -- the whole step -- has finished, so the host cannot run
        # ahead into the next batch. On the CUDA box this loop left the card at 47% utilisation, and a Metal queue
        # stalls the same way. The per-batch means are collected as device scalars and read back once every
        # `LOSS_ACCUMULATION` batches (one `tf.stack` plus one copy), which bounds both the synchronisations and
        # the tensors held alive; the list below is then exactly what the synchronous form appended, element for
        # element, so `np.mean(losses)` -- and the history, the snapshots and every number derived from them -- is
        # unchanged. A float32 running SUM would not be: eight thousand additions at magnitude 2 lose the fifth
        # decimal, which is the precision the epoch line prints.
        losses, pending = [], []
        for samples in train.epoch(args.batch, workers, prefetch):
            x, targets = tensors(samples)
            total, _, _ = step(x, targets, training=True)
            pending.append(total)
            if len(pending) == LOSS_ACCUMULATION:
                losses.extend(float(value) for value in tf.stack(pending).numpy())
                pending = []
        if pending:
            losses.extend(float(value) for value in tf.stack(pending).numpy())
        # ★ VAL IS OPTIONAL. It has failed as a selector ten times over (>=10 recorded inversions between val and
        # delivered bits), and the deployment decision is now made by averaging the tail weights, which needs no
        # metric at all. `--no-val` therefore skips the pass; the run keeps only the train loss and, if asked, a
        # periodic health check via `--monitor-every`.
        monitored = (not args.no_val) or (args.monitor_every > 0 and
                                          (epoch % args.monitor_every == 0 or epoch == args.epochs))
        want_bler = args.bler_samples > 0 and (epoch % max(1, args.bler_every) == 0
                                               or epoch == args.epochs) and monitored
        if monitored:
            val_loss, val_ber, val_mag, val_bler, val_skipped, val_band, val_groups = evaluate(want_bler)
        else:
            val_loss, val_ber, val_mag, val_bler = float("nan"), float("nan"), float("nan"), float("nan")
            val_skipped, val_band, val_groups = 0, float("nan"), {}
        history.append({"epoch": epoch, "train": float(np.mean(losses)), "val": val_loss,
                        "val_ber": val_ber, "val_llr_mean": val_mag, "val_bler": val_bler,
                        "val_bler_skipped": val_skipped})
        if epoch == 1 or epoch % max(1, args.epochs // 20) == 0 or epoch == args.epochs:
            print(f"epoch {epoch:4d}  train {np.mean(losses):.5f}  val {val_loss:.5f}  "
                  f"uncoded BER {val_ber:.4f}  coded BLER {val_bler:.4f}  "
                  f"mean|LLR| {val_mag:6.1f}  ({time.time() - started:.0f}s)")
            # THE GROUPS THE AVERAGE HIDES. Printed on the same schedule as the epoch line, worst first, with the share of the
            # validation bits each group carries: a group at twice the global loss that holds two percent of the bits is noise
            # to fix, and one that holds twenty percent is the model. That distinction is what the global number cannot make.
            if args.group_report > 0 and val_groups:
                total_bits = sum(v[1] for v in val_groups.values())
                worst = sorted(val_groups.items(), key=lambda kv: -(kv[1][0] / max(kv[1][1], 1.0)))
                worst = [kv for kv in worst if kv[1][2] >= 2][:args.group_report]
                for (qm, snr_name, tb_name), (sum_bce, nbits, nsamples) in worst:
                    mean_bce = sum_bce / max(nbits, 1.0)
                    print(f"    group {str(qm):<7} {snr_name:<6} {tb_name:<7}"
                          f" bce {mean_bce:.5f}  x{nsamples:<4}  bits {nbits/total_bits*100:5.1f}%")
        # EARLY TERMINATION, the way the LDPC decoder has it (design section 19.437). The validation curve is flat long before the
        # epoch budget is spent -- E4's minimum is at epoch 18 of 40, and the remaining twenty-two epochs are five-thousandths of
        # noise -- so a patience stops the run when it has stopped learning. The weights kept are the BEST ones, which the loop
        # already saves, so stopping early cannot make the result worse than training the full count; it only stops paying for it.
        # WHAT "BEST" MEANS (design section 19.438): the validation loss by default, or -- with `--select-on band` -- the BER of
        # the low-SNR band, which is the quantity the acceptance gates measure. Choosing on the latter means the checkpoint kept
        # is the one the gates would have picked, so an early stop can no longer miss a later gate-optimal epoch.
        # THE WEIGHTS GO FIRST, THE STATE AFTER THE DECISION (2026-11-03). An interruption while deciding must
        # still leave a checkpoint one epoch old rather than none, so `depth3_last` is written here; the state
        # that says which epoch was BEST is written below, once that is known.
        model.save(os.path.join(args.out, "depth3_last.keras"))
        # `--select-on none` keeps no best checkpoint: the tail average is the model, and a selection metric that
        # has been wrong ten times should not get a vote.
        chosen = (float("inf") if args.select_on == "none" else
                  val_band if (args.select_on == "band" and np.isfinite(val_band)) else val_loss)
        # THE TRIGGER'S TWO VARIABLES, RE-READ EVERY EPOCH (2026-11-03). A run that must be restarted to change
        # its patience cannot be used to find out what the patience should be: the restart costs the epochs
        # already spent and changes the noise the comparison would have to be made under. The file is optional
        # and its absence is not an error, so a run started without one behaves exactly as before.
        patience_now, min_delta_now, control_src = float(args.patience), float(args.min_delta), "argv"
        if args.control:
            try:
                with open(args.control) as _ctl:
                    _c = json.load(_ctl)
                if "patience" in _c:
                    patience_now = float(_c["patience"])
                if "min_delta" in _c:
                    min_delta_now = float(_c["min_delta"])
                control_src = "file"
            except (OSError, ValueError, json.JSONDecodeError) as _err:
                print(f"  WARNING: could not read {args.control} ({_err}); keeping the command-line values",
                      file=sys.stderr)
        history[-1]["patience"] = patience_now
        history[-1]["min_delta"] = min_delta_now
        if chosen < best - min_delta_now:
            best = chosen
            epochs_without_improvement = 0
            model.save(os.path.join(args.out, "depth3_best.keras"))
            best_written = True
        else:
            epochs_without_improvement += 1

        # THE RESUMABLE STATE, WRITTEN AFTER THE DECISION (2026-11-03). Until now it was written before it,
        # so the file always lagged one decision: a live run's best of epoch 13 was recorded as epoch 9's
        # value, and a crash-resume would have treated a WORSE epoch as an improvement and overwritten
        # `depth3_best.keras` -- the one file every measurement downstream reads.
        # THE HISTORY TRAVELS WITH IT. Without the list a resumed run starts empty and writes a history.json
        # holding only the epochs after the resume, and every snapshot it takes inherits the truncation.
        with open(os.path.join(args.out, "resume.json"), "w") as _state:
            json.dump({"epoch": epoch, "best": float(best),
                       "since": int(epochs_without_improvement),
                       "history": history,
                       "patience": patience_now, "min_delta": min_delta_now,
                       "control": control_src}, _state)

        # ---- snapshots ----------------------------------------------------------------------------
        # `snapshot` writes the whole training site: the model WITH its optimizer, the history up to this
        # epoch, the trigger variables, and which of the three reasons produced it. A run that ends on the
        # patience cannot answer "what would the next ten epochs have done", and that is the question a
        # recipe comparison asks -- so by default the trigger snapshots and continues.
        def snapshot(reason):
            snap_dir = os.path.join(args.out, "snapshots")
            os.makedirs(snap_dir, exist_ok=True)
            stem = os.path.join(snap_dir, f"epoch_{epoch:04d}_{reason}")
            model.save(stem + ".keras")
            with open(stem + ".json", "w") as _h:
                json.dump({"reason": reason, "epoch": epoch, "train": float(np.mean(losses)),
                           "val": val_loss, "val_ber": val_ber, "val_bler": val_bler,
                           "best": float(best), "since": int(epochs_without_improvement),
                           "patience": patience_now, "min_delta": min_delta_now,
                           "control": control_src, "select_on": args.select_on,
                           "history": history}, _h, indent=1)
            return stem

        fired = (patience_now > 0) and (epochs_without_improvement >= patience_now)
        if fired:
            stem = snapshot("early_stop")
            print(f"TRIGGER at epoch {epoch}: no improvement for {int(epochs_without_improvement)} epoch(s) "
                  f"(patience {patience_now:g}, min_delta {min_delta_now:g}, from {control_src}); "
                  f"best {best:.5f}; snapshot {os.path.basename(stem)}.keras")
            if args.stop_on_early:
                break
            # CONTINUE, WITH THE COUNTER RESET. The next trigger needs a fresh run of unimproved epochs; leaving
            # the counter over the threshold would fire every epoch from here on.
            epochs_without_improvement = 0
        if (args.snapshot_every > 0) and (epoch % args.snapshot_every == 0) and not fired:
            stem = snapshot("periodic")
            print(f"  snapshot at epoch {epoch} ({os.path.basename(stem)}.keras)")

    with open(os.path.join(args.out, "history.json"), "w") as handle:
        json.dump({"history": history, "parameters": parameters,
                   "weights": weights, "scale": scale, "modulation": args.modulation}, handle,
                  indent=1)
    np.save(os.path.join(args.out, "val_llr_stats.npy"),
            np.array([[h["val_ber"], h["val_llr_mean"], h["val_bler"]] for h in history]))
    # THE CLOSING LINE MAY NOT CLAIM A FILE THAT WAS NOT WRITTEN, BUT ITS PREFIX STAYS VERBATIM. The only downstream
    # reader of this line is `work_tmp/tmp/finalize_conditioned.sh`, which waits for a last line matching
    # `*"best val loss"*` and then checks `[ -f .../depth3_best.keras ]` itself; keeping the prefix means a watcher
    # cannot hang, and dropping the false "wrote" clause means a human (or a log grep) cannot be misled either.
    if best_written:
        print(f"best val loss {best:.5f}; wrote {args.out}/depth3_best.keras and history.json")
    else:
        print(f"best val loss {best:.5f}; no best checkpoint kept (select-on {args.select_on}); "
              f"wrote {args.out}/history.json")

    # THE DECLARED RANGE. From the knob when given, otherwise from the training set itself: the 1st to 99th
    # percentile of `snr_nominal`, which is the range the reporting heads actually saw. The extremes are left out
    # on purpose -- a handful of samples at the very edge are not a domain, and declaring them would widen the
    # gate to receptions the model has essentially never met.
    snr_range = cli_snr_range(args)
    if snr_range is None and args.train:
        nominal = load_snr_nominal(args.train)
        # ONLY THE FINITE ENTRIES: one NaN poisons the percentile, and a bundle that declares 'nan,nan' is read by
        # the engine as "no range declared" -- the domain gate would be silently off (measured 2026-10-17, on the
        # first export of the final checkpoint).
        nominal = nominal[np.isfinite(nominal)]
        snr_range = (float(np.percentile(nominal, 1)), float(np.percentile(nominal, 99)))
        print(f"declared SNR range from the training set: {snr_range[0]:.2f} to {snr_range[1]:.2f} dB "
              f"(1st to 99th percentile of {nominal.size} sample(s))")

    # Export AFTER training, from the trained weights. Exporting first would produce a deployment
    # graph with random weights that passes every shape check, which is the kind of artifact that
    # reaches a receiver and produces plausible nonsense.
    if args.export_probe:
        model.load_weights(os.path.join(args.out, "depth3_best.keras"))
        export_deployment(model, qm, args.export_probe, cond=args.modcond, snr_range=snr_range,
                          sinr_cal=cli_sinr_cal(args))

    # THE NARROW-BAND BUCKETS, from the trained weights (model_network_design section 49).
    export_buckets(args, parser, source=model)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
