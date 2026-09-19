# 交接：GPU-PHY 会话状态（2026-09-13 #5，供新会话接续）

> **本文是本次会话切换的快照（只读，不回填）**。前几轮依次读
> `session_handoff_2026-09-13-1.md`（S-4b…S-5c）、`session_handoff_2026-09-13-2.md`（S-5c…S-6c-1a，
> 含用户 vision / 硬规则 / 坑表 / OTA 基线）、`session_handoff_2026-09-13-3.md`（S-6c-2/1b 的设计与落地）、
> `session_handoff_2026-09-13-4.md`（S-6c-1b 的 OTA 验证 + 跨平台修复 + **OTA/跨平台硬规则**）。
> **#4 里唯一过期的**：HEAD 不是 `4a73905c50`，tag `gpu_phy_pre_ldpc` 已**移到** `4a73905c50`（见 §0）。

---

## 0. 状态

- 分支 `apple-silicon`，HEAD = **`079b114894`**（*build: keep the pipeline-mode test free of range-loop
  temporaries*），其后无未推送提交（`757593c84a` S-7a → `079b114894` 构建修复，均已推送）。
- **TAG `gpu_phy_pre_ldpc`**：annotated，指向 **`4a73905c50`**（"LDPC 之前的 GPU pipeline 完成 + OTA 验证"
  的锚点，macOS/Ubuntu 双平台都验证过）。S-7a 的两笔在其**之后**，tag 未移动。
