#!/usr/bin/env python3
"""Shared ctest log parsing for the macOS triage scripts (no side effects)."""
import re

HEAD = re.compile(r"^\s*\d+/\d+\s+Test\s+#(\d+):\s+(.*)$")
TIME = re.compile(r"([0-9.]+)\s+sec\s*$")
START = re.compile(r"^\s*Start\s+(\d+): (.*?)\s*$")


def normalize(text):
    t = text.strip()
    t = t.lstrip("*").strip()
    if "Not Run (Disabled)" in t:
        return "Disabled"
    for key in ("Passed", "Timeout", "Skipped", "Not Run"):
        if t.startswith(key) or t.endswith(key):
            return key
    if "Exception" in t or "aborted" in t or "SEGFAULT" in t or "Child aborted" in t:
        return "Crashed"
    if "Failed" in t:
        return "Failed"
    return t or "Unknown"


def parse_line(line):
    """Returns (index, name, outcome, seconds) for a ctest progress line, or None."""
    m = HEAD.match(line)
    if not m:
        return None
    idx, rest = int(m.group(1)), m.group(2)
    tm = TIME.search(rest)
    if not tm:
        return None
    body = rest[: tm.start()]
    seconds = float(tm.group(1))
    # ctest pads the name with a run of dots; fall back to a run of spaces for very long names.
    parts = re.split(r"\.{2,}", body, maxsplit=1)
    if len(parts) == 2:
        name, tail = parts
    else:
        parts = re.split(r"\s{2,}", body.strip(), maxsplit=1)
        if len(parts) != 2:
            return None
        name, tail = parts
    return idx, name.strip(), normalize(tail), seconds


def parse_log(path):
    """Returns (results, started) where results maps index -> {name, outcome, sec}."""
    results, started = {}, {}
    with open(path, "r", errors="replace") as fh:
        for raw in fh:
            line = raw.rstrip("\n")
            m = START.match(line)
            if m:
                started[int(m.group(1))] = m.group(2)
                continue
            parsed = parse_line(line)
            if parsed:
                idx, name, outcome, seconds = parsed
                results[idx] = {"name": name, "outcome": outcome, "sec": seconds}
    return results, started
