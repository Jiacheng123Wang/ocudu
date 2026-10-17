# 会话交接 · 2026-10-17-1（深度-3 接入 PHY 并空口跑通 + R6 + 延迟优化第 1 步）

> **给下一位（或 compaction 后的我）**：先读本文件的 §1（一句话状态）、§7（下一步的精确命令）、§9（陷阱）。
> 其余按需跳读。**所有结论都指到证据所在**，不要凭记忆复述数字。

---

## 1. 一句话状态

★★ **里程碑 M-3.0 已达成**（高层文档 §8）：一份**调制条件化**的深度-3 模型（替代四套按调制模型）已接入
PHY 流水线、在 **N78/20 MHz/TDD** 上**空口跑通**，手机附着 + `ping`/`iperf3` 流量全部跑通；
精度判据与工程件**达标**；但 **R6 的 `net` 上报源实测拖慢吞吐 12.6% ⇒ 不采纳，默认仍是 `classic`**；
`deliver()` 优化做了第 1 步（`pow`→`exp2`，正确性已验、**加速未实测**）；
**放置（ANE vs GPU）从未测过**，这是下一个最重要的未知。

- 目标：`goal-8a9f10af-3c2f-4cc4-a1b6-9acd3f2c7197`，**状态 blocked**（自主运行已结束）。
  阻塞原因是"效用判据需要对端 UE"—— ★ 该阻塞**已被四次飞行解除**（§19.158/19.161 已出结论），
  ★ 但 goal 仍停在 blocked ⇒ **恢复工作前先 `update_goal action=resume`**（或直接按本文件继续，不必动 goal）。
- 工作树干净 ✓；本会话 34 次提交，最后一条 `29f28beebc`。

---

## 2. 入口与目录

| 东西 | 位置 |
|---|---|
| **高层状态/里程碑** | `doc_chinese/llr_ai_detection/llr_ai_detection_high_level_status_and_plan.md` §8（M-3.0） |
| **设计/实施记录（活文档）** | `doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md` §19.112–§19.167 |
| **训练好的模型（已入库 ✓）** | `lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets/depth3_modcond_ft/`（1.2 MB：`.mlmodelc` + `.mlpackage` + `.keras` + `history.json` + `README.md`） |
| **构建默认模型** | `OCUDU_DEPTH3_DEFAULT_MODEL` → 上面那个 `.mlmodelc`（`apps/units/flexible_o_du/o_du_low/CMakeLists.txt`） |
| **旋钮/环境变量清单** | `doc_chinese/ocudu_env_knobs_inventory_and_leg_whitelist.md` 末尾"深度-3 AI 接收机"一节 |
| **训练管线** | `lib/phy/upper/signal_processors/channel_estimator/metal/ai_train/`（`train_depth3.py` / `depth3.py` / `depth3_loss.py` / `merge_depth3_datasets.py` / `eval_modcond.py` / `check_reporting_heads.py`（★ 本轮从 gitignore 的 work_tmp 移入 ✓）） |
| **工作区（gitignore）** | `doc_chinese/llr_ai_detection/work_tmp/`：`data/models/`（多份 bundle）、`data/one_sample/`（同口径真值 npz）、`probe/`（两个 R6 探针 py）、`tmp/evidence/`（39 个证据文件）、`legs/r6ab/` `legs/r7abba/` `legs/r8abba/`（三次飞行的 gNB 日志） |
| **数据集（未入库 ✗ 太大）** | `work_tmp/data/datasets/d3_modcond_{train,val}.npz`（688 MB / 69 MB）；重建见模型 README |

---

## 3. 本会话改动的代码（都已提交）

**契约与臂**（`lib/phy/upper/channel_processors/pusch/`）
- `pusch_depth3_receiver_impl.cpp/.h`：条件化契约（平面宽度、每调制 LLR stride、per-Qm 引擎选择、混契约拒绝）、
  定义域门（`measurement_outside_domain`，域外**整次回落**）、`net` 下 RSRP/EPRE 发布、运行时一致计数、
  `deliver()` 的 `pow→exp2`、几何拒绝、`bound_nsc()`。
