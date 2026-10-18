# 飞行速查表（现场用 · 一次一条腿 · gNB 先起）

> 完整说明见 `FLIGHT_RUNBOOK_MANUAL.md`。这一页只有命令，飞行时照着敲。

## 0. 开头（同一终端里执行一次，变量会留在这个 shell 里）

```bash
cd ~/dev/ocudu
M=$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models
W=$PWD/doc_chinese/llr_ai_detection/work_tmp
C1=$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml   # 采集小区（第一批，MCS 20）
C2=$PWD/configs/gnb_rf_b200_tdd_n78_20mhz.yml                         # 投递小区（第二批，MCS 自由）
R=doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
UE=<手机IP>          # 每批第一条腿前从 CN 机查一次；批内不变，两批之间会变
```

流量命令（**每条腿在 gNB 起来、ping 通之后**跑一次，约 130 秒）：

```bash
ssh jwang@jwang-dn "ping -i 0.2 -c 200 $UE; echo '--- iperf3 ---'; iperf3 -c $UE -R -t 120"    # 不许 -b，必须 -R
```

---

## 第一批 `r9r` — 接收机对照（采集小区 `$C1`，腿序 a c c a）

```bash
# 1/4  r9ra1  臂 a（AI 臂 + 现役产物）
sudo -E LEG_CONFIG=$C1 LEG_LOGDIR=$W/legs/r9r bash $R gpu r9ra1 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_ft.mlmodelc

# 2/4  r9rc1  臂 c（经典链）
sudo -E LEG_CONFIG=$C1 LEG_LOGDIR=$W/legs/r9r bash $R gpu r9rc1 \
  --expert_phy.pusch_receiver_backend=classic

# 3/4  r9rc2  臂 c
sudo -E LEG_CONFIG=$C1 LEG_LOGDIR=$W/legs/r9r bash $R gpu r9rc2 \
  --expert_phy.pusch_receiver_backend=classic

# 4/4  r9ra2  臂 a
sudo -E LEG_CONFIG=$C1 LEG_LOGDIR=$W/legs/r9r bash $R gpu r9ra2 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_ft.mlmodelc
```

**每条腿**：起上面那条命令 → 看到 `AI ARM ACTIVE over 4 model(s)`（臂 a）或 `receiver=classic`（臂 c）→
`ssh jwang@jwang-dn "ping -c 1 -W 2 $UE"` 通了 → 跑流量命令 → **Ctrl+C 一次**停 gNB → 等收尾报告打完 →
记下 `iperf3` 的 **receiver Mbit/s**。

---

## 4. 两批之间

等第一批最后一条腿完全停下 → 手机重连（必要时开关飞行模式）→ **从 CN 机重新查 IP**，`UE=<新IP>`。

```bash
ssh jwang@jwang-dn "ip route show dev ogstun | grep '10\.45\.0\.'"     # 或你惯用的那条
ssh jwang@jwang-dn "ping -c 1 -W 2 <新IP>"
```

---

## 第二批 `r9c` — 标定 / R6（投递小区 `$C2`，腿序 a b c d d c b a）

```bash
# 1/8  r9ca1  臂 a
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9ca1 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_ft.mlmodelc

# 2/8  r9cb1  臂 b（校准产物）—— 启动必须出现 "the published SINR is CALIBRATED by the bundle"
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cb1 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_cal.mlmodelc

# 3/8  r9cc1  臂 c
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cc1 \
  --expert_phy.pusch_receiver_backend=classic

# 4/8  r9cd1  臂 d（R6 交付形态：校准 + net）
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cd1 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_cal.mlmodelc \
  --expert_phy.pusch_reporting_source=net

# 5/8  r9cd2  臂 d
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cd2 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_cal.mlmodelc \
  --expert_phy.pusch_reporting_source=net

# 6/8  r9cc2  臂 c
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cc2 \
  --expert_phy.pusch_receiver_backend=classic

# 7/8  r9cb2  臂 b
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9cb2 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_cal.mlmodelc

# 8/8  r9ca2  臂 a
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$W/legs/r9c bash $R gpu r9ca2 \
  --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$M/d3_modcond_ft.mlmodelc
```

---

## 6. 要发给我的（每腿一行）

| 腿 | `iperf3` receiver (Mbit/s) | Retr | ping avg (ms) |
|---|---|---|---|
| r9ra1 | | | |
| r9rc1 | | | |
| r9rc2 | | | |
| r9ra2 | | | |
| r9ca1 | | | |
| r9cb1 | | | |
| r9cc1 | | | |
| r9cd1 | | | |
| r9cd2 | | | |
| r9cc2 | | | |
| r9cb2 | | | |
| r9ca2 | | | |

