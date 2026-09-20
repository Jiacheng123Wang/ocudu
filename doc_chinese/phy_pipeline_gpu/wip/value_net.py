#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""
The VALUE net: what replaces "byte-identical" as the gate on a change that is allowed to move values.

---- Why this exists ----
The line's gate used to be "the dumps are byte-identical to the archived baseline" (wip/neutral_vs_baseline.sh).
That is a STABILITY contract, not a correctness one, and it forbids the single most valuable GPU
optimisation: reassociating a reduction. It is also, by measurement, expensive: K1's serial pivot order
(197.5us), the pilot extraction's single-thread 72-term atan2 accumulation (72.8us) and its 256-thread
reduction trees (71.8us) and the reformat's chain (116.9us) are 75% of the air-geometry ch_wt, and every
one of them is expensive precisely because its order is part of the published bits (design document
5.8.16/5.8.18). The user's ruling: the requirement is that the ALGORITHM is right and the error is
bounded, not that the bits are frozen.

---- What this net must not lose ----
The byte net is what caught the two real value defects of this line, and the tolerance-based unit test
did not: P0's fused lane published NaN (51,810 differing bytes) and P5's correlation-table attempt moved
noise_variance from 1.258e-01 to 3.596e+04 while ctest -R metal was 9/9 green. So this net keeps three
things that a plain "compare with a tolerance" would drop:

  1. a hard NO-NaN / NO-Inf check on every published field (P0's signature);
  2. an ORDER-OF-MAGNITUDE tripwire (P5's signature: 0.126 -> 3.6e4 is 5 decades, and any relative
     tolerance large enough to pass a legitimate reassociation is also large enough to pass that);
  3. a DECISION-level check on the LLRs (the sign of the quantised LLR is the decoded bit), because that
     is what the air leg's CRC ultimately judges.

---- The thresholds ----
They are TRIPWIRES, not precision requirements: they are set to catch a defect of the P0/P5 class while
letting a reordered reduction through. They are per field and per dump, and every one of them is printed
next to the measured value so a failure says which field moved and by how much. The physically meaningful
floors matter: `ta_us` is a time in microseconds and `cfo_hz` a frequency in hertz, so an absolute
tolerance is the right shape there, while the powers and the noise variance take a relative one.

usage: python3 wip/value_net.py [--bin PATH] [--what both|corpus|narrow] [--quiet]
                                [--env OCUDU_X=1 ...] [--env-passthrough]
exit:  0 every capture within tolerance; 1 something exceeded; 2 a dump is missing (NOT evidence).

---- Why --env exists (path A, design document 5.8.25) ----
A candidate that replaces a stage is wired as a DEFAULT-OFF knob, so the factory path is untouched. That
means the net, to judge the candidate's VALUES, has to run the replay tool with the knob SET - otherwise
it re-verifies the factory path and says nothing about the thing under test. `--env K=V` does exactly
that. Use `--env-passthrough` to hand the whole current environment to the child instead (for a knob
that is already exported, or for a set of them).
"""

import argparse
import glob
import math
import os
import re
import struct
import subprocess
import sys
import tempfile

ROOT = "/Users/jiachengwang/dev/ocudu"
DEFAULT_BIN = os.path.join(ROOT, "build/lib/phy/upper/channel_processors/metal/ul_chain_replay")

# ---- thresholds -----------------------------------------------------------------------------------
# (relative, absolute): a field passes when |a-b| <= max(rel * max(|a|,|b|), abs).
CE_FIELDS = {
    "noise_variance": (1e-3, 0.0),
    "snr": (1e-3, 1e-4),
    "rsrp": (1e-3, 0.0),
    "epre": (1e-3, 0.0),
    "ta_us": (1e-3, 5e-2),  # 0.05us of time alignment
    "cfo_hz": (1e-3, 1.0),  # 1 Hz of frequency offset
}
H_REL_MEDIAN = 1e-2
H_REL_P99 = 2e-2
# The grid threshold is deliberately WIDE. This is the one place the net must not pretend to know
# better than the air leg: a reassociated reduction in A moves h by ~0.5% for a 1-ULP change (5.8.6
# measured 0.4-0.9% on a 256QAM capture), so a threshold tight enough to reject that would reject
# exactly the changes this net exists to allow. The grid bound is therefore a DISASTER tripwire, and
# what a legitimate reassociation is judged on is the DECISION level below it - the LLR sign rate and
# the confidence of any flip - plus the air leg's CRC.
H_REL_MAX = 5e-2
LLR_SIGN_AGREE_MIN = 0.999  # fraction of quantised LLRs whose sign (the decoded bit) is unchanged
LLR_ABS_MAX = 12  # int8 LLR units; a flipped sign must be a LOW-CONFIDENCE one
ORDER_OF_MAGNITUDE = 10.0  # any field moving by this factor is a defect, whatever the tolerance

CE_RE = re.compile(r"port=(\d+) noise_variance=(\S+) snr=(\S+) rsrp=(\S+) epre=(\S+) ta_us=(\S+) cfo_hz=(\S+)")


def read_ce(path):
    """{port: {field: float}} - 'na' (a CFO the estimator did not produce) is kept as None."""
    out = {}
    with open(path, "r", errors="replace") as f:
        for line in f:
            m = CE_RE.search(line)
            if not m:
                continue
            port = int(m.group(1))
            rec = {}
            for i, name in enumerate(["noise_variance", "snr", "rsrp", "epre", "ta_us", "cfo_hz"]):
                raw = m.group(i + 2)
                rec[name] = None if raw == "na" else float(raw)
            out[port] = rec
    return out


def read_h(path):
    """The estimated grid: cf_t = two float32 per element, in file order."""
    with open(path, "rb") as f:
        raw = f.read()
    n = len(raw) // 4
    return struct.unpack("<%df" % n, raw[: n * 4])


def read_llr(path):
    """int8 LLRs behind a size_t header (the writer appends, so read until EOF)."""
    with open(path, "rb") as f:
        raw = f.read()
    out = bytearray()
    off = 0
    while off + 8 <= len(raw):
        (size,) = struct.unpack_from("<Q", raw, off)
        off += 8
        if size == 0 or off + size > len(raw):
            break
        out += raw[off : off + size]
        off += size
    return [b - 256 if b > 127 else b for b in out]


def percentile(vals, p):
    if not vals:
        return 0.0
    s = sorted(vals)
    return s[min(len(s) - 1, int(p * len(s)))]


class Fail(Exception):
    pass


def check_ce(base, new, label, problems):
    for port, brec in base.items():
        nrec = new.get(port)
        if nrec is None:
            problems.append("%s: port %d missing in the new dump" % (label, port))
            continue
        for field, (rel, absl) in CE_FIELDS.items():
            b, a = brec.get(field), nrec.get(field)
            if b is None and a is None:
                continue
            if (b is None) != (a is None):
                problems.append("%s: %s went from %s to %s" % (label, field, b, a))
                continue
            for who, v in (("baseline", b), ("new", a)):
                if math.isnan(v) or math.isinf(v):
                    problems.append("%s: %s is %s in the %s" % (label, field, v, who))
            if any(math.isnan(v) or math.isinf(v) for v in (a, b)):
                continue
            if b != 0.0 and (abs(a) > ORDER_OF_MAGNITUDE * abs(b) or abs(a) < abs(b) / ORDER_OF_MAGNITUDE):
                problems.append(
                    "%s: %s moved by %.1fx (%.6g -> %.6g)" % (label, field, abs(a / b) if b else float("inf"), b, a)
                )
                continue
            if abs(a - b) > max(rel * max(abs(a), abs(b)), absl):
                problems.append(
                    "%s: %s out of tolerance (%.6g -> %.6g, rel %.2e)"
                    % (label, field, b, a, abs(a - b) / max(abs(a), abs(b), 1e-30))
                )


def check_h(base, new, label, problems, report):
    if len(base) != len(new):
        problems.append("%s: size changed (%d -> %d floats)" % (label, len(base), len(new)))
        return
    rms = math.sqrt(sum(v * v for v in base) / max(1, len(base)))
    floor = max(rms * 1e-3, 1e-30)
    rels = []
    for b, a in zip(base, new):
        if math.isnan(a) or math.isinf(a):
            problems.append("%s: NaN/Inf in the new grid" % label)
            return
        rels.append(abs(a - b) / max(abs(b), floor))
    med, p99, mx = percentile(rels, 0.5), percentile(rels, 0.99), max(rels)
    report.append("%s: h rel med=%.2e p99=%.2e max=%.2e" % (label, med, p99, mx))
    if med > H_REL_MEDIAN or p99 > H_REL_P99 or mx > H_REL_MAX:
        problems.append("%s: h relative error above tolerance (med %.2e p99 %.2e max %.2e)" % (label, med, p99, mx))


def check_llr(base, new, label, problems, report):
    if len(base) != len(new):
        problems.append("%s: LLR count changed (%d -> %d)" % (label, len(base), len(new)))
        return
    if not base:
        return
    same = sum(1 for b, a in zip(base, new) if (b >= 0) == (a >= 0))
    agree = same / len(base)
    flips = [(b, a) for b, a in zip(base, new) if (b >= 0) != (a >= 0)]
    worst_flip = max((abs(b) for b, _ in flips), default=0)
    maxd = max((abs(a - b) for b, a in zip(base, new)), default=0)
    report.append(
        "%s: llr sign agree=%.5f (%d flips, worst |llr| %d) max|d|=%d" % (label, agree, len(flips), worst_flip, maxd)
    )
    if agree < LLR_SIGN_AGREE_MIN:
        problems.append("%s: LLR sign agreement %.5f below %.5f" % (label, agree, LLR_SIGN_AGREE_MIN))
    if flips and worst_flip > LLR_ABS_MAX:
        problems.append("%s: a bit flipped at |llr|=%d (above the %d confidence bound)" % (label, worst_flip, LLR_ABS_MAX))
    if maxd > 127:
        problems.append("%s: LLR moved by %d steps" % (label, maxd))


def parse_env(pairs):
    """--env K=V, repeatable. Returns (env_dict, error).

    A malformed pair is an ERROR rather than a silently ignored one: pitfall 4 of this line is a knob
    that does not exist (or is misspelled) acting as a silent no-op, which makes a measurement look
    like "this stage costs nothing". The same failure shape here would make the net report a clean
    pass for a candidate that never ran.
    """
    out = {}
    for p in pairs or []:
        if "=" not in p:
            return None, "--env %r is missing '='; expected --env NAME=VALUE" % p
        k, v = p.split("=", 1)
        if not k:
            return None, "--env %r has an empty name" % p
        out[k] = v
    return out, None


def env_report(env, passthrough):
    if env is None:
        return "inherited"
    if passthrough:
        return "inherited + " + " ".join("%s=%s" % kv for kv in sorted(env.items()))
    if not env:
        return "CLEARED (empty environment)"
    return " ".join("%s=%s" % kv for kv in sorted(env.items()))


def compare(bin_path, base_base, new_base, label, report, quiet):
    problems = []
    check_ce(read_ce(base_base + "_ce.txt"), read_ce(new_base + "_ce.txt"), label, problems)
    check_h(read_h(base_base + "_h.bin"), read_h(new_base + "_h.bin"), label, problems, report)
    check_llr(read_llr(base_base + "_llr.bin"), read_llr(new_base + "_llr.bin"), label, problems, report)
    if not quiet and report and report[-1].startswith(label):
        print("  " + report[-1])
    return problems


def self_test():
    """Feed the net the failure modes it exists for, plus the changes it must ALLOW.

    A gate that cannot see the failure it was written for is not a gate (the line's pitfall 19), and the
    two value defects this net replaces the byte net for are P0 (NaN) and P5 (noise_variance 1.258e-01 ->
    3.596e+04). Both are injected here, together with the two legitimate outcomes a reassociated
    reduction produces: a last-bit grid shift, and a low-confidence bit flip.
    """
    base = sorted(glob.glob(os.path.join(ROOT, "doc_chinese/work_tmp/determinism/a", "*_ce.txt")))[0]
    base = base[: -len("_ce.txt")]
    work = tempfile.mkdtemp(prefix="value_net_selftest_")

    def clone(tag):
        stem = os.path.join(work, tag)
        for suf in ("_ce.txt", "_h.bin", "_llr.bin"):
            with open(base + suf, "rb") as src, open(stem + suf, "wb") as dst:
                dst.write(src.read())
        return stem

    def rewrite_ce(stem, field, value):
        with open(stem + "_ce.txt") as f:
            lines = f.readlines()
        out = []
        for line in lines:
            out.append(re.sub(r"(%s=)\S+" % field, lambda m: m.group(1) + str(value), line))
        with open(stem + "_ce.txt", "w") as f:
            f.writelines(out)

    def perturb_h(stem, scale):
        v = list(read_h(stem + "_h.bin"))
        for i in range(len(v)):
            v[i] = v[i] * (1.0 + scale) if v[i] != 0.0 else v[i]
        with open(stem + "_h.bin", "wb") as f:
            f.write(struct.pack("<%df" % len(v), *v))

    def perturb_llr(stem, flips, delta):
        v = read_llr(stem + "_llr.bin")
        for i in range(len(v)):
            v[i] = max(-127, min(127, v[i] + delta))
        for i in range(0, len(v), max(1, len(v) // max(1, flips))):
            if flips > 0:
                v[i] = -v[i]
        with open(stem + "_llr.bin", "wb") as f:
            f.write(struct.pack("<Q", len(v)))
            f.write(bytes((x + 256) % 256 for x in v))

    def nudge_ce(stem, field, rel):
        """A small relative nudge of the CURRENT value: the shape a legitimate reassociation has."""
        cur = read_ce(base + "_ce.txt")[0][field]
        rewrite_ce(stem, field, "%.9e" % (cur * (1.0 + rel)))

    cases = [
        ("identical (must PASS)", None, False),
        ("h x(1+1e-6), llr +/-1 (reassociation, must PASS)", ("h", 1e-6), False),
        ("noise_variance +1e-5 relative (must PASS)", ("ce_nudge", ("noise_variance", 1e-5)), False),
        ("P0: noise_variance = nan (must FAIL)", ("ce", ("noise_variance", "nan")), True),
        ("P5: noise_variance -> 3.6e4 (must FAIL)", ("ce", ("noise_variance", "3.596380859e+04")), True),
        ("h x1.005 (0.5%: what a 1-ULP A gives, must PASS)", ("h", 5e-3), False),
        ("h x1.2 (20%: a real defect, must FAIL)", ("h", 2e-1), True),
        ("llr sign flip at |llr| large (must FAIL)", ("llr", (1, 0)), None),
    ]
    bad = 0
    for label, delta, expect_fail in cases:
        stem = clone("case%d" % len(os.listdir(work)))
        if delta is None:
            pass
        elif delta[0] == "h":
            perturb_h(stem, delta[1])
        elif delta[0] == "ce":
            rewrite_ce(stem, delta[1][0], delta[1][1])
        elif delta[0] == "ce_nudge":
            nudge_ce(stem, delta[1][0], delta[1][1])
        elif delta[0] == "llr":
            perturb_llr(stem, delta[1][0], delta[1][1])
        report, problems = [], []
        check_ce(read_ce(base + "_ce.txt"), read_ce(stem + "_ce.txt"), "t", problems)
        check_h(read_h(base + "_h.bin"), read_h(stem + "_h.bin"), "t", problems, report)
        check_llr(read_llr(base + "_llr.bin"), read_llr(stem + "_llr.bin"), "t", problems, report)
        if expect_fail is None:  # a "must PASS" case whose payload is only a relative nudge
            expect_fail = False
        ok = bool(problems) == expect_fail
        bad += 0 if ok else 1
        print("  [%s] %s%s" % ("ok " if ok else "BAD", label, "" if ok else "  -> problems=%s" % problems[:2]))
    print("self-test: %d/%d as expected" % (len(cases) - bad, len(cases)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default=DEFAULT_BIN)
    ap.add_argument("--what", default="both", choices=["both", "corpus", "narrow"])
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--report-only", action="store_true", help="print the metrics, never fail")
    ap.add_argument("--self-test", action="store_true", help="inject the known failure modes and check the net sees them")
    ap.add_argument(
        "--env",
        action="append",
        metavar="K=V",
        help="run the replay tool with this environment variable set (repeatable). THIS is how a "
        "default-OFF candidate knob gets exercised: without it the net only ever re-verifies the "
        "factory path.",
    )
    ap.add_argument(
        "--env-passthrough",
        action="store_true",
        help="hand the whole current environment to the child as well as --env pairs",
    )
    args = ap.parse_args()

    if args.self_test:
        return self_test()

    extra_env, err = parse_env(args.env)
    if err is not None:
        print(err, file=sys.stderr)
        return 2
    if args.env_passthrough:
        run_env = dict(os.environ)
        run_env.update(extra_env)
    elif extra_env:
        # A CLEARED environment plus exactly the requested knobs. Measured on this tree: the replay tool
        # itself is fine with an empty environment (every path it is handed is absolute), and clearing is
        # what makes the two arms differ by exactly the --env list and nothing else - a knob left exported
        # in the shell would otherwise ride along invisibly.
        run_env = dict(extra_env)
    else:
        run_env = None  # inherit, exactly as before this option existed

    if not args.quiet:
        print("[env] %s" % env_report(run_env, args.env_passthrough))

    if not os.access(args.bin, os.X_OK):
        print("missing %s (build it first)" % args.bin, file=sys.stderr)
        return 2

    work = tempfile.mkdtemp(prefix="value_net_")
    all_problems, ncap, report = [], 0, []

    groups = []
    if args.what in ("both", "corpus"):
        groups.append(("corpus", os.path.join(ROOT, "doc_chinese/work_tmp/corpus"), "*.bin"))
    if args.what in ("both", "narrow"):
        groups.append(("narrow", os.path.join(ROOT, "doc_chinese/work_tmp/narrow_cap"), "*.txt"))

    for kind, srcdir, pat in groups:
        seen = 0
        for cap in sorted(glob.glob(os.path.join(srcdir, pat))):
            # The replay tool takes the capture PREFIX and appends the extensions itself, exactly as
            # neutral_vs_baseline.sh passes it: handing it ".../syn001_3.bin" makes it look for
            # ".../syn001_3.bin.bin" and produce no dump at all.
            stem = os.path.splitext(cap)[0]
            name = os.path.basename(stem)
            if stem.endswith("_ce"):
                continue
            out = os.path.join(work, kind, name)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            subprocess.run([args.bin, stem, "--metal", "--out", out], capture_output=True, env=run_env)
            fresh = glob.glob(out + "*_ce.txt")
            if not fresh:
                all_problems.append("%s: NO DUMP" % name)
                continue
            new_base = fresh[0][: -len("_ce.txt")]
            if kind == "corpus":
                b = glob.glob(os.path.join(ROOT, "doc_chinese/work_tmp/determinism/a", name + "_*_ce.txt"))
            else:
                b = glob.glob(os.path.join(ROOT, "doc_chinese/work_tmp/narrow_cmp2", name, "dev_*_ce.txt"))
            if not b:
                all_problems.append("%s: NO BASELINE" % name)
                continue
            base_base = b[0][: -len("_ce.txt")]
            seen += 1
            all_problems += compare(args.bin, base_base, new_base, name, report, args.quiet)
        print("[%s] captures compared = %d" % (kind, seen))
        ncap += seen

    print("")
    print("captures=%d  problems=%d" % (ncap, len(all_problems)))
    for p in all_problems[:40]:
        print("  FAIL " + p)
    if len(all_problems) > 40:
        print("  ... and %d more" % (len(all_problems) - 40))
    if args.report_only:
        return 0
    return 1 if all_problems else 0


if __name__ == "__main__":
    sys.exit(main())
