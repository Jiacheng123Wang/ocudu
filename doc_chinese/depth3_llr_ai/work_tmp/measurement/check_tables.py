#!/usr/bin/env python3
"""Find Markdown tables that a GFM renderer will refuse to draw.

A GFM table needs the delimiter row to have EXACTLY as many cells as the header row. When it does not, the whole table
silently degrades to a paragraph and the pipes appear inline -- which is what happened to the register's section 1.1 on
2026-10-27: nine header cells against eight delimiters, and only that one table in the file was affected.

Usage: check_tables.py [paths...]        (default: every .md under doc_chinese/)
"""
import pathlib
import re
import sys

SEP = re.compile(r"^\|[\s:\-|]+\|$")


def cells(row):
    """The number of cells, ignoring ESCAPED pipes: `\|` is a literal inside a cell, not a delimiter.

    Two documents legitimately use `max\|slot\|` inside code spans in a cell header, which naive counting reports as a
    broken table; the escaped form is what GFM expects and renders correctly.
    """
    return row.replace("\\|", "").count("|")


def check(path):
    bad = []
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    for i in range(1, len(lines)):
        if SEP.match(lines[i].strip()) and lines[i - 1].lstrip().startswith("|"):
            head, sep = cells(lines[i - 1]), cells(lines[i])
            if head != sep:
                bad.append((i + 1, head, sep, lines[i - 1].strip()[:60]))
    return bad


def main(argv):
    paths = [pathlib.Path(a) for a in argv[1:]] or sorted(pathlib.Path("doc_chinese").rglob("*.md"))
    total = 0
    for path in paths:
        if not path.is_file():
            continue
        bad = check(path)
        total += len(bad)
        for line, head, sep, text in bad:
            print(f"{path}:{line}: header has {head} pipes, separator {sep} -- this table will not render\n    {text}")
    print(f"checked {len(paths)} file(s): {total} broken table(s)")
    return 1 if total else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
