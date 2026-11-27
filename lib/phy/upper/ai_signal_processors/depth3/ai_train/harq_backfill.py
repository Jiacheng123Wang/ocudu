#!/usr/bin/env python3
"""Recover ground truth for failed first transmissions from a later retransmission of the same block.

WHY. An uplink transport block is chosen by the phone, so the receiver knows its bits only if it decodes it: a first
transmission that fails has no label, and the corpus is therefore made only of successes (section 19.626). But a HARQ
process carries the SAME block across its retransmissions, so when a later attempt decodes -- with the live chain's soft
combining, which the offline replay does not reproduce -- that decode supplies the bits for the earlier attempt. The
sample stays a single-reception sample; the retransmission is only where the label came from.

INPUT is what dump_tb.sh already writes: an index with unit, tb_bits, crc, sinr_db, iterations, iterations_max, slot,
tbs_bytes, plus the .bits of every unit that decoded. The PDU sidecars in the capture directory supply rnti, harq_id and
rv.

SAFEGUARDS, because a wrong label is worse than a missing one:
  * chains are split on a slot gap (a HARQ id is reused once the block is acknowledged, and merging two different
    blocks would hand one block's bits to the other's grid);
  * a chain only backfills when the block size matches between the failed attempt and the decoded one;
  * the decoded member must come AFTER the failed one, never before.

OUTPUT: <out>/index_harq.csv with an extra column label_from in {self, harq, none}, and copies of the recovered .bits
under <out>/tb/.

THE THIRD FILE IS THE ONE WITH NO LABEL. index_ko.csv holds every unit whose label_from is none, i.e. the receptions the
classical chain did not decode and no retransmission recovered. It is the complement of index_self.csv and
index_harq_only.csv, so the three partition the dump. Its use is the other direction: with the capture directory beside it
it names exactly what another decode back end may still crack, and every row that back end turns into a CRC pass becomes
a sample. The rows carry the PDU fields that decide how to retry (new_data, rv, harq_id, rnti) and a reason column:
"no-buffer" when the per-unit pass never reached the LDPC decoder (a retransmission without its soft buffer), and
"decode-failed" when it ran and the CRC still failed.

THE RECOVERED ROW CARRIES THE DECODE THAT LABELLED IT. For label_from=harq, `iterations` and `iterations_max` describe
the donor's decode, i.e. the combined decode when the donor is a live dump; `own_iterations` and `own_iterations_max`
keep the failed first attempt's counts, which saturate at the decoder's iteration cap and therefore say nothing about
how hard the block was to recover. When the donor's counts are unknown both fields are empty, never the failed count.

A SECOND, BETTER DONOR SOURCE. `--live-tb <dir>` reads the transport blocks the gNB dumped DURING the flight
(OCUDU_HELENA_DUMP_DIR): one `tb_<idx>_tbs<bits>.bits` per successful decode, plus `dd_meta.csv` with columns
idx,ts_us,tb_bits,nof_cb,frame,slot[,iter_mean,iter_max]. The frame and the slot are the monotonic pair, and the last
two columns, when present, are the LDPC iteration counts of the decode that passed the CRC. Those decodes used the live
chain's HARQ soft combining, which an offline replay of the same retransmission cannot reproduce -- so for a
retransmission that only decodes after combining, this is the only place the bits, and the iteration counts, exist.
When the flag is given it is preferred; the offline replay is the fallback.

Usage: harq_backfill.py --leg <capture dir> --index <dump index.csv> --out <dir> [--max-gap-slots 40] [--live-tb DIR]
"""
import argparse
import collections
import concurrent.futures
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
                if sep and key in ("rnti", "harq_id", "rv", "new_data", "slot", "scs_khz"):
                    fields[key] = value
    except OSError:
        return None
    return fields


