#!/usr/bin/env python3
"""Build a supervised dataset with a KNOWN target, by replaying synthetic grids through the chain.

Why the real corpus cannot supply this
--------------------------------------
The real label corpus looks like a supervised set - an input grid and a target grid - but its
target is a decision-directed reconstruction, and measurements in sections 35.5/36.3 of
`model_network_design.md` show that what separates the two is the labels' own noise floor:
uncorrelated with the input, flat in SNR, and symmetric between predicting X from Y and Y from X.
A network trained on it can only learn to smooth its input again.

This builder makes the target true by construction. For each capture unit it knows the decoded
transport block, so it can rebuild the transmitted symbols exactly; it knows the channel
recorded next to the capture, so it can build a noise-free received grid; and it injects noise
of its own choosing. Replaying that grid through the real receive path then produces the pair

    input  = the chain's channel estimate (what the chain actually computes)
    target = the channel the grid was built from (ground truth)

so the difference between them is a real estimation error, and it can be made arbitrarily small
by raising the SNR. That is the quantity a refinement network would have to reduce.

Units are restricted to those whose allocation is NOT at PRB 0 and whose absolute slot is well
above the slots-per-frame count. Three separate bugs in this pipeline were only invisible because
of those two coincidences (sections 35.2, 36.2), so the checks below are not paranoia - each one
corresponds to a bug that already happened once.

Usage
-----
    build_synth_dataset.py --units <index.csv> --tb <dir> --corpus <dir> \
        --out <dir> [--snrs 12,18,24] [--max-units N]
"""

import argparse
import glob
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
AI_TRAIN = HERE
# The repository root is found by walking up to the directory that holds `build/`, rather than
# by counting ".." levels from here. The counting version breaks the moment this file is
# imported from a copy living somewhere else, which silently pointed the replay path at
# `signal_processors/build/...` and made every run produce garbage instead of failing loudly.
def _find_repo(start):
    path = start
    for _ in range(12):
        if os.path.isdir(os.path.join(path, "build")) and os.path.isdir(os.path.join(path, "lib")):
            return path
        parent = os.path.dirname(path)
        if parent == path:
            break
        path = parent
    raise SystemExit(f"cannot locate the repository root from {start}")


REPO = _find_repo(HERE)
REPLAY = os.path.join(REPO, "build", "lib", "phy", "upper", "channel_processors",
                      "metal", "ul_chain_replay")

sys.path.insert(0, AI_TRAIN)
import dmrs as DV              # noqa: E402
import synth as SN             # noqa: E402  (the shared, tracked synthesis core)

NOF_SYMBOLS = 14
BUCKET_SUBCARRIERS = 612       # the window the network works on (51 PRB)
# `slot_point::slot_index()` reduces modulo this, and the DM-RS PRG is seeded from it.
SLOTS_PER_FRAME_30KHZ = 20
# The clean target is a property of the unit, not of the SNR, so it is computed once.
TDL_CACHE = {}


read_config = SN.read_config


def fit_tdl_coefficients(observed, subcarriers, nof_taps=16, fft_len=1024):
    """Fit a tapped-delay-line channel to observations at `subcarriers`.

    This is what makes the target clean. The channel recorded next to a capture (`_h.bin`) is
    the chain's own estimate, so using it as the target would build the estimator's error into
    the answer. Fitting a physical model instead projects the observations onto the subspace a
    real channel can occupy, which removes most of the noise: measured on `C_1777_17921`, a
    16-tap fit reproduces the pilot LS estimate to -28.7 dB, while 8 taps only reach -2.6 dB -
    i.e. the channel genuinely needs about 16 taps here, and beyond that the fit stops improving
    because what is left is noise rather than channel structure.

    Returns the tap coefficients.
    """
    freq = subcarriers / fft_len
    basis = np.exp(-2j * np.pi * np.outer(freq, np.arange(nof_taps)))
    coefficients, _, _, _ = np.linalg.lstsq(basis, observed, rcond=None)
    return coefficients


