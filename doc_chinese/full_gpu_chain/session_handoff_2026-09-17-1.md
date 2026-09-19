# Session handoff —— 2026-09-16/17（GPU-PHY：IQ→LLR 零宿主拷贝里程碑 + RF 环境阻塞）

> 换环境回来先读这一份，再读 `s2_full_chain_design.md` 的 **§48.146(a) 状态索引**。
> 本文件的定位：**当前代码状态 / 已验证成果 / 昨晚为什么停下 / 回来第一步 / 主线待办 / 纪律与坑**。

---

## 0. 一句话现状

**代码是好的，射频环境坏了。**
IQ→demapper LLR 的"零宿主拷贝"里程碑已上机验证通过（tag `gpu_phy_iq2llr_zero_host_copy`）；
昨晚（09-16 晚）上机时链路突然劣化，经完整排除（回退到与里程碑**逐字节相同**的二进制、**纯 CPU 路径**同样症状）
确认**与代码无关**，是外部干扰/RX 被打饱和。今天收工，换环境继续。

---

## 0b. 换环境回来后的更新（2026-09-17 晚，**先读这段**）

换环境后**手机能驻留、ping 通**（§3 的 RF 阻塞解除），Ubuntu NUC 以 `ssh jwang@192.168.100.131` 免密可用。
本轮打完三个修复，全部过了全部门（详见设计文档 **§48.176**）：

| 项 | 新值 |
|---|---|
| 分支 / HEAD | **`50e989fef3`**（`cbea…` 序列：`c9dd22edf0` S-7g-14 → `dfbea28562` S-7g-15 → `50e989fef3`），已 push |
| 新增① | **S-7g-14**：Linux configure 曾**从 S-2c 起就没绿过**（`lib/phy/metal` 未 gate 的 `enable_language(OBJCXX)`）；现已 gate 在 `METAL_OFFLOADS_ENABLED` 上。**Ubuntu GCC 全量构建 `BUILD_RC=0`** |
| 新增② | **S-7g-15**：主线 §5.1 的 deferred-burst 缺陷**已修**（run 只能合并"同属一个分配"的符号）。它不是"稳定复现"，是 **24/30**（地址布局决定）；判据 `wrap replaces=3 ⇔ FAIL` |
| 新增③ | `compat::aligned_free` 现在会通知 wrap 缓存 ⇒ **映射不再比分配活得久**；离线 `zero-copy wraps` 契约由 FAILED 转 **OK**、`contract MET` |
| 参考二进制 | **必须重建**（§48.176(d)）：归档的 `a88f8f73…` **过期**，会让四网假红。已换成当天重建的 **`c1e9d824c6f294e4c59b3506ee6bcbaddf1913879ecc811b5d37fd7da4fd3f8f`**；旧的留档为 `ref/stale_2026-09-15_…` |
| 本轮门结果 | `ctest -L phy` **163/163**；四网 **`AB_ALL_RC=0`**（严格/有界×3/CPU/设备网格 全 27/27，worst=0）；六门 `ALL_GATES_RC=0`；Metal 自测 **5/5**（上轮那条 MISMATCH 消失）；两条配置构建门 exit=0；**Ubuntu 全量构建 exit=0** |

**下一步**：主线顺序仍是 §5（融合车道的前置缺陷已清掉 ⇒ 可以直接做 S-7g-13 符号对齐接收块 / 融合车道）；
上机前按 §4.2 跑一条腿，并顺带确认 S-7g-12 的仪器行（`wrap_copies=0`、`misaligned=0`、`gaps=0`）。

---

## 1. 代码状态（回来第一件事：核对这一节的哈希）

