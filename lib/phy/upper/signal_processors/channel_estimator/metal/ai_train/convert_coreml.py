#!/usr/bin/env python
"""SavedModel -> Core ML (fixed input shape) + latency benchmark.

Usage: convert_coreml.py <savedmodel_dir> <out.mlpackage> [--batch N] [--shape SC]
"""
import sys, time
import numpy as np
import coremltools as ct

def main():
    sm_dir, out_path = sys.argv[1], sys.argv[2]
    batch = int(sys.argv[sys.argv.index('--batch') + 1]) if '--batch' in sys.argv else 1
    shape = int(sys.argv[sys.argv.index('--shape') + 1]) if '--shape' in sys.argv else 612
    spec_input = ct.TensorType(name='input_1', shape=(batch, shape, 14, 2), dtype=np.float32)
    t0 = time.time()
    mlmodel = ct.convert(sm_dir, source='tensorflow', inputs=[spec_input],
                         minimum_deployment_target=ct.target.macOS15)
    mlmodel.save(out_path)
    print(f'converted -> {out_path} in {time.time()-t0:.1f}s')

    x = np.random.randn(batch, shape, 14, 2).astype(np.float32)
    for cu in ['CPU_ONLY', 'CPU_AND_GPU', 'CPU_AND_NE', 'ALL']:
        ml = ct.models.MLModel(out_path, compute_units=getattr(ct.ComputeUnit, cu))
        ml.predict({'input_1': x})
        ts = []
        for _ in range(200):
            t0 = time.perf_counter()
            ml.predict({'input_1': x})
            ts.append((time.perf_counter() - t0) * 1e6)
        ts = sorted(ts)
        print(f'{cu:12s} n=200: p50={ts[100]:7.1f}us p95={ts[190]:7.1f}us p99={ts[198]:7.1f}us '
              f'mean={sum(ts)/200:7.1f}us')

if __name__ == '__main__':
    main()