- `pusch_depth3_measurements.{h,cpp}`：残差法噪声估计 + 平面自证 SNR（二阶差分、平面单位同口径）、
  发布 RSRP 补上 DM-RS/数据比（修掉 3 dB）。
- `pusch_depth3_input_plane.h`：`plane_channels` 成员 / `set_plane_channels()`（条件化 16 通道）。
- `pusch_processor_notifier_adaptor.h`：**锁存**解调器自测量并**重新写进结果拷贝**（修 `net` 发布被延迟估计器覆盖）。
- `processor_factories.cpp`：覆盖横幅（`computes/degrades/contract/geometry/reporting heads trained on`）。
- `pusch_demodulator_notifier.h`：`demodulation_stats` 增 `rsrp_dB`/`epre_dB`。

**引擎**（`lib/phy/upper/signal_processors/channel_estimator/metal/`）
- `ocudu_coreml_nn_engine.{h,mm}`：读 `depth3_layout` / `depth3_qm_set` / `depth3_head_stride` /
  `depth3_snr_range` / **`depth3_nsc`**（★ 新 ✓，并与图交叉核对）；输入数组宽度改用**产物声明**；
  条件化守卫（照声明服务全部调制）；失败路径**具名报错**（不再静默 `return false`）。

**配置接线**（`apps/units/flexible_o_du/o_du_low/` + `include/`）
- `--expert_phy.pusch_reporting_source=classic|net`（默认 `classic`；`net` 需 `backend=ai`，validator 拒绝其余组合）。
- 默认模型路径改为**入库产物**；`upper_phy_factories.*` / `factories.h` 传递新字段。

**工具**
- `ai_train/train_depth3.py`：`--snr-range`、`depth3_snr_range` 与 `depth3_nsc` 写入（★ 从导出的形状 ✓）、
  `--init` 微调；`check_reporting_heads.py`：`--cond` / `--per-modulation`（并移入版本库）。

**验证脚本（work_tmp，gitignore）**：`probe/compare_ai_arm.py`、`probe/criterion1.py`、
`tmp/template_conditioned.sh`、`tmp/finetune_conditioned.sh`、`tmp/fly_r6_abba.md`、`tmp/fly_r6_ab.sh`、
`tmp/verify_deployed.sh`、`tmp/preview_rest.sh`。

---

## 4. 关键数字（每条都能在设计文档找到）

| 项 | 数 | 出处 |
|---|---|---|
| **采纳门**（末检查点，四调制 × 300 捕获） | 与各专用模型**判决级 1200/1200 相同**，0 退化；256QAM criterion 1 **MET**（−58.33 vs −58.67 pp） | §19.148 |
| **部署产物**（微调检查点）复验 | 仍 **1200/1200 完全相同**，0 退化 | §19.154 |
| head 3（信道估计） | NMSE **−25~−27 dB**、功率比 **±0.1 dB** | §19.114/19.120 |
| head 4（噪声）微调后 | 12~30 dB 误差 **+0.26~+1.22 dB**；跨度 **0.213 → 1.070**（5×）| §19.149 |
| 指定 SNR 域行为 | 域外**整次回落经典**，公布值与经典**逐位相同** | §19.115/19.117 |
| RSRP 修复 | 修前低 3.11/2.96 dB → 修后 **0.02~0.11 dB** | §19.122 |
| 独立尺子（平面自证 SNR） | 对指定真值 30 dB 内跟踪 28 dB，偏差 **+2.6~+3.3 dB（常数）**；对经典 **−2.0~+0.5 dB** | §19.138/19.146 |
| 时延（臂，实测） | 平面 ~27 µs + **forward p50 1 004–1 041 µs** + **deliver p50 474–603 µs** ≈ **1.5 ms** | §19.164/19.165 |
| `deliver()` 成本拆分 | pow **0.29 ms** + 量化 0.12 + LLR 读 0.05 + 解扰 0.03 ≈ 0.47–0.52（与实测吻合）| §19.165 |
| **活网 `net` vs `classic`** | 公布 SINR 低 **−2.5 dB** ⇒ 上行吞吐 **−12.6%**（ABBA、去限速、条件匹配：A 11.2 vs B 9.79 Mbit/s） | §19.161 |
| 飞行 | 4 批共 15 条腿；`ping` 0% 丢包；上行 10–19 Mbit/s | §19.157–19.161 |

