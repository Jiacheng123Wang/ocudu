#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
#
# WHAT THIS IS (2026-09-27): the OCUDU_* environment knobs this tree reads, with the facts a reader needs
# before flying a leg - the DEFAULT each one resolves to (read off its guard expression, not off prose),
# where it is read, how many sites read it, and HOW MANY LEGS HAVE ACTUALLY FLOWN IT.
#
# WHY IT EXISTS. The tree had accumulated ~110 knobs with no single list: the record names them one at a
# time, in the section that introduced each, and two questions kept being asked by hand - "is this switch
# part of the delivery shape (default ON) or a measurement arm?" and "has anyone ever flown this?".
# Both are mechanical, so they are generated. The last column is the load-bearing one: a knob that no leg
# has ever carried is either a new instrument or a retired one, and the two look identical in the source.
#
# The classification is a HEURISTIC on the guard, and it says so per row (`?` = read the comment). It is
# generated output: regenerate rather than edit, and put curated prose in the .md, not here.
#
# usage:  python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/phy_latency/knob_inventory.md

import collections
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
LOGDIR = os.path.join(ROOT, "doc_chinese", "phy_pipeline_gpu", "wip", "logs")
SRC_EXT = (".cpp", ".h", ".mm", ".metal")

READ_RX = re.compile(r'getenv\(\s*"(OCUDU_[A-Z0-9_]+)"\s*\)')


def tracked_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "lib", "apps", "include", "tests"],
                         capture_output=True, text=True, check=True).stdout
    return [p for p in out.split("\n") if p.endswith(SRC_EXT)]


def classify_default(window):
    """ON / OFF / AUTO / ? - read off the guard expression, in the three shapes this tree uses.
    `window` is the 4 source lines around the getenv() (or a string)."""
    w = " ".join(("\n".join(window) if isinstance(window, (list, tuple)) else window).split())
    # default ON: `(env == nullptr) || (strtoul(env, nullptr, 10) != 0)`
    if re.search(r"env\s*==\s*nullptr\)\s*\|\|\s*\(?\s*(?:std::)?strtoul[^;]*!=\s*0", w):
        return "ON"
    # default OFF written positively: `(env != nullptr) && (strtoul(env, nullptr, 10) != 0)`
    if re.search(r"env\s*!=\s*nullptr\)\s*&&\s*\(?\s*(?:std::)?strtoul[^;]*!=\s*0", w):
        return "OFF"
    # default OFF written negatively: `(env == nullptr) || (strtoul(...) == 0)`
    if re.search(r"env\s*==\s*nullptr\)\s*\|\|\s*\(?\s*(?:std::)?strtoul[^;]*==\s*0", w):
        return "OFF"
    if re.search(r"env\s*!=\s*nullptr\)\s*&&\s*\(?\s*(?:std::)?strtoul[^;]*==\s*0", w):
        return "ON"
    if re.search(r"return\s+0\s*;\s*//\s*AUTO", w) or re.search(r"//\s*AUTO", w):
        return "AUTO"
    # default OFF written as an early return: `if (getenv("X") == nullptr) { ... return; }`
    if re.search(r"==\s*nullptr\)\s*\{", w) and re.search(r"\breturn\b", w):
        return "OFF"
    # the knob read as a PREDICATE: `return std::getenv("X") != nullptr;` (set = on) / `== nullptr;`
    if re.search(r"return\s+(?:std::)?getenv\([^)]*\)\s*!=\s*nullptr", w):
        return "OFF"
    if re.search(r"return\s+(?:std::)?getenv\([^)]*\)\s*==\s*nullptr", w):
        return "ON"
    return "?"


def default_of(sites):
    """The first site whose guard this script recognises. Sites are not equal: the first read of a knob is
    often a predicate helper (`return getenv("X") != nullptr;`) rather than the guard that decides the
    default, and either one answers the question - so the first RECOGNISED shape wins."""
    for _, _, win in sites:
        d = classify_default(win)
        if d != "?":
            return d
    return "?"


def scope_of(path):
    """Where the first read lives. `test` = an offline harness/unit test (its arms never fly on air, which
    is why they have no leg and no record - that is normal, not dead code)."""
    if path.startswith("tests/") or "/test/" in path:
        return "test"
    return path.split("/")[0]


def module_of(path):
    parts = path.split("/")
    if len(parts) >= 4 and parts[0] == "lib" and parts[1] == "phy":
        return "/".join(parts[:4]) if parts[2] in ("upper", "lower", "generic_functions", "metal") else "/".join(parts[:3])
    return "/".join(parts[:2])


