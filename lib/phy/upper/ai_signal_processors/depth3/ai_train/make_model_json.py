#!/usr/bin/env python3
"""Write the sidecar that the naming convention requires, from the artefacts themselves.

WHY IT IS GENERATED. The recipe table records the intent (batch 16) while the run used what fitted (batch 12, passed as a
second --batch), and the corpus slice is invisible in most file names. The only trustworthy record is the argv the trainer
printed and the bytes of the compiled bundle, so both are read here rather than retyped.

usage: make_model_json.py --argv-log RUN.log --bundle B.mlmodelc --name d3_w2_c128b18_e2e-cond-norep_e33 \
                           [--keras SNAP.keras] [--snapshot-epoch 33] [--aliases a,b] [--out DIR/model.json]
"""
import argparse, hashlib, json, os, pathlib, re, sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def dir_bytes(root):
    total = 0
    for p in pathlib.Path(root).rglob("*"):
        if p.is_file():
            total += p.stat().st_size
    return total


def parse_argv(log):
    """The trainer prints one `argv:` line; a repeated flag is resolved the way argparse does -- last one wins."""
    line = None
    for raw in open(log, errors="replace"):
        if raw.startswith("argv:"):
            line = raw[len("argv:"):].strip()
            break
    if line is None:
        sys.exit(f"no argv: line in {log}")
    toks, flags, key = line.split(), {}, None
    for tok in toks:
        if tok.startswith("--"):
            key = tok[2:]
            flags[key] = True
        elif key is not None:
            flags[key] = tok
            key = None
    return flags


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--argv-log", required=True)
    ap.add_argument("--bundle", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--keras")
    ap.add_argument("--snapshot-epoch", type=int)
    ap.add_argument("--aliases", default="")
    ap.add_argument("--best", action="store_true",
                    help="this snapshot is the validation-selected best; the name must end in 'best'")
    ap.add_argument("--out")
    a = ap.parse_args()

    # THE NAME IS THE CONTRACT (2026-11-06, user ruling): a best-epoch snapshot carries "best" after its epoch,
    # because "best" is chosen by validation and validation has picked the weaker snapshot on the ruler three times.
    # A name that says "best" for a file that is not one, or omits it for one that is, is worse than no suffix at all.
    if a.best and not a.name.endswith("best"):
        sys.exit(f"--best given but the name {a.name!r} does not end in 'best'")
    if not a.best and a.name.endswith("best"):
        sys.exit(f"name {a.name!r} claims best; pass --best to record that claim")

    f = parse_argv(a.argv_log)
    init = f.get("init")
    train_mode = "ft@" + pathlib.Path(init).parent.name if init else "e2e"
    rec = {
        "name": a.name,
        "aliases": [x for x in a.aliases.split(",") if x],
        "dataset": {
            "train": pathlib.Path(f.get("train", "")).name,
            "val": pathlib.Path(f.get("val", "")).name,
        },
        "arch": {
            "c_hid": int(f.get("c-hid", 0)),
            "blocks": int(f.get("blocks", 0)),
            "cond": "modcond" in f,
            "reporting_heads": "no-reporting-heads" not in f,
        },
        "train": {
            "mode": train_mode,
            "init_from": init,
            "batch": int(f.get("batch", 0)),
            "lr": float(f.get("lr", 0.0)),
            "epochs_planned": int(f.get("epochs", 0)),
            "snapshot_epoch": a.snapshot_epoch,
            "is_best": bool(a.best),
            "early_stop_patience": int(f.get("patience", 0)),
            "seed": int(f.get("seed", 0)) if str(f.get("seed", "")).lstrip("-").isdigit() else f.get("seed"),
            "workers": int(f.get("workers", 0)) if str(f.get("workers", "")).isdigit() else None,
            "snapshot_every": int(f.get("snapshot-every", 0)) if str(f.get("snapshot-every", "")).isdigit() else None,
        },
        "artifact": {
            "mlmodelc": str(a.bundle),
            "mlmodelc_bytes": dir_bytes(a.bundle),
        },
        "measured": {},
    }
    coreml = pathlib.Path(a.bundle) / "coremldata.bin"
    if coreml.is_file():
        rec["artifact"]["coremldata_sha256"] = sha256(coreml)
    if a.keras and os.path.isfile(a.keras):
        rec["artifact"]["keras_sha256"] = sha256(a.keras)
    out = a.out or os.path.join(a.bundle, "model.json")
    json.dump(rec, open(out, "w"), indent=2)
    print(f"wrote {out}")
    print(json.dumps(rec, indent=2)[:600])


if __name__ == "__main__":
    main()
