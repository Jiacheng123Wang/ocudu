#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# Plots the BLER benchmark results (test/bler_results/*.csv) produced by
# ldpc_metal_bler_test. Two figures per (base graph, lifting size):
#   1. bler_bg{bg}_z{z}.png - SNR vs BLER curve pairs, one curve pair per code
#      rate (CPU solid, GPU dashed) with 95% Wilson confidence bands.
#   2. bler_bg{bg}_z{z}_latency.png - SNR vs decode time (us, log scale) per
#      decoder from the per-SNR latency columns of the CSV (mean/median/p95/p99
#      of the CRC-OK decodes). CPU mean solid, GPU mean dashed, GPU p95 dotted.
#      A decoder not evaluated at an SNR (--snrs-cpu split sweeps) or without
#      any CRC-OK sample contributes no point there.
#
# Usage: python3 plot_bler.py [--results DIR] [--out DIR] [--show] [--no-ci] [--no-latency]

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


def _f(parts, idx):
    """Parses a float CSV cell; empty cells (decoder not evaluated at the SNR) are None."""
    if idx >= len(parts) or not parts[idx].strip():
        return None
    try:
        return float(parts[idx])
    except ValueError:
        return None


def load_series(path):
    """Returns (snrs, cpu_bler, gpu_bler, cpu_ci, gpu_ci, cpu_times, gpu_times, cpu_lat, gpu_lat).

    cpu_bler/gpu_bler are None where the decoder was not evaluated (split --snrs sweeps).
    cpu_lat/gpu_lat are (mean, median, p95, p99) in us per SNR, None when the decoder had
    no CRC-OK sample at that SNR.
    """
    snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times = [], [], [], [], [], [], []
    cpu_lat, gpu_lat = [], []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("#") or line.startswith("snr_db") or not line:
                continue
            parts = line.split(",")
            snr = _f(parts, 0)
            cpu_pass = _f(parts, 1)
            gpu_pass = _f(parts, 2)
            total = _f(parts, 3)
            snrs.append(snr)
            if cpu_pass is not None and total:
                cpu.append(1.0 - int(cpu_pass) / int(total))
                cpu_ci.append(wilson_bler_bounds(int(cpu_pass), int(total)))
            else:
                cpu.append(None)
                cpu_ci.append(None)
            if gpu_pass is not None and total:
                gpu.append(1.0 - int(gpu_pass) / int(total))
                gpu_ci.append(wilson_bler_bounds(int(gpu_pass), int(total)))
            else:
                gpu.append(None)
                gpu_ci.append(None)
            cpu_times.append(_f(parts, 4))
            gpu_times.append(_f(parts, 5))
            # Latency column blocks (us): mean, median, min, max, p95, p99.
            cpu_block = parts[6:12] if len(parts) > 6 else []
            gpu_block = parts[12:18] if len(parts) > 12 else []
            if cpu_block and any(x.strip() for x in cpu_block):
                cpu_lat.append((_f(cpu_block, 0), _f(cpu_block, 1), _f(cpu_block, 4), _f(cpu_block, 5)))
            else:
                cpu_lat.append(None)
            if gpu_block and any(x.strip() for x in gpu_block):
                gpu_lat.append((_f(gpu_block, 0), _f(gpu_block, 1), _f(gpu_block, 4), _f(gpu_block, 5)))
            else:
                gpu_lat.append(None)
    return snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times, cpu_lat, gpu_lat


def _style_axes(ax, xlabel, ylabel, title):
    ax.set_facecolor(SURFACE)
    ax.set_xlabel(xlabel, color=INK)
    ax.set_ylabel(ylabel, color=INK)
    ax.set_title(title, color=INK, fontsize=12)
    for spine in ax.spines.values():
        spine.set_color(GRID)
    ax.tick_params(colors=INK, labelsize=9)
    ax.grid(True, which="major", color=GRID, lw=0.8)
    ax.grid(True, which="minor", color=GRID, lw=0.4, ls=":")


