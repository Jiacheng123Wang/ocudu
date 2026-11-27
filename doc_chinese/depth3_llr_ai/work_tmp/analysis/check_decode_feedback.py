"""What does the decoder-feedback channel actually carry? Three checks on a real sample.

Run on a machine that has TensorFlow and the training corpus:

    ~/.venvs/tf-metal-2.18/bin/python check_decode_feedback.py [dataset]

It proves the three properties the second pass depends on: a perfect demapper gets CONFIRMATION (sign
agreement ~100%), a random one gets mixed feedback laid out per modulation bit, and the gradient reaches
the first pass through the decoder unless --decode-feedback-detach is set.
"""
import sys

import numpy as np

AI = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
sys.path.insert(0, AI)
import depth3_paths                                                   # noqa: E402
depth3_paths.add_ce_shared()

import tensorflow as tf                                               # noqa: E402

import code_loss as CL                                                # noqa: E402
from train_depth3 import Batches                                      # noqa: E402

DATA = sys.argv[1] if len(sys.argv) > 1 else "/Users/jiachengwang/ai_depth3/datasets_train_w2v3c"
batches = Batches(DATA, "QPSK", max_samples=6, cond=True)
index = None
for candidate in range(len(batches.data["qm"])):
    if CL.prepare(batches, candidate, row=0) is not None:
        index = candidate
        break
prepared = CL.prepare(batches, index, row=0)
targets = CL.stack_prepared([prepared])
perfect = ((2.0 * batches.sample(index)["llr_shape"] - 1.0) * 30.0)[None].astype(np.float32)
print("sample", index, "bits", len(prepared["flat_index"]))

feedback = CL.tf_decode_feedback(tf.constant(perfect), targets, planes=8, scale=20.0)
flat = tf.reshape(feedback, (1, -1)).numpy()[0]
idx = prepared["flat_index"]
truth = perfect.reshape(-1)[idx]
agree = float(np.mean(np.sign(flat[idx]) == np.sign(truth)))
print(f"perfect LLRs  -> max |v| = {float(tf.reduce_max(tf.abs(feedback))):.3f}, "
      f"sign agreement with the demapper {100 * agree:.1f}% (the code confirms what it was told)")

rng = np.random.default_rng(0)
random = (rng.standard_normal(perfect.shape) * 20.0).astype(np.float32)
feedback = CL.tf_decode_feedback(tf.constant(random), targets, planes=8, scale=20.0)
print(f"random LLRs   -> feedback max |v| = {float(tf.reduce_max(tf.abs(feedback))):.6f}, "
      f"non-zero planes {int(tf.reduce_sum(tf.cast(tf.reduce_max(tf.abs(feedback), axis=(0, 1, 2)) > 0, tf.int32)))}/8")

variable = tf.Variable(random)
with tf.GradientTape() as tape:
    loss = tf.reduce_sum(CL.tf_decode_feedback(variable, targets, planes=8, scale=20.0))
gradient = tape.gradient(loss, variable)
print(f"gradient through the decoder (not detached): max |g| = {float(tf.reduce_max(tf.abs(gradient))):.6f}")
with tf.GradientTape() as tape:
    loss = tf.reduce_sum(CL.tf_decode_feedback(variable, targets, planes=8, scale=20.0, detach=True))
gradient = tape.gradient(loss, variable)
print(f"gradient with --decode-feedback-detach:     "
      f"{'None' if gradient is None else float(tf.reduce_max(tf.abs(gradient)))}")
