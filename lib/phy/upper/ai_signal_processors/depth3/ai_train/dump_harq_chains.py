#!/usr/bin/env python3
"""Recover the LDPC iteration counts of the decode that supplied a HARQ sample's label.

WHY. A first transmission that the classical chain fails has no label of its own; harq_backfill.py borrows the bits from
a later retransmission of the same block, and the dump's index then reports the FAILED attempt's iteration counts, which
sit at the decoder's cap and say nothing about how hard the block was. The number that matters is the one from the decode
that passed the CRC, and it exists in two places only: the live chain's `dd_meta.csv` (recorded since 2026-11-27) and a
chain replay, where one process walks the whole HARQ chain and shares the soft buffer. A per-unit replay never combines,
which is why the standard dump runs this second pass after the per-unit one.

WHAT IT DOES. Groups the captures into HARQ chains with the same rule harq_backfill.py labels them with, replays each
chain whose FIRST transmission failed in one process (`--also ... --harq-chain`), reads the per-reception CSV the replay
writes, and records the first hop that decoded.

INPUT
  --leg DIR      capture directory (the PDU sidecars live here)
  --index CSV    the dump's index.csv (per-unit verdicts; the chain's first transmission must be crc!=OK to qualify)
  --live-tb DIR  optional: `<leg>/tb_live`, the gNB's in-flight dumps. Given, the decoded hop's bits are compared byte for
                 byte against the live dump of the same reception, which is what proves the replay reconstructed the same
                 block rather than a different one.

THE RECOVERED BITS LAND IN THE DUMP'S tb/ (--tb-out). A recovered sample's label is the transport block the chain
decode produced, and the builder reads exactly `<tb>/<unit>.bits`; writing it here makes the dump hold every trainable
sample, self-decoded and recovered alike, without depending on the flight's live dump. When --live-tb is given the same
bytes are compared against it, which is what proves the reconstruction.

OUTPUT (--out CSV), one row per candidate chain
  unit,chain_len,hop,decoded_unit,decoded_frame,decoded_slot,iterations,iterations_max,sinr_db,nof_prb,bits_check,
  tb_written
  `unit` is the chain's first transmission (the sample the label belongs to), `hop` the 1-based position of the member
  that decoded, `iterations*` its counts (the combined decode's), `bits_check` one of match / mismatch / no-live /
  no-live-row.

  The file feeds harq_backfill.py --chain-iters, which prefers the live record when a leg has one and otherwise uses
  these counts in place of the failed attempt's.

SAFEGUARDS
  * a chain qualifies only when its first member is a first transmission (`new_data=1`) and the index has it as crc!=OK;
  * a hop with an empty TB file or a decode-timeout is reported, never guessed;
  * the output lists the decode that happened, not the one expected: a chain that still fails writes no row.

Usage: dump_harq_chains.py --leg <capture dir> --index <index.csv> --out <csv> [--tb-out DIR] [--jobs N] [--limit N]
                            [--live-tb DIR]
"""
import argparse
import concurrent.futures
import csv
import os
import shutil
import subprocess
import sys
import tempfile

import harq_backfill


def live_rows(tb_live):
    """Map (frame, slot) to the live dump of that reception: its bits path and its iteration counts, when recorded."""
    rows = {}
    meta = os.path.join(tb_live, "dd_meta.csv")
    if not os.path.exists(meta):
        return rows
    for line in open(meta, errors="ignore"):
        parts = line.strip().split(",")
        if len(parts) < 6:
            continue
        try:
            idx, bits, frame, slot = int(parts[0]), parts[2], int(parts[4]), int(parts[5])
        except ValueError:
            continue
        iters = None
        if len(parts) >= 8:
            try:
                mean, mx = int(parts[6]), int(parts[7])
            except ValueError:
                mean, mx = 0, 0
            if mx > 0:
                iters = (mean, mx)
        rows[(frame, slot)] = (os.path.join(tb_live, f"tb_{idx:08d}_tbs{bits}.bits"), iters)
    return rows


