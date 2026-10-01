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
# 2026-10-01 REPAIR (four defects, each found by comparing the output against the source):
#   * SELF-REFERENCE: doc_mentions() walked doc_chinese/** including THIS SCRIPT'S OUTPUT, and the inventory
#     lists every knob by construction - so every knob counted as "mentioned in the record" from the second
#     generation onwards and section 3.1 (the retirement list) could only ever shrink to 0. It read 0 for
#     the 8 offline arms that the previous generation had itself listed. The output file is now skipped.
#   * WINDOW OVERRUN: the 10-line window runs past this knob's read into the NEXT knob's read, so
#     OCUDU_CE_EDGE_CHECK (a diagnostic, default OFF) was classified ON on the guard of the knob 9 lines
#     below it and printed in section 1 - the delivery-shape whitelist. The window is now cut at the first
#     read of a DIFFERENT knob.
#   * VARIABLE NAME: the boolean shapes assumed the value is held in `env`; OCUDU_DFT_WAIT_PER_SLOT holds it
#     in `arm`, and OCUDU_DFT_RELEASE_BLOCK's guard is `if (env == nullptr) { return true; }` (default ON
#     since 5.9.49, per its own warning text) which the crude early-return rule called OFF. Both shapes are
#     recognised now, bound to the identifier this knob's own getenv() is assigned to.
#   * VALUE DEFAULTS: a knob whose default is a plain number (`= 8`) or a string (`= "vdsp"`) reported `?`.
#     Recognised in the ternary and strcmp forms; `?` remains for the "set = on" inline predicates, which are
#     deliberately NOT read as ON - section 1 is a whitelist and a probe must never enter it.
#
# usage:  python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/ocudu_env_knobs_inventory_and_leg_whitelist.md