def truth_from_pilots(prefix, cfg, alloc, sub, H):
    """`sub` is where the truth is wanted (the kept subcarriers); the fit itself uses the
    DM-RS positions, and the fitted model is then evaluated on `sub`."""
    """A clean channel for the allocation, via a TDL fit to the DM-RS symbols.

    The same fit is applied per DM-RS symbol, so a channel that drifts within the slot is
    represented rather than averaged away; the two fits are then interpolated across the slot
    the way a receiver interpolates between its pilot symbols.
    """
    grid = np.fromfile(prefix + ".bin", dtype=np.complex64)
    bwp = int(cfg["bwp_size_rb"])
    grid = grid[:NOF_SYMBOLS * bwp * 12].reshape(NOF_SYMBOLS, bwp * 12)
    slot = int(cfg["slot"]) % 20
    pilots = DV.pilot_subcarriers(alloc, dmrs_type=int(cfg["dmrs_type"]))
    dmrs_syms = [int(x) for x in cfg["dmrs_symbols"].split(",")]

    fits = {}
    for symbol in dmrs_syms:
        sequence = DV.sequence(slot, symbol, int(cfg["dmrs_scrambling_id"]),
                               int(cfg["dmrs_n_scid"]), alloc)
        # The DM-RS REs are the even offsets within each allocated PRB; `sub[::2]` is NOT the
        # same set and `pilot_subcarriers` is the only thing that gets it right, so use it.
        coefficients = fit_tdl_coefficients(grid[symbol, pilots] / sequence, pilots)
        # Evaluate the fitted physical channel on the subcarriers the caller asked for.
        freq = sub / 1024.0
        fits[symbol] = np.exp(-2j * np.pi * np.outer(freq, np.arange(len(coefficients)))) @ coefficients

    # Interpolate between the fitted pilot responses, holding flat outside them, which is the
    # usual receiver behaviour and keeps the target defined on every symbol.
    clean = np.zeros((NOF_SYMBOLS, len(sub)), dtype=np.complex64)
    lo, hi = min(dmrs_syms), max(dmrs_syms)
    for symbol in range(NOF_SYMBOLS):
        if symbol in fits:
            clean[symbol] = fits[symbol]
        elif symbol < lo:
            clean[symbol] = fits[lo]
        elif symbol > hi:
            clean[symbol] = fits[hi]
        else:
            below = max(s for s in dmrs_syms if s < symbol)
            above = min(s for s in dmrs_syms if s > symbol)
            weight = (symbol - below) / (above - below)
            clean[symbol] = (1.0 - weight) * fits[below] + weight * fits[above]
    return clean


def _pair_nmse(estimate, target):
    """Complex-array form of `pair_nmse_db`, for the in-loop quality gate.

    Kept as a separate entry point only because the gate works on the [symbol, subcarrier] arrays
    it already holds; it forwards to the same formula.
    """
    return pair_nmse_db(estimate, target)