def plot_group(bg, z, files, outdir, show, with_ci, with_latency):
    fig, ax = plt.subplots(figsize=(8, 5.5), dpi=150)
    fig.patch.set_facecolor(SURFACE)

    # One rate per color slot (ascending rate = slot order); decoder = line style.
    by_rate = {}
    for f in sorted(files):
        m = FILE_RE.match(os.path.basename(f))
        snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times, cpu_lat, gpu_lat = load_series(f)
        by_rate.setdefault(float(m.group(4)), (snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times, cpu_lat, gpu_lat))

    legend_handles = []
    lat_fig = None
    lat_ax = None
    if with_latency:
        lat_fig, lat_ax = plt.subplots(figsize=(8, 5.5), dpi=150)
        lat_fig.patch.set_facecolor(SURFACE)
    lat_legend = []

    for slot, rate in enumerate(sorted(by_rate)):
        color = RATE_COLORS[slot % len(RATE_COLORS)]
        snrs, cpu, gpu, cpu_ci, gpu_ci, cpu_times, gpu_times, cpu_lat, gpu_lat = by_rate[rate]
        # NaN keeps the point out of the line without breaking the color/legend.
        cpu_np = np.asarray([np.nan if v is None else v for v in cpu])
        gpu_np = np.asarray([np.nan if v is None else v for v in gpu])
        (l_cpu,) = ax.plot(snrs, cpu_np, color=color, lw=2, ls="-", marker="o", ms=8,
                           mfc=color, mec=SURFACE, mew=1, label=f"R={rate:.3g} CPU", zorder=3)
        (l_gpu,) = ax.plot(snrs, gpu_np, color=color, lw=2, ls="--", marker="^", ms=9,
                           mfc=SURFACE, mec=color, mew=1.5,
                           label=f"R={rate:.3g} GPU", zorder=3)
        legend_handles.extend([l_cpu, l_gpu])
        if with_ci:
            for snr, c, ci in zip(snrs, cpu, cpu_ci):
                if c is None or ci is None:
                    continue
                lo, hi = ci
                ax.errorbar([snr], [c], yerr=[[c - lo], [hi - c]], fmt="none",
                            ecolor=color, elinewidth=1, capsize=3, zorder=2)
            for snr, g, ci in zip(snrs, gpu, gpu_ci):
                if g is None or ci is None:
                    continue
                lo, hi = ci
                ax.errorbar([snr], [g], yerr=[[g - lo], [hi - g]], fmt="none",
                            ecolor=color, elinewidth=1, capsize=3, zorder=2)

        # Per-decoder timing annotations at each curve's end: the CPU and GPU
        # curves carry their own total wall time and average per-SNR-point time.
        cpu_pts = [(s, c) for s, c in zip(snrs, cpu) if c is not None]
        gpu_pts = [(s, g) for s, g in zip(snrs, gpu) if g is not None]
        valid_cpu_t = [t for t in cpu_times if t is not None]
        valid_gpu_t = [t for t in gpu_times if t is not None]
        if cpu_pts and valid_cpu_t:
            total_s = sum(valid_cpu_t)
            avg_s = total_s / len(valid_cpu_t)
            ax.annotate(f"R={rate:.3g} CPU: total {total_s:.0f}s · avg {avg_s:.1f}s/pt",
                        xy=cpu_pts[-1], xytext=(8, 10 + 14 * slot), textcoords="offset points",
                        fontsize=7, color=INK_MUTED,
                        bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec=color, lw=0.7, alpha=0.9),
                        zorder=5)
        if gpu_pts and valid_gpu_t:
            total_s = sum(valid_gpu_t)
            avg_s = total_s / len(valid_gpu_t)
            ax.annotate(f"R={rate:.3g} GPU: total {total_s:.0f}s · avg {avg_s:.1f}s/pt",
                        xy=gpu_pts[-1], xytext=(8, -12 - 14 * slot), textcoords="offset points",
                        fontsize=7, color=color,
                        bbox=dict(boxstyle="round,pad=0.25", fc=SURFACE, ec=color, lw=0.7, alpha=0.9),
                        zorder=5)

        if lat_ax is not None:
            cpu_mean = np.asarray([np.nan if lat is None or lat[0] is None else lat[0] for lat in cpu_lat])
            gpu_mean = np.asarray([np.nan if lat is None or lat[0] is None else lat[0] for lat in gpu_lat])
            gpu_p95 = np.asarray([np.nan if lat is None or lat[2] is None else lat[2] for lat in gpu_lat])
            (lc,) = lat_ax.plot(snrs, cpu_mean, color=color, lw=2, ls="-", marker="o", ms=7,
                                mfc=color, mec=SURFACE, mew=1, label=f"R={rate:.3g} CPU mean", zorder=3)
            (lg,) = lat_ax.plot(snrs, gpu_mean, color=color, lw=2, ls="--", marker="^", ms=8,
                                mfc=SURFACE, mec=color, mew=1.5, label=f"R={rate:.3g} GPU mean", zorder=3)
            (lp,) = lat_ax.plot(snrs, gpu_p95, color=color, lw=1, ls=":", marker=".", ms=4,
                                alpha=0.75, label=f"R={rate:.3g} GPU p95", zorder=2)
            lat_legend.extend([lc, lg, lp])

    ax.set_yscale("log")
    ax.set_ylim(3e-3, 1.5)
    _style_axes(ax, "SNR (Es/N0, dB)", "BLER", f"LDPC BLER — BG{bg} Z{z} — CPU vs Metal GPU")
    ax.legend(handles=legend_handles, loc="lower left", fontsize=8, ncol=2,
              frameon=True, facecolor=SURFACE, edgecolor=GRID, labelcolor=INK)

    out_path = os.path.join(outdir, f"bler_bg{bg}_z{z}.png")
    fig.tight_layout()
    fig.savefig(out_path, facecolor=SURFACE)
    print(f"wrote {out_path}")
    if show:
        plt.show()
    plt.close(fig)

    if lat_ax is not None:
        lat_ax.set_yscale("log")
        _style_axes(lat_ax, "SNR (Es/N0, dB)", "Decode time (us, CRC-OK decodes)",
                    f"LDPC decode latency — BG{bg} Z{z} — CPU vs Metal GPU")
        lat_ax.legend(handles=lat_legend, loc="upper right", fontsize=8, ncol=2,
                      frameon=True, facecolor=SURFACE, edgecolor=GRID, labelcolor=INK)
        lat_path = os.path.join(outdir, f"bler_bg{bg}_z{z}_latency.png")
        lat_fig.tight_layout()
        lat_fig.savefig(lat_path, facecolor=SURFACE)
        print(f"wrote {lat_path}")
        if show:
            plt.show()
        plt.close(lat_fig)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "bler_results"))
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "bler_results"))
    ap.add_argument("--show", action="store_true")
    ap.add_argument("--no-ci", action="store_true", help="Suppress the 95%% Wilson confidence bands")
    ap.add_argument("--no-latency", action="store_true", help="Suppress the decode-latency figures")
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
        plot_group(bg, z, files, args.out, args.show, with_ci=not args.no_ci, with_latency=not args.no_latency)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
