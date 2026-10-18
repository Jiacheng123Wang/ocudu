#!/usr/bin/env python3
"""Classic vs AI on ONE recorded reception at a time: what does the arm change?

The question this answers is the one a live leg cannot: on the SAME IQ, with the SAME PDU, the
SAME decoder and the SAME iteration cap, the two depth-3 arms are run one after the other and
their verdicts and publications are compared per reception. Nothing about the radio, the
scheduler, the phone or the machine can enter the difference, because there is only one input
and the two runs differ in exactly one configuration knob (`OCUDU_REPLAY_RECEIVER`).

This is what makes an ATTACH capture useful: the attach is the traffic the corpus does not
contain -- Msg3, the RRC/capability/registration chain, narrow allocations, low MCS, plenty of
retransmissions -- and it is where the two arms could differ for a reason that has nothing to
do with throughput.

METHOD
  for every capture <dir>/C_<slot>_<rnti>{.txt,.bin}:
    ul_chain_replay <prefix> --metal --out <stage>            OCUDU_REPLAY_RECEIVER=classic
    ul_chain_replay <prefix> --metal --out <stage>            OCUDU_REPLAY_RECEIVER=ai
                                                              OCUDU_REPLAY_MODEL=<bundle>
  and the `[...] tbs=.. mod: crc=.. iterations=.. sinr=..` line each run prints is parsed.

  STRICTLY SERIAL. The harness carries a measured warning: under many concurrent instances it
  intermittently produces WRONG results (39 of 980 captures differed between two runs of the same
  configuration). One replay at a time, and nothing else on the GPU, is the only reading that is
  a measurement.

WHAT ONE-SHOT MEANS FOR A RETRANSMISSION
  Each capture is replayed in its own process, so a `new_data=0` reception meets an EMPTY soft
  buffer. Its one-shot verdict is therefore not the field's verdict (the field combined with the
  earlier transmission) and is reported separately (`--split-rv`): for those receptions the
  trustworthy comparison is the PUBLISHED SINR and whether the two arms agree on the verdict,
  not whether either decoded. A retransmission that decodes one-shot did so on its own evidence.

Usage:
  attach_ab.py --captures <dir> --model <d3_modcond_ft.mlmodelc> [--limit N] [--stride K]
               [--min-prb N] [--max-prb N] [--out <csv>] [--keep-staged]
  attach_ab.py --captures <dir>                      # classic only: the reference pass
"""

import argparse
import csv
import math
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
REPLAY = os.path.join(ROOT, "build/lib/phy/upper/channel_processors/metal/ul_chain_replay")

# The tool's per-reception line:
#   "  [0] tbs=976 slot=10007 rnti=17921 QPSK: crc=OK iterations=2 sinr=16.59 dB epre=0.00 dB rsrp=0.00 dB"
RESULT = re.compile(
    r"\[\d+\]\s+tbs=(?P<tbs>\d+)\s+slot=(?P<slot>\d+)\s+rnti=(?P<rnti>\d+)\s+(?P<mod>\S+):"
    r"\s+crc=(?P<crc>OK|KO)\s+iterations=(?P<iters>\d+)\s+sinr=(?P<sinr>-?[\d.]+) dB"
    r"(?:\s+epre=(?P<epre>-?[\d.]+) dB)?(?:\s+rsrp=(?P<rsrp>-?[\d.]+) dB)?")


def read_capture(prefix):
    """The metadata the A/B reports: what the reception WAS, independent of either arm."""
    fields = {}
    with open(prefix + ".txt") as f:
        for line in f:
            if "=" in line:
                key, value = line.rstrip("\n").split("=", 1)
                fields[key] = value
    return fields


def replay(prefix, receiver, model, staged, dec_iterations):
    env = dict(os.environ, OCUDU_REPLAY_RECEIVER=receiver)
    env.pop("OCUDU_REPLAY_MODEL", None)
    if model:
        env["OCUDU_REPLAY_MODEL"] = model
    if dec_iterations:
        env["OCUDU_REPLAY_DEC_ITERATIONS"] = str(dec_iterations)
    out = os.path.join(staged, "s")
    proc = subprocess.run([REPLAY, prefix, "--metal", "--out", out],
                          env=env, capture_output=True, text=True, check=False)
    text = proc.stdout + proc.stderr
    match = RESULT.search(text)
    if match is None:
        return {"error": (proc.stdout + proc.stderr).strip().splitlines()[-1:] or ["no output"]}, text
    return match.groupdict(), text


