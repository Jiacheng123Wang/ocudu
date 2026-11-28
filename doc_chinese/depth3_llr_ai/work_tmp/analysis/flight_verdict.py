#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI
"""Read one gnb flight log and say, in one screen, what the leg did.

WHY A PARSER AND NOT A GREP LIST. A flight answers four questions at once -- did the radio stay continuous, did the
phone attach, did data flow, and was the AI arm actually computing -- and the answers live in four unrelated corners of
the log (the pipeline report, the CU-CP's N2 lines, the MAC console table, the receiver's own banner). Reading them by
hand is how a leg gets reported as "attached" when the only evidence was an NG Setup that later timed out.

Usage:
  flight_verdict.py <log> [--json]

Exit code: 0 when the RADIO and DATA paths are both good, 1 otherwise (ATTACH counts as part of DATA), so a script can
gate on it. Every line it prints is a line the log contains, so a verdict can always be traced back.
"""
import argparse
import json
import re
import sys


def find_one(pattern, text, cast=str):
    match = re.search(pattern, text, re.MULTILINE)
    return None if match is None else (cast(match.group(1)) if cast is not str else match.group(1))


def parse(text):
    out = {}

    # RADIO: the lower PHY's continuity report. A gap means samples were lost or repeated, which invalidates every
    # latency number in the leg (the leg then measures the recovery, not the receiver).
    continuity = re.search(r"radio sample continuity: (\d+) gaps over ([\d,]+) blocks.*?-> (\w+)", text)
    out["radio"] = None if continuity is None else {
        "gaps": int(continuity.group(1)),
        "blocks": int(continuity.group(2).replace(",", "")),
        "result": continuity.group(3),
    }

    # THE RADIO'S REAL-TIME LOOP, AND THE FIRST THING TO READ WHEN A UE DROPS. `underflow` is the TX ring running dry
    # (the host could not produce samples in time) and `late` is a receive block arriving after its slot -- both corrupt
    # the air interface whatever the receiver does, and neither is a model property. Measured 2026-11-28: 7 703 of them
    # in one attach leg whose UE was released 2.4 s after registering, against 0 in the same rig's idle bench.
    out["rf_failures"] = len(re.findall(r"Real-time failure in RF", text))
    out["rf_kinds"] = {kind: len(re.findall(rf"Real-time failure in RF: {kind}", text))
                       for kind in ("late", "underflow")}

    # THE RADIO'S OTHER TWO ACCOUNTANTS: DL/UL HARQ giving up after four retransmissions, and SRB1 retransmissions --
    # which is what a DL that never reaches the phone looks like from the gNB side (the 2026-11-28 release was exactly
    # this: the RRC reconfiguration was never ACKed, so the UE context was torn down).
    out["dl_harq_discards"] = len(re.findall(r"Discarding DL HARQ process TB", text))
    out["ul_harq_discards"] = len(re.findall(r"Discarding UL HARQ process TB", text))
    out["srb1_retx"] = len(re.findall(r"SRB1 DL: RETX", text))
    out["pusch_ok"] = len(re.findall(r"PUSCH: rnti=.*crc=OK", text))
    out["pusch_nok"] = len(re.findall(r"PUSCH: rnti=.*crc=(?:KO|NOK|fail)", text))

    # WHO ENDED THE UE: the AMF (a command from the core) or this node (the release-complete it sends). The distinction is
    # the difference between "the core rejected us" and "our own link failed", so both sides are counted.
    out["rrc_setups"] = len(re.findall(r'RRC Setup Procedure" finished successfully', text))
    out["release_started"] = "UE Context Release Procedure\" started" in text or \
                             "\"UE Context Release Procedure\" started" in text
    out["release_by_amf"] = len(re.findall(r"Rx PDU .*UEContextReleaseCommand", text))
    out["release_complete_tx"] = len(re.findall(r"Tx PDU .*UEContextReleaseComplete", text))

    # CU-CP: an N2 connection proves the core is reachable; a timed-out NG Setup proves it was not, and the two can
    # both appear in one leg (the retry succeeded), so both are reported.
    out["n2_connected"] = find_one(r"N2: Connection to AMF on ([\d.]+:\d+) completed", text)
    out["ng_setup_timeouts"] = len(re.findall(r'"NG Setup Procedure" timed out', text))
    out["amf"] = out["n2_connected"] or "not connected"

    # ATTACH: the MAC console table prints one row per UE. A row means the phone got a C-RNTI; the uplink columns carry
    # the last interval's ok/nok counts, which is the cheapest evidence that its grants were being decoded.
    rows = re.findall(r"^\s*\d+\s+(\d+)\s*\|\s*(.*)$", text, re.MULTILINE)
    rntis = sorted({rnti for rnti, _rest in rows})
    # THE UPLINK GROUP, IN THE HEADER'S ORDER (`pusch rsrp ri mcs brate ok nok (%) bsr ta phr`), where the first column
    # is the PUSCH SINR the group is named after. Reading it as [rsrp, ri, mcs, ...] shifts every field by one and still
    # prints plausible numbers -- measured on the 2026-11-06 leg, where it turned 17.2k bit/s into "ok=4".
    last = None
    if rows:
        parts = [p for p in rows[-1][1].split("|")]
        if len(parts) >= 2:
            uplink = parts[1].split()
            if len(uplink) >= 8:
                last = {"sinr": uplink[0], "rsrp": uplink[1], "mcs": uplink[3], "brate": uplink[4],
                        "ok": uplink[5], "nok": uplink[6], "pct": uplink[7]}
    out["ue_rntis"] = rntis
    out["ue_rows"] = len(rows)
    out["ue_last"] = last

    # DATA: bytes actually delivered and decoded hops, straight from the arm's/PHY's own accounting.
    out["ul_bytes"] = find_one(r"\[ul_mac_pdu_size\] total=([\d.]+)B", text, float)
    out["ul_hops"] = find_one(r"\[ul_by_size\].*?(\d+) CRC-OK hop", text, int)

    # THE AI ARM: three independent statements, because a run can name the receiver and still have the identity arm
    # compute (no model bound), which is the substitution this workflow exists to catch.
    out["receiver_ai"] = "receiver=ai" in text
    out["receiver_classic"] = "receiver=classic" in text
    out["arm_active"] = find_one(r"AI ARM ACTIVE over (\d+) model\(s\)", text, int)
    out["buckets"] = None
    buckets = re.search(r"narrow-band buckets bound: ([\d ]+?)subcarrier", text)
    if buckets is not None:
        out["buckets"] = [int(w) for w in buckets.group(1).split()]
    out["no_buckets"] = "no narrow-band buckets bound" in text
    out["qms"] = sorted({int(q) for q in re.findall(r"computes Qm=(\d)", text)})
    out["pure"] = "PURE mode" in text
    out["startup_grace_s"] = find_one(r"startup grace = (\d+) s", text, int)
    out["degradations"] = len(re.findall(r"reception FAILED", text))
    out["slow_state"] = "SLOW STATE" in text

    # LATENCY: the two report lines the flight exists to compare, plus the per-width forward times.
    def report(name):
        match = re.search(rf"\[{name}\] samples=(\d+) mean=([\d.]+)us median=([\d.]+)us .*?p99=([\d.]+)us", text)
        if match is None:
            return None
        return {"n": int(match.group(1)), "mean": float(match.group(2)),
                "median": float(match.group(3)), "p99": float(match.group(4))}
    out["pipeline"] = report("ul_pipeline")
    out["eq_demod"] = report("ul_equalization_demod")
    # THE RICHEST LINE, NOT THE LAST ONE. `[depth3-timing]` prints once per N forwards and once more at exit; the exit
    # block can cover a single reception, so taking "the last" reports one width out of four (it did, on the 11-06 leg).
    best = {}
    for line in re.findall(r"mean predict per width: (.+)", text):
        parsed = {int(w): float(us) for w, us in re.findall(r"(\d+):([\d.]+)us", line)}
        if len(parsed) >= len(best):
            best = parsed
    out["predict_per_width"] = best
    mix = ""
    for line in re.findall(r"allocation mix \(subcarriers x receptions\): (.+)", text):
        if line.count("x") >= mix.count("x"):
            mix = line.strip()
    out["allocation_mix"] = mix or None

    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log", nargs="+", help="one or more gnb logs (console and/or the config's INFO log)")
    parser.add_argument("--json", action="store_true", help="machine-readable output")
    args = parser.parse_args()
    logs = args.log if isinstance(args.log, list) else [args.log]
    out = parse("")
    for path in logs:
        with open(path, encoding="utf-8", errors="replace") as handle:
            parsed = parse(handle.read())
        for key, value in parsed.items():
            if isinstance(value, int):
                out[key] = max(out.get(key, 0) or 0, value or 0)
            elif isinstance(value, dict):
                current = out.get(key) or {}
                # Only the count maps (RF kinds) merge numerically; the other dicts hold parsed text (the UE row) and
                # merging them with max() compares str to int.
                if all(isinstance(v, (int, float)) for v in value.values()):
                    out[key] = {**current, **{k: max(current.get(k, 0), v) for k, v in value.items()}}
                else:
                    out[key] = current or value
            elif value is not None:
                out[key] = out.get(key) or value
    # `amf` is DERIVED, so it has to be derived AFTER the merge: `parse("")` seeds it with the "not connected" default,
    # which is truthy, so the `or` above could never replace it and a connected leg printed "not connected" (it did, on
    # the 17:52 leg whose own log carries `N2: Connection to AMF on 192.168.31.211:38412 completed`).
    out["amf"] = out["n2_connected"] or "not connected"

    # A LEG WITH REAL-TIME FAILURES IS NOT A MEASUREMENT OF ANYTHING: the air interface was corrupted, so a UE that drops
    # did not reject the receiver. It fails the radio gate even if the continuity counter still reads OK.
    # RADIO IS JUDGED ON WHAT BROKE THE LINK, NOT ON A ZERO. A few `Real-time failure in RF` lines are survivable (the
    # 2026-11-28 E4 leg: 564 over 3.2 M blocks, zero RLF, 243/243 CRC-OK); 118 337 of them with every fourth PUSCH failing
    # is not. So the gate is: continuity OK AND no HARQ gave up AND no UE was released AND the failures are a rounding
    # error of the block count. Absolute zero was the first version's rule and it failed a working leg.
    blocks = (out["radio"] or {}).get("blocks", 0) or 0
    rf_rate = (out["rf_failures"] / blocks) if blocks else None
    radio_ok = (out["radio"] is not None and out["radio"]["gaps"] == 0 and out["radio"]["result"] == "OK"
                and (rf_rate is None or rf_rate < 0.001)
                and out["dl_harq_discards"] == 0 and out["release_complete_tx"] == 0)
    # DATA IS THE PROOF OF ATTACH. A leg logged at `warning` level carries no MAC UE table, so "no UE row" says nothing;
    # decoded transport blocks say everything -- a UE that was not attached cannot deliver 127 kB of them.
    # ...AND "SOME DATA" IS NOT "DATA WORKS". When the PUSCH lines are in the log (INFO level), the success rate is the
    # gate: 92 OK against 488 failures (16 %) is a dead link, not a working one.
    attempts = out["pusch_ok"] + out["pusch_nok"]
    ul_ok_rate = (out["pusch_ok"] / attempts) if attempts else None
    data_ok = ((out["ul_bytes"] or 0) > 0 or (out["ul_hops"] or 0) > 0) and (ul_ok_rate is None or ul_ok_rate >= 0.8)
    attach_ok = bool(out["ue_rntis"]) or data_ok or out["rrc_setups"] > 0
    # "AI ARM ACTIVE" is the construction-time statement that a model was bound and computes; the `receiver=` line is
    # the configuration's. A leg whose log predates that line still proves the arm computed, so the banner is enough.
    arm_ok = bool(out["arm_active"]) and not out["receiver_classic"]

    if args.json:
        print(json.dumps({**out, "verdict": {"radio": radio_ok, "attach": attach_ok, "data": data_ok,
                                             "ai_arm": arm_ok}}, indent=2))
        return 0 if (radio_ok and data_ok) else 1

    def line(label, value):
        print(f"  {label:<22s} {value}")

    print(f"=== 飞行判定：{'  '.join(logs)}")
    print("[1] 射频")
    if out["radio"] is None:
        line("sample continuity", "★ 日志里没有（腿没到退出路径？）")
    else:
        r = out["radio"]
        line("sample continuity", f"{r['gaps']} gaps / {r['blocks']} blocks -> {r['result']}")
    # `N2: Connection ...` is an INFO line; a leg logged at `warning` does not carry it even though the link is up. The
    # decoded transport blocks are the stronger evidence, so the label says which one answered (2026-11-28: a clean leg
    # printed "not connected" while delivering 108 kB).
    n2_text = out["amf"]
    if (out["n2_connected"] is None) and data_ok:
        n2_text = "未记录（INFO 关），但 UL 有数据 ⇒ 已连接"
    line("AMF / N2", f"{n2_text}；NG Setup 超时 {out['ng_setup_timeouts']} 次")

    print("[1b] 射频实时性 / HARQ（UE 掉线的第一嫌疑）")
    line("Real-time failures", f"{out['rf_failures']}（late {out['rf_kinds'].get('late', 0)} / "
                               f"underflow {out['rf_kinds'].get('underflow', 0)}）"
                               + (f"，占 {rf_rate * 100:.4f}% 的块" if rf_rate is not None else ""))
    line("DL/UL HARQ 放弃", f"{out['dl_harq_discards']} / {out['ul_harq_discards']}（4 次重传用尽）")
    line("SRB1 DL 重传", str(out["srb1_retx"]))
    line("PUSCH crc", f"OK {out['pusch_ok']} / 失败 {out['pusch_nok']}"
                      + (f" ⇒ 成功率 {ul_ok_rate * 100:.1f}%" if ul_ok_rate is not None else "（INFO 被关，未记录）"))
    if out["release_started"] or out["release_complete_tx"] or out["release_by_amf"]:
        line("UE 释放", f"AMF 命令 {out['release_by_amf']} 次；本节点 ReleaseComplete {out['release_complete_tx']} 次")

    print("[2] 接入与数据面")
    line("RRC 建立", f"{ out['rrc_setups'] } 次" if out["rrc_setups"] else "未记录（INFO 被关）")
    line("UE (C-RNTI)", ", ".join(out["ue_rntis"]) if out["ue_rntis"] else
         ("无 UE 行（日志级别 warning？），但 UL 有数据 ⇒ 视为已接入" if data_ok
          else "★ 日志里没有 UE 行，也没有 UL 数据（不能据此断定没接入）"))
    if out["ue_last"] is not None:
        u = out["ue_last"]
        line("UE 最后一行 UL", f"sinr={u['sinr']} mcs={u['mcs']} brate={u['brate']} "
                                f"ok={u['ok']} nok={u['nok']} ({u['pct']})")
    line("UL MAC PDU", "缺席" if out["ul_bytes"] is None else f"{out['ul_bytes']:.0f} B")
    line("UL CRC-OK hops", "缺席" if out["ul_hops"] is None else str(out["ul_hops"]))

    print("[3] AI 臂")
    line("receiver", "ai" if out["receiver_ai"] else
         ("classic（★ 不是 AI 腿）" if out["receiver_classic"] else "未打印"))
    line("AI ARM ACTIVE", "未见" if out["arm_active"] is None else f"over {out['arm_active']} model(s)")
    line("桶", ", ".join(str(w) for w in out["buckets"]) if out["buckets"] else
         ("★ 无桶（全带在跑）" if out["no_buckets"] else "未打印"))
    line("覆盖调制", ",".join(str(q) for q in out["qms"]) if out["qms"] else "未打印")
    line("模式", ("PURE" if out["pure"] else "fallback") +
         (f"；startup grace {out['startup_grace_s']} s" if out["startup_grace_s"] else ""))
    line("reception FAILED", str(out["degradations"]))
    line("SLOW STATE", "★ 出现（Core ML 走了慢路径）" if out["slow_state"] else "未出现")

    print("[4] 时延")
    for name, key in (("ul_pipeline", "pipeline"), ("ul_equalization_demod", "eq_demod")):
        r = out[key]
        line(name, "缺席" if r is None else
             f"n={r['n']} median={r['median']:.1f}us p99={r['p99']:.1f}us")
    line("桶分配", out["allocation_mix"] or "未打印")
    line("各宽度 predict", ", ".join(f"{w}:{us:.0f}us" for w, us in sorted(out["predict_per_width"].items()))
         or "未打印")

    print("[5] 结论")
    print(f"  {'RADIO':<8s} {'OK' if radio_ok else 'FAIL'}")
    print(f"  {'ATTACH':<8s} {'OK' if attach_ok else 'NO'}")
    print(f"  {'DATA':<8s} {'OK' if data_ok else 'NO'}")
    print(f"  {'AI ARM':<8s} {'ACTIVE' if arm_ok else '★ 未确认（看 [3] 的两行）'}")
    return 0 if (radio_ok and data_ok) else 1


if __name__ == "__main__":
    sys.exit(main())
