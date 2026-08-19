#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# Plots the BLER benchmark results (test/bler_results/*.csv) produced by
# ldpc_metal_bler_test: SNR vs BLER curve pairs, one figure per (base graph,
# lifting size), one curve pair per code rate (CPU solid, GPU layered-NMS
# dashed) with 95% Wilson confidence bands.
#
# Usage: python3 plot_bler.py [--results DIR] [--out DIR] [--show] [--no-ci]

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

FILE_RE = re.compile(r"bler_(\w+)_bg(\d)_z(\d+)_r(\d+\.\d+)\.csv")

# 95% two-sided Wilson score interval on the pass probability (handles p = 0/1
# without degenerating). BLER = 1 - pass probability.
Z95 = 1.959963984540054


def wilson_bler_bounds(pass_count, total):
    p = pass_count / total
    n = total
    denom = 1.0 + Z95 * Z95 / n
    center = (p + Z95 * Z95 / (2.0 * n)) / denom
    half = Z95 * np.sqrt(p * (1.0 - p) / n + Z95 * Z95 / (4.0 * n * n)) / denom
    lo_pass, hi_pass = max(0.0, center - half), min(1.0, center + half)
    # BLER bounds: the pass-interval flips.
    return 1.0 - hi_pass, 1.0 - lo_pass


def load_series(path):
    """Returns (snrs, bler_cpu, bler_gpu, cpu_ci, gpu_ci, cpu_times, gpu_times)."""
    snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times = [], [], [], [], [], [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or line.startswith("snr_db") or not line:
                continue
            parts = line.split(",")
            snr, cpu_pass, gpu_pass, total = (float(parts[0]), int(parts[1]), int(parts[2]), int(parts[3]))
            snrs.append(snr)
            cpu.append(1.0 - cpu_pass / total)
            gpu.append(1.0 - gpu_pass / total)
            cpu_ci.append(wilson_bler_bounds(cpu_pass, total))
            gpu_ci.append(wilson_bler_bounds(gpu_pass, total))
            # Optional per-decoder per-SNR wall-clock columns (seconds, added for
            # the timing annotation; absent in older CSVs).
            if len(parts) >= 6 and parts[4] and parts[5]:
                cpu_times.append(float(parts[4]))
                gpu_times.append(float(parts[5]))
            else:
                cpu_times.append(None)
                gpu_times.append(None)
    return snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times


def plot_group(bg, z, files, outdir, show, with_ci):
    fig, ax = plt.subplots(figsize=(8, 5.5), dpi=150)
    fig.patch.set_facecolor(SURFACE)
    ax.set_facecolor(SURFACE)

    # One rate per color slot (ascending rate = slot order); decoder = line style.
    by_rate = {}
    for f in sorted(files):
        m = FILE_RE.match(os.path.basename(f))
        snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times = load_series(f)
        by_rate.setdefault(float(m.group(4)), (snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times))

    legend_handles = []
    for slot, rate in enumerate(sorted(by_rate)):
        color = RATE_COLORS[slot % len(RATE_COLORS)]
        snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times = by_rate[rate]
        (l_cpu,) = ax.plot(snrs, cpu, color=color, lw=2, ls="-", marker="o", ms=8,
                           mfc=color, mec=SURFACE, mew=1, label=f"R={rate:.3g} CPU", zorder=3)
        (l_gpu,) = ax.plot(snrs, gpu, color=color, lw=2, ls="--", marker="^", ms=9,
                           mfc=SURFACE, mec=color, mew=1.5,
                           label=f"R={rate:.3g} GPU", zorder=3)
        legend_handles.extend([l_cpu, l_gpu])
        if with_ci:
            ax.errorbar(snrs, cpu, yerr=([b - l for (l, _), b in zip(cpu_ci, cpu)],
                                         [h - b for (_, h), b in zip(cpu_ci, cpu)]),
                        fmt="none", ecolor=color, elinewidth=1, capsize=3, zorder=2)
            ax.errorbar(snrs, gpu, yerr=([b - l for (l, _), b in zip(gpu_ci, gpu)],
                                         [h - b for (_, h), b in zip(gpu_ci, gpu)]),
                        fmt="none", ecolor=color, elinewidth=1, capsize=3, zorder=2)

        # Per-decoder timing annotations at each curve's end: the CPU and GPU
        # curves carry their own total wall time and average per-SNR-point time
        # (absent in older CSVs: skipped).
        if any(t is not None for t in cpu_times):
            valid = [t for t in cpu_times if t is not None]
            total_s = sum(valid)
            avg_s = total_s / len(valid)
            ax.annotate(f"R={rate:.3g} CPU: total {total_s:.0f}s · avg {avg_s:.1f}s/pt",
                        xy=(snrs[-1], cpu[-1]),
                        xytext=(8, 10 + 14 * slot), textcoords="offset points",
                        fontsize=7, color=INK_MUTED,
                        bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec=color, lw=0.7, alpha=0.9),
                        zorder=5)
        if any(t is not None for t in gpu_times):
            valid = [t for t in gpu_times if t is not None]
            total_s = sum(valid)
            avg_s = total_s / len(valid)
            ax.annotate(f"R={rate:.3g} GPU: total {total_s:.0f}s · avg {avg_s:.1f}s/pt",
                        xy=(snrs[-1], gpu[-1]),
                        xytext=(8, -12 - 14 * slot), textcoords="offset points",
                        fontsize=7, color=color,
                        bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec=color, lw=0.7, alpha=0.9),
                        zorder=5)

    ax.set_yscale("log")
    ax.set_ylim(3e-3, 1.5)
    ax.set_xlabel("SNR (Es/N0, dB)", color=INK)
    ax.set_ylabel("BLER", color=INK)
    ax.set_title(f"LDPC BLER — BG{bg} Z{z} — CPU vs Metal GPU", color=INK, fontsize=12)

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
    ap.add_argument("--no-ci", action="store_true", help="Suppress the 95%% Wilson confidence bands")
    args = ap.parse_args()

    groups = {}
    for path in sorted(glob.glob(os.path.join(args.results, "bler_*_bg*.csv"))):
        m = FILE_RE.match(os.path.basename(path))
        if not m:
            continue
        groups.setdefault((int(m.group(2)), int(m.group(3))), []).append(path)

    if not groups:
        print(f"no bler_*.csv found in {args.results}")
        return 1
    for (bg, z), files in groups.items():
        plot_group(bg, z, files, args.out, args.show, with_ci=not args.no_ci)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