def load_units(rows, leg):
    """Join every index row with the PDU fields a HARQ chain needs.

    Returns the units that name an rnti, a HARQ id and a slot, keyed by capture name. Shared with dump_harq_chains.py so
    that both tools order receptions by the same coordinate.
    """
    # THE SIDECARS ARE SMALL AND NUMEROUS, AND THE OPEN IS THE COST. Ten thousand of them took twenty minutes on the
    # replay host while a dump shared the disk, almost all of it per-file latency; sixteen threads read them in seconds.
    def sidecar(row):
        return read_pdu(os.path.join(leg, row["unit"] + ".txt"))

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        pdus = list(pool.map(sidecar, rows))

    units = {}
    for row, pdu in zip(rows, pdus):
        name = row["unit"]
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
        # A MONOTONIC COORDINATE FOR ORDERING. Frames are monotonic (extended_frame); the slot is NOT slot-in-frame
        # but a system-slot counter that wraps at 20480 (10.24 s at 30 kHz). Adding it unmodulated to the frame gives
        # a 20-per-frame term plus a 20480-period sawtooth, so two receptions 5.12 s apart can land eight ord units
        # apart and the order is aliased -- measured on one leg: 1206 inversions against the live decode record, and
        # seventeen recoveries lost to a chain being cut before its donor arrived.
        # slots per frame from the PDU (the dump index does not carry it): 30 kHz is the deployed shape, 20 slots.
        slots_per_frame = 20 if str(pdu.get("scs_khz", "30")) == "30" else 10
        units[name] = {
            "row": row,
            "key": key,
            "ord": key[0] * slots_per_frame + (key[1] % slots_per_frame),
            "rnti": pdu["rnti"],
            "harq": pdu["harq_id"],
            "rv": pdu.get("rv", "?"),
            "new_data": pdu.get("new_data", "?"),
            "slot": int(pdu["slot"]),
            "tbs": row.get("tbs_bytes", ""),
        }
    return units