---

## 5. 当前限制（★ 全部有据，不许淡化）

1. **`net` 不采纳** ✗：活网公布 SINR 低 2.5 dB ⇒ 吞吐 −12.6% ⇒ 默认 **`classic`**。
2. **经典估计器仍在跑** ✗：它供给链条测量与 `est_results`（定义域门、并排比较都用）。去掉它 = §19.123 第 4 步，
   前提是先有**独立**且**标定过**的尺子（残差法已接近，但活网口径未标定）。
3. **几何固定 612 子载波**（51 PRB）：产物声明 `depth3_nsc=612`；N1/5 MHz（300 子载波）**在绑定期被拒绝**。
4. **定义域 12~48 dB**：域外整次回落经典并计数；10 dB 以下 head 4 明显不准（+15.4/+6.7 dB）⇒ 下限不要再降。
5. **仪器缺口**：① 活网一致计数恒 0/0（抽样时经典 `epre/rsrp` 尚不可用，延迟估计器）—— **待修**；
   ② 引擎诊断走 PHY logger，回放的 stderr 里看不到；③ 运行器里 "did not load, so it declares no modulation"
   这句措辞是**猜测**且未定位到源码（grep 无匹配）。
6. **仅 macOS/Apple Silicon**：引擎是 Objective-C++ + CoreML，Linux 无此路径。
7. **`deliver()` 的 `pow→exp2` 加速未实测** ✗（原因见 §7 第 1 项）。

---

## 6. 仍然开着的 flag（高层文档口径）

- **S0′**（ANE 地板 `t(W)`）与 **S1**（三引擎量测台）：文档写着"已预登记、未执行" ⇒ **未做** ✗。
- **depth-3 的实际放置（ANE / GPU / CPU）**：**从未测过** ✗ —— 代码只设了 `MLComputeUnitsAll`
  （`ocudu_coreml_nn_engine.mm:387/439`，注释"ANE > GPU > CPU"），**没有读回放置**。
- **延迟目标**：与 fused lane（600–700 µs）同级未达 ✗（当前网络 ~1.0 ms）。
- `net` 采纳 ✗、经典估计器未去 ✗、N1 几何 ✗、三处仪器缺口 ✗。
- **已达成**：P0 结案 ✓、S-1 ✓、M-3.0（接链 + 空口）✓、条件化合并采纳 ✓、上报头精度 ✓。

---

## 7. 下一步（按优先级；命令可直接用）

### ① 测量 `pow→exp2` 的实际加速（纯测量，不改行为）
`[depth3-timing]` 环形缓冲在**进程退出**时打印，而回放工具**每进程只回放一条捕获** ⇒ 单条只有 1 个冷样本。
需要**一个进程回放多帧**（`ul_chain_replay` 的 `--harq-chain` 或同进程多捕获），再对**同一批捕获**做
"改前/改后"两次构建的 A/B：
```bash
cd ~/dev/ocudu
# 改后（当前树）
cmake --build build --target ul_chain_replay -j 6
# 取 N 条捕获的时间线（grep [depth3-timing]），再 git stash 掉 exp2 那处、重建、再测一遍，最后恢复
```
**判据**：`deliver` p50 应落在 **0.2 ms 左右**（pow 那 0.29 ms 消失）；若没有明显下降 ⇒ 说明我对成本的归因错了，
**先查清楚再动下一处**（不要叠加改动）。