| 项 | 值 |
|---|---|
| 分支 / HEAD | `apple-silicon` / **`cbb243010b`**（已 push）|
| HEAD 的内容 | = `07ef36705e`（S-7g-12c），**`git diff 07ef36705e HEAD` 为空** |
| 里程碑 tag | **`gpu_phy_iq2llr_zero_host_copy`** → `3d026939eb`（annotated，已 push；上机验证过）|
| 树里的二进制 | `build/apps/gnb/gnb`，stamp = `cbb243010b`（`strings … \| grep -c <sha>` == 2）|
| 参考二进制（对拍用）| **`doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref`**（已从 /tmp 备份；`/tmp` 副本换环境后会消失，持久副本不会）|
| 语料 | **`doc_chinese/work_tmp/corpus/`**（27 个合成捕获的持久副本；门脚本缺省已指向它）|

**提交链（最新在前）**：

```
cbb243010b  Reapply S-7g-12c   ← 当前 HEAD（= 07ef36705e 的内容）
9224cec09b  Reapply S-7g-12b
1f6cb3034a  Reapply S-7g-12
fb140cdf64 / 4fd98ab859 / 0a50c05501   三个 Revert（怀疑期做的，已被上面的 Reapply 抵消）
07ef36705e  S-7g-12c：GPU 时间探针 opt-in；DFT 绝不越界映射/静默拷贝；连续性探针排除 ts=0
d0cdeae5ab  S-7g-12b：arm_gpu_time 必须在 commit 之前；Metal 引擎自测门
a46ecca5b4  S-7g-12：GPUStartTime/GPUEndTime 仪器 + 对齐硬化 + [ul_rx] 连续性
3d026939eb  ← 里程碑 tag（= S-7g-11 = 零拷贝验证版）
9b1d6cf6df  S-7g-10：相位判据用收到块的实际 ts  ⇒ assembled=0
bac399657c  S-7g-9 / 23e179eef6 S-7g-8 / 38a9c71b02 S-7g-7 …
```

**S-7g-12 三个提交里有什么（都确认无罪，可放心留用）**：
1. `shared_queue::arm_gpu_time()`：命令缓冲的 `GPUStartTime/GPUEndTime` 统计（**opt-in**：`OCUDU_METAL_GPU_TIME=1`；默认关，因为每条缓冲一个 completion handler 会落在实时提交路径上）；
2. DFT `wrap_buffer`：页大小改用运行期 `compat::page_size()`、**映射长度向下取整到分配内**（绝不越界映射，也绝不因为"页取整超了"而回落成拷贝）、拷贝路径在 cache 里记**真实长度**；表/预热缓冲按运行期页对齐；新增 `wrap_copies` 计数；
3. 解调器切片绑定：按内核实参元素大小校验偏移（`float2`=8/`float`=4/`char`=1），不满足则计数 `misaligned` + 告警一次 + 回落 staging；该计数进入 "zero-copy wraps" 契约判据；
4. `[ul_rx]` 接收连续性探针 + 契约 `radio sample continuity`（**已排除 UHD 停止/超时路径的 ts=0 块**，单独计 `ts0_blocks`）；
5. `wip/run_metal_engines.sh`：把 5 个"建了但没注册进 ctest"的 Metal 引擎自测纳入每腿门。

**未提交 / 不要提交**：`configs/*.yml`（你的本地改动，含 AMF 地址与增益）、`doc_chinese/`（gitignored：设计文档 + 上机日志）、`Testing/`、`*.csv`、`scripts/switch_gnb_plmn.sh`。

---

## 2. 已验证的成果（里程碑，§48.172）

**断言**：电台 IQ → demapper LLR 之间，**没有任何宿主代码路径拷贝或重读样本**。

上机证据（`gnb_s7g10_{cpugpu,cpu}.log`，两条腿，stamp `9b1d6cf6df`）：

| | 腿 A `cpu_gpu`（73 s）| 腿 B `cpu`（87 s）|
|---|---|---|
| `[ul_host]` | `symbols=977368 in_place=977368 assembled=0` | `symbols=1159662 in_place=1159662 assembled=0` |
| 契约 | `contract MET (6 of 6)`、`host sample assembly … 0 copied -> OK` | `contract MET (4 of 6)` |
| 失败率 | **0 / 73190 = 0.00000%** | **0 / 86607 = 0.00000%** |

