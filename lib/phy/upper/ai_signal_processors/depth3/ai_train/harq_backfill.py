#!/usr/bin/env python3
"""Recover ground truth for failed first transmissions from a later retransmission of the same block.

WHY. An uplink transport block is chosen by the phone, so the receiver knows its bits only if it decodes it: a first
transmission that fails has no label, and the corpus is therefore made only of successes (section 19.626). But a HARQ
process carries the SAME block across its retransmissions, so when a later attempt decodes -- with the live chain's soft
combining, which the offline replay does not reproduce -- that decode supplies the bits for the earlier attempt. The
sample stays a single-reception sample; the retransmission is only where the label came from.

INPUT is what dump_tb.sh already writes: an index with unit, tb_bits, crc, sinr_db, iterations, slot, tbs_bytes, plus
the .bits of every unit that decoded. The PDU sidecars in the capture directory supply rnti, harq_id and rv.

SAFEGUARDS, because a wrong label is worse than a missing one:
  * chains are split on a slot gap (a HARQ id is reused once the block is acknowledged, and merging two different
    blocks would hand one block's bits to the other's grid);
  * a chain only backfills when the block size matches between the failed attempt and the decoded one;
  * the decoded member must come AFTER the failed one, never before.

OUTPUT: <out>/index_harq.csv with an extra column label_from in {self, harq, none}, and copies of the recovered .bits
under <out>/tb/.

Usage: harq_backfill.py --leg <capture dir> --index <dump index.csv> --out <dir> [--max-gap-slots 40]
"""
import argparse
import collections
import csv
import os
import shutil
import sys


def read_pdu(path):
    """The fields a HARQ chain needs from one PDU sidecar."""
    fields = {}
    try:
        with open(path, errors="ignore") as handle:
            for line in handle:
                key, sep, value = line.strip().partition("=")
                if sep and key in ("rnti", "harq_id", "rv", "slot"):
                    fields[key] = value
    except OSError:
        return None
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, help="capture directory holding the PDU sidecars")
    ap.add_argument("--index", required=True, help="index.csv written by dump_tb.sh")
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-gap-slots", type=int, default=40,
                    help="a gap this large starts a new chain: the HARQ id has been reused for another block")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(args.index)))
    if not rows:
        print("empty index", file=sys.stderr)
        return 2

    # Join each unit with its PDU fields, and keep only units whose sidecar names a HARQ process.
    units = {}
    for row in rows:
        name = row["unit"]
        pdu = read_pdu(os.path.join(args.leg, name + ".txt"))
        if pdu is None or "harq_id" not in pdu or "rnti" not in pdu or "slot" not in pdu:
            continue
        units[name] = {
            "row": row,
            "rnti": pdu["rnti"],
            "harq": pdu["harq_id"],
            "rv": pdu.get("rv", "?"),
            "slot": int(pdu["slot"]),
            "tbs": row.get("tbs_bytes", ""),
        }

    # Chains: same (rnti, harq), consecutive in slot within the gap, in time order.
    by_process = collections.defaultdict(list)
    for name, info in units.items():
        by_process[(info["rnti"], info["harq"])].append((info["slot"], name))
    chains = []
    for key, members in by_process.items():
        members.sort()
        current = [members[0]]
        for slot, name in members[1:]:
            if slot - current[-1][0] > args.max_gap_slots:
                chains.append(current)
                current = []
            current.append((slot, name))
        chains.append(current)

    recovered = []
    for chain in chains:
        if len(chain) < 2:
            continue
        # the first decoded member AFTER a failure is the donor; an earlier decode means there was nothing to recover
        donor = None
        for slot, name in chain:
            if units[name]["row"]["crc"] == "OK":
                donor = (slot, name)
                break
        if donor is None:
            continue
        for slot, name in chain:
            if slot >= donor[0]:
                break
            if units[name]["row"]["crc"] == "OK":
                continue
            failed, ok = units[name], units[donor[1]]
            if failed["tbs"] and ok["tbs"] and failed["tbs"] != ok["tbs"]:
                continue                     # different block sizes: not the same transport block, do not guess
            recovered.append((name, donor[1]))

    os.makedirs(os.path.join(args.out, "tb"), exist_ok=True)
    donor_of = dict(recovered)
    label_from = {}
    for name in units:
        label_from[name] = "self" if units[name]["row"]["crc"] == "OK" else ("harq" if name in donor_of else "none")
    for name, donor in recovered:
        src = os.path.join(os.path.dirname(args.index), "tb", donor + ".bits")
        if not os.path.exists(src):
            label_from[name] = "none"
            continue
        shutil.copy(src, os.path.join(args.out, "tb", name + ".bits"))

    with open(os.path.join(args.out, "index_harq.csv"), "w", newline="") as handle:
        cols = list(rows[0].keys()) + ["label_from", "donor"]
        writer = csv.DictWriter(handle, fieldnames=cols)
        writer.writeheader()
        for row in rows:
            name = row["unit"]
            out = dict(row)
            out["label_from"] = label_from.get(name, "none")
            out["donor"] = donor_of.get(name, "")
            # A recovered unit is usable by the builder, so it is written as OK -- the extra column records why.
            if out["label_from"] == "harq":
                out["crc"] = "OK"
                out["tb_bits"] = str(os.path.getsize(os.path.join(args.out, "tb", name + ".bits")) * 8)
            writer.writerow(out)

    n_self = sum(1 for v in label_from.values() if v == "self")
    n_harq = sum(1 for v in label_from.values() if v == "harq")
    print(f"units with a HARQ process: {len(units)}  chains: {len(chains)}")
    print(f"decoded on their own: {n_self}   recovered from a retransmission: {n_harq}")
    print(f"wrote {os.path.join(args.out, 'index_harq.csv')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