def fnum(value):
    try:
        return float(value)
    except (TypeError, ValueError):
        return float("nan")


def stats(values):
    values = [v for v in values if not math.isnan(v)]
    if not values:
        return float("nan"), float("nan"), float("nan"), float("nan")
    mean = sum(values) / len(values)
    rms = math.sqrt(sum(v * v for v in values) / len(values))
    ordered = sorted(values)
    p90 = ordered[min(len(ordered) - 1, int(round(0.9 * (len(ordered) - 1))))]
    return mean, rms, p90, max(abs(v) for v in values)


def median_delta(rows):
    """Median published-SINR difference over the receptions the AI arm actually COMPUTED.

    A forwarded (degraded) reception's numbers are the classical chain's, so its difference is 0.00
    by construction; counting it as agreement would turn "the arm did not run" into "the arms match"
    -- the one conflation this probe exists to prevent.
    """
    values = [r["dsinr"] for r in rows if r.get("arm_ran")]
    return sorted(values)[len(values) // 2] if values else float("nan")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--captures", required=True, help="directory of C_<slot>_<rnti> captures")
    parser.add_argument("--model", default="", help="bundle for the AI arm (absent: classic only)")
    parser.add_argument("--limit", type=int, default=0, help="replay at most N captures")
    parser.add_argument("--stride", type=int, default=1, help="take every K-th capture")
    parser.add_argument("--min-prb", type=int, default=0)
    parser.add_argument("--max-prb", type=int, default=0)
    parser.add_argument("--only-new-data", action="store_true",
                        help="skip new_data=0: a one-shot retransmission verdict is not the field's")
    parser.add_argument("--dec-iterations", type=int, default=0,
                        help="decoder cap (default: the application's 6, set by the tool)")
    parser.add_argument("--staged", default=os.path.join(os.path.dirname(__file__), "..",
                                                         "work_tmp", "tmp", "ab_stage"))
    parser.add_argument("--keep-staged", action="store_true")
    parser.add_argument("--out", default="")
    args = parser.parse_args()

    if not os.path.exists(REPLAY):
        raise SystemExit(f"missing {REPLAY}: build it first (cmake --build build --target ul_chain_replay)")
    if args.model and not os.path.isdir(args.model):
        raise SystemExit(f"--model {args.model} is not a bundle directory")

    staged = os.path.abspath(args.staged)
    os.makedirs(staged, exist_ok=True)

    stems = []
    for name in sorted(os.listdir(args.captures)):
        if not name.startswith("C_") or not name.endswith(".txt") or name.endswith("_ce.txt"):
            continue
        prefix = os.path.join(args.captures, name[:-4])
        if not os.path.exists(prefix + ".bin"):
            continue
        stems.append(prefix)
    if not stems:
        raise SystemExit(f"no C_<slot>_<rnti>{{.txt,.bin}} capture in {args.captures}")

    rows = []
    for index, prefix in enumerate(stems):
        if index % args.stride:
            continue
        meta = read_capture(prefix)
        nof_prb = int(meta.get("alloc_nof_rb", 0))
        if args.min_prb and nof_prb < args.min_prb:
            continue
        if args.max_prb and nof_prb > args.max_prb:
            continue
        if args.only_new_data and meta.get("new_data", "1") == "0":
            continue
        if args.limit and len(rows) >= args.limit:
            break

        classic, classic_text = replay(prefix, "classic", "", staged, args.dec_iterations)
        row = {
            "stem": os.path.basename(prefix),
            "slot": meta.get("slot", ""),
            "rnti": meta.get("rnti", ""),
            "mod": meta.get("modulation", ""),
            "rv": meta.get("rv", ""),
            "new_data": meta.get("new_data", "1"),
            "nof_prb": nof_prb,
            "code_rate": meta.get("target_code_rate", ""),
            "harq_id": meta.get("harq_id", ""),
        }
        if "error" in classic:
            row.update(crc_classic="ERR", note=classic["error"][0][:120])
            rows.append(row)
            print(f"  [{len(rows)}] {row['stem']}: classic replay FAILED: {row['note']}")
            continue
        row.update(crc_classic=classic["crc"], iters_classic=classic["iters"],
                   sinr_classic=fnum(classic["sinr"]))

        if args.model:
            arm, arm_text = replay(prefix, "ai", args.model, staged, args.dec_iterations)
            if "error" in arm:
                row.update(crc_ai="ERR", note=arm["error"][0][:120])
            else:
                row.update(crc_ai=arm["crc"], iters_ai=arm["iters"], sinr_ai=fnum(arm["sinr"]))
                row["dsinr"] = row["sinr_ai"] - row["sinr_classic"]
                # Did the arm actually COMPUTE this reception? Its own stderr says so: an `ai` arm without
                # a bundle forwards and says that once, and every reception outside the arm's envelope is
                # reported as `DEGRADED to the classical chain` (first occurrence, then every 1000th). A
                # degraded reception's numbers are the CLASSICAL chain's, so a zero difference is not
                # "the two arms agree" -- it is "the second one did not run". Never conflate the two.
                degraded = re.search(r"DEGRADED to the classical chain: ([^(]+)", arm_text)
                row["arm_ran"] = ("WITHOUT a model" not in arm_text) and degraded is None
                if degraded:
                    row["degraded"] = degraded.group(1).strip()
                elif "WITHOUT a model" in arm_text:
                    row["degraded"] = "no model bound"
        if not args.keep_staged:
            for leftover in os.listdir(staged):
                path = os.path.join(staged, leftover)
                if os.path.isfile(path):
                    os.remove(path)

        rows.append(row)
        shown = f"  [{len(rows)}] {row['stem']} {row['mod']}/rv{row['rv']}/{row['nof_prb']}PRB"
        shown += f" crc {row['crc_classic']}"
        if "crc_ai" in row:
            shown += f"->{row['crc_ai']} sinr {row['sinr_classic']:.2f}->{row['sinr_ai']:.2f}"
            shown += f" ({row['dsinr']:+.2f}) iters {row['iters_classic']}->{row['iters_ai']}"
        else:
            shown += f" sinr {row['sinr_classic']:.2f} iters {row['iters_classic']}"
        print(shown)

    if not args.keep_staged and os.path.isdir(staged):
        shutil.rmtree(staged, ignore_errors=True)

    out = args.out or os.path.join(os.path.dirname(os.path.abspath(args.captures)),
                                   "ab_" + os.path.basename(os.path.abspath(args.captures)) + ".csv")
    keys = []
    for row in rows:
        for key in row:
            if key not in keys:
                keys.append(key)
    with open(out, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=keys)
        writer.writeheader()
        writer.writerows(rows)
    print(f"\n{len(rows)} reception(s) -> {out}")

    # ---- verdict, per modulation, split by whether the verdict can mean anything ----------------
    both = [r for r in rows if "crc_ai" in r]
    print("\n" + "=" * 96)
    print("CLASSIC vs AI on the same IQ")
    print("=" * 96)
    mods = sorted({r["mod"] for r in both}, key=lambda m: (m != "QPSK", m))
    header = f"{'mod':>7} {'n':>5} {'new':>4} {'fwd':>4} {'crc OK c/a':>12} {'ai gain':>8} {'ai loss':>8}"
    header += f" {'dSINR med':>10} {'dSINR rms':>10} {'dSINR p90':>10} {'max|d|':>7} {'iters c->a':>12}"
    print(header)
    for mod in mods:
        group = [r for r in both if r["mod"] == mod]
        # A forwarded reception's numbers are the classical chain's, so it can enter the CRC columns
        # (its verdicts are identical by construction) but never the deviation columns: including it
        # would report "the arm agrees to 0.00 dB" for receptions the arm did not run.
        ran = [r for r in group if r.get("arm_ran")]
        classic_ok = sum(1 for r in group if r["crc_classic"] == "OK")
        ai_ok = sum(1 for r in group if r["crc_ai"] == "OK")
        gain = sum(1 for r in ran if r["crc_classic"] != "OK" and r["crc_ai"] == "OK")
        loss = sum(1 for r in ran if r["crc_classic"] == "OK" and r["crc_ai"] != "OK")
        ds = [r["dsinr"] for r in ran]
        med = sorted(ds)[len(ds) // 2] if ds else float("nan")
        mean, rms, p90, mx = stats(ds)
        it_c = sum(int(r["iters_classic"]) for r in ran) / max(1, len(ran))
        it_a = sum(int(r["iters_ai"]) for r in ran) / max(1, len(ran))
        print(f"{mod:>7} {len(group):>5} {sum(1 for r in group if r['new_data'] != '0'):>4}"
              f" {len(group) - len(ran):>4} {classic_ok:>5}/{ai_ok:<6} {gain:>8} {loss:>8}"
              f" {med:>+10.2f} {rms:>10.2f} {p90:>10.2f} {mx:>7.2f}"
              f" {it_c:>5.1f}->{it_a:<6.1f}")

    retx = [r for r in both if r["new_data"] == "0"]
    if retx:
        print(f"\nretransmissions (new_data=0, one-shot: the field combined with earlier soft bits):"
              f" n={len(retx)}"
              f" classic OK={sum(1 for r in retx if r['crc_classic'] == 'OK')}"
              f" ai OK={sum(1 for r in retx if r['crc_ai'] == 'OK')}"
              f" (their SINR is still comparable: median {median_delta(retx):+.2f} dB"
              f" over the {sum(1 for r in retx if r.get('arm_ran'))} it computed)")

    narrow = [r for r in both if r["nof_prb"] <= 12]
    if narrow:
        print(f"narrow (<=12 PRB, the attach allocations): n={len(narrow)}"
              f" classic OK={sum(1 for r in narrow if r['crc_classic'] == 'OK')}"
              f" ai OK={sum(1 for r in narrow if r['crc_ai'] == 'OK')}"
              f" dSINR median {median_delta(narrow):+.2f} dB"
              f" over the {sum(1 for r in narrow if r.get('arm_ran'))} it computed)")

    worst = sorted([r for r in both if r.get("arm_ran")], key=lambda r: -abs(r["dsinr"]))[:8]
    if worst:
        print("\nlargest SINR deviations (the arm against the classical estimator on one grid):")
        for r in worst:
            print(f"  {r['stem']} {r['mod']:>7} rv{r['rv']} {r['nof_prb']:>3} PRB cr={r['code_rate']:>4}"
                  f"  {r['sinr_classic']:>6.2f} -> {r['sinr_ai']:>6.2f} dB ({r['dsinr']:+.2f})")

    computed = [r for r in both if r.get("arm_ran")]
    skipped = [r for r in both if not r.get("arm_ran")]
    print(f"\nAI arm COMPUTED {len(computed)}/{len(both)} reception(s)")
    if skipped:
        reasons = {}
        for r in skipped:
            reasons[r.get("degraded", "unknown")] = reasons.get(r.get("degraded", "unknown"), 0) + 1
        print(f"  forwarded to the classical chain: {len(skipped)} "
              f"(their numbers ARE the classical chain's - a zero difference there is not agreement)")
        for reason, count in sorted(reasons.items(), key=lambda kv: -kv[1]):
            print(f"    {count:>5}x {reason}")
        widths = sorted({r["nof_prb"] for r in skipped})
        if widths:
            print(f"    their allocation widths: {widths[0]}..{widths[-1]} PRB")
    if computed:
        ds = [r["dsinr"] for r in computed]
        print(f"  published SINR against the classical estimator, computed receptions only:"
              f" median {sorted(ds)[len(ds) // 2]:+.2f} dB,"
              f" mean {sum(ds) / len(ds):+.2f}, RMS {math.sqrt(sum(d * d for d in ds) / len(ds)):.2f},"
              f" min {min(ds):+.2f}, max {max(ds):+.2f}")
        flips = [r for r in computed if r["crc_classic"] != r["crc_ai"]]
        print(f"  verdict flips on computed receptions: {len(flips)}"
              + ("".join(f"\n    {r['stem']} {r['mod']} {r['nof_prb']}PRB: {r['crc_classic']} -> {r['crc_ai']}"
                         for r in flips[:10]) if flips else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