import collections
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", ".."))
LOGDIR = os.path.join(ROOT, "doc_chinese", "phy_pipeline_gpu", "wip", "logs")
SELF = os.path.join(ROOT, "doc_chinese", "ocudu_env_knobs_inventory_and_leg_whitelist.md")  # own output
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
    # The window is 10 lines, so it can run past this knob's read into the NEXT knob's read (two knobs a few
    # lines apart are common). A guard that belongs to the next knob must not be read as this knob's default:
    # cut at the first read of a DIFFERENT knob - reading the same knob twice in one window is fine.
    first = re.search(r'getenv\(\s*"(OCUDU_[A-Z0-9_]+)"', w)
    if first:
        for m in re.finditer(r'getenv\(\s*"(OCUDU_[A-Z0-9_]+)"', w[first.end():]):
            if m.group(1) != first.group(1):
                w = w[:first.end() + m.start()]
                break
    # The variable that holds THIS knob's value is not always called `env` (OCUDU_DFT_WAIT_PER_SLOT uses `arm`),
    # so the boolean shapes below are bound to the identifier that this knob's own getenv() is assigned to.
    name = first.group(1) if first else None
    bound = [m.group(1) for m in re.finditer(r'(\w+)\s*=\s*(?:std::)?getenv\(\s*"(OCUDU_[A-Z0-9_]+)"', w)
             if m.group(2) == name] or ["env"]
    # default ON: `(v == nullptr) || (strtoul(v, nullptr, 10) != 0)`
    # default OFF written positively: `(v != nullptr) && (strtoul(v, nullptr, 10) != 0)`
    # default OFF written negatively: `(v == nullptr) || (strtoul(v, ...) == 0)`
    # ... and the fourth combination.
    strtoul_shapes = [("==", r"\|\|", "!=", "ON"), ("!=", "&&", "!=", "OFF"),
                      ("==", r"\|\|", "==", "OFF"), ("!=", "&&", "==", "ON")]
    for null_cmp, glue, zero_cmp, verdict in strtoul_shapes:
        for v in bound:
            pattern = (re.escape(v) + r"\s*" + null_cmp + r"\s*nullptr\)\s*\(?\s*" + glue + r"\s*\(?\s*"
                       + r"(?:std::)?strtoul[^;]*" + zero_cmp + r"\s*0")
            if re.search(pattern, w):
                return verdict
    if re.search(r"return\s+0\s*;\s*//\s*AUTO", w) or re.search(r"//\s*AUTO", w):
        return "AUTO"
    # default OFF written as an early return: `if (getenv("X") == nullptr) { ... return; }` (void), and the
    # mirror of it that returns a VERDICT: `if (env == nullptr) { return true; }` is ON - that is
    # OCUDU_DFT_RELEASE_BLOCK's shape, whose default has been ON since 5.9.49 ("using the default (armed)"
    # in its own warning text). The crude `== nullptr) {` + `return` rule below would call that OFF.
    for v in bound + ["env"]:
        m = re.search(r"if\s*\(\s*" + re.escape(v) + r"\s*==\s*nullptr\s*\)\s*\{[^{}]*?\breturn\s+(true|false)\b", w)
        if m:
            return "ON" if m.group(1) == "true" else "OFF"
    if re.search(r"==\s*nullptr\)\s*\{", w) and re.search(r"\breturn\b", w):
        return "OFF"
    # the knob read as a PREDICATE: `return std::getenv("X") != nullptr;` (set = on) / `== nullptr;`
    if re.search(r"return\s+(?:std::)?getenv\([^)]*\)\s*!=\s*nullptr", w):
        return "OFF"
    if re.search(r"return\s+(?:std::)?getenv\([^)]*\)\s*==\s*nullptr", w):
        return "ON"
    # a VALUE default rather than a boolean: `x = (env == nullptr) ? 1U : strtoul(env, ...)`. The unset case IS
    # the default a leg runs with, so report the value instead of giving up (the knob inventory's job is what a
    # reader needs before flying; `?` for a knob whose default is a plain number hides it).
    m = re.search(r"env\s*==\s*nullptr\)\s*\?\s*([0-9]+)[uU]?\s*:", w)
    if m:
        return f"= {m.group(1)}"
    for v in bound:
        m = re.search(re.escape(v) + r"\s*==\s*nullptr\)\s*\?\s*([0-9]+)[uU]?\s*:", w)
        if m:
            return f"= {m.group(1)}"
    # a STRING default: `(name == nullptr) || (std::strcmp(name, "value") == 0)` - unset means this value,
    # i.e. the same shape as the boolean predicate above with one string comparison instead of strtoul
    # (OCUDU_DFT_BACKEND). The variable is not always named `env`, and the platform `#if` that guards the
    # whole expression can sit above the read window, so the row is the VALUE and the note says where it
    # applies.
    m = re.search(r"(\w+)\s*==\s*nullptr\)\s*\|\|\s*\(?\s*(?:std::)?strcmp\(\s*\1\s*,\s*\"([^\"]+)\"\s*\)\s*==\s*0", w)
    if m:
        return f'= "{m.group(2)}"'
    # a VALUE default in the ternary form: `const char* e = getenv("X"); ... (e != nullptr) ? strtoul(e,...) : 8U`.
    # The identifier that holds the getenv result has to be the one the ternary tests, so a ternary on some
    # OTHER variable inside the same window (OCUDU_LANE_ABLATE_EVERY) is not mistaken for the default.
    for name in set(re.findall(r"(\w+)\s*=\s*(?:std::)?getenv\(", w)):
        # `[^;]*?` rather than `[^:;]*`: the "then" branch usually contains `std::` (a colon of its own), so the
        # separator has to be found by backtracking from the FIRST literal after a colon.
        m = re.search(r"\(\s*" + re.escape(name) + r"\s*!=\s*nullptr\s*\)\s*\?\s*[^;]*?:\s*"
                      r"([0-9][0-9.eE+-]*[uUfF]?|\"[^\"]*\")\s*[;),]", w)
        if m:
            return f"= {m.group(1).rstrip('uUfF')}"
        # the mirrored form: the default is the THEN branch - `(mode_override == nullptr) ? "auto" : ...`
        m = re.search(r"\(\s*" + re.escape(name) + r"\s*==\s*nullptr\s*\)\s*\?\s*"
                      r"([0-9][0-9.eE+-]*[uUfF]?|\"[^\"]*\")\s*:", w)
        if m:
            return f"= {m.group(1).rstrip('uUfF')}"
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
    logs alone would report a knob flown in the s-series as never used. The record covers the history.

    THIS FILE'S OWN OUTPUT IS NOT THE RECORD (fixed 2026-10-01): the inventory lists every knob by
    construction, so counting it made every knob 'mentioned' from the second generation onwards and the
    retirement list of section 3.1 could only ever shrink to 0 - a self-reference, not a measurement."""
    counts = collections.Counter()
    files = collections.Counter()
    for base, _, names in os.walk(os.path.join(ROOT, "doc_chinese")):
        for n in names:
            if not n.endswith(".md"):
                continue
            path = os.path.join(base, n)
            if os.path.abspath(path) == SELF:
                continue
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

    print("# OCUDU_* 环境旋钮清单 + 验收腿白名单（**生成物** + 人工判读）\n")
    print("> 范围是**整棵树**（`lib/`、`apps/`、`include/`、`tests/` 里的 `getenv(\"OCUDU_*\")`），不限于某一条工作线。")
    print("> 2026-10-01 从 `phy_latency/knob_inventory.md` 上移到本目录并改名（旧路径只作历史）。")
    print()
    print("> 生成方式：`python3 doc_chinese/phy_latency/wip/gen_knob_inventory.py > doc_chinese/ocudu_env_knobs_inventory_and_leg_whitelist.md`")
    print(f"> 本次生成：commit `{head}`。**不要手改正文**——改生成器或改人工判读小节。")
    print("> （生成器把**生成那一刻的 HEAD**写进这一行；要把这一行也追平 HEAD，就重跑生成器再提交一次——那一次是纯文档差异。）")
    print(">")
    print("> **默认值**是**从守卫表达式读出来的**（`ON` = 不设或非 0 都开；`OFF` = 必须显式置 1；`AUTO` = 由别处推导；"
          "`= 14` / `= \"vdsp\"` = 默认是一个**值**而不是开关，腿不设它时用的就是这个值；`?` = 需要读注释）。")
    print("> 生成器只认**本旋钮自己那次读取**的守卫（窗口在下一个旋钮的读取处截断），并且只认几种写法："
          "`?` 里绝大多数是「置位即开」的探针/实验选择器（`static const bool x = (std::getenv(\"X\") != nullptr);`），"
          "生成器**故意不把它们判成 `ON`** —— §1 是验收腿白名单，**宁可漏，不可错**。")
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
        "OCUDU_DFT_RELEASE_TOKENS_EARLY": "P2-E：输入令牌在\"最后一个读输入的派发\"之后释放（默认开的理由 = 去掉输入保持；"
                                          "**不是接收尾巴的修复** —— 见开发文档 6.157）",
    }
    on_list = [k for k in sorted(hits) if default_of(hits[k]) == "ON"]
    for knob in on_list:
        rows = hits[knob]
        site = f"{rows[0][0]}:{rows[0][1]}"
        print(f"| `{knob}` | ON | `{site}` | {leg_counts.get(knob, 0)} | {notes_on.get(knob, '—')} |")
    print()
    print("## 2. 探针、实验臂与消去法（**不是**交付形态；默认 `OFF` 或需要显式给值）")
    print()
    print("| 旋钮 | 默认 | 读取点 | 飞过的腿 | 说明 |")
    print("|---|---|---|---|---|")
    curated_new = {
        "OCUDU_LANE_ABLATE": "消去法总开关：各阶段的绑定换成 `lane_ablate_noop`（只换 kernel，网格/屏障/提交结构不变）",
        "OCUDU_LANE_ABLATE_EVERY": "**修饰符**（默认 1 = 每跳都消去；只在 `OCUDU_LANE_ABLATE=1` 时有意义）：`=8` = 每 8 跳消去 1 跳，全消去手机接不进来（p79）",
        "OCUDU_LANE_ABLATE_STAGE": "**修饰符**（只在 `OCUDU_LANE_ABLATE=1` 时有意义）：只消去哪一**阶段族**——`front_end`/`ce`/`eq`/`demap`（`|` 或 `,` 组合），不设 = `all` = 历史行为；拼错的名字按 `all` 处理并打 WARNING。族级账单靠它，覆盖度看报告里的 `Q9-F5 ablation coverage`（开发文档 6.162）",
        "OCUDU_METAL_GPU_TIME": "**探针**：给每条 cb 装 GPU 时间戳（per-label 表的来源；验收腿一直带着它）",
        "OCUDU_UL_PHASE_SEGMENTS": "**探针**：上行相位分段读数（验收腿一直带着它）",
        "OCUDU_UL_TIMING_EVENTS": "**探针（新，开发文档 6.240–6.243）**：打印**最慢的 K 次接收等待**与**最迟的 K 次 DL 交接**，各带**宿主墙钟**（`wall=` UTC + `epoch_ms=` + 窗口两端 `began_ms=`/`due_ms=`）、当时的 `load1`，以及**本进程在那个窗口里的 CPU 时间与自愿/非自愿切换增量**（`cpu=`/`ivcsw=`/`nvcsw=`/`base_age=`）—— 用来把 `[ul_rx_wait]` 的尖峰、DL 的迟到和 `.log` 里 `[RF] Real-time failure in RF` 的行对到**同一条时间轴**上。★ 判读只看 **`cpu=` 对窗口**：`cpu ≈ wait` ⇒ 进程一直有 CPU ⇒ 是**电台/USB 侧**晚；`cpu ≈ 0` 或 `ivcsw>0` ⇒ 进程没被调度 ⇒ **宿主调度**。（`load1` 是 60 s 平均，**看不见 10 ms 级事件**，别用它判 —— 纪律 78）",
        "OCUDU_UL_SLOT_TRACE": "**探针**：每**时隙**时间线（`=N` = 最多记 N 个时隙，非数字 = 开且用默认上限）。和上面两个一样被两条闸门当「任意值」接受，但它比相位分段宽得多，**验收腿不需要它**——只在追「某个时隙为什么晚」时开（开发文档 6.145⑹⑴；`=64` 曾打出 512 行，见 `ul_pipeline_probe.h` 的注）",
        "OCUDU_DFT_BACKEND": "前端变换的**后端选择**：`=vdsp`（Apple 上**不设就是它**，所以白名单接受）｜`=generic`（**A/B 对照臂**，n78 p170/p171、n1 p172/p173 用它跑 generic 那一侧）。非 Apple 平台根本不编进这条分支，所以这一行的「默认」只在 Apple 上有意义",
        "OCUDU_DFT_BATCH_SYMBOLS": "前端批量：不设 = `AUTO`（= 一个时隙自己的符号数，n78 上是 **14**）｜`=1` = 每符号对照臂｜`=7`/`=2` 是中间臂。白名单只接受与 AUTO 等价的 `=14`",
        "OCUDU_CE_LANE_ORDER": "信道估计的四种车道顺序：`merged`（**默认**，估计器的派发搭车道共享 cb）｜`event`｜`wait`/`host_wait`｜`burst`（旧名 `OCUDU_CE_FUSED_BURST`）。拼错的值打 error 并按 `merged` 跑（代码里那条 warning 的原文就写着 \"using merged\"）。白名单只接受 `=merged`",
    }
    for knob, note in curated_new.items():
        rows = hits.get(knob, [])
        site = f"{rows[0][0]}:{rows[0][1]}" if rows else "—"
        dflt = default_of(rows) if rows else "?"
        print(f"| `{knob}` | {dflt} | `{site}` | {leg_counts.get(knob, 0)} | {note} |")
    print()
    print("> ⚠ **验收腿的旋钮白名单**（`milestone_audit.sh` 的 `kNOB_ANY`/`kNOB_EQ` 与 `leg_gate.sh` 的 `KNOB_ANY`/`KNOB_EQ` **就是它**，两边逐字一致）。")
    print("> **任意值**（探针）：`OCUDU_METAL_GPU_TIME`、`OCUDU_UL_PHASE_SEGMENTS`、`OCUDU_UL_SLOT_TRACE`、"
          "`OCUDU_UL_TIMING_EVENTS`（2026-10-01 加入：只**打印**最慢的接收等待 / 迟到交接及其宿主墙钟与进程 CPU 增量，"
          "不改变任何交付决定；关着不读时钟、不打印，开着最多存 64 条事件 —— 见开发文档 6.240/6.241）。")
    print("> **视为「等于交付默认」**：`OCUDU_DFT_BATCH_SYMBOLS=14`、`OCUDU_DFT_OPEN_BLOCK=1`、`OCUDU_DFT_RELEASE_BLOCK=1`、`OCUDU_CE_LANE_ORDER=merged`、"
          "`OCUDU_DFT_BACKEND=vdsp`（2026-10-01 加入：Apple 上这就是不设它时的值）。其余一律判 FAIL（**fail-closed**）。")
    print("> `OCUDU_DFT_BACKEND=generic` **故意不**在白名单里：那是一条 A/B **臂**——臂可以满足其余所有判据（p84 就是这样），闸门拦的就是它。")
    print("> 6.215 起交付车道的网格由 **host** 写，所以 `OCUDU_DFT_BATCH_SYMBOLS`/`OCUDU_DFT_OPEN_BLOCK`/`OCUDU_DFT_RELEASE_BLOCK` 对交付腿是 **MOOT**（那个引擎根本不在路上）；**最有力的交付腿是一个旋钮都不设**，白名单只是给「已经设了」的腿留出等于默认的写法。")
    print("> `OCUDU_UL_RX_SYMBOLS` 也不在白名单里，但它的**默认值就是 `= 1`**（每符号一跳）——交付腿不设它即得交付形态，设 `=7`/`=14` 才是改形状。")
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