def doc_mentions():
    """How often the RECORD names each knob (dev doc + memos + the plan). The `knob :` registration line
    only exists in the newer legs - measured 2026-09-27: 3 of the 108 `s`-series legs carry it - so the
    logs alone would report a knob flown in the s-series as never used. The record covers the history."""
    counts = collections.Counter()
    files = collections.Counter()
    for base, _, names in os.walk(os.path.join(ROOT, "doc_chinese")):
        for n in names:
            if not n.endswith(".md"):
                continue
            path = os.path.join(base, n)
            try:
                txt = open(path, errors="replace").read()
            except OSError:
                continue

            for k in set(READ_RX.findall(txt)) | set(re.findall(r"OCUDU_[A-Z0-9_]+", txt)):
                c = txt.count(k)
                if c:
                    counts[k] += c
                    files[k] += 1
    return counts, files


def legs_per_knob():
    """How many legs carried each knob, from the `knob : NAME=VALUE` lines run_leg.sh prints into stderr.
    This is the column that separates 'new instrument' from 'retired': the source cannot tell them apart."""
    counts = collections.Counter()
    values = collections.defaultdict(set)
    if not os.path.isdir(LOGDIR):
        return counts, values
    for name in os.listdir(LOGDIR):
        if not name.endswith(".log.stderr"):
            continue
        try:
            with open(os.path.join(LOGDIR, name), errors="replace") as fh:
                head = fh.read(262144)  # the knob lines are in the report header
        except OSError:
            continue
        for m in re.finditer(r"^knob\s*:\s*(OCUDU_[A-Z0-9_]+)=(\S*)", head, re.M):
            counts[m.group(1)] += 1
            values[m.group(1)].add(m.group(2))
    return counts, values


