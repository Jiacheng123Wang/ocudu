# 交接：GPU-PHY 会话状态（2026-09-13 #4，供新会话接续）

> **本文是本次会话切换的快照（只读，不回填）**。前几轮依次读
> `session_handoff_2026-09-13-1.md`（S-4b…S-5c）、`session_handoff_2026-09-13-2.md`（S-5c…S-6c-1a，
> 含用户 vision / 硬规则 / 坑表 / OTA 基线）、`session_handoff_2026-09-13-3.md`（S-6c-2/1b 的设计与落地）。
> **#3 已过期的两处**：HEAD 是 `658a7de6ef`（不是 `11f7f3dcf6`/`d417d5120f`）；
> #3 里推荐的 `| tee` 采集命令**会丢探针**（见本文 §3）。

---

## 0. 状态

- 分支 `apple-silicon`，HEAD = **`4a73905c50`**（*build: break the static-link cycle around the PUSCH debug capture*）。
  其后两笔（`54fb61e412`、`4a73905c50`）是**跨平台构建修复**（macOS 行为不变、OTA 结论不变）；
  OTA 验证过的那个状态是 `658a7de6ef`。
- **TAG `gpu_phy_pre_ldpc`**（annotated，**已推送**）指向 `658a7de6ef` ⇒ "LDPC 之前的 GPU pipeline 完成 + OTA 验证"
  的基线锚点（tag message 含当时的证据与运行命令）。其后的两笔只是跨平台构建修复，**未移动该 tag**
  （需要"双系统全绿"锚点可另打一个）。
- 工作树干净（`lib/`+`include/`+`tests/` 零改动），**全部已推送**。
- 门禁：`ctest -L phy` **161/161**、十个 metal 单测二进制、CE 单测的 Test 9/11/12 全绿。
- 自 #3 以来新增两笔：`d417d5120f`（探针记账修正：`defer_wait` 单独报告，不再并入 `gpu_path`/`total`）、
  `658a7de6ef`（demodulator 打印"估计是就地读还是主机读"的一次性 info 日志）。

## 1. 本轮 OTA 验证结论（S-6c-1b 成立）

固定 MCS（`min_ue_mcs == max_ue_mcs == 10`）配对 A/B，**设备栈 vs host 栈（`OCUDU_CE_CPU_CE=1`）**：

| 指标 | 设备栈 | host 栈 |
|---|---|---|
| `ul_pipeline` median | 4430.0 µs | 4336.0 µs |
| **CE 段 median** | **341.3** | **293.4** |
| eq+demod median | 288.6 | 299.4 |
| TF / FAPI / LDPC median | 231.6 / 4.1 / 3552.0 | 231.8 / 4.1 / 3540.0 |
| `[mmse_time_sum] mean total` | 334.8（GPU 只忙 130.9） | 284.1（GPU 103.2） |
| `defer_wait` | 13.4 | 0.2 |

- **2c 生效的证据**：`defer_wait` 单独可测；`ul_channel_estimation min` 从 224 µs 塌到 **20.3 µs**
  （估计的算力已移出该窗口）。
- **但设备栈仍比 host 栈慢 ~50 µs/跳**，差异**全在 CE 段**（GPU 多忙 28 µs = K3/K4 两个 kernel，
  其余是 K3/K4 staging 与推迟收尾）⇒ 2c 的重叠（上限 = `defer_wait`）不足以抵消。
- **CE 是 CPU-bound**：设备栈每跳 334.8 µs 里只有 130.9 是 GPU 忙时，其余 ~204 µs 是 CPU
  （把 ~170 KB 的 A/R_hp/y/导频 memcpy 进引擎槽位 + 编码 + 收尾）⇒ CE 的下一个杠杆是**减少主机 staging**。
- LDPC 占 `ul_pipeline` 的 **80%**（3552/4430）——数量级仍在它那里。

## 2. 文档地图（新会话按需读，不必全读）

| 要什么 | 读哪 |
|---|---|
| **现状架构：每个边界的往返/同步清单**（唯一权威描述） | 活文档 `s2_full_chain_design.md` **§45** |
| 下一步的**融合规划**（`--phy_pipeline gpu`，IQ→LLR） | 路线图 `full_chain_gpu_uma_zero_copy_refactor_plan.md` **§10** |
| 用户已定的决策 | 路线图 **§10.9**（多 CB+一次等待 ✓、PUCCH/PRACH 留 CPU ✓、跳频/多端口见 §10.6 R4） |
| 同步模型（无全局栅栏 ⇒ 依赖推到 dispatch/CB 边界）+ 7 条新条目 | 路线图 **§10.10** |
| **终局约束：MAC PDU 才出 GPU，LLR 出口是临时的；要留的 8 个门** | 路线图 **§10.11** |
| OTA 验证与探针语义变化 | 活文档 **§44**（含 `mean total` 记账口径的变化） |
| S-6c 系列落地细节 | 活文档 **§38–§43** |
| 工作方法 / 硬规则 / 坑表 / gate 命令 / 本地文件清单 | memo **#3**（§2/§5） |