def replay_chain(unit_names, leg, binary, workdir, timeout):
    """Replay one HARQ chain in a single process and return the per-reception rows the replay wrote."""
    stage = os.path.join(workdir, "stage")
    bits = os.path.join(workdir, "bits")
    csv_path = os.path.join(workdir, "replay.csv")
    os.makedirs(bits, exist_ok=True)
    cmd = [binary, os.path.join(leg, unit_names[0])]
    for name in unit_names[1:]:
        cmd += ["--also", os.path.join(leg, name)]
    cmd += ["--phy_pipeline", "cpu", "--pusch_receiver_backend", "classic", "--harq-chain", "--out", stage]
    env = dict(os.environ)
    env["OCUDU_REPLAY_CSV"] = csv_path
    env["OCUDU_HELENA_DUMP_DIR"] = bits
    env["OMP_NUM_THREADS"] = "1"
    try:
        proc = subprocess.run(cmd, env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    rows = []
    if os.path.exists(csv_path):
        with open(csv_path, newline="") as handle:
            rows = list(csv.DictReader(handle))
    if proc.returncode != 0 and not rows:
        return None, "replay-failed"
    return (rows, bits), None


def bits_of_hop(bits_dir, slot):
    """Find the TB file the replay wrote for the hop that decoded, by its system slot.

    THE FRAME COLUMN OF A REPLAY'S dd_meta IS PROCESS-RELATIVE and cannot be matched across runs: extended_frame() counts
    wraps from the start of the process (ul_capture_frame.h), so a replay that starts mid-leg reports the raw SFN while
    the flight's dump reports the count that included every wrap since take-off. The capture's own frame, taken from its
    name, is the number the live dump agrees with. Within one chain replay the slots are distinct, so the slot alone
    identifies the hop here.
    """
    meta = os.path.join(bits_dir, "dd_meta.csv")
    if not os.path.exists(meta):
        return None
    for line in open(meta, errors="ignore"):
        parts = line.strip().split(",")
        if len(parts) < 6:
            continue
        try:
            idx, tb_bits, row_slot = int(parts[0]), int(parts[2]), int(parts[5])
        except ValueError:
            continue
        if row_slot != slot:
            continue
        return os.path.join(bits_dir, f"tb_{idx:08d}_tbs{tb_bits}.bits")
    return None


def compare_bits(mine, live_path):
    if not mine or not os.path.exists(mine):
        return "no-bits"
    if not live_path or not os.path.exists(live_path):
        return "no-live-row"
    try:
        if os.path.getsize(mine) != os.path.getsize(live_path):
            return "mismatch"
        with open(mine, "rb") as a, open(live_path, "rb") as b:
            return "match" if a.read() == b.read() else "mismatch"
    except OSError:
        return "no-bits"


def one_chain(task, leg, binary, live, timeout, tb_out=""):
    """Replay one chain and return its output row, or None when the chain produced no decode."""
    first, members = task
    name = first
    workdir = tempfile.mkdtemp(prefix="dhc_")
    try:
        result, error = replay_chain(members, leg, binary, workdir, timeout)
        if error is not None:
            return dict(unit=name, chain_len=len(members), hop="", decoded_unit="", decoded_frame="",
                        decoded_slot="", iterations="", iterations_max="", sinr_db="", nof_prb="",
                        bits_check=error, tb_written="no")
        rows, bits_dir = result
        for hop, row in enumerate(rows, start=1):
            if row.get("crc") != "OK":
                continue
            try:
                frame, slot = (int(x) for x in row["capture"].split("_")[1:3])
            except (KeyError, ValueError, IndexError):
                frame, slot = "", row.get("slot", "")
            live_bits, live_iters = live.get((frame, slot), (None, None)) if live else (None, None)
            mine = bits_of_hop(bits_dir, slot) if isinstance(slot, int) else None
            # TWO DIFFERENT REASONS FOR "NO COMPARISON": none was asked for, and the live dump has no such reception.
            check = compare_bits(mine, live_bits) if live else "no-live"
            written = "no"
            if tb_out and mine and os.path.exists(mine) and check != "mismatch":
                os.makedirs(tb_out, exist_ok=True)
                shutil.copyfile(mine, os.path.join(tb_out, name + ".bits"))
                written = "yes"
            return dict(unit=name, chain_len=len(members), hop=hop, decoded_unit=row.get("capture", ""),
                        decoded_frame=frame, decoded_slot=row.get("slot", ""),
                        iterations=row.get("iterations", ""), iterations_max=row.get("iterations_max", ""),
                        sinr_db=row.get("sinr_db", ""), nof_prb=row.get("nof_prb", ""),
                        bits_check=check, tb_written=written)
        return dict(unit=name, chain_len=len(members), hop="", decoded_unit="", decoded_frame="",
                    decoded_slot="", iterations="", iterations_max="", sinr_db="", nof_prb="",
                    bits_check="no-decode", tb_written="no")
    finally:
        shutil.rmtree(workdir, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, help="capture directory holding the PDU sidecars")
    ap.add_argument("--index", required=True, help="index.csv written by dump_tb.sh")
    ap.add_argument("--out", required=True, help="output CSV")
    ap.add_argument("--binary", default="", help="replay binary; defaults to the CPU build in the repository")
    ap.add_argument("--live-tb", default="", help="<leg>/tb_live, to verify the replayed bits against the live dump")
    ap.add_argument("--tb-out", default="", help="write each recovered block to <dir>/<unit>.bits (the dump's tb/)")
    ap.add_argument("--jobs", type=int, default=12, help="chains replayed at once, one process each")
    ap.add_argument("--limit", type=int, default=0, help="only the first N candidate chains (smoke test)")
    ap.add_argument("--timeout", type=int, default=300, help="seconds per chain before it is reported as timeout")
    args = ap.parse_args()

    binary = args.binary or os.path.join(
        os.path.dirname(os.path.abspath(__file__)),
        "..", "..", "..", "..", "..", "..", "build", "lib", "phy", "upper", "channel_processors",
        "ul_chain_replay_cpu")
    binary = os.path.normpath(binary)
    if not os.access(binary, os.X_OK):
        print(f"no replay binary at {binary}", file=sys.stderr)
        return 2

    rows = list(csv.DictReader(open(args.index)))
    if not rows:
        print("empty index", file=sys.stderr)
        return 2
    index = {r["unit"]: r for r in rows}
    units = harq_backfill.load_units(rows, args.leg)
    chains = harq_backfill.build_chains(units)

    # A CHAIN QUALIFIES ONLY WHEN ITS FIRST TRANSMISSION FAILED. A chain that decoded on its own needs no second decode:
    # its index row already carries the counts of the decode that produced its label.
    tasks = []
    for chain in chains:
        if len(chain) < 2:
            continue
        first = chain[0][1]
        if units[first]["new_data"] != "1":
            continue
        if index[first]["crc"] == "OK":
            continue
        members = [name for _, name in chain]
        if not all(os.path.exists(os.path.join(args.leg, m + ".bin")) and
                   os.path.exists(os.path.join(args.leg, m + ".txt")) for m in members):
            continue
        tasks.append((first, members))
    tasks.sort()
    if args.limit:
        tasks = tasks[:args.limit]
    print(f"candidate chains: {len(tasks)} (of {len(chains)} chains)", file=sys.stderr)

    live = live_rows(args.live_tb) if args.live_tb else {}
    if args.live_tb:
        print(f"live dump: {len(live)} reception(s) from {args.live_tb}", file=sys.stderr)

    out_rows = []
    done = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        futures = [pool.submit(one_chain, t, args.leg, binary, live, args.timeout, args.tb_out) for t in tasks]
        for fut in concurrent.futures.as_completed(futures):
            row = fut.result()
            if row is not None:
                out_rows.append(row)
            done += 1
            if done % 500 == 0:
                print(f"  {done}/{len(tasks)} chains", file=sys.stderr)
    out_rows.sort(key=lambda r: r["unit"])
    with open(args.out, "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=["unit", "chain_len", "hop", "decoded_unit", "decoded_frame",
                                                    "decoded_slot", "iterations", "iterations_max", "sinr_db",
                                                    "nof_prb", "bits_check", "tb_written"])
        writer.writeheader()
        writer.writerows(out_rows)

    decoded = [r for r in out_rows if r["hop"] != ""]
    status = {}
    for row in out_rows:
        status[row["bits_check"]] = status.get(row["bits_check"], 0) + 1
    written = sum(1 for r in out_rows if r.get("tb_written") == "yes")
    print(f"wrote {len(out_rows)} row(s) to {args.out}: decoded={len(decoded)} "
          f"({len(decoded)/len(out_rows)*100:.1f}%)  " + " ".join(f"{k}={v}" for k, v in sorted(status.items()))
          + (f"  tb_written={written}" if args.tb_out else ""), file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
