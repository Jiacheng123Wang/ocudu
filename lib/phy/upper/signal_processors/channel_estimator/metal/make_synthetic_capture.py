#!/usr/bin/env python3
"""Synthesize a ul_chain_replay capture (the OCUDU_UL_DUMP format) without an over-the-air run.

Why this exists: the gates in capture_gates.sh need recordings, and the 980-capture corpus that
lived in /tmp did not survive a forced power-off. The format is fully described by the writer
(ul_capture::capture_grid) and the reader (ul_chain_replay::parse_capture): a key=value text file
plus a raw grid of complex64, port-major, MAX_NSYMB_PER_SLOT symbols, bwp_size_rb * 12 subcarriers
each.

The grid is random noise, and that is enough for what it is used for here: sig2 compares the DEVICE
noise variance against the HOST's on the very same pilots, so the content only has to be finite -
whether it looks like a real channel is irrelevant to that comparison. It is NOT a substitute for
the recorded corpus: the byte-identity gates (k0d, k0dm, ydev, combos) and any decoding-decision
measurement still need real receptions.

Usage: make_capture.py <prefix> [bwp_size_rb] [dmrs_symbols] [seed]
"""
import struct
import sys
import random

prefix = sys.argv[1]
bwp_size_rb = int(sys.argv[2]) if len(sys.argv) > 2 else 25
dmrs_symbols = sys.argv[3] if len(sys.argv) > 3 else "2,7,11"
seed = int(sys.argv[4]) if len(sys.argv) > 4 else 1
nof_tx_layers = int(sys.argv[5]) if len(sys.argv) > 5 else 1
nof_rx_ports = int(sys.argv[6]) if len(sys.argv) > 6 else 1

NOF_SYMB = 14
NOF_SUBC_PER_RB = 12
alloc = list(range(bwp_size_rb))

fields = [
    ("slot", 10049),
    ("scs_khz", 30),
    ("cp", "normal"),
    ("rnti", 17921),
    ("harq_id", 0),
    ("bwp_size_rb", bwp_size_rb),
    ("bwp_start_rb", 0),
    ("n_id", 1),
    ("nof_tx_layers", nof_tx_layers),
    ("start_symbol_index", 0),
    ("nof_symbols", NOF_SYMB),
    ("modulation", "QPSK"),
    # In units of 1/1024, as the capture writes it (the reader takes it back as an integer).
    ("target_code_rate", 308),
    ("rv", 0),
    ("ldpc_base_graph", 2),
    ("new_data", 1),
    ("tbs_lbrm", 25344),
    ("nof_harq_ack", 0),
    ("dc_position", -1),
    ("dmrs_type", 1),
    ("dmrs_scrambling_id", 1),
    ("dmrs_n_scid", 0),
    ("dmrs_nof_cdm_groups_without_data", 2),
    ("rx_ports", ",".join(str(p) for p in range(nof_rx_ports))),
    ("dmrs_symbols", dmrs_symbols),
    ("alloc_nof_rb", len(alloc)),
    ("alloc_prb", ",".join(str(p) for p in alloc)),
]

with open(prefix + ".txt", "w") as f:
    for key, value in fields:
        f.write(f"{key}={value}\n")

rng = random.Random(seed)
nof_subc = bwp_size_rb * NOF_SUBC_PER_RB
with open(prefix + ".bin", "wb") as f:
    for _port in range(nof_rx_ports):
        for _symbol in range(NOF_SYMB):
            # A flat channel times a per-symbol phase ramp, plus noise: enough structure that the
            # pilots are not all identical, which is what makes the noise estimate non-degenerate.
            phase = rng.uniform(0.0, 6.283)
            c = complex(0.35 * __import__("math").cos(phase), 0.35 * __import__("math").sin(phase))
            for _subc in range(nof_subc):
                x = c + complex(rng.gauss(0.0, 0.08), rng.gauss(0.0, 0.08))
                f.write(struct.pack("<ff", x.real, x.imag))

print(f"{prefix}: bwp={bwp_size_rb}PRB dmrs_symbols={dmrs_symbols} layers={nof_tx_layers} ports={nof_rx_ports} grid={NOF_SYMB}x{nof_subc}")
