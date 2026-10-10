#!/usr/bin/env python3
"""Regenerate survey_citations.md: which surveyed paper is cited by which document.

The reference PDFs are named after the paper title (see fetch_refs.sh), but the
survey and memo files cite papers by arXiv ID. This script closes that gap: for
every arXiv ID cited anywhere under llr_ai_detection/, it records the citing
files and the citing line, and resolves the ID to the archived PDF when one
exists. That makes the archive auditable in the direction that matters -- from a
paper back to the argument that relies on it -- without hand-maintaining a
second, drift-prone index.

Run from anywhere:

    python3 build_survey_citations.py

Output is committed; the PDFs are not (see doc_chinese/.gitignore).
"""

from __future__ import annotations

import json
import re
import subprocess
import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent                      # llr_ai_detection/
DOC_ROOT = ROOT.parent                  # doc_chinese/

# "arXiv:2508.06275", "arXiv 2508.06275", "arxiv.org/abs/2508.06275" and bare
# "2508.06275" all appear in this tree. New-style IDs (post-2015) are YYMM.NNNNN;
# old-style ones (e.g. 1011.2113) share the shape, so one pattern covers both.
ID_RE = re.compile(r"\b(\d{4}\.\d{4,5})\b")
ARXIV_URL_RE = re.compile(r"arxiv\.org/(?:abs|pdf)/(\d{4}\.\d{4,5})")
TITLE_FROM_PDF = re.compile(r"^(.+?)\.pdf$")

API = "http://export.arxiv.org/api/query"


def cited_ids() -> tuple[dict[str, list[tuple[Path, int, str]]], dict[str, str]]:
    """Map arXiv ID -> [(file, line number, citing text), ...], plus a context blurb.

    The citing *text* is the enclosing document paragraph, not just the line that
    carries the ID: these files are hard-wrapped, so a single line is usually a
    sentence fragment. The survey files already state why each paper matters, so
    quoting that paragraph is the honest way to explain an entry -- far better
    than a summary written from the title alone.
    """
    found: dict[str, list[tuple[Path, int, str]]] = {}
    context: dict[str, str] = {}
    for path in sorted(DOC_ROOT.rglob("*.md")):
        if "ref_paper" in path.parts:
            continue
        try:
            text = path.read_text(encoding="utf-8")
        except UnicodeDecodeError:
            continue
        rel = path.relative_to(DOC_ROOT)

        for match in re.finditer(r"^\s*[-*]\s+(.*?)(?=\n\s*[-*]\s|\n#|\n---|\Z)",
                                 text, re.S | re.M):
            block = " ".join(match.group(1).split())
            ids = set(ID_RE.findall(block)) | set(ARXIV_URL_RE.findall(block))
            for arxiv_id in ids:
                context.setdefault(arxiv_id, block[:400])

        for lineno, line in enumerate(text.splitlines(), 1):
            ids = set(ID_RE.findall(line)) | set(ARXIV_URL_RE.findall(line))
            for arxiv_id in ids:
                found.setdefault(arxiv_id, []).append((rel, lineno, line.strip()))
    return found, context


def archived_titles() -> dict[str, str]:
    """Map arXiv ID -> archived filename stem.

    Two sources, each authoritative for its half: the download log says which ID
    belongs to which title, and the directory says which titles are really on disk.
    A row counts as archived only when both agree, so a stale log cannot claim a
    PDF that is not there. The log is tab-delimited; the space-delimited fallback
    is kept for logs written before that change (it truncates titles, which the
    on-disk check then rejects rather than mis-reporting).
    """
    on_disk = {p.stem for p in HERE.glob("*.pdf")}
    by_id: dict[str, str] = {}
    log = HERE / "download_log.txt"
    if not log.exists():
        return by_id
    for line in log.read_text(encoding="utf-8").splitlines():
        parts = line.split("\t") if "\t" in line else line.split(None, 3)
        if len(parts) >= 3 and parts[0] in {"OK", "SKIP"}:
            title = parts[2].strip()
            if title in on_disk:
                by_id[parts[1]] = title
    return by_id