def main():
    hits = collections.defaultdict(list)
    for path in tracked_files():
        try:
            with open(os.path.join(ROOT, path), errors="replace") as fh:
                lines = fh.read().splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            for m in READ_RX.finditer(line):
                hits[m.group(1)].append((path, i + 1, lines[i:i + 10]))
    leg_counts, leg_values = legs_per_knob()
    doc_counts, doc_files = doc_mentions()

    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=10", "HEAD"],
                          capture_output=True, text=True).stdout.strip()

    print("# OCUDU_* 旋钮清单（**生成物** + 人工判读）")
    print()
    print("> 生成方式：`python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/phy_latency/knob_inventory.md`")
    print(f"> 本次生成：commit `{head}`。**不要手改正文**——改生成器或改人工判读小节。")
    print("> （生成器把**生成那一刻的 HEAD**写进这一行；要把这一行也追平 HEAD，就重跑生成器再提交一次——那一次是纯文档差异。）")
    print(">")
    print("> **默认值**是**从守卫表达式读出来的**（`ON` = 不设或非 0 都开；`OFF` = 必须显式置 1；`AUTO` = 由别处推导；`?` = 需要读注释）。")
    print("> **飞过的腿数**来自 `logs/*.log.stderr` 顶部的 `knob : NAME=VALUE` 登记行 —— 这是**唯一能区分「新仪器」与「已退役」的一列**，源码里两者长得一样。")
    print()
    n_leg = sum(1 for k in hits if leg_counts.get(k))
    n_doc = sum(1 for k in hits if doc_counts.get(k) and not leg_counts.get(k))
    n_none = sum(1 for k in hits if not leg_counts.get(k) and not doc_counts.get(k))
    n_on = sum(1 for k in hits if default_of(hits[k]) == "ON")
    n_test = sum(1 for k in hits if scope_of(hits[k][0][0]) == "test")
    print(f"合计 **{len(hits)}** 个旋钮：**{n_on}** 个默认 `ON`"
          f"（`OCUDU_DFT_RELEASE_TOKENS_EARLY` 于 2026-09-28 由 OFF 改为 ON：**理由 = 输入保持**，"
          f"见开发文档 6.157；6.156 当初写的\"吃掉接收尾巴 60-70×\"**已被 6.157 撤回**）"
          f"（= 交付形态的一部分）；"
          f"**{n_leg}** 个有腿登记行、**{n_doc}** 个只在记录里出现过、**{n_none}** 个两处都没有；其中 **{n_test}** 个的首个读取点在 `test/`（离线臂）。")
    print()

    # ---- the curated part: what a reader needs before flying -----------------------------------------
    print("## 1. 交付形态的一部分（默认 `ON`）——**验收腿上不许出现「改成 OFF」的值**")
    print()
    print("| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |")
    print("|---|---|---|---|---|")
    notes_on = {
        "OCUDU_DFT_OPEN_BLOCK": "前端一个时隙的变换合成一条派发（P2-B′，V1 −38.7% 的那一刀）",
        "OCUDU_DFT_RELEASE_BLOCK": "D1 交棒：前端块**不提交**就交给车道（融合车道的定义之一，5.9.49 起默认开）",
        "OCUDU_EQ_DIRECT_GRID": "均衡直接读网格（`y_gather` 消失，派发 10→6/跳）",
        "OCUDU_DEMOD_DEFER_ENCODE": "解映射延迟编码（融合车道的分组形状）",
        "OCUDU_CE_Y_DIRECT": "信道估计直接读 y（省一次 gather）",
    }
    on_list = [k for k in sorted(hits) if default_of(hits[k]) == "ON"]
    for knob in on_list:
        rows = hits[knob]
        site = f"{rows[0][0]}:{rows[0][1]}"
        print(f"| `{knob}` | ON | `{site}` | {leg_counts.get(knob, 0)} | {notes_on.get(knob, '—')} |")
    print()
    print("## 2. 本轮（2026-09-27）新增的仪器（默认 `OFF`）")
    print()
    print("| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |")
    print("|---|---|---|---|---|")
    curated_new = {
        "OCUDU_LANE_ABLATE": "消去法总开关：各阶段的绑定换成 `lane_ablate_noop`（只换 kernel，网格/屏障/提交结构不变）",
        "OCUDU_LANE_ABLATE_EVERY": "**修饰符**（默认 1 = 每跳都消去；只在 `OCUDU_LANE_ABLATE=1` 时有意义）：`=8` = 每 8 跳消去 1 跳，全消去手机接不进来（p79）",
        "OCUDU_METAL_GPU_TIME": "**探针**：给每条 cb 装 GPU 时间戳（per-label 表的来源；验收腿一直带着它）",
        "OCUDU_UL_PHASE_SEGMENTS": "**探针**：上行相位分段读数（验收腿一直带着它）",
    }
    for knob, note in curated_new.items():
        rows = hits.get(knob, [])
        site = f"{rows[0][0]}:{rows[0][1]}" if rows else "—"
        dflt = default_of(rows) if rows else "?"
        print(f"| `{knob}` | {dflt} | `{site}` | {leg_counts.get(knob, 0)} | {note} |")
    print()
    print("> ⚠ **验收腿的旋钮白名单**（`milestone_audit.sh` / `leg_gate.sh` 判的就是它）：`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS` 任意值；")
    print("> `OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged` 视为「等于交付默认」。其余一律判 FAIL（**fail-closed**）。")
    print()

    # ---- the generated part ---------------------------------------------------------------------------
    print("## 3. 全部旋钮（自动生成）")
    print()
    print("| 旋钮 | 默认 | 范围 | 首次读取点 | 站点 | 模块 | 腿登记行 | 记录提及(次/文件) | 腿上出现过的值 |")
    print("|---|---|---|---|---|---|---|---|---|")
    for knob in sorted(hits):
        path, lineno, window = hits[knob][0]
        dflt = default_of(hits[knob])
        n = len(hits[knob])
        vals = ",".join(sorted(leg_values.get(knob, []))[:6]) or "—"
        print(f"| `{knob}` | {dflt} | {scope_of(path)} | `{path}:{lineno}` | {n} | `{module_of(path)}` | {leg_counts.get(knob, 0)} | {doc_counts.get(knob, 0)}/{doc_files.get(knob, 0)} | {vals} |")
    print()
    never = sorted(k for k in hits if not leg_counts.get(k) and not doc_counts.get(k))
    never_delivery = [k for k in never if scope_of(hits[k][0][0]) != "test"]
    never_test = [k for k in never if scope_of(hits[k][0][0]) == "test"]
    print(f"### 3.1 既没有腿登记行、也从未在记录里出现过：{len(never)} 个")
    print()
    print(f"* **离线/测试臂 {len(never_test)} 个**（首个读取点在 `test/`）——它们本来就不上空口，没有腿、没有记录是**正常**的：")
    print()
    print("```")
    for k in never_test:
        print(k)
    print("```")
    print()
    print(f"* ★ **落在交付代码里的 {len(never_delivery)} 个 = 真正的退役候选**（源码里分不出「新仪器」与「已死」，要读注释再决定）：")
    print()
    print("```")
    for k in never_delivery:
        print(k)
    print("```")


if __name__ == "__main__":
    sys.exit(main())
