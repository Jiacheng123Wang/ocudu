# ai_train/tools — 一键启动脚本（G-5 全流程）

三个脚本覆盖"采 → 训 → 比"闭环，sudo 处会在 console 提示输入密码。

| 脚本 | 功能 | 依赖 |
|---|---|---|
| `run_capture.sh` | 起 gnb（helena + dump）→ 起 OAI UE → 自动检测接入 → ping + iperf3 → Ctrl-C 收尾 → QA 体检 + 日志存档 | 核心网（自备）、iperf3 服务端（自备，127 上 5201/5202）、153 免密 ssh |
| `run_training.sh` | 采集目录 → 配对 → 标签重建 → −13 门划分 → 微调（--norm 0.15）→ 转换 → 入库 `ai_assets/` + 打印使用方法 | `~/ai_ce_work/venv`（已验证环境）、macOS `xcrun coremlcompiler` |
| `run_compare.sh` | 新腿 gnb 日志 vs `tools/legs/` 历史腿：CRC + SINR 可比性 + 按 rnti 分账 | 无（纯分析） |

## 典型流程

```bash
# 1) 采（约 8 分钟 + 接入等待）：核心网 + iperf 服务端先备好
tools/run_capture.sh --dur 480 --tag test1
#    （脚本最后自动跑 capture_qa；按提示手动确认数据有效）

# 2) 训（离线，约 30-60 分钟）
tools/run_training.sh ~/ai_ce_work/capture/site_<日期>_test1 --name test1

# 3) 比（跑完对照腿后）
tools/run_compare.sh /tmp/gnb.log      # 与 tools/legs/ 历史腿对比
```

## 约定与注意

- **每次采集用新目录**（脚本自动生成 `site_MMDD_HHMM[_tag]`，复用目录会破坏配对）；
- 采集用 `gnb_uhd_oaiue.yaml`（3489.42 MHz）——双 UE（手机+OAI UE）唯一可接入组合；
- 采集时 CE 固定 helena（dump 钩子只在 helena 路径存在）；**生产默认 CE 是 cpu**；
- 手机手动接入步骤以注释写在 `run_capture.sh` 头部与运行提示中；
- 历史对照日志自动存 `tools/legs/`（采集脚本收尾时 `cp /tmp/gnb.log`）；
- 环境变量：`VENV_PY`（训练 python，默认 `~/ai_ce_work/venv/bin/python`）、
  `OAIUE_*` 如需换 UE 路径可直接改脚本头部常量。