def fetch_metadata(ids: list[str]) -> dict[str, dict[str, str]]:
    """Fetch title/date/comment for IDs the download log cannot resolve."""
    meta: dict[str, dict[str, str]] = {}
    for start in range(0, len(ids), 40):
        chunk = ids[start:start + 40]
        query = urllib.parse.urlencode({"id_list": ",".join(chunk), "max_results": len(chunk)})
        try:
            with urllib.request.urlopen(f"{API}?{query}", timeout=90) as response:
                raw = response.read()
        except Exception as exc:                                  # noqa: BLE001
            print(f"  ! metadata fetch failed for {chunk[0]}..: {exc}", file=sys.stderr)
            continue
        for entry in ET.fromstring(raw).findall("{http://www.w3.org/2005/Atom}entry"):
            id_el = entry.find("{http://www.w3.org/2005/Atom}id")
            title_el = entry.find("{http://www.w3.org/2005/Atom}title")
            date_el = entry.find("{http://www.w3.org/2005/Atom}published")
            if id_el is None or id_el.text is None:
                continue
            m = re.search(r"abs/(\d{4}\.\d{4,5})", id_el.text)
            if not m:
                continue
            meta[m.group(1)] = {
                "title": " ".join((title_el.text or "").split()) if title_el is not None else "",
                "date": (date_el.text or "")[:10] if date_el is not None else "",
            }
    return meta


def main() -> int:
    citations, context = cited_ids()
    log_ids = archived_titles()
    print(f"cited arXiv IDs: {len(citations)}; archived per log: {len(log_ids)}")

    # IDs cited but absent from the log need metadata from the API to be named.
    unresolved = [i for i in sorted(citations) if i not in log_ids]
    meta = fetch_metadata(unresolved) if unresolved else {}

    rows = []
    for arxiv_id in sorted(citations, key=lambda s: (s.split(".")[0], int(s.split(".")[1]))):
        refs = citations[arxiv_id]
        files = sorted({str(f) for f, _, _ in refs})
        title = log_ids.get(arxiv_id) or meta.get(arxiv_id, {}).get("title", "")
        rows.append({
            "id": arxiv_id,
            "title": title,
            "archived": arxiv_id in log_ids,
            "files": files,
            "n_citations": len(refs),
            "first_context": refs[0][2][:400],
            "why": context.get(arxiv_id, ""),
        })

    out = HERE / "survey_citations.md"
    with out.open("w", encoding="utf-8") as fh:
        fh.write("# `ref_paper/` — 逐篇出处索引（**自动生成，勿手改**）\n\n")
        fh.write("> 生成方式：`python3 build_survey_citations.py`（从本目录运行）。\n")
        fh.write("> 它扫描 `doc_chinese/` 下所有 `.md`，把每个 arXiv ID 映射到**引用它的文件与行**，\n")
        fh.write("> 再与 `download_log.txt` 对照，标出该篇**是否已存档为 PDF**。\n")
        fh.write("> ⚠ **PDF 不入 git**（`doc_chinese/.gitignore`）；本索引入 git。\n\n")
        archived = sum(1 for r in rows if r["archived"])
        fh.write(f"**共 {len(rows)} 个被引用的 arXiv ID；其中 {archived} 篇已存档 PDF，"
                 f"{len(rows) - archived} 篇未存档。**\n\n")
        fh.write("| arXiv | 标题 | 存档 | 引用次数 | 引用它的文档 |\n")
        fh.write("|---|---|---|---|---|\n")
        for r in rows:
            mark = "✅" if r["archived"] else "—"
            title = r["title"].replace("|", "\\|") or "*（未解析）*"
            files = "<br>".join(f"`{f}`" for f in r["files"])
            fh.write(f"| `{r['id']}` | {title} | {mark} | {r['n_citations']} | {files} |\n")

    (HERE / "survey_citations.json").write_text(
        json.dumps(rows, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"wrote {out.name} and survey_citations.json ({len(rows)} rows)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