def build_chains(units):
    """Group the units into HARQ chains, each opened by a first transmission.

    A chain is cut where the scheduler starts a new block (`new_data=1`), not on a slot gap: a HARQ id is reused as soon
    as the previous block is acknowledged, so a gap guard either merges two blocks or cuts a real retransmission (see the
    note in `main`). Shared with dump_harq_chains.py: the chains that supply labels and the chains that are replayed must
    be the same ones.
    """
    by_process = collections.defaultdict(list)
    for name, info in units.items():
        by_process[(info["rnti"], info["harq"])].append((info["ord"], name))
    chains = []
    for key, members in by_process.items():
        members.sort()
        current = []
        for slot, name in members:
            starts = (units[name]["new_data"] == "1")
            too_far = False
            if starts or too_far or not current:
                if current:
                    chains.append(current)
                current = [(slot, name)]
            else:
                current.append((slot, name))
        if current:
            chains.append(current)
    return chains


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--leg", required=True, help="capture directory holding the PDU sidecars")
    ap.add_argument("--index", required=True, help="index.csv written by dump_tb.sh")
    ap.add_argument("--out", required=True)
    ap.add_argument("--live-tb", default="",
                    help="directory holding the live dd_meta.csv and tb_*.bits (OCUDU_HELENA_DUMP_DIR during the leg)")
    ap.add_argument("--max-gap-slots", type=int, default=40,
                    help="a gap this large starts a new chain: the HARQ id has been reused for another block")
    ap.add_argument("--chain-iters", default="",
                    help="CSV from dump_harq_chains.py: the combined decode's iteration counts, for legs whose live "
                         "dump predates the iteration columns")
    args = ap.parse_args()

    live_by_slot = {}
    live_iters = {}
    if args.live_tb:
        meta = os.path.join(args.live_tb, "dd_meta.csv")
        if os.path.exists(meta):
            for line in open(meta, errors="ignore"):
                parts = line.strip().split(",")
                # SIX COLUMNS: idx,ts_us,tb_bits,nof_cb,frame,slot. The frame is the monotonically increasing one,
                # because the slot alone repeats every 10.24 s and matching on it picks the wrong block (or none).
                # TWO MORE WHEN THE LEG WAS CAPTURED WITH THE ITERATION HOOK: iter_mean,iter_max, the LDPC counts of
                # the decode that passed the CRC. Zeros there mean the decode produced no observation (it was skipped
                # because the codeblock CRC was already OK), so they are treated as unknown rather than as one.
                if len(parts) < 6:
                    continue
                idx, bits, frame, slot = parts[0], parts[2], int(parts[4]), int(parts[5])
                path = os.path.join(args.live_tb, f"tb_{int(idx):08d}_tbs{bits}.bits")
                if os.path.exists(path):
                    live_by_slot[(frame, slot)] = path
                    if len(parts) >= 8:
                        try:
                            it_mean, it_max = int(parts[6]), int(parts[7])
                        except ValueError:
                            it_mean, it_max = 0, 0
                        if it_max > 0:
                            live_iters[(frame, slot)] = (it_mean, it_max)
        print(f"live dumps: {len(live_by_slot)} reception(s) from {meta}"
              f" (iteration counts on {len(live_iters)})", file=sys.stderr)

    # THE CHAIN REPLAY'S COUNTS, when the leg was captured before the live dump recorded them. They describe the same
    # decode: one process walking the whole chain with the soft buffer shared, CRC-checked, which is the combined decode
    # the live chain performed. The live record still wins when it exists, because it is the one that decided the label.
    # A STANDARD DUMP LEAVES ITS CHAIN REPLAY IN THE DUMP DIRECTORY. Picking it up here keeps the combined counts on
    # every re-run of the labelling, including the one build_mother_set.sh makes: without it, a rebuild would quietly
    # replace the recovered samples' difficulty with the failed attempt's cap.
    chain_iters_path = args.chain_iters
    if not chain_iters_path:
        candidate = os.path.join(args.out, "chain_iters.csv")
        if os.path.exists(candidate):
            chain_iters_path = candidate
    chain_iters = {}
    chain_units = {}
    if chain_iters_path:
        for row in csv.DictReader(open(chain_iters_path)):
            if not row.get("hop") or not row.get("iterations_max", "").isdigit():
                continue
            if int(row["iterations_max"]) <= 0:
                continue
            chain_iters[row["unit"]] = (int(row["iterations"]) if row["iterations"].isdigit() else 0,
                                        int(row["iterations_max"]))
            chain_units[row["unit"]] = row.get("decoded_unit", "")
        print(f"chain replays: {len(chain_iters)} chain(s) with iteration counts from {chain_iters_path}",
              file=sys.stderr)

    rows = list(csv.DictReader(open(args.index)))
    if not rows:
        print("empty index", file=sys.stderr)
        return 2

    units = load_units(rows, args.leg)
    chains = build_chains(units)

    recovered = []
    for chain in chains:
        if len(chain) < 2:
            continue
        chain_first = chain[0][1]
        # THREE PLACES A DECODE CAN COME FROM, in decreasing authority: the live chain's dump, the chain replay the
        # standard dump ran (its bits are already in tb/ under the chain's first transmission), and a member that
        # happened to decode on its own in the offline per-unit pass. The first two are the same combined decode; the
        # third needs no combining at all.
        donor = None
        for slot, name in chain:
            decoded_offline = units[name]["row"]["crc"] == "OK"
            decoded_live = units[name]["key"] in live_by_slot
            if decoded_offline or decoded_live:
                donor = (slot, name)
                break
        chain_dump = chain_iters.get(chain_first)
        if donor is None and chain_dump is not None:
            for slot, name in chain:
                if name == chain_units.get(chain_first, ""):
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
            target = os.path.join(args.out, "tb", name + ".bits")
            src = os.path.join(os.path.dirname(args.index), "tb", donor[1] + ".bits")
            from_live = units[donor[1]]["key"] in live_by_slot and (not failed["tbs"] or
                                             int(units[donor[1]]["tbs"] or 0) == 0 or
                                             units[donor[1]]["row"]["crc"] != "OK")
            if from_live:
                src = live_by_slot[units[donor[1]]["key"]]  # the live decode is the only place these bits exist
            elif chain_dump is not None and os.path.exists(os.path.join(args.out, "tb", chain_first + ".bits")):
                # the chain pass wrote one block per chain, named after the transmission the label belongs to
                src = os.path.join(args.out, "tb", chain_first + ".bits")
            elif os.path.exists(target):
                src = target
            # THE ITERATION COUNTS THAT BELONG TO THE LABEL. A recovered sample's difficulty is how hard the decode
            # that supplied its bits was, which is the combined decode, not this reception's failed attempt (always the
            # cap). Live record first, then the chain replay, then a donor that decoded on its own.
            combined = None
            if from_live and units[donor[1]]["key"] in live_iters:
                combined = live_iters[units[donor[1]]["key"]]
            elif chain_dump is not None:
                combined = chain_dump
            else:
                donor_row = units[donor[1]]["row"]
                if donor_row["crc"] == "OK" and donor_row.get("iterations", "").isdigit() \
                        and donor_row.get("iterations_max", "").isdigit():
                    combined = (int(donor_row["iterations"]), int(donor_row["iterations_max"]))
            recovered.append((name, donor[1], src, combined))

    os.makedirs(os.path.join(args.out, "tb"), exist_ok=True)
    donor_of = {name: donor for name, donor, _, _ in recovered}
    iters_of = {name: iters for name, _, _, iters in recovered}
    label_from = {}
    for name in units:
        label_from[name] = "self" if units[name]["row"]["crc"] == "OK" else ("harq" if name in donor_of else "none")
    for name, donor, src, _ in recovered:
        if not os.path.exists(src):
            label_from[name] = "none"
            continue
        target = os.path.join(args.out, "tb", name + ".bits")
        if os.path.abspath(src) != os.path.abspath(target):
            shutil.copy(src, target)

    # TWO INDEXES, NOT ONE. A sample whose label came from its own decode and a sample whose label came from a
    # retransmission are different populations: the second is a reception the reference chain failed on, so it can train
    # but must never enter an acceptance set that compares against that chain. Keeping them in separate files means a
    # build (and a later merge) decides which population it wants, instead of having to re-derive it from a column.
    self_rows = [r for r in rows if label_from.get(r["unit"]) == "self"]
    # AND ONLY THE FIRST TRANSMISSIONS. A chain that recovered a block has more than one failed member: the
    # retransmissions that did not decode before the donor did are labelled harq by the loop above, because they were
    # failed attempts in a chain that succeeded. They are not the population this file is for. The recovered arm means
    # "the reception whose FIRST transmission failed", and a middle member's input plane is a different reception --
    # a later retransmission, with its own allocation and its own noise. Measured on one leg: 251 chain members, of
    # which 196 are first transmissions and 55 are middle members. Feeding all 251 to the builder put the wrong kind
    # in a dataset, which is a label defect, not a size one.
    harq_rows = [r for r in rows
                 if label_from.get(r["unit"]) == "harq"
                 and units[r["unit"]]["new_data"] == "1"
                 and units[r["unit"]]["rv"] == "0"]
    # BOTH FILES CARRY THE AUDIT COLUMNS. Without them a recovered row cannot be checked against the transmission it
    # came from -- the first version wrote only the plain columns and made the population unverifiable after the fact.
    cols_plain = list(rows[0].keys()) + ["label_from", "donor"]
    # THE RECOVERED FILES CARRY BOTH COUNTS: the labelling decode's in the ordinary columns, this reception's failed
    # attempt beside them. Two numbers, two meanings; one column pair would have to pick one of them and lose the other.
    cols_harq = cols_plain + ["own_iterations", "own_iterations_max"]
    with open(os.path.join(args.out, "index_self.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_plain, extrasaction="ignore")
        writer.writeheader()
        for row in self_rows:
            out = dict(row)
            out["label_from"] = "self"
            out["donor"] = ""
            writer.writerow(out)
    with open(os.path.join(args.out, "index_harq_only.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_harq)
        writer.writeheader()
        for row in harq_rows:
            out = dict(row)
            out["crc"] = "OK"       # usable by the builder; the file name says where the label came from
            out["tb_bits"] = str(os.path.getsize(os.path.join(args.out, "tb", row["unit"] + ".bits")) * 8)
            out["label_from"] = "harq"
            out["donor"] = donor_of.get(row["unit"], "")
            out["own_iterations"] = row.get("iterations", "")
            out["own_iterations_max"] = row.get("iterations_max", "")
            combined = iters_of.get(row["unit"])
            out["iterations"] = str(combined[0]) if combined else ""
            out["iterations_max"] = str(combined[1]) if combined else ""
            writer.writerow(out)

    with open(os.path.join(args.out, "index_harq.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_harq, extrasaction="ignore")
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
                out["own_iterations"] = row.get("iterations", "")
                out["own_iterations_max"] = row.get("iterations_max", "")
                combined = iters_of.get(name)
                out["iterations"] = str(combined[0]) if combined else ""
                out["iterations_max"] = str(combined[1]) if combined else ""
            writer.writerow(out)

    # THE COMPLEMENT, FOR A SECOND DECODE BACK END. Every unit here has no label from the classical chain: the per-unit
    # pass failed it and no retransmission recovered it. With the capture directory beside this file it is the work list
    # for any other decoder, and its rows decide how to retry: a first transmission (new_data=1) can be replayed on its
    # own by another back end, while a retransmission (new_data=0) needs the chain, because a standalone replay never has
    # its soft buffer. The reason column says whether the classical chain even reached the LDPC decoder.
    ko_rows = [r for r in rows if label_from.get(r["unit"]) == "none"]
    cols_ko = list(rows[0].keys()) + ["new_data", "rv", "harq_id", "rnti", "reason"]
    with open(os.path.join(args.out, "index_ko.csv"), "w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=cols_ko, extrasaction="ignore")
        writer.writeheader()
        for row in ko_rows:
            name = row["unit"]
            info = units[name]
            out = dict(row)
            out["new_data"] = info["new_data"]
            out["rv"] = info["rv"]
            out["harq_id"] = info["harq"]
            out["rnti"] = info["rnti"]
            # An empty iteration count means the decoder was never reached: the harness reserves the soft buffer with
            # the capture's own new_data flag and a retransmission has none to reserve.
            out["reason"] = "no-buffer" if not row.get("iterations", "") else "decode-failed"
            writer.writerow(out)

    n_self = sum(1 for v in label_from.values() if v == "self")
    n_harq = sum(1 for v in label_from.values() if v == "harq")
    print(f"units with a HARQ process: {len(units)}  chains: {len(chains)}")
    print(f"decoded on their own: {n_self}   recovered from a retransmission: {n_harq}")
    print(f"wrote index_self.csv ({len(self_rows)}) and index_harq_only.csv ({len(harq_rows)}) in {args.out}")
    print(f"wrote index_ko.csv ({len(ko_rows)}): the units no decode labelled")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
