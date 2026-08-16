#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# Plots the BLER benchmark results (test/bler_results/*.csv) produced by
# ldpc_metal_bler_test: SNR vs BLER curve families, one figure per (base graph,
# lifting size), one curve pair per code rate (CPU solid, GPU dashed).
#
# Usage: python3 plot_bler.py [--results DIR] [--out DIR] [--show]

import argparse
import glob
import os
import re

import matplotlib.pyplot as plt
import numpy as np

# Validated categorical palette (dataviz reference, light mode), fixed slot order.
RATE_COLORS = ["#2a78d6", "#eb6834", "#1baf7a", "#eda100"]
SURFACE = "#fcfcfb"
INK = "#0b0b0b"
INK_MUTED = "#52514e"
GRID = "#e8e8e6"

FILE_RE = re.compile(r"bler_bg(\d)_z(\d+)_r(\d+\.\d+)\.csv")


def load_series(path):
    """Returns (header: dict, snrs: [float], bler_cpu: [float], bler_gpu: [float])."""
    header = {}
    snrs, cpu, gpu = [], [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#"):
                for tok in line[1:].split():
                    if "=" in tok:
                        k, v = tok.split("=", 1)
                        header[k] = v
                continue
            if line.startswith("snr_db") or not line:
                continue
            parts = line.split(",")
            snr, cpu_pass, gpu_pass, total = (float(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]))
            snrs.append(snr)
            cpu.append(1.0 - cpu_pass / total)
            gpu.append(1.0 - gpu_pass / total)
    return header, snrs, cpu, gpu


def plot_group(bg, z, files, outdir, show):
    fig, ax = plt.subplots(figsize=(8, 5.5), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)

    # One rate per color slot (ascending rate = slot order); decoder = line style.
    rates = []
    for f in sorted(files):
        m = FILE_RE.match(os.path.basename(f))
        header, snrs, cpu, gpu = load_series(f)
        rates.append((float(m.group(3)), header, snrs, cpu, gpu))
    rates.sort(key=lambda t: t[0])

    legend_handles = []
    for slot, (rate, header, snrs, cpu, gpu) in enumerate(rates):
        color = RATE_COLORS[slot % len(RATE_COLORS)]
        label_cpu = f"R={rate:.3g} CPU"
        label_gpu = f"R={rate:.3g} GPU"
        (l_cpu,) = ax.plot(snrs, cpu, color=color, lw=2, ls="-", marker="o", ms=8,
                           mfc=color, mec=SURFACE, mew=1, label=label_cpu, zorder=3)
        (l_gpu,) = ax.plot(snrs, gpu, color=color, lw=2, ls="--", marker="^", ms=9,
                           mfc=SURFACE, mec=color, mew=1.5, label=label_gpu, zorder=3)
        legend_handles.extend([l_cpu, l_gpu])

    ax.set_yscale("log")
    ax.set_ylim(3e-3, 1.5)
    ax.set_xlabel("SNR (Es/N0, dB)", color=INK)
    ax.set_ylabel("BLER", color=INK)
    ax.set_title(f"LDPC BLER — BG{bg} Z{z} — ocudu CPU vs Metal GPU", color=INK, fontsize=12)

    for spine in ax.spines.values():
        spine.set_color(GRID)
    ax.tick_params(colors=INK, labelsize=9)
    ax.grid(True, which="major", color=GRID, lw=0.8)
    ax.grid(True, which="minor", color=GRID, lw=0.4, ls=":")
    ax.legend(handles=legend_handles, loc="lower left", fontsize=8, ncol=2,
              frameon=True, facecolor=SURFACE, edgecolor=GRID, labelcolor=INK)

    out_path = os.path.join(outdir, f"bler_bg{bg}_z{z}.png")
    fig.tight_layout()
    fig.savefig(out_path, facecolor=SURFACE)
    print(f"wrote {out_path}")
    if show:
        plt.show()
    plt.close(fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "bler_results"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "bler_results"))
    ap.add_argument("--show", action="store_true")
    args = ap.parse_args()

    groups = {}
    for path in sorted(glob.glob(os.path.join(args.results, "bler_bg*.csv"))):
        m = FILE_RE.match(os.path.basename(path))
        if not m:
            continue
        groups.setdefault((int(m.group(1)), int(m.group(2))), []).append(path)

    if not groups:
        print(f"no bler_*.csv found in {args.results}")
        return 1
    for (bg, z), files in groups.items():
        plot_group(bg, z, files, args.out, args.show)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