### ② `deliver()` 第 2 步：量化 SIMD（仅在 ① 成立后）
要求与 `log_likelihood_ratio::quantize` **逐步等价**（同裁剪同舍入）⇒ 仍用四列不变量验收：
```bash
cd doc_chinese/llr_ai_detection/work_tmp
nice -n 15 ~/ai_ce_work/venv/bin/python probe/compare_ai_arm.py \
  --tool ~/dev/ocudu/build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
  --corpus corpus/aillr_256qam_ai_s1 --model probe/d3_256qam.mlmodelc \
  --list tmp/stride_baseline_list.txt --out tmp/x.csv --work tmp/wx --clean
# 与 tmp/stride_baseline.csv 比 crc/exact_pct/sign_agreement/mean_abs_llr_ai 四列必须全等
```

### ③ 测 depth-3 的**实际放置**（★ 最重要的未知，零训练）
同一份 bundle，三档 `computeUnits`（`All` / `CPUAndGPU` / `CPUOnly`）计时对比（需要一个进程多帧，见 ①）。
- 若 `All` **不显著快于** `CPUAndGPU` ⇒ 没吃到 ANE ⇒ 查 ANE 编译器为何拒图（通道数/padding/精度/算子）；
- 若 `All` 明显更快 ⇒ 已在 ANE ⇒ 直接压网络本身（§19.164 第 3 项）。
★ 这一步同时把高层文档里"已预登记、未执行"的 **S0′** flag 落掉。

### ④ 修仪器（小改，不需手机）
把估计器通知器交付的值**锁存**（与 `pusch_processor_notifier_adaptor.h` 里已有的锁存同一手法），
让 `[depth3-measure]` 的经典列与"一致计数"从锁存读 ⇒ 下次飞行自动报出 `net − classic` 偏差。

### ⑤ 标定 head 4 的绝对电平（在 ④ 之后）
用经典估值（已验到 0.4 dB）做参考，取实飞样本偏差分布给 noise 头加**常数校正**（本轮 −2.5 dB ⇒ 因子 ≈1.78），
或按 16~18 dB 工作点分层微调。**校正前后都打印**，便于下次飞行判对错。

### ⑥ 重飞 ABBA（需要手机）
判据已写死在 §19.159：A/B/B/A、去限速、同位置；相邻腿经典 SNR 差 ≤1.5 dB 才可比；
**B 两腿平均吞吐 ≥ A 两腿平均** 才谈采纳 `net`。
```bash
sudo -E LEG_CONFIG=$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml \
        LEG_LOGDIR=$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/<新批次> \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <腿名> \
    --expert_phy.pusch_receiver_backend=ai \
    --expert_phy.pusch_receiver_model=$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc \
    [--expert_phy.pusch_reporting_source=net]
```
流量（**四腿逐字相同**）：`ping -i 0.2 -c 200 <手机IP>` 然后 `iperf3 -c <手机IP> -R -t 120`（**不要 `-b`**，
否则测的是我们给的上限而非链路容量）。每腿一次 Ctrl+C 并等报告打完。

### ⑦（可选，最大性能项）去掉经典估计器
前提 ③④⑤ 完成（要先有独立且标定过的尺子）。设计 §19.98/§19.123。

---

## 8. 环境与操作

- **工作目录**：`~/dev/ocudu`（`apple-silicon` 分支）；构建目录 `build/`（增量构建 ✓）。
- **构建 gnb**：`cmake --build build --target gnb -j 6`；**回放工具**：`--target ul_chain_replay`。
- **必须**：`-DENABLE_METAL_CHEST=ON`（⇒ `OCUDU_DEPTH3_AI_AVAILABLE=1`）。
- ★ `OCUDU_DEPTH3_DEFAULT_MODEL` 是 **CACHE 变量**：改了源码默认**不会**自动进旧构建树 ⇒ 需要
  `cmake -B build -DOCUDU_DEPTH3_DEFAULT_MODEL=<path>`。
- **飞行**：见 §7 ⑥；日志在 `LEG_LOGDIR`；臂的消息在 **`*.log.stderr`**（`*.log` 是 ocudulog，200+ MB，
  里面没有臂的行 ✗ —— 这是本会话踩过的坑）。
- **CN 机**：`jwang@jwang-dn`（跑 ping/iperf3 的那台；每次 gNB 重启后 UE 拿到**新 IP**：`.31/.32/...`）。
  该机的 `/tmp` 与 macOS 的 `/tmp` 是**两台机器**；iperf3 输出请存家目录，或直接把 console 摘要贴出来。