**关闭的胶水台账**（§48.163(g)/§48.146(b)）：IQ→DFT 输入（S-7f-6f）、CFO 往返（S-7g-1）、宿主指标（S-7g-2）、
CE `pilots_power`/参考 staging（S-7g-3/4）、**逐符号装配拷贝（S-7g-7→10）**、每跳 wrap 重映射（S-7f-5x）、
LSE 回读（S-7f-5y）、完成回读（S-7f-6a）。

**原始日志与判读**：`doc_chinese/full_gpu_chain/air_logs/`（`gnb_s7g8_*`、`gnb_s7g10_*` + `s7g10_milestone_evidence.md`，含 sha256）。

### 关键机制（回来别忘）
- **接收侧**：`ul_process()` 只向电台请求"补到下一个 slot 边界"的样本（接收契约：样本数由 buffer 大小决定），
  ⇒ 块永远是整 slot ⇒ 符号永不跨块；**流启动的相位块被丢弃**（≤1 slot，UE 还不可能在发）。
- **FSM**：`symbol_start` 状态；符号样本完整落在块内就**读在原地**（切片），跨块或 CFO≠0 才装配。
- **生命周期契约（S-7g-7）**：接收缓冲活到"读过它的变换全部完成"；池子下界由流水线跨度决定。

---

## 3. 昨晚为什么停下：RF 环境（**与代码无关**，证据完整）

失败的三个二进制都做了同一件事：**手机注册成功 → 约 3 秒后被 release，反复多次**。
AMF 侧看到 UE 主动 `Deregistration request`；gnb 侧 `RLF: 100 consecutive undecoded CSIs`。

### 排除过程（关键证据表）

| 运行 | 时间 | OK/KO | OK 的 SINR 中位数 | **KO 的 SINR 中位数** |
|---|---|---|---|---|
| S-7g-10（跑通那次）| 13:21 | 866/209（80.6%）| **+28.5 dB** | **+12.9 dB**（正常边缘失败）|
| S-7g-12（我的改动）| 15:55 | 39/135 | +20.3 dB | **−21.2 dB（垃圾）** |
| S-7g-12d（**回退版 = 里程碑逐字节相同**）| 15:59 | 54/98 | +21.0 dB | **−21.1 dB（垃圾）** |
| S-7g-12d **纯 CPU 路径**（`--phy_pipeline cpu`）| 16:13 | — | — | **Msg3 全部 `crc=KO sinr=-17~-21 dB`** |

⇒ ① 回退到里程碑同样坏；② **纯 CPU 路径同样坏**（完全不经过 Metal 代码）；③ 两份日志**启动配置逐字段相同**（`dl_arfcn 430500`、`tx_gain 70/rx_gain 55`…）。
**⇒ 代码无罪，变量在射频环境。**

### 失效的"形状"指向什么
- OK 的接收仍有 **+20~28 dB** ⇒ 链路没有整体变弱；
- KO 从"＋12.9 dB 的正常边缘失败"变成"**−21 dB 的垃圾**" ⇒ **一部分时隙被打烂、一部分完好** ⇒ 突发型强干扰；
- `tx_gain=30` 那轮：**PRACH 检测到 13 次、RAR/Msg3 授权 13 次，但每次 Msg3 的 PUSCH 都解不出** ⇒
  **PRACH（相关器）能过、PUSCH（DM-RS 信道估计+均衡）全废** ⇒ 更像**强阻塞把 RX 打饱和（ADC 削顶）**，
  而不是单纯的"信号弱"。

