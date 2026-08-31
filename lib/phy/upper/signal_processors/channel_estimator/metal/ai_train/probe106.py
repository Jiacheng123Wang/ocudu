#!/usr/bin/env python3
"""Day-1 106-PRB latency probe: zero-training width transfer from the 52-PRB
HELENA model, then convert + benchmark. Run from ~/ai_ce_work/work with the
venv python; expects the repo ai_train dir on PYTHONPATH.
"""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import numpy as np
from tf_keras.models import load_model
from helena_arch import transfer_from_51

WORK = os.path.expanduser('~/ai_ce_work/work')

def main():
    t0 = time.time()
    m52 = load_model(os.path.join(WORK, 'helena_pusch52_sm'))
    print(f'loaded 52 SavedModel in {time.time()-t0:.1f}s')
    print('52 layer names:', [l.name for l in m52.layers[:8]], '...')

    m106 = transfer_from_51(m52, 106)
    m106.compile()
    x = np.random.randn(1, 1272, 14, 2).astype(np.float32)
    y = m106.predict(x, verbose=0)
    assert y.shape == (1, 1272, 14, 2), f'bad shape {y.shape}'
    assert np.isfinite(y).all(), 'non-finite output'
    print(f'sanity ok: out shape {y.shape}, mean |y| = {np.abs(y).mean():.4f}')

    out = os.path.join(WORK, 'helena_pusch106_sm')
    m106.save(out)
    print(f'saved -> {out} in {time.time()-t0:.1f}s total')

if __name__ == '__main__':
    main()