- **复验用的三条 pin 捕获**：`work_tmp/tmp/stride_baseline_list.txt`；基线 CSV：`tmp/stride_baseline.csv`。

---

## 9. 陷阱清单（每条都真踩过）

1. **`run_leg.sh` 默认小区是 N1 / 5 MHz / FDD**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`）⇒ 必须显式
   `LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml`（丢过一条腿）。
2. **臂的日志在 `.log.stderr`，不在 `.log`**（`.log` 是 ocudulog 的 200+ MB）；计数为 0 时先查自己读错了文件。
3. **`std::move(engine)` 之后再读 `engine->...`** ⇒ 空指针崩溃（§19.118）。声明要在 move **之前**读。
4. **平面的"12 通道常量"** 有五个消费点（引擎输入宽度、臂 LLR stride、引擎逐调制守卫、测量块步长、
   测量尺寸守卫）—— 换契约时**每一个**都要跟着宽度走，漏一个就静默错。
5. **`/tmp` 会被清**（macOS 重启）⇒ 产物与证据已移进 `work_tmp` 并入库（§19.155/19.162）。新东西别再放 `/tmp`。
6. **改正在运行的 bash 脚本不安全**（bash 按偏移续读）⇒ 改完要重启该脚本。
7. **脚本里的 `$PY` 没有 `-u`** ⇒ epoch 行被缓冲，看不到实时进度（不影响正确性）。
8. **`criterion1.py` 的 NOT MET 在零事件样本上是样本量的话**（+2.5 pp @150）⇒ 不是网络不行；600 条才勉强够。
9. **`--count N` 不保证选到同一批捕获** ⇒ 比较必须用 `--list`（pin 住）。
10. **两边都错的"证据"**：比较必须用**不依赖上报源**的量（经典 SNR、RTT、iperf3 吞吐）—— 网络侧 SNR 在 `net`
    腿里本来就是被换掉的数，拿它判可比性是自证。
11. **`tee`/重定向在 CN 机上没落盘**（跨机器/会话）⇒ 直接贴 console 即可，我这边会把数字落盘入库。
12. **`git commit -m` 里带引号/反引号会破坏 heredoc** ⇒ 用 `-F /tmp/msg.txt`（写文件再提交）。
13. **`work_tmp` 被 gitignore** ⇒ 需要长存的东西（模型/文档/工具）要放进**跟踪目录**（模型在 `metal/ai_assets/`）。

---

## 10. 诚实边界（哪些结论有实测支撑，哪些只是代码审读）

- **有实测**：采纳门 1200/1200；部署产物复验；四列不变量（多次）；RSRP 修复；定义域门行为；
  四次飞行的吞吐/丢包/SNR；`deliver` 与 `forward` 的 p50；`pow→exp2` 后**正确性**（四列不变）。
- **仅代码审读**：臂侧"几何声明 vs 612"的拒绝（负对照走的是引擎的"声明 vs 图"检查，因为造不出一个
  **图本身 300 宽**的产物 ✗）；`ENABLE_METAL_CHEST=OFF` 时的行为；`submit_fused` 写 int8 LLR 与主机侧跳过
  量化循环的**时序开销**（只读了代码，没量）。
- **未测**：`pow→exp2` 的加速倍数；depth-3 的实际放置；N1 上的任何行为。

---

## 11. 快速自检（接手后 2 分钟）

```bash
cd ~/dev/ocudu && git status --porcelain | wc -l          # 应为 0
git log --oneline -3                                       # 最后应为 29f28beebc 或其后
ls lib/phy/upper/signal_processors/channel_estimator/metal/ai_assets/depth3_modcond_ft/   # 5 个条目
strings build/apps/gnb/gnb 2>/dev/null | grep -c ai_assets/depth3_modcond_ft               # 应为 1
tail -60 doc_chinese/llr_ai_detection/llr_ai_detection_design_and_implementation.md | head -20   # §19.16x 在末尾
```
