#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Synthesize PUSCH captures NARROWER than one estimation block (1 and 2 PRB), from a 3 PRB one.

---- Why this exists (S13-P1's air leg found the gap it covers) ----
The estimator works in blocks of `block_prb` PRBs (3, see MAX_BLOCK_PRB). A hop narrower than one
block has `n_std_blocks == 0`, and `tail_slots_on_device = std_slots_filled` is false for it (that
flag describes the STANDARD group's slots, which such a hop does not have). The host therefore builds
the correlation matrices itself and stages A/R_hp into device memory - a host -> device write of tens
of kilobytes on every 1-2 PRB hop.

Measured on air (leg `s13p1b_0919_2322`, batch S13-P1's new site and refusal counter): 368 of 3748
hops, exactly the narrow allocations (78 of 1 PRB + 290 of 2 PRB), 0.10 host writes per hop, 16.6 MB.
It had been there all along and no counter could see it.

The 27-capture corpus contains no allocation below 3 PRB, so it cannot exercise the path at all.

WARNING - these captures are a PATH TRIGGER, not a numerical corpus. Cropping the 3 PRB grid changes
the allocation the metadata declares while the samples still carry a 3 PRB transmission, so the DM-RS
sequence the demodulator regenerates no longer matches the data: the CPU reference chain fails on them
too (-11.6 dB, every CRC KO). They are good for exactly one thing - making the estimator take the
narrow path (the site and `corr_geometry` fire) - and for that the determinism they do have is enough.

What they CANNOT do is decide which of the two correlation builds is right for a narrow hop. That
question (the host's A carries NO diagonal loading on a device route - measured 1.00000095 against the
device's 1.86681008 on the STANDARD geometry, with R_hp bit-identical) needs a REAL 1-2 PRB reception
from `ul_capture::capture_grid`, and its criterion is the 2 PRB rows of the air legs, whose CRC is 0.0%
in every one of them.

---- How ----
Both captures are `syn001_3` cropped in frequency: the grid is dumped as one row of
`bwp_size_rb * 12` complex samples per (port, symbol), so keeping the first `prb * 12` samples of every
row and rewriting the metadata is enough. Nothing else changes - the DM-RS pattern, the modulation and
the TBS are the same, so the two captures differ from the reference only in the allocation width.

usage: python3 make_narrow_captures.py [corpus-dir] [output-dir]
       (defaults: doc_chinese/work_tmp/corpus, doc_chinese/work_tmp/corpus_narrow)
"""
import os
import struct
import sys

SRC_NAME = "syn001_3"
NOF_PRBS = (1, 2)
NOF_PORTS = 1
NOF_SYMBOLS = 14
CF_T_BYTES = 8  # cf_t is a pair of floats


def main() -> int:
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
    corpus = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, "doc_chinese", "work_tmp", "corpus")
    out_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(root, "doc_chinese", "work_tmp", "corpus_narrow")
    src = os.path.join(corpus, SRC_NAME)
    if not os.path.exists(src + ".txt"):
        print(f"missing {src}.txt - pass the corpus directory as the first argument", file=sys.stderr)
        return 2

    meta = open(src + ".txt").read()
    src_prb = int([line for line in meta.splitlines() if line.startswith("bwp_size_rb=")][0].split("=")[1])
    data = open(src + ".bin", "rb").read()
    row_bytes = src_prb * 12 * CF_T_BYTES
    expected = NOF_PORTS * NOF_SYMBOLS * row_bytes
    if len(data) != expected:
        print(f"{src}.bin is {len(data)} bytes, expected {expected}", file=sys.stderr)
        return 2

    os.makedirs(out_dir, exist_ok=True)
    for prb in NOF_PRBS:
        name = os.path.join(out_dir, f"syn_narrow{prb}")
        with open(name + ".bin", "wb") as f:
            for symbol in range(NOF_SYMBOLS):
                row = data[symbol * row_bytes : (symbol + 1) * row_bytes]
                f.write(row[: prb * 12 * CF_T_BYTES])
        text = meta.replace("bwp_size_rb=%d" % src_prb, f"bwp_size_rb={prb}")
        text = text.replace("alloc_nof_rb=%d" % src_prb, f"alloc_nof_rb={prb}")
        text = text.replace(
            "alloc_prb=" + ",".join(str(i) for i in range(src_prb)),
            "alloc_prb=" + ",".join(str(i) for i in range(prb)),
        )
        open(name + ".txt", "w").write(text)
        print(f"{name}: {prb} PRB, {os.path.getsize(name + '.bin')} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