## 3. ⚠️ OTA 采集：**不要用 `| tee`**（本轮踩到，会白跑一轮）

用户实测：`sudo ... 2>&1 | tee /tmp/x.out` 时，**Ctrl-C 退出时打印的探针既没进 tee 的文件、也没进 console**
（退出序列与管道竞争）。⇒ 用**直接重定向**采集：

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml expert_phy \
  --pusch_channel_estimator_algo metal_mmse --pusch_ldpc_decoder_type metal \
  --pusch_dft_type metal --pusch_channel_equalizer_backend metal > /tmp/gnb_ota.out 2>&1
# 跑够流量后 Ctrl-C；探针在 /tmp/gnb_ota.out 里
```

其他 OTA 硬规则（本轮确认）：
1. **必须 `sudo`**：usrsctp 需要 root 才能收发 raw socket，非 root 起得来但 `NG Setup` **必然超时**
   （agent 曾误判为"核心没运行"，并用 TCP 探 SCTP 端口当证据——**不成立**）。
2. **手机初始 PRACH 功率每次不同是正常的**（UE 按 DL power 从最小可行功率逐步抬升）⇒
   **PRACH RSSI 不能当链路质量判据**。
3. **`all_level: info` 下日志没有 UL CRC / PUCCH 行**（脚本自己也注明）⇒ "PUSCH results: 0 / UL CRC ok=0"
   **不等于**失败；判据是**探针**（`[ul_pipeline] samples`、`[ul_ldpc_decode]`、HARQ discards）。
4. 跑 OTA 时**不要编译/跑测试**（并发会把尾部 max 拉到数 ms）；agent 需完全待机。
5. 想做可信 A/B ⇒ 固定 MCS（`min_ue_mcs == max_ue_mcs`），并先比**负载指纹**（hops/授权、`dft commits`、
   调制分布、`ldpc_decode` mean）。

## 3.5 ⚠️ 跨平台硬规则：**Ubuntu 必须保持可编可测**（本轮踩到，已修）

OCUDU 原本是 Linux/Ubuntu 项目（macOS 是后加的 port）。GPU path 的改动默认只针对 Apple Silicon，
但**不能把 Ubuntu 弄坏**。远程机器与命令：

```bash
ssh jwang@192.168.31.211          # 免密
cd ~/work/ocudu                   # 与 macOS 同一分支
git pull --ff-only
cmake -S . -B build && cmake --build build -j16 -- -k      # 注意：-k 要放在 -- 之后（cmake 3.28 不认它）
cmake --build build --target test                            # = ctest，全套 7614 个
```
**Ubuntu 上的基线（`4a73905c50` 实测）**：构建 exit=0；`--target test` = **7614/7614 通过**（347 s）。

平台隔离的三条约定（照抄仓库既有做法）：
1. **宏**：metal 相关代码用 `#if defined(OCUDU_METAL_EQUALIZER / OCUDU_METAL_DEMODULATION / OCUDU_METAL_DFT /
   OCUDU_METAL_CHEST / OCUDU_METAL_LDPC)`；Linux 上这些选项全 OFF、对应 target **不存在**。
2. **CMake**：引用 metal target 必须放进 `if(ENABLE_METAL_*)` 块里（测试也一样）。
3. **不要跨库调用**：静态库之间**不能成环**（见 §7.6）。

本轮修掉的三处（都是这部分工作的欠账）：
- `ul_capture.cpp` 用 `%u` 打印 `rnti_t` ⇒ **GCC 的 `-Werror=format=` 报错、clang 不报** ⇒ 改 `to_value(...)`（§7.4）；
- `pusch_demodulator_deferred_chain_test` 无条件链接 metal target、包含 metal 头 ⇒ 已条件化 +
  用 `OCUDU_HAS_METAL_PUSCH_CHAIN` 隔离三个 metal 测试（§7.5）；
- `ul_capture.cpp` 与 demodulator 库**互相依赖** ⇒ 把它移进 demodulator 库（§7.6）。

## 4. 决策已清空（无阻塞问题）

- ✅ **多 CB + 一次等待**（不合并编码器）——Metal 上 CB 边界就是屏障，见路线图 §10.10。
- ✅ **PUCCH/PRACH 留 CPU**；PUCCH **共享 FFT 输出**（按需主机镜像，≈16.8 KB/端口/slot，不另算 FFT）。
- ✅ **跳频**：全库核查 ⇒ 没有任何地方设置 `hopping_symbol_index`、调度器硬编码 `no_hopping`、
  PUSCH demodulator 无 hopping 概念 ⇒ **死代码**，只需 assert，**不是"拒绝"**。
- ✅ **多端口**：用 `(slot,port,hop,symbol)` **arena** 解决（K3 写对应区域，均衡器绑一个 buffer 按 port 索引），
  **不拒绝**；v1 先跑 1 port/1 hop，但接口/布局按多维设计。
- ✅ **终局**：MAC PDU 才出 GPU ⇒ 现在就要留 D1–D8（§10.11）。

## 5. 下一步（唯一）

**S-7a：CLI + 探针重构**（路线图 §10.1/§10.7/§10.8）：
1. `expert_phy --phy_pipeline {cpu, cpu_gpu, gpu}`（`cpu` 为默认；`gpu` 暂时返回"未实现"明确报错）；
2. 启动时打印**有效配置**（模式 + 每模块后端）；
3. 探针按模式切换：`gpu` 档用"进 GPU→出 GPU"（residency/busy/gap/period + `[ul_llr_ready]`），
   `cpu`/`cpu_gpu` 档保留现有分段。

**门禁**：现有 161/161 + 十个 metal 二进制；**OTA 一轮数字与 tag `gpu_phy_pre_ldpc` 逐项一致**（纯管道，零行为变化）。
回退：不需要（没动数据流）。

之后按 §10.8：S-7b（FFT 直写设备网格）→ S-7c（K0-a 导频提取）→ S-7d（K0-b/c/d）→ S-7e（均衡 gather + LLR 设备化）
→ S-7f（ring + push/collect + 打开 `gpu` 档）→ S-7g（可选：CE 并入共享 burst）。

## 6. LDPC（之后的下一块，用户已定要先做融合）

进入 LDPC 的第一个动作是**测量**：它占 pipeline 80%、逐码块 `commit+wait`（`max_in_flight=1`）
⇒ 用 §10.7 的 **busy/gap** 分解回答"3540 µs 里 GPU 真忙多少、等 CPU 多少"，据此决定改 kernel 还是改架构。
注意 srsRAN 的 LDPC 代码块循环里有大量**小数据控制逻辑**（CB 分段、CB CRC、TB 拼接、FAPI 打包）
⇒ 终局大多是"控制留 CPU、LLR 与译码留设备"（§10.11.3）。

## 7. 本轮新增的坑（前几轮的坑见 memo #3 §5）

1. **同一跳内的多批次会覆盖共享暂存**：`gpu_h` 被所有批次共用 ⇒ 后续批次必须先收尾前一个批次
   （`run_engine_blocks` 开头 + `defer_unpack` 立即置 `stage_pending`）。**Test 11 用 15 dB 的差异抓到过**。
2. **"逐位不变"的门禁必须比对数字，不能只看 PASS/FAIL**：`filtered_pilots_lse` 的 12 子载波窗口偏移
   丢过一次，只有 Test 9 的**漂移数字**露馅（1.236 vs 1.039）。
3. **探针口径会骗人**：推迟之后 `ul_channel_estimation` 的 min 变成 20 µs 是**真实现象**（估计工作移出该窗口），
   不是变快；`mean total` 一度因记账污染从 329.6 变 416.6（已在 `d417d5120f` 修正）。
4. **Python `str.replace` 手术危险**：本会话两次误伤（`s_ptr`→不存在的 `s_binding`；页对齐改动没匹配上却以为改了）。
   改完必须 grep 复核。
4. **GCC 比 clang 严的地方**：`-Werror=format=`（强类型传给 `%u` 之类）在 clang 上不报 ⇒
   在 macOS 上"编过了"不代表 Ubuntu 能过。**每次提交前都应在 Ubuntu 上跑一遍**（§3.5 的命令）。
5. **静态库成环只在 Linux 上暴露**：两个 `.a` 互相调用时，符号能否解析取决于命令行顺序 ⇒
   macOS/`-dynamiclib` 宽松、Linux 严格。判据：**不要跨库调用**（要么合并、要么抽出第三个库）。
6. **不要动用户的本地文件**：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`（`pusch.max_ue_mcs: 20` +
   `min_ue_mcs`/`pdsch` 注释）、`configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`、`configs/gnb_zmq.yaml`、
   `scripts/switch_gnb_plmn.sh`、`bler_metal_bg1_z64_*.csv`、`configs/*.swp`。用户的 OTA 日志在 `/tmp/gnb.log`
   （另存了 `/tmp/gnb_prev_e086ff00f8.log` 作参考）。
