#!/usr/bin/env python3
"""Per-modulation reading of a MODULATION-CONDITIONED checkpoint, before it is deployed.

WHY IT EXISTS. The conditioned bundle is one artifact serving four modulations, and criterion 1 is judged per
modulation -- so "the validation loss went down" is not enough: a single number can hide a modulation the shared
trunk fits badly. This evaluates the checkpoint the training keeps (`depth3_best.keras`) over the merged
validation set and prints, PER MODULATION, the uncoded BER of the gathered LLRs and the mean |LLR| through the
contract's own layout (`label_plane_cond` / `llr_gather_cond`), which is the same walk the arm does.

It is a PROGRESS reading, not the acceptance: the acceptance is criterion 1 on real corpora through the chain
(`compare_ai_arm.py` -> `criterion1.py`), and it needs the engine to run the conditioned bundle. What this
answers is the earlier question -- is the Qm conditioning working for every modulation at once.

Usage: eval_modcond.py --data <mixed_val.npz> --model <run>/depth3_best.keras [--per-modulation N]
"""
import argparse
import collections
import importlib.util
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("T", os.path.join(HERE, "train_depth3.py"))
T = importlib.util.module_from_spec(spec)
sys.modules["T"] = T
spec.loader.exec_module(T)
D3 = T.D3


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--data", required=True)
    parser.add_argument("--model", required=True)
    parser.add_argument("--per-modulation", type=int, default=200)
    parser.add_argument("--blocks", type=int, default=18)
    parser.add_argument("--layout", default="spatial")
    args = parser.parse_args()

    import tensorflow as tf

    # EVERY sample is loaded, and the cap is applied per modulation in the loop below: passing the cap to the
    # dataset would take the first N samples of the FILE, and the merged set is ordered by modulation -- so a
    # 100-sample run would report QPSK alone and look like a complete per-modulation reading (measured).
    batches = T.Batches(args.data, "256QAM", 0, seed=1, layout=args.layout, cond=True)
    model = T.build(blocks=args.blocks, qm=8, cond=True, layout=args.layout,
                    in_channels={"folded": D3.NOF_CHANNELS, "probe8": 8}.get(args.layout,
                                                                             D3.NOF_CHANNELS_SPATIAL)
                    + len(D3.COND_QM_ORDER))
    model.load_weights(args.model)
    print(f"{args.model}: {batches.n} validation samples, layout {args.layout}, "
          f"{model.inputs[0].shape[-1]} input channels")

    per_qm = collections.defaultdict(lambda: {"bits": 0, "wrong": 0, "llr": [], "n": 0})
    for index in range(batches.n):
        qm = D3.BITS_PER_SYMBOL[batches.modulation_of(index)]
        if per_qm[qm]["n"] >= args.per_modulation:
            continue
        per_qm[qm]["n"] += 1
        sample = batches.sample(index)
        x = np.asarray(sample["x"])[None, ...]
        llr = model(x, training=False)["llr"].numpy()[0]
        # The arm's own walk: gather the reception's bits out of the conditioned layout.
        gathered = D3.llr_gather_cond(llr, batches.allocation_of(index),
                                      batches.dmrs_symbols_of(index),
                                      batches.modulation_of(index))
        transmitted = np.unpackbits(batches.data["bits"][index])[:int(batches.data["nof_bits"][index])]
        if gathered.size != transmitted.size:
            raise ValueError(f"Qm={qm}: {gathered.size} LLRs for {transmitted.size} transmitted bits")
        # A positive LLR means bit 0 (the contract), so the decision is `llr < 0`.
        decided = (gathered < 0).astype(np.uint8)
        per_qm[qm]["bits"] += transmitted.size
        per_qm[qm]["wrong"] += int((decided != transmitted).sum())
        per_qm[qm]["llr"].append(float(np.abs(gathered).mean()))

    print(f"{'Qm':>4} {'samples':>8} {'uncoded BER':>12} {'mean|LLR|':>10}")
    total_bits = total_wrong = 0
    for qm in sorted(per_qm):
        row = per_qm[qm]
        samples = row["n"]
        ber = row["wrong"] / max(row["bits"], 1)
        total_bits += row["bits"]
        total_wrong += row["wrong"]
        print(f"{qm:>4} {samples:>8} {ber:>12.5f} {np.mean(row['llr']):>10.2f}")
    print(f"{'all':>4} {sum(row['n'] for row in per_qm.values()):>8} "
          f"{total_wrong / max(total_bits, 1):>12.5f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