### 还没做的诊断（换环境后按序做）
1. **扫频**：n1 DL 2150–2155 MHz（就用 8 月 26 日选频那套方法），有条件再看 **UL 子带 1962.5 MHz**（DL 2152.5 − 190 MHz）；
2. **打开 RU 指标**（现在 `metrics=0`，对削顶/峰值功率完全盲）⇒ 每符号平均/峰值功率 + **削顶样本数**；
   削顶非零 ⇒ 阻塞确诊；削顶为 0 而 SINR 仍负 ⇒ 窄带干扰正好落在 PUSCH 的 RB 上；
3. **换 ARFCN** 或直接切 `configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`（换频段，最彻底）；
4. **tx_gain 回到 70**（30 太弱，DL 走不完 RACH）；
5. 需要的话我给一个 **UHD 抓谱脚本**（失败当下抓 2 秒 RX → FFT），直接看干扰形态。

---

## 4. 换环境后的第一步（照抄即可）

### 4.1 环境体检（5 分钟）
- 扫频（如上）；检查天线/馈线接头（FDD 需要 TX/RX 与 RX2 两根分置，配置文件头部有接线说明）；
- 确认 AMF 在 **192.168.64.3**、gNB 绑定 **192.168.64.1**（`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`）。

### 4.2 先做一次"链路是否恢复"的最小验证（一条腿）
```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.phy_pipeline cpu_gpu --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto --expert_phy.device_resource_grid on \
  --log.all_level info --log.filename /tmp/gnb_<leg>_cpugpu.log
```
判据（**任一不满足就说明环境仍不可用，先解决 RF，不要动代码**）：
- 手机**能驻留**并跑通 ping/iperf3；
- PUSCH **没有负 SINR**，`crc=OK` 比例 ≥ 70%；
- 失败率 ≤ 0.0116%（口径：`Real-time failure in RF` 行数 ÷ 墙钟时隙数，×1000）；
- 契约：`host sample assembly … 0 copied -> OK`、`wrap … misaligned=0`、`dft … wrap_copies=0`、`radio sample continuity -> OK`。

### 4.3 环境正常后：确认 S-7g-12 的仪器数字（顺带完成它的上机）
重点看四行：`[metal_stats] gpu busy (front_end/back_end)`（**714 µs 暴露期里 GPU 占多少**；默认不出，需要
`OCUDU_METAL_GPU_TIME=1`）、`dft … wrap_copies=0`、`wrap … misaligned=0`、`[ul_rx] … gaps=0`（关停后 `ts0_blocks` 非零是正常的）。
失败率与 `assembled=0` 是红线。

---

## 5. 主线待办（按优先级，回来照这个顺序）

1. **修既有 deferred-burst 缺陷（离线可做，不需要电台）**
   `lib/phy/upper/channel_modulation/metal/test/demodulation_mapper_metal_unit_test` 报
   `3 staged submits in flight + single wait bit-identical: MISMATCH`
   （**里程碑 tag 上同样失败**，4/4 稳定复现；见 `wip/run_metal_engines.sh`）。
   ⇒ 这是**融合车道与 lane 并发的前置缺陷**（生产今天 `burst commits==waits, max_in_flight=1`，所以还没炸）。
2. **S-7g-13：符号对齐接收块（拿回前端与采样到达的重叠）**
   现状：为"符号永不跨块"改成整 slot 接收，代价是 `[ul_time_frequency]` 222.7 → 714.8 µs（暴露了整段前端）。
   做法：按**整数个 OFDM 符号**请求（最小 1 个符号；一个 slot 缓冲渐进填充，池与生命周期不变），**FSM 不改**。
   判据仪器：`[ul_rx] gaps` 必须 0（决定块能切多小的唯一依据）；`assembled` 必须仍为 0；
   `[ul_time_frequency]`/`[ul_pipeline]` 应回落。**注意**：`[ul_time_frequency]` 是延迟窗口不是 DFT 成本，不得跨接收策略比较。