def pair_nmse_db(estimate, target):
    """Scale-invariant NMSE of `estimate` against `target`, in dB, as complex arrays.

    The gain maps the ESTIMATE onto the TARGET (the estimate is the thing being corrected), so
    `g = <target, estimate> / <estimate, estimate>` and the residual is `target - g * estimate`.
    Both directions were tried while this was being pinned down and they differ by the whole
    scale convention, so the formula is stated here once rather than open-coded at call sites.
    """
    # The two arguments MUST be complex. A real array shaped [..., 2] holding (re, im) pairs
    # accepted here would be flattened into independent real numbers, which is a different
    # quantity entirely and reads about 17 dB better than the truth (measured: -0.64 dB where the
    # complex value is -17.57 dB). Callers convert via `pairs_from_npz`.
    estimate = np.asarray(estimate)
    target = np.asarray(target)
    assert np.iscomplexobj(estimate) and np.iscomplexobj(target), (
        "pair_nmse_db needs complex arrays; a real (re, im) array would be flattened into "
        "independent real numbers and report a completely different number")
    estimate = estimate.ravel()
    target = target.ravel()
    # `np.vdot(a, b)` conjugates its FIRST argument, so `<estimate, target>` is
    # `np.vdot(estimate, target)`. Writing the arguments the other way round silently applies the
    # conjugate on the wrong side: the fitted gain then has the wrong PHASE (its magnitude still
    # comes out right, because |<target, estimate>| = |<estimate, target>|), the residual is no
    # longer orthogonal to the estimate, and the answer is wrong by a large factor - measured at
    # +2.82 dB where the truth is -17.57 dB on the same pair.
    gain = np.vdot(estimate, target) / (np.vdot(estimate, estimate) + 1e-30)
    residual = target - gain * estimate
    reference = gain * estimate
    # At the optimal gain the residual is orthogonal to the estimate. Asserting it turns a silent
    # phase error into a loud failure; the tolerance is loose enough to survive float32 data.
    orthogonality = abs(np.vdot(residual, estimate)) / (
        np.sqrt(np.sum(np.abs(residual) ** 2) * np.sum(np.abs(estimate) ** 2)) + 1e-30)
    # float32 capture data leaves the residual only approximately orthogonal; 1e-3 is far below
    # the ~0.9 that a wrong conjugate produces, and far above the ~5e-5 seen with float32.
    assert orthogonality < 1e-3, (
        f"the fitted gain is not the least-squares optimum (|<residual, estimate>| / "
        f"|residual||estimate| = {orthogonality:.3e}); the conjugate is probably on the wrong side")
    return 10 * np.log10(np.sum(np.abs(residual) ** 2) / (np.sum(np.abs(reference) ** 2) + 1e-30))


def pairs_from_npz(path):
    """Yield (estimate, target) complex pairs from a dataset produced by this tool."""
    data = np.load(path)
    X, Y, widths = data["X"], data["Y"], data["width"]
    for k in range(len(X)):
        n = int(widths[k])
        yield (X[k, :n, :, 0] + 1j * X[k, :n, :, 1],
               Y[k, :n, :, 0] + 1j * Y[k, :n, :, 1])


def selfcheck(path):
    """Recompute the per-pair NMSE from the stored file, so the number has one authority."""
    data = np.load(path)
    snr = data["snr_train"]
    values = np.array([pair_nmse_db(e, t) for e, t in pairs_from_npz(path)])
    print(f"{path}: {len(values)} pairs")
    for level in sorted(set(snr)):
        print(f"  {level:5.1f} dB : median {np.median(values[snr == level]):+7.2f} dB "
              f"(n={int((snr == level).sum())})")
    print(f"  overall    : median {np.median(values):+7.2f} dB")
    return values


def pilot_observations(prefix, cfg, alloc, bwp):
    """The raw pilot LS estimate on the full BWP grid, plus a validity mask.

    This is the network's second input and the only place genuinely NEW information can come from.
    The chain's own estimate is smoothed in the delay domain, which is visible in the data
    (adjacent-subcarrier phase step std 0.037 rad for the chain against 0.052 for the raw pilot
    LS), so refining it alone is bounded by what the smoothing preserved. Giving the network the
    unfiltered observations lets it use the part the chain discarded.

    Returns two [symbol, subcarrier] arrays: the LS values at the pilot REs (zero elsewhere) and
    a 1.0/0.0 mask marking where they are valid.
    """
    grid = np.fromfile(prefix + ".bin", dtype=np.complex64)
    grid = grid[:NOF_SYMBOLS * bwp * 12].reshape(NOF_SYMBOLS, bwp * 12)
    pilots = DV.pilot_subcarriers(alloc, dmrs_type=int(cfg["dmrs_type"]))
    slot = int(cfg["slot"]) % SLOTS_PER_FRAME_30KHZ
    values = np.zeros((NOF_SYMBOLS, bwp * 12), dtype=np.complex64)
    mask = np.zeros((NOF_SYMBOLS, bwp * 12), dtype=np.float32)
    for symbol in [int(x) for x in cfg["dmrs_symbols"].split(",")]:
        sequence = DV.sequence(slot, symbol, int(cfg["dmrs_scrambling_id"]),
                               int(cfg["dmrs_n_scid"]), alloc)
        values[symbol, pilots] = grid[symbol, pilots] / sequence
        mask[symbol, pilots] = 1.0
    return values, mask


