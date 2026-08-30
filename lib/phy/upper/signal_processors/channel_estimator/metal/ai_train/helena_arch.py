#!/usr/bin/env python
"""HELENA architecture reconstruction (from the .keras layer config) with a
parameterized PRB width. All trainable weights (Conv/Dense/MHA/LayerNorm) are
width-independent - only the Reshape targets change with the width - so a
52-PRB model can be initialized layer-by-layer from the 51-PRB weights.
"""
import tensorflow as tf
import tf_keras
from tf_keras import layers, Model, Input

def build_helena(prb, name='HELENA'):
    nfft = prb * 12
    inp = Input((nfft, 14, 2), name='input_1')
    x = layers.Conv2D(32, (12, 2), padding='same', activation='relu', name='conv2d')(inp)
    x = layers.Conv2D(2, (6, 7), padding='same', activation='relu', name='conv2d_1')(x)
    x = layers.Dropout(0.1, name='dropout')(x)
    x = layers.Reshape((prb, 336), name='reshape')(x)
    d = layers.Dense(64, name='dense')(x)
    a = layers.MultiHeadAttention(num_heads=4, key_dim=64, value_dim=64, name='multi_head_attention')(d, d)
    x = layers.Add(name='add')([d, a])
    x = layers.LayerNormalization(axis=2, epsilon=1e-3, name='layer_normalization')(x)
    # Explicitly-named Lambda: a raw tf.reduce_mean would auto-name itself and can
    # collide with the MHA-internal ops (the '_1' suffix broke the layer-name
    # matching against the original .keras graph).
    g = layers.Lambda(lambda z: tf.reduce_mean(z, axis=1, keepdims=True),
                      name='tf.math.reduce_mean')(x)
    g = layers.Dense(16, activation='relu', name='dense_1')(g)
    g = layers.Dense(64, activation='sigmoid', name='dense_2')(g)
    x = layers.Multiply(name='multiply')([x, g])
    x = layers.Dense(336, name='dense_3')(x)
    x = layers.Reshape((nfft, 14, 2), name='reshape_1')(x)
    out = layers.Add(name='add_1')([inp, x])
    return Model(inp, out, name=name)

def transfer_from_51(model_51, prb):
    """Build the prb-wide model and copy every width-independent weight."""
    m = build_helena(prb)
    for layer in m.layers:
        if layer.name in ('input_1', 'dropout', 'reshape', 'reshape_1', 'add', 'add_1',
                          'multiply', 'tf.math.reduce_mean'):
            continue
        src = model_51.get_layer(layer.name)
        layer.set_weights(src.get_weights())
    return m