- 工作树干净（**只有用户自己的 `configs/*` 改动**：`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、
  `gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`、`gnb_zmq.yaml`；未提交、不要动）。
- 门禁（本会话实测）：
  - macOS：全量构建 `error:` 计数 **0**；`ctest -L phy` **161/161**；十个 metal 二进制全过
    （CE 的 Test 9/11/12 数字未变）；新单测 `du_low_phy_pipeline_test` **14/14**；YAML 往返 18/18。
  - Ubuntu（`079b114894`）：构建 exit 0、`error:` 0；`--target test` **7615/7615**（338.9 s）。
- **S-7a 已落地**（本文档 §1）。**下一步是 S-7b**（§4）。

## 1. S-7a 落地内容（一句话版）

`expert_phy --phy_pipeline {auto,cpu,cpu_gpu,gpu}`；`du_low_phy_pipeline.{h,cpp}` 是**唯一的解析规则**；
启动打印有效配置（模式 + 逐模块后端 + 降级标注）；`ul_pipeline_probe` 在 `gpu` 档不再记/不再打印三个分段。
细节与证据见活文档 **§46**（含 §46.2 的"默认值 `auto` 而不是 `cpu`"的理由、§46.4 的 ODR bug）。

**记忆点（新会话最容易踩的两条）**：
1. 三个模块开关（`--pusch_dft_type`、`--pusch_channel_estimator_algo`、`--pusch_channel_equalizer_backend`）
   的默认值已从 `cpu` 改成 **`auto`**（＝跟随模式）。不要把它们当成"用户显式给了 cpu"。
2. `--phy_pipeline cpu` 是**严格档**：只要有一个 offload 开关就报错（不静默、不降级）。
   Linux/无 Metal 构建上跑 metal 开关时**不要**显式给 `--phy_pipeline cpu`（用默认 `auto`）。

## 2. Ubuntu 上的构建坑（本会话新增两条）

1. **GCC `-Werror=range-loop-construct`**：`for (const std::string& v : {"a","b"})` 在 clang 上过、GCC 上
   **报错**（绑定临时量）。⇒ 遍历 `const char*`。第 N 次"clang 能过 ≠ GCC 能过"。
2. **ODR + inline + 平台宏**：把"Metal 后端是否编进本二进制"做成 header 里的 `inline` 函数 ⇒ 不同 TU 有不同
   定义 ⇒ 链接器只留一份（实测留下的是全 0 的那份，所有 metal 后端被静默降级为 CPU）。
   ⇒ 可用性查询必须是**唯一一个 .cpp** 里的非 inline 函数。

## 3. 文档地图（新会话按需读，不必全读）

| 要什么 | 读哪 |
|---|---|
| **现状架构：每个边界的往返/同步清单**（唯一权威描述） | 活文档 `s2_full_chain_design.md` **§45** |
| **S-7a 落地记录**（含规则表、ODR bug、Ubuntu 数字） | 活文档 **§46** |
| 下一步的**融合规划**（`--phy_pipeline gpu`，IQ→LLR） | 路线图 `full_chain_gpu_uma_zero_copy_refactor_plan.md` **§10**（§10.1 有 S-7a 的偏差说明） |
| 用户已定的决策 | 路线图 **§10.9** |
| 同步模型（无全局栅栏 ⇒ 依赖推到 dispatch/CB 边界）+ 7 条新条目 | 路线图 **§10.10** |
| **终局约束：MAC PDU 才出 GPU；要留的 8 个门（D1–D8）** | 路线图 **§10.11** |
| OTA 采集硬规则（必须 sudo / 不要 `| tee` / 探针语义） | memo **#4 §3** |
| 跨平台硬规则（ssh 命令 / 基线 / 三条隔离约定） | memo **#4 §3.5** |
| 工作方法 / 硬规则 / 坑表 / 本地文件清单 | memo **#3 §2/§5** |

## 4. 下一步（唯一）：**S-7b 设备网格（FFT 直写）**

路线图 §10.8 的 S-7b：**FFT 直写设备 resource grid（master 在设备）+ 按需主机镜像 + 与主机网格逐位比对的单测**。

开工前必须先读/决定的三件事（S-7a 已经铺好路）：
1. **模式怎么到 lower PHY**：现在 `effective.dft` 已经经 RU 配置到了 `lower_phy_configuration.dft_processor_type`；
   S-7b 还要把**模式本身**（或"网格是否设备化"的开关）传下去。建议照 `dft_processor_type` 的样子走
   `flexible_o_du_ru_config` → `ru_sdr_config_translator` → `lower_phy_configuration`，
   **不要**在 lib 里读 `phy_pipeline_mode_registry`（那是探针专用，文档已注明）。
2. **网格的消费者**：PUCCH（`pucch_processor_impl` 读 `resource_grid_reader`）、PRACH 检测器、
   以及 PUSCH 侧 `get_ch_data_re`。⇒ 主机镜像必须**按需**生成，并且那次等待记在**需求方**账上
   （路线图 §10.6 R2、§10.10.2）。
3. **逐位比对的门禁方式**：`Filtered` 教训 —— 必须比对**数字**（不是 PASS/FAIL）。
   建议在 metal 单测或 `metal_chain_probe` 风格的工具里做 IQ→(metal FFT)→设备网格 vs
   (CPU FFT)→主机网格的逐 RE 比对（容差按 bf16/cbf16 的既有口径）。

**S-7b 之后**：S-7c（K0-a 导频提取）→ S-7d（K0-b/c/d）→ S-7e（均衡器 `ch_re` + LLR 设备化）→
S-7f（ring + push/collect + 打开 `gpu` 档 + `[ul_gpu_lane]`/`[ul_llr_ready]`）→ S-7g（可选）。

## 5. 待用户做的验证（S-7a 的 OTA 门禁）

S-7a **零数据流变化**，所以 OTA 一轮必须与 tag `gpu_phy_pre_ldpc` **逐项一致**（TF/CE/eq+demod/LDPC/
`ul_pipeline`/`mean total`/`wrap failures=0`/`hops_no_gpu=0` …）。命令与采集规则见 memo #4 §3（**必须 sudo**、
**不要 `| tee`**、先比负载指纹）。本轮会**多出**两行启动日志：

```
[phy_pipeline] mode=cpu_gpu fused=no lane=IQ->LLR (expert_phy --phy_pipeline auto)
[phy_pipeline]   dft=metal channel_estimator=metal_mmse equalizer=metal demapper=metal ldpc_decoder=metal
```

（这两行是 S-7a 的产物，不是异常；`fused=no` 表示还没进融合档。）

## 6. LDPC（更后面的一块，用户已定先做融合）

进入 LDPC 的第一个动作仍是**测量**：它占 `ul_pipeline` 的 80%、逐码块 `commit+wait`（`max_in_flight=1`）
⇒ 用 §10.7 的 **busy/gap** 分解回答"3540 µs 里 GPU 真忙多少、等 CPU 多少"。
注意 srsRAN 的 LDPC 代码块循环里有大量小数据控制逻辑（CB 分段、CB CRC、TB 拼接、FAPI 打包）
⇒ 终局大多是"控制留 CPU、LLR 与译码留设备"（§10.11.3）。

## 7. 本轮新增的坑

1. **CLI11 的 `parse(argc, argv)` 会跳过 `argv[0]`**（当程序名）⇒ 单测里自己拼 argv 必须补一个占位。
   漏掉时的症状很误导：报"arguments were not expected: cpu --phy_pipeline"（子命令没进，选项全成了多余参数）。
2. **"探针降级"比"探针说谎"更难发现**：ODR 那次所有后端都静默变 CPU，**编译、单测、启动全都不报错**，
   只有启动日志里那两行 `metal->cpu (requested backend not built in)` 露馅。⇒ 新写的"有效配置"日志
   本身就是这类 bug 的探针，值得优先做。
3. **不要用 `pgrep` 之外的假设判断"有没有残留进程"**：本轮为验证做过两次短跑（ZMQ 草稿配置、空闲端口、
   无 RF、无 `sudo`），已确认退出干净；跑 OTA 前仍建议 `pgrep -fl gnb` 复核。
4. **用户的 config 里有 `expert_phy:` 段**（例如 `gnb_zmq.yaml` 里 `pusch_channel_estimator_algo: cpu`）：
   配置里的值是"显式值"。默认 `auto` 模式下没问题（CLI 覆盖即可）；但将来若给 `--phy_pipeline gpu`，
   配置里的 `cpu` 会与融合档冲突并报错 —— 这是**设计如此**（不是 bug）。