3. **融合车道**：把 CE → EQ → demap 融合成**一条后端命令缓冲**，跨队列依赖用 **`MTLEvent`** 代替宿主等待
   （现在 `gpu_wait≈341 µs` + `defer_wait≈567 µs`/lane）。不要折叠 DFT（那会退回一个 slot 的延迟）。
4. **lane 并发**：`max_in_flight` 1 → N（依赖 §1 的既有缺陷先修 + S-7g-7 生命周期契约）。
5. 其它：把 5 个 Metal 自测**注册进 ctest**（`if(APPLE AND ENABLE_METAL_*)` + 标签 `metal`，Linux 走
   `DISABLED TRUE` 的可见分支，镜像现有 `macos_unsupported` 惯例）+ "无孤儿测试二进制"的机械校验；
   Linux/GCC 构建门仍缺机器（§48.161）；`--phy_pipeline gpu` 仍被 validator 以"融合车道未实现"拒绝——**这是设计，不是 bug**。

---

## 6. 纪律与已知的坑（血泪清单）

1. **测量不得改变被测对象**：GPU 时间探针改成 opt-in 就是因为每条命令缓冲一个 completion handler 会落在实时提交路径上。
2. **Metal 宿主契约**：`addCompletedHandler` 必须在 `commit()` **之前**（之后是断言失败，`Completed handler provided after commit call` —— 昨晚就是这么 abort 的）。
3. **GPU 的内存要求要"检查"而不是"撞上"**：基址页对齐（`baseband_gateway_buffer_dynamic_aligned`：运行期 `compat::page_size()`、页取整、不重分配）、
   切片偏移满足元素对齐（`float2`=8）；分配登记表 `compat::describe_aligned_allocation` 是权威。**页大小跨平台不同（Linux 4 KiB / Apple 16 KiB）**，任何硬编码都会被咬。
4. **绝不把"向上取整到页"当映射长度**：那会越界；而"发现超了就不映射、改回落拷贝"更糟——**对被 GPU 持续写入的缓冲（网格）就是数据损坏**。
5. **仪器语义**：`[ul_time_frequency]` 等三段是**延迟窗口**（端点随接收策略移动），不是模块成本；融合后它们没有意义，
   只能用 `GPUStartTime/GPUEndTime` 表达"样本进 GPU → LLR 出 GPU"。
6. **失败率口径**（§48.106(b)）：`Real-time failure in RF` 行数 ÷ 墙钟时隙数；预算 0.1148%。
   `[ul_pipeline] samples` 等序列**只记 CRC-OK**，且**只在退出时打印**（在 stderr，不在 `.log` 里）；"是否停摆"看 `crc=` 行的时间分布。
7. **根因必须有本地可复现证据**（昨天的教训）：我在只有假设（"网格被拷贝"）的情况下让你多跑了一轮上机；后来证明那条对拍并不能复现。**离线门复现不了的现象，先加仪器再谈结论。**
8. **每腿必过的离线门**：`ctest -L phy`；三网对拍 `run_ab_all.sh` + **设备网格网**（`ab_tol.sh … "--metal --device-grid"`，
   这条网是这次才补的：三网从来没开过 `--device-grid`，而 gNB 上机一直在用）；六门 `run_gates.sh`；
   两条配置构建门（`/tmp/build_nostats`、`/tmp/build_nometal`）；`run_metal_engines.sh`（5 个 Metal 自测，判据 `METAL_ENGINES_RC=0`，
   已知例外：上面 §5.1 那条 MISMATCH）。
9. **硬约束清单（被命名的例外，不是欠债）**：CFO≠0 ⇒ 必须装配；`--execution_profile single` ⇒ 保留包长块（会装配）；
   LLR 要交给 CPU 的 LDPC 解码器 ⇒ **每 lane 末尾一次同步不可消除**（除非 LDPC 上设备）；`gpu` 模式 = 全设备 + 融合车道，尚未实现。

---

## 7. 路径清单（回来先确认这些还在不在）

