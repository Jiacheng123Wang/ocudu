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
