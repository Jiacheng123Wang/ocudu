#!/usr/bin/env python3
"""Compare two PUSCH captures stage by stage (see OCUDU_UL_DUMP in the PUSCH processor).

Each configuration of the receiver (CPU chain, Metal chain with and without the DFT
pipelining) writes one set of files per reception:

    <prefix>_<slot>_<rnti>.txt       PDU configuration
    <prefix>_<slot>_<rnti>.bin       received resource grid      -> DFT and its pipelining
    <prefix>_<slot>_<rnti>_ce.txt    channel estimator scalars   -> channel estimation
    <prefix>_<slot>_<rnti>_llr.bin   demodulated soft bits       -> equalization, demapping,
                                                                    soft-bit scale

Given two capture prefixes this script reports, per reception, which of those stages differ and
by how much.  The first stage that differs localizes the defect; stages after it are expected to
differ as well.

Usage:  scripts/ul_stage_diff.py <prefix_a> <prefix_b> [--max-receptions 8] [--quiet]
"""

import argparse
import glob
import math
import os
import struct
import sys


def read_text(path):
    values = {}
    with open(path, "r", errors="ignore") as handle:
        for line in handle:
            if "=" in line:
                key, _, value = line.strip().partition("=")
                values[key] = value
    return values


def read_grid(path, nof_ports, nof_subc, nof_symbols=14):
    """Returns a list of per-port symbol lists of complex floats."""
    raw = open(path, "rb").read()
    expect = nof_ports * nof_symbols * nof_subc * 8
    if len(raw) != expect:
        raise ValueError("grid %s: %d bytes, expected %d" % (path, len(raw), expect))
    values = struct.unpack("<%df" % (len(raw) // 4), raw)
    ports = []
    cursor = 0
    for _ in range(nof_ports):
        symbols = []
        for _ in range(nof_symbols):
            symbol = [(values[cursor + 2 * i], values[cursor + 2 * i + 1]) for i in range(nof_subc)]
            symbols.append(symbol)
            cursor += 2 * nof_subc
        ports.append(symbols)
    return ports


def read_llr(path):
    """Returns the concatenated soft bits of the codeblocks."""
    out = []
    with open(path, "rb") as handle:
        while True:
            header = handle.read(4)
            if len(header) < 4:
                break
            (size,) = struct.unpack("<I", header)
            data = handle.read(size)
            if len(data) < size:
                break
            out.extend(struct.unpack("<%db" % size, data))
    return out


def grid_difference(a, b):
    """Returns (max |diff|, mean |diff|, number of differing REs, total REs)."""
    max_diff = 0.0
    sum_diff = 0.0
    differing = 0
    total = 0
    for ports_a, ports_b in zip(a, b):
        for symbol_a, symbol_b in zip(ports_a, ports_b):
            for (ra, ia), (rb, ib) in zip(symbol_a, symbol_b):
                diff = max(abs(ra - rb), abs(ia - ib))
                max_diff = max(max_diff, diff)
                sum_diff += diff
                differing += diff > 1e-12
                total += 1
    return max_diff, sum_diff / max(1, total), differing, total


def llr_summary(llr):
    if not llr:
        return None
    zeros = sum(1 for v in llr if v == 0)
    clips = sum(1 for v in llr if abs(v) >= 127)
    mean_abs = sum(abs(v) for v in llr) / len(llr)
    return {"n": len(llr), "zero_pct": 100.0 * zeros / len(llr), "clip_pct": 100.0 * clips / len(llr), "mean_abs": mean_abs}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("prefix_a")
    parser.add_argument("prefix_b")
    parser.add_argument("--max-receptions", type=int, default=16)
    parser.add_argument("--quiet", action="store_true", help="only the verdict per reception")
    args = parser.parse_args()

    def index(prefix):
        found = {}
        for path in glob.glob(prefix + "_*_*.txt"):
            if path.endswith("_ce.txt"):
                continue
            key = os.path.basename(path)[len(os.path.basename(prefix)) + 1 : -4]
            found[key] = path
        return found

    a_index = index(args.prefix_a)
    b_index = index(args.prefix_b)
    shared = sorted(set(a_index) & set(b_index))
    print("captures: %s -> %d receptions, %s -> %d receptions, %d in common"
          % (args.prefix_a, len(a_index), args.prefix_b, len(b_index), len(shared)))
    if not shared:
        print("no reception is present in both captures: the two runs saw different traffic")
        return 1

    verdicts = []
    for key in shared[: args.max_receptions]:
        cfg_a = read_text(a_index[key])
        cfg_b = read_text(b_index[key])
        row = {"key": key, "pdu_match": True, "grid": "n/a", "ce": "n/a", "llr": "n/a", "first": "llr"}

        for field in ("modulation", "target_code_rate", "rv", "bwp_size_rb", "alloc_nof_rb",
                      "dmrs_symbols", "nof_tx_layers", "rx_ports", "dmrs_nof_cdm_groups_without_data"):
            if cfg_a.get(field) != cfg_b.get(field):
                row["pdu_match"] = False

        nof_ports = len(cfg_a.get("rx_ports", "0").split(","))
        nof_subc = int(cfg_a.get("bwp_size_rb", "0")) * 12

        path_a = a_index[key][:-4] + ".bin"
        path_b = b_index[key][:-4] + ".bin"
        if os.path.exists(path_a) and os.path.exists(path_b):
            try:
                grid_a = read_grid(path_a, nof_ports, nof_subc)
                grid_b = read_grid(path_b, nof_ports, nof_subc)
                max_diff, mean_diff, differing, total = grid_difference(grid_a, grid_b)
                row["grid"] = "max %.3e mean %.3e (%d/%d REs differ)" % (max_diff, mean_diff, differing, total)
                row["first"] = "grid" if differing != 0 else "later"
            except ValueError as error:
                row["grid"] = str(error)

        path_a = a_index[key][:-4] + "_ce.txt"
        path_b = b_index[key][:-4] + "_ce.txt"
        if os.path.exists(path_a) and os.path.exists(path_b):
            ce_a = [read_text(path_a)] if os.path.getsize(path_a) else []
        else:
            ce_a = []
        if ce_a:
            lines_a = open(path_a).read().strip().split("\n")
            lines_b = open(path_b).read().strip().split("\n")
            for line_a, line_b in zip(lines_a, lines_b):
                fields_a = dict(item.split("=") for item in line_a.split())
                fields_b = dict(item.split("=") for item in line_b.split())
                deltas = []
                for name in ("noise_variance", "snr", "rsrp", "epre", "ta_us"):
                    try:
                        va, vb = float(fields_a[name]), float(fields_b[name])
                    except (KeyError, ValueError):
                        continue
                    if va > 0.0 and vb > 0.0:
                        deltas.append("%s %.3f dB" % (name, 10.0 * math.log10(vb / va)) if "var" in name or name in ("rsrp", "epre")
                                      else "%s %+.4f" % (name, vb - va))
                row["ce"] = ", ".join(deltas) if deltas else "identical"
                if deltas and row["first"] == "later":
                    row["first"] = "ce"

        path_a = a_index[key][:-4] + "_llr.bin"
        path_b = b_index[key][:-4] + "_llr.bin"
        if os.path.exists(path_a) and os.path.exists(path_b):
            llr_a = read_llr(path_a)
            llr_b = read_llr(path_b)
            summary_a = llr_summary(llr_a)
            summary_b = llr_summary(llr_b)
            if summary_a and summary_b:
                first = next((i for i, (x, y) in enumerate(zip(llr_a, llr_b)) if x != y), None)
                row["llr"] = ("n %d/%d, mean|L| %.1f/%.1f, zero %.1f%%/%.1f%%, clip %.1f%%/%.1f%%, "
                              "first differing index %s"
                              % (summary_a["n"], summary_b["n"], summary_a["mean_abs"], summary_b["mean_abs"],
                                 summary_a["zero_pct"], summary_b["zero_pct"],
                                 summary_a["clip_pct"], summary_b["clip_pct"],
                                 "none" if first is None else first))
                if first is not None and row["first"] == "later":
                    row["first"] = "llr"

        verdicts.append(row)
        if not args.quiet:
            print("\n[%s] pdu %s" % (key, "matches" if row["pdu_match"] else "DIFFERS"))
            print("    grid : %s" % row["grid"])
            print("    ce   : %s" % row["ce"])
            print("    llr  : %s" % row["llr"])

    print("\n== verdict: first stage that differs")
    for row in verdicts:
        print("  %-16s %s" % (row["key"], row["first"] if row["pdu_match"] else "PDU (different reception)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
