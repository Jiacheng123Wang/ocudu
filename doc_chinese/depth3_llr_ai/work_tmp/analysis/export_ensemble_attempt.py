"""Ensemble the three finished c32 arms by averaging their LLRs, and export one deployable bundle.

WHY. Each arm is individually at or below the plain baseline (L6 -52 880 bits on the ruler, CE head -114 952), but
they fail in different places: L6 was the best on the probe and the CE head has the most conservative LLRs. Averaging
LLRs is the cheapest remaining combination -- no training, and the three checkpoints already exist.

The average is a real Core ML graph (three sub-models plus a mean), so the measurement path is unchanged: one bundle.
"""
import sys
import tensorflow as tf

CODE = "/Users/jiachengwang/dev/ocudu/lib/phy/upper/ai_signal_processors/depth3/ai_train"
sys.path.insert(0, CODE)
import train_depth3 as T   # noqa: E402

PATHS = ["/Users/jiachengwang/ai_depth3/models/d3_w2v3c_c32b18dense_sc003_sw2s3_jit_e40.keras",
         "/tmp/nz_e40.keras", "/tmp/ceh_e40.keras"]
models = [tf.keras.models.load_model(p, compile=False,
                                     custom_objects={"FoldedScaleConv": T.FoldedScaleConv}) for p in PATHS]
# Keras 3 names every Functional "functional", and three of them nested in one graph collide. Rename each arm.
# THE COLLISION IS INTERNAL: all three arms were built by the same trainer, so their layers share names
# (entry_1x1, cv0_3x3_d1, ...). Keras 3 names graph nodes after those layers, so nesting three of them makes every
# node name appear three times. Prefixing each arm's layer names fixes it at the source of the names.
for i, m in enumerate(models):
    m._name = f"arm{i}"
    for layer in m.layers:
        layer._name = f"arm{i}_{layer.name}"                          # noqa: SLF001 - no public rename in Keras 3
inp = tf.keras.Input(shape=(612, 14, 16), name="x")
# each sub-model returns {"llr": [1, 612, 112], ...}; the average keeps the contract's width
llrs = [m(inp)["llr"] for m in models]
avg = tf.keras.layers.Average(name="llr_flat")(llrs)
ens = tf.keras.Model(inp, {"llr": avg}, name="llr_ensemble")
ens.summary(print_fn=lambda *a, **k: None)
print("ensemble outputs:", ens.output_names, "params:", f"{int(sum(tf.size(w).numpy() for w in ens.weights)):,}")
T.export_deployment(ens, 8, "/tmp/ens_export/m.mlpackage", cond=True, snr_range=(0, 48))
print("exported")
