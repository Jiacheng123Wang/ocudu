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

A SECOND, BETTER DONOR SOURCE. `--live-tb <dir>` reads the transport blocks the gNB dumped DURING the flight
(OCUDU_HELENA_DUMP_DIR): one `tb_<idx>_tbs<bits>.bits` per successful decode, plus `dd_meta.csv` whose last column is
the system slot. Those decodes used the live chain's HARQ soft combining, which an offline replay of the same
retransmission cannot reproduce -- so for a retransmission that only decodes after combining, this is the only place the
bits exist. When the flag is given it is preferred; the offline replay is the fallback.

Usage: harq_backfill.py --leg <capture dir> --index <dump index.csv> --out <dir> [--max-gap-slots 40] [--live-tb DIR]
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
                if sep and key in ("rnti", "harq_id", "rv", "slot", "scs_khz"):
                    fields[key] = value
    except OSError:
        return None
    return fields


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, help="capture directory holding the PDU sidecars")
    ap.add_argument("--index", required=True, help="index.csv written by dump_tb.sh")
    ap.add_argument("--out", required=True)
    ap.add_argument("--live-tb", default="",
                    help="directory holding the live dd_meta.csv and tb_*.bits (OCUDU_HELENA_DUMP_DIR during the leg)")
    ap.add_argument("--max-gap-slots", type=int, default=40,
                    help="a gap this large starts a new chain: the HARQ id has been reused for another block")
    args = ap.parse_args()

    live_by_slot = {}
    if args.live_tb:
        meta = os.path.join(args.live_tb, "dd_meta.csv")
        if os.path.exists(meta):
            for line in open(meta, errors="ignore"):
                parts = line.strip().split(",")
                # SIX COLUMNS SINCE 2026-10-27: idx,ts_us,tb_bits,nof_cb,frame,slot. The frame is the monotonic one,
                # because the slot alone repeats every 10.24 s and matching on it picked the wrong block (or none).
                if len(parts) < 6:
                    continue
                idx, bits, frame, slot = parts[0], parts[2], int(parts[4]), int(parts[5])
                path = os.path.join(args.live_tb, f"tb_{int(idx):08d}_tbs{bits}.bits")
                if os.path.exists(path):
                    live_by_slot[(frame, slot)] = path
        print(f"live dumps: {len(live_by_slot)} reception(s) from {meta}", file=sys.stderr)

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
        # THE KEY COMES FROM THE NAME, BOTH FIELDS. `cap_<frame>_<slot>_<rnti>` is how the capture identifies the
        # reception, and the live dump writes exactly that pair. The PDU's own `slot=` is the GRANT's slot, which trails
        # the capture slot by the processing delay (measured: 78 slots apart on the same reception), so using it here
        # would miss every match -- which is precisely what it did before this line was corrected.
        try:
            parts = name.split("_")
            key = (int(parts[1]), int(parts[2]))
        except (IndexError, ValueError):
            continue
        # A MONOTONIC COORDINATE FOR ORDERING AND GAPS. The slot alone restarts every 10.24 s, so a chain sorted by
        # slot breaks at every wrap and its retransmission lands before its first transmission -- which is why one leg
        # with eight and a half thousand retransmissions recovered sixty-eight labels. Frames are already monotonic
        # (extended_frame), so the coordinate is frame * slots_per_frame + slot.
        # slots per frame from the PDU (the dump index does not carry it): 30 kHz is the deployed shape, 20 slots.
        slots_per_frame = 20 if str(pdu.get("scs_khz", "30")) == "30" else 10
        units[name] = {
            "row": row,
            "key": key,
            "ord": key[0] * slots_per_frame + key[1],
            "rnti": pdu["rnti"],
            "harq": pdu["harq_id"],
            "rv": pdu.get("rv", "?"),
            "slot": int(pdu["slot"]),
            "tbs": row.get("tbs_bytes", ""),
        }

    # CHAINS START AT A FIRST TRANSMISSION, NOT AT AN RNTI AND A HARQ ID.
    #
    # The first version grouped by (rnti, harq) and split only on a large slot gap, which is wrong on real data: a HARQ
    # id is reused for a NEW block as soon as the previous one is acknowledged, so on one leg with one RNTI the whole
    # capture collapsed into three enormous chains and nothing could be recovered -- every failure had a "donor" far
    # earlier in the same chain, which is not a retransmission of it.
    #
    # What actually identifies a chain is the redundancy-version sequence: rv=0 opens a block, and any following
    # transmissions with rv != 0 at a short interval belong to it. A retransmission whose first transmission was not
    # captured opens a chain of its own and can supply no label.
    by_process = collections.defaultdict(list)
    for name, info in units.items():
        by_process[(info["rnti"], info["harq"])].append((info["ord"], name))
    chains = []
    for key, members in by_process.items():
        members.sort()
        current = []
        for slot, name in members:
            rv = units[name]["rv"]
            starts = (rv == "0")
            too_far = bool(current) and (slot - current[-1][0] > args.max_gap_slots)
            if starts or too_far or not current:
                if current:
                    chains.append(current)
                current = [(slot, name)]
            else:
                current.append((slot, name))
        if current:
            chains.append(current)

    recovered = []
    for chain in chains:
        if len(chain) < 2:
            continue
        # the first decoded member AFTER a failure is the donor; an earlier decode means there was nothing to recover
        donor = None
        for slot, name in chain:
            decoded_offline = units[name]["row"]["crc"] == "OK"
            decoded_live = units[name]["key"] in live_by_slot
            if decoded_offline or decoded_live:
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
            src = os.path.join(os.path.dirname(args.index), "tb", donor[1] + ".bits")
            if units[donor[1]]["key"] in live_by_slot and (not failed["tbs"] or
                                             int(units[donor[1]]["tbs"] or 0) == 0 or
                                             units[donor[1]]["row"]["crc"] != "OK"):
                src = live_by_slot[units[donor[1]]["key"]]  # the live decode is the only place these bits exist
            recovered.append((name, donor[1], src))

    os.makedirs(os.path.join(args.out, "tb"), exist_ok=True)
    donor_of = {name: donor for name, donor, _ in recovered}
    label_from = {}
    for name in units:
        label_from[name] = "self" if units[name]["row"]["crc"] == "OK" else ("harq" if name in donor_of else "none")
    for name, donor, src in recovered:
        if not os.path.exists(src):
            label_from[name] = "none"
            continue
        shutil.copy(src, os.path.join(args.out, "tb", name + ".bits"))

    # TWO INDEXES, NOT ONE. A sample whose label came from its own decode and a sample whose label came from a
    # retransmission are different populations: the second is a reception the reference chain failed on, so it can train
    # but must never enter an acceptance set that compares against that chain. Keeping them in separate files means a
    # build (and a later merge) decides which population it wants, instead of having to re-derive it from a column.
    self_rows = [r for r in rows if label_from.get(r["unit"]) == "self"]
    harq_rows = [r for r in rows if label_from.get(r["unit"]) == "harq"]
    # BOTH FILES CARRY THE AUDIT COLUMNS. Without them a recovered row cannot be checked against the transmission it
    # came from -- the first version wrote only the plain columns and made the population unverifiable after the fact.
    cols_plain = list(rows[0].keys()) + ["label_from", "donor"]
    with open(os.path.join(args.out, "index_self.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_plain, extrasaction="ignore")
        writer.writeheader()
        for row in self_rows:
            out = dict(row)
            out["label_from"] = "self"
            out["donor"] = ""
            writer.writerow(out)
    with open(os.path.join(args.out, "index_harq_only.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_plain)
        writer.writeheader()
        for row in harq_rows:
            out = dict(row)
            out["crc"] = "OK"       # usable by the builder; the file name says where the label came from
            out["tb_bits"] = str(os.path.getsize(os.path.join(args.out, "tb", row["unit"] + ".bits")) * 8)
            out["label_from"] = "harq"
            out["donor"] = donor_of.get(row["unit"], "")
            writer.writerow(out)

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
    print(f"wrote index_self.csv ({len(self_rows)}) and index_harq_only.csv ({len(harq_rows)}) in {args.out}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