def build_and_replay(cfg, tb_bits, H, alloc, snr_db, seed, staged,
                     truth_channel=None):
    """Return the chain's channel estimate for a synthetic grid at `snr_db`.

    `truth_channel` is a full-width [symbol, subcarrier] channel to BUILD the grid from when the
    target is a physical fit rather than the recorded estimate. Passing a subcarrier subset here
    would leave the rest of the band empty, and the chain estimates the whole BWP, so it must be
    full width - a narrow array is what silently turned the estimate into noise before.
    """
    dmrs_syms = [int(x) for x in cfg["dmrs_symbols"].split(",")]
    dmrs_mask = sum(1 << s for s in dmrs_syms)
    mod_enum = {"QPSK": 2, "16QAM": 4, "64QAM": 6, "256QAM": 8}[cfg["modulation"]]

    X, data_pos = SN.encode_symbols(tb_bits, len(alloc), NOF_SYMBOLS, mod_enum, dmrs_mask,
                                    int(cfg["n_id"]), int(cfg["rnti"]), int(cfg["rv"]))
    grid_channel = truth_channel if truth_channel is not None else H
    grid = SN.build_clean(cfg, grid_channel, X, data_pos, alloc, dmrs_syms)

    sig = float(np.mean(np.abs(grid[np.abs(grid) > 0]) ** 2))
    sigma2 = sig / (10.0 ** (snr_db / 10.0))
    rng = np.random.default_rng(seed)
    noise = rng.standard_normal(grid.shape) + 1j * rng.standard_normal(grid.shape)
    grid = grid + noise * np.sqrt(sigma2 / 2.0)

    grid.astype(np.complex64).tofile(staged + ".bin")
    H.astype(np.complex64).tofile(staged + "_h.bin")
    with open(staged + ".txt", "w") as dst:
        dst.write(open(staged + ".src").read())

    subprocess.run([REPLAY, staged, "--metal", "--out", staged + "_out"],
                   check=False, capture_output=True)
    found = glob.glob(staged + "_out*_h.bin")
    if not found:
        return None, None
    return np.fromfile(found[0], dtype=np.complex64), sigma2


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--units")
    parser.add_argument("--tb")
    parser.add_argument("--corpus")
    parser.add_argument("--out", required=True)
    parser.add_argument("--name", default="synth_dataset",
                        help="output basename, so several corpora can be built into one "
                             "directory without overwriting each other")
    parser.add_argument("--snrs", default="12,18,24")
    parser.add_argument("--max-units", type=int, default=0)
    parser.add_argument("--truth", choices=("tdl", "hbin"), default="tdl",
                        help="where the supervised TARGET comes from. 'tdl' fits a 16-tap "
                             "channel to the pilots, which removes most of the noise and gives a "
                             "target independent of the estimator being measured; 'hbin' uses "
                             "the channel recorded next to the capture, which is the chain's own "
                             "estimate and therefore folds its error into the answer.")
    parser.add_argument("--selfcheck", action="store_true",
                        help="recompute the per-pair NMSE of an existing dataset and exit; "
                             "use with --out pointing at the .npz file")
    parser.add_argument("--min-prb", type=int, default=43,
                        help="minimum allocation width in PRB. The network's window is 612 "
                             "subcarriers (51 PRB), so a narrow allocation would leave most of "
                             "it empty and the pair would not represent the operating point. "
                             "43 PRB is 516 subcarriers, the widest class below the 51 PRB units "
                             "that all report sinr around -13 dB (section 36.1).")
    parser.add_argument("--seed", type=int, default=20261010)
    parser.add_argument("--staged", default="/tmp/synth_stage")
    args = parser.parse_args()

    if args.selfcheck:
        selfcheck(args.out)
        return 0
    missing = [n for n in ("units", "tb", "corpus") if getattr(args, n) is None]
    if missing:
        parser.error("missing required arguments: " + ", ".join("--" + m for m in missing))

    snrs = [float(x) for x in args.snrs.split(",")]
    os.makedirs(args.out, exist_ok=True)
    os.makedirs(args.staged, exist_ok=True)

    # --- select units -------------------------------------------------------------------
    # The PRB-0 and small-slot restrictions are load-bearing: see the module docstring.
    candidates = []
    with open(args.units) as handle:
        next(handle)
        for line in handle:
            fields = line.strip().split(",")
            if len(fields) < 6 or fields[2] != "OK":
                continue
            name = fields[0]
            if not os.path.exists(os.path.join(args.tb, name + ".bits")):
                continue
            prefix = os.path.join(args.corpus, name)
            if not (os.path.exists(prefix + ".bin") and os.path.exists(prefix + "_h.bin")):
                continue
            cfg = read_config(prefix)
            alloc = [int(x) for x in cfg["alloc_prb"].split(",")]
            if min(alloc) == 0:
                continue                      # would hide a PRG-start bug
            if int(cfg["slot"]) < 40:
                continue                      # would hide a slot-convention bug
            if len(alloc) < args.min_prb:
                continue                      # too narrow to fill the network's window
            candidates.append((name, prefix, cfg, alloc))

    if args.max_units:
        candidates = candidates[:args.max_units]
    print(f"{len(candidates)} candidate units "
          f"(allocation off PRB 0, absolute slot > 40, width >= {args.min_prb} PRB)")

    inputs, targets, meta = [], [], []
    pilots_in, pilot_masks = [], []
    pair_nmse_log = []
    skipped = []
    for i, (name, prefix, cfg, alloc) in enumerate(candidates):
        bwp = int(cfg["bwp_size_rb"])
        H = np.fromfile(prefix + "_h.bin", dtype=np.complex64).reshape(NOF_SYMBOLS, bwp * 12)
        tb_bits = np.unpackbits(np.fromfile(os.path.join(args.tb, name + ".bits"), dtype=np.uint8))

        staged = os.path.join(args.staged, name)
        with open(staged + ".src", "w") as handle:
            handle.write(open(prefix + ".txt").read())

        # Absolute subcarriers of the allocation, and the slice of the network window they fill.
        sub = np.concatenate([np.arange(r * 12, (r + 1) * 12) for r in alloc])
        # Drop the window-edge subcarriers where the chain's estimate degrades (measured in 36.4).
        keep = sub[:-max(1, int(len(sub) * 0.03))]

        # The channel the grid is built from, and the target it is compared against. With
        # --truth tdl this is the physical fit rather than the recorded estimate, so the pair
        # does not inherit the estimator's own error; see the docstring of `fit_tdl`. It does
        # not depend on the SNR, so it is computed once per unit.
        # `truth` is what the pair is SUPERVISED against (restricted to the kept subcarriers).
        # `grid_channel` is what the grid is BUILT from and must span the whole BWP. With
        # --truth hbin they are the same channel, so no rebuild is needed.
        truth = TDL_CACHE.get(name)
        if truth is None:
            truth = (truth_from_pilots(prefix, cfg, alloc, keep, H) if args.truth == "tdl"
                     else H[:, keep])
            TDL_CACHE[name] = truth
        if truth.shape != (NOF_SYMBOLS, len(keep)):
            print(f"  {name}: truth shape {truth.shape} unexpected, skipped")
            continue
        # The pilot observations are a property of the capture, not of the SNR, so they are read
        # once per unit and reused for every SNR point.
        pilot_vals, pilot_mask = pilot_observations(prefix, cfg, alloc, bwp)
        pilot_vals = pilot_vals[:, keep]
        pilot_mask = pilot_mask[:, keep].astype(np.float32)

        grid_channel = None
        if args.truth == "tdl":
            grid_channel = np.zeros_like(H)
            grid_channel[:, keep] = truth

        for snr in snrs:
            # A unit whose transport block does not match its allocation cannot be re-encoded at
            # all. That is a property of the capture, not of this tool, so it is recorded and
            # skipped rather than allowed to abort the whole corpus.
            try:
                hat, sigma2 = build_and_replay(cfg, tb_bits, H, alloc, snr, args.seed + i,
                                               staged, truth_channel=grid_channel)
            except (AssertionError, ValueError) as error:
                skipped.append((name, str(error).split("\n")[0][:120]))
                break
            if hat is None:
                continue
            hat = hat.reshape(NOF_SYMBOLS, bwp * 12)
            if hat.shape != H.shape:
                continue

            # Per-pair diagnostic. A pair whose input does not track its target is worthless,
            # and the failure mode is silent (the chain still returns an array), so the NMSE is
            # computed and printed as the set is built rather than only at the end.
            reference = truth.astype(np.complex64)
            estimate = hat[:, keep].astype(np.complex64)
            aa = reference.ravel()
            bb = estimate.ravel()
            gain = np.vdot(aa, bb) / np.vdot(aa, aa)
            pair_nmse = _pair_nmse(estimate, reference)
            if pair_nmse > -6.0:
                print(f"  {name} @ {snr} dB: NMSE {pair_nmse:+.2f} dB looks wrong, dropped "
                      f"(|hat|/|H| = {np.abs(bb).mean() / np.abs(aa).mean():.3f})")
                continue
            pair_nmse_log.append(pair_nmse)

            inputs.append(estimate)
            targets.append(reference)
            pilots_in.append(pilot_vals.astype(np.complex64))
            pilot_masks.append(pilot_mask)
            meta.append((name, snr, float(np.mean(np.abs(truth[0]) ** 2)), float(sigma2)))

        if (i + 1) % 10 == 0:
            print(f"  ... {i + 1}/{len(candidates)} units, {len(inputs)} pairs")

    if skipped:
        print(f"{len(skipped)} unit(s) could not be re-encoded and were skipped:")
        for name, reason in skipped[:5]:
            print(f"  {name}: {reason}")
        if len(skipped) > 5:
            print(f"  ... and {len(skipped) - 5} more")

    if not inputs:
        raise SystemExit("no pairs produced")

    # The network works on [subcarrier, symbol, complex] (section 0.4), while the arrays above
    # are naturally [symbol, subcarrier] because that is how the grid is laid out on disk.
    # Allocations differ in width between units (43/46/50 PRB here), so every pair is padded
    # into the fixed window on its own rather than assuming one common width.
    X = np.zeros((len(inputs), BUCKET_SUBCARRIERS, NOF_SYMBOLS, 2), dtype=np.float32)
    Y = np.zeros_like(X)
    P = np.zeros_like(X)                     # pilot LS observations, same layout as X
    Pm = np.zeros((len(inputs), BUCKET_SUBCARRIERS, NOF_SYMBOLS), dtype=np.float32)
    width_arr = np.zeros(len(inputs), dtype=np.int64)
    for k, (a, b) in enumerate(zip(inputs, targets)):
        # `a`/`b` are [symbol, subcarrier]; the window is [subcarrier, symbol].
        n = min(a.shape[1], BUCKET_SUBCARRIERS)
        width_arr[k] = n
        a = a[:, :n].T          # [symbol, subcarrier] -> [subcarrier, symbol]
        b = b[:, :n].T
        X[k, :n, :, 0] = a.real
        X[k, :n, :, 1] = a.imag
        Y[k, :n, :, 0] = b.real
        Y[k, :n, :, 1] = b.imag
        pv = pilots_in[k][:, :n].T          # [symbol, subcarrier] -> [subcarrier, symbol]
        P[k, :n, :, 0] = pv.real
        P[k, :n, :, 1] = pv.imag
        Pm[k, :n, :] = pilot_masks[k][:, :n].T

    # Cross-check the packed arrays against the values measured while building. The window
    # stores complex samples as a trailing [re, im] pair, so a magnitude MUST be taken as
    # hypot(re, im): `np.abs()` on that pair yields |re| and |im| separately, and averaging those
    # understates the modulus by a factor that looks exactly like a scale error (measured: 0.64,
    # i.e. -3.9 dB, which is the size of the "zero-output" score the file reports). Getting this
    # wrong silently makes a usable set look broken.
    for k in range(min(2, len(inputs))):
        n = int(width_arr[k])
        packed_a = Y[k, :n]
        packed_b = X[k, :n]
        packed_nmse = _pair_nmse(packed_b[:, :, 0] + 1j * packed_b[:, :, 1],
                                 packed_a[:, :, 0] + 1j * packed_a[:, :, 1])
        direct_nmse = _pair_nmse(inputs[k], targets[k])
        print(f"  check pair {k} (width {n}): packed {packed_nmse:+.2f} dB, "
              f"from arrays {direct_nmse:+.2f} dB")

    snr_arr = np.array([m[1] for m in meta], dtype=np.float64)
    np.savez_compressed(os.path.join(args.out, args.name + ".npz"),
                        X=X, Y=Y, P=P, P_mask=Pm, snr_train=snr_arr, width=width_arr)

    # A per-pair NMSE tells immediately whether the set is usable at all.
    # Norms are taken over the valid [0, width) rows only, so the zero padding does not dilute.
    def _as_complex(block):
        """[subcarrier, symbol, (re, im)] -> [subcarrier, symbol] complex."""
        return block[:, :, 0] + 1j * block[:, :, 1]

    def _nmse_after_gain(estimate, target):
        """Scalar NMSE once the best single complex gain is removed."""
        a = target.ravel()
        b = estimate.ravel()
        gain = np.vdot(a, b) / np.vdot(a, a)
        return float(np.sum(np.abs(b - gain * a) ** 2) / np.sum(np.abs(gain * a) ** 2))

    nmse = np.array([10 * np.log10(_nmse_after_gain(
        _as_complex(X[k, :width_arr[k]]), _as_complex(Y[k, :width_arr[k]])))
        for k in range(len(X))])
    print()
    print(f"wrote {args.out}/{args.name}.npz  X{X.shape} Y{Y.shape} "
          f"widths {int(width_arr.min())}..{int(width_arr.max())}")
    if pair_nmse_log:
        print(f"per-pair NMSE of the input against its target: median "
              f"{np.median(pair_nmse_log):+.2f} dB over {len(pair_nmse_log)} pairs")
    print("per-pair |X-Y|^2/|Y|^2 (dB), i.e. what a zero-output network would score:")
    for snr in sorted(set(snr_arr)):
        sel = snr_arr == snr
        print(f"  {snr:5.1f} dB : median {np.median(nmse[sel]):+7.2f} dB  (n={int(sel.sum())})")
    with open(os.path.join(args.out, args.name + "_meta.csv"), "w") as handle:
        handle.write("unit,snr_db,channel_power,noise_variance\n")
        for name, snr, cp, s2 in meta:
            handle.write(f"{name},{snr},{cp:.6g},{s2:.6g}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
