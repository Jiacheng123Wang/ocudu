#!/usr/bin/env python3
"""Map modified test sources to the ctest cases they produce, using results.json.

For every test source file passed on the command line (or every modified test file reported by
`git status`), extract the gtest suite / fixture / instantiation names it declares and list the
matching ctest entries with their recorded outcome.
"""
import json
import os
import re
import subprocess
import sys

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(TOOLS_DIR, "..", "..", ".."))
TRIAGE = os.environ.get("TRIAGE_DIR") or os.path.join(REPO, "build", "macos_triage")

with open(os.path.join(TRIAGE, "results.json")) as fh:
    results = {int(k): v for k, v in json.load(fh).items()}

TEST_MACRO = re.compile(r"^\s*(?:TYPED_)?TEST(?:_F|_P)?\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)?", re.M)
INST = re.compile(r"INSTANTIATE_(?:TYPED_)?TEST_SUITE_P\(\s*([A-Za-z_][A-Za-z0-9_]*)\s*,\s*([A-Za-z_][A-Za-z0-9_]*)", re.M)


def ctest_key(name):
    """ctest name -> (group, case). Handles 'inst/suite.case/param' and 'suite.case'."""
    head = name.split(".", 1)[0]
    return head


def suites_of(path):
    src = open(os.path.join(REPO, path), errors="replace").read()
    suites = {m.group(1) for m in TEST_MACRO.finditer(src)}
    insts = {(m.group(1), m.group(2)) for m in INST.finditer(src)}
    keys = set(suites)
    for inst, suite in insts:
        keys.add(f"{inst}/{suite}")
    return keys, {c for _, c in ((m.group(1), m.group(2)) for m in TEST_MACRO.finditer(src)) if c}


def main(paths):
    for path in paths:
        if not os.path.exists(os.path.join(REPO, path)):
            print(f"## {path}\n  (file not found)\n")
            continue
        keys, cases = suites_of(path)
        matched = []
        for idx, r in sorted(results.items()):
            group = ctest_key(r["name"])
            if group in keys:
                matched.append((idx, r))
            elif r["name"] in keys:
                matched.append((idx, r))
        # plain add_test binaries: match by file stem too
        stem = os.path.splitext(os.path.basename(path))[0]
        for idx, r in sorted(results.items()):
            if r["name"] == stem and (idx, r) not in matched:
                matched.append((idx, r))
        outcomes = {}
        for idx, r in matched:
            outcomes.setdefault(r["outcome"], []).append(idx)
        print(f"## {path}")
        print(f"  gtest suites: {sorted(keys) if keys else '-'}")
        if not matched:
            print("  no ctest entry matched\n")
            continue
        for k in sorted(outcomes):
            idxs = sorted(outcomes[k])
            rng = f"{idxs[0]}..{idxs[-1]}" if len(idxs) > 1 else f"{idxs[0]}"
            print(f"  {k}: {len(idxs)} cases (index {rng})")
        print()


if __name__ == "__main__":
    args = sys.argv[1:]
    if not args:
        out = subprocess.run(["git", "status", "--porcelain"], cwd=REPO, capture_output=True, text=True).stdout
        args = [l[3:].strip() for l in out.splitlines() if l[3:].strip().startswith("tests/")]
    main(args)