日志不用拷（已在 `$W/legs/r9r` 与 `$W/legs/r9c`）。

---

## 9. 现场排错（三行版）

| 症状 | 处置 |
|---|---|
| gNB 报 `REFUSING …` | 看括号里的原因：脏树 / 小区不对 / 已有 gNB 在跑 / 参数组合非法 |
| 手机两分钟不通 | 开关飞行模式；确认 IP 是**这批**的；还是不行就停腿重来（没对端＝没测） |
| 所有腿都 ≈19 Mbit/s 且几乎不动 | **你加了 `-b`**，去掉重跑那条腿 |
| 找不到臂的日志 | 读 **`.log.stderr`**，不是 `.log` |

---

## 5. 批次 `c32dmrs` — 新产物（c32，2,7,11 已学）vs `ft` vs 经典链

> 目的：验证 §19.212 达标的产物在活网上是否比现役 `ft` 和经典链更好。
> 顺序镜像 `a b c c b a`（信道漂移在两个半场各出现一次，相消）。
> 每条腿顺带录 **60 个采集**（约 8 MB），只为了事后确认这条腿跑的是 **哪种 DM-RS 图案** ——
> 因为臂的成败由图案决定（`2,11` 赢、`2,7,11` 一轮没学过就会崩，§19.211），**图案不同的腿不能相互比较**。

```bash
cd ~/dev/ocudu
W=$PWD/doc_chinese/llr_ai_detection/work_tmp
R=doc_chinese/phy_pipeline_gpu/wip/run_leg.sh
C2=$PWD/configs/gnb_rf_b200_tdd_n78_20mhz.yml          # 投递小区（MCS 自由）
NEW=$W/data/runs/d3_dmrs2711_c32/d3_dmrs2711_c32.mlmodelc
FT=$W/data/models/d3_modcond_ft.mlmodelc
B=$W/legs/c32dmrs
mkdir -p $B/pat/a1 $B/pat/b1 $B/pat/c1 $B/pat/c2 $B/pat/b2 $B/pat/a2

# 0) 预检必须 GO
LEG_LABEL=c32dmrs-preflight bash doc_chinese/llr_ai_detection/probe/preflight_check.sh delivery
```

**六条腿，逐条飞（每条：起腿 → 看到臂标记 → ping 通 → 跑流量 → Ctrl+C 一次）**

```bash
# 1/6  a1  新产物
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/a1/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-a1 --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$NEW
# 2/6  b1  现役 ft
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/b1/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-b1 --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$FT
# 3/6  c1  经典链
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/c1/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-c1 --expert_phy.pusch_receiver_backend=classic
# 4/6  c2  经典链
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/c2/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-c2 --expert_phy.pusch_receiver_backend=classic
# 5/6  b2  现役 ft
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/b2/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-b2 --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$FT
# 6/6  a2  新产物
sudo -E LEG_CONFIG=$C2 LEG_LOGDIR=$B OCUDU_UL_DUMP=$B/pat/a2/C OCUDU_UL_DUMP_COUNT=60 \
  bash $R gpu c32dmrs-a2 --expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model=$NEW
```

**流量（每条腿 ping 通之后跑一次，约 130 秒；不许 `-b`，必须 `-R`）**

```bash
ssh jwang@jwang-dn "ping -i 0.2 -c 200 $UE; echo '--- iperf3 ---'; iperf3 -c $UE -R -t 120"
```

**每条腿必须看到的（看不到就停腿，别继续）**

| 臂 | 必须出现 | 不允许出现 |
|---|---|---|
| a | `AI ARM ACTIVE over 4 model(s)` | `receiver=classic` |
| b | `AI ARM ACTIVE over 4 model(s)` | 同上 |
| c | `receiver=classic` | 任何 `AI ARM ACTIVE` |

- 任一臂若出现 `the published SINR is CALIBRATED by the bundle`：**记下来**（`ft` 和新产物都不该有）。
- 头 10 秒的 `DEGRADED` 行要少；成片出现说明臂没在算。
- 记下 `iperf3` 的 **receiver Mbit/s**，按飞行顺序贴回来。

**汇总（我来跑或你跑）**

```bash
python3 doc_chinese/llr_ai_detection/probe/arms_summary.py --batch $B \
  --rate a=<a1> --rate b=<b1> --rate c=<c1> --rate c=<c2> --rate b=<b2> --rate a=<a2>
```

它会**先从每条腿的采集里读出 DM-RS 图案**：若同一臂的两条腿图案不同（或各臂图案不齐），它会**拒绝出裁决**——
因为那时比的不是臂，而是图案。