| 内容 | 路径 | 备注 |
|---|---|---|
| 设计文档（唯一活文档）| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | gitignored；**§48.146(a) 是状态索引，从这里开始读** |
| 上机日志与判读 | `doc_chinese/full_gpu_chain/air_logs/` | S-7g-8/S-7g-10 两条腿 + `s7g10_milestone_evidence.md`（含 sha256）|
| 每腿门脚本 | `doc_chinese/full_gpu_chain/wip/{run_ab_all.sh,ab_tol.sh,ab_cpu.sh,ab_strict.sh,run_gates.sh,run_metal_engines.sh}` | 全在仓库里（不是 /tmp）|
| 参考二进制 | **`doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref`（持久副本）**；原 `/tmp/ul_chain_replay_s7g5_ref` 已备份 | sha256 `a88f8f73…beebf`；重建配方见 `work_tmp/README.md` / §48.166(d) |
| 合成语料 | **`doc_chinese/work_tmp/corpus/`（27 个捕获，持久副本）**；原 `/tmp/corpus` 已备份 | 生成器在树里：`…/metal/make_synthetic_capture.py <prefix> [bwp_rb] [dmrs_symbols] [seed]` |
| RF 失败证据日志 | **`doc_chinese/work_tmp/air_logs_s7g12_rf/`**（09-16 晚三份）| §48.174 的排除证据；长期归档在 `doc_chinese/full_gpu_chain/air_logs/` |
| 上机配置 | `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`（n1 FDD 5 MHz，ARFCN 430500，tx70/rx55，AMF 192.168.64.3）| **本地已修改，不要提交** |
| 备选配置 | `configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`（n78 TDD 20 MHz）| 换频段的首选 |
| 实验台 | gNB 在 Mac（192.168.64.1）；AMF 在 UTM VM（192.168.64.3）；手机：OnePlus 8T（n1）/ iPhone 17（n78）| |

---

## 7b. `doc_chinese/work_tmp/` 约定（**立此存照**）

`/tmp` 被重启清空过一次（09-16 那次带走了 237 个捕获、参考二进制、门脚本、上机日志，导致几乎无法复跑对拍）。
`doc_chinese/` 不在 git 里，但**磁盘上持久**，所以"下一腿还要用、不能指望 /tmp"的东西一律放
**`doc_chinese/work_tmp/`**（见该目录的 `README.md`：放什么/不放什么、命名、登记表、参考二进制重建配方）。

- 已抢救：`work_tmp/ref/ul_chain_replay_s7g5_ref`、`work_tmp/corpus/`（27 捕获）、`work_tmp/air_logs_s7g12_rf/`（RF 证据）。
- **门脚本已默认优先用持久副本**：`run_gates.sh` / `ab_tol.sh` / `ab_cpu.sh` / `ab_strict.sh` 的 `CORPUS`（缺省时先看
  `work_tmp/corpus`，再回落 `/tmp/corpus`）；参考二进制仍显式传参（`bash wip/ab_strict.sh doc_chinese/work_tmp/ref/ul_chain_replay_s7g5_ref 27`）。
- **不放**：构建目录（用配方重建）、代码（进 git）、>50 MB 的单文件。
- 新条目请在 `work_tmp/README.md` 的登记表追加一行。

## 8. 回来后的第一个动作（TL;DR）

1. `git log --oneline -3` 确认 HEAD = `cbb243010b`；`cmake -P build/build_info.cmake && cmake --build build --target gnb -j 14`；核对 stamp。
2. **先体检射频**（扫频 / 接头 / 或直接换频段或换地点）。
3. 用 §4.2 的命令跑一条腿：手机能驻留 + `crc=OK ≥70%` + `assembled=0` ⇒ 环境 OK，回主线（§5 顺序）。
4. 环境仍坏 ⇒ **不要动代码**（代码已被证明无罪），继续 RF 排查（§3 的三项诊断）。
