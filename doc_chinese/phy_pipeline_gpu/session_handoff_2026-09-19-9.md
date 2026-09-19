# 交接 — 用户要的 IQ→LLR 测量已实现（**等一条腿**）+ S13-P2 的下一步

> 上一份：`session_handoff_2026-09-19-8.md`（本文件是**新的入口**）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md`
> —— 批次表 §5（**新增"测"与 S13-P1/P2 三行**）、写侧 §19（§19.6.2b 有勘误块）、
> **本轮新增 §20（`[ul_gpu_pipeline]` 的口径与判据）**。
> 计划文件：`wip/S13_fallback_coverage.md`（S13 方案 + §5b 是 P1 的第一个结果）。

---

## 1. 一句话状态 + 下一步

**代码 HEAD = `7d968cfb84`**（**已推送**；本文件是它之后的**文档提交**，工作树干净）。
`build/hashes.h` 的戳**已刷新到实际 HEAD**（本文件所在的那次提交）⇒ **腿可以直接跑**；
若 `run_leg.sh` 仍然拒绝（例如这之后又有了新提交），照它给的一行修：
`touch build/hashes.h && cmake --build build --target gnb`。
Ubuntu 已 pull：**构建 rc=0、`ctest` 7618/7618 全过**（多的一条是新用例，
在 `ENABLE_FLOW_PROBES=OFF` 下按设计跳过）。

**本会话做完了用户新加的测量（第 1 条）的离线部分**：`mode=gpu` 下多一条
`[ul_gpu_pipeline]` = **IQ 进 GPU → LLR 出 GPU**（= 车道外三个分段之和的替代物），
`leg_report.sh` 也把 shutdown 的延迟系列打出来了。**判据只缺一条空中腿**（§4）。

**下一步（按优先级）**：

1. **★ 跑一条腿**（§4）：确认 `[ul_gpu_pipeline]` 真的出现、数字合理。**这是本会话唯一欠的判据**；
2. **S13-P2**（§5.1）：消掉那条**每跳**往返（设备自建 `gpu_rx_pilots` + EPRE 由设备发布）。
   **本轮已经把实现路线读到可动手的程度**（§5.1 有具体落点与不能碰的地方）；
3. **S13-P1 剩余**（§5.2）：回退门计数 + A/R_hp / y staging 两个站点。

---

## 2. 本会话做了什么

| 提交 | 内容 |
|---|---|
| `7d968cfb84` | **`[ul_gpu_pipeline]`**：`ul_pipeline_probe` 在 fused 模式下用**一条 IQ→LLR 跨度替代**三个分段（车道外照旧）；`leg_report.sh` 增 `-- latency (shutdown series)` 段；**新增单测** `tests/unittests/support/executors/ul_pipeline_probe_test.cpp`（含反证）；设计文档 **§20** |

上一会话（`b3f72deadb` 之前）的清单见 handoff-8 §2。本会话**没有**改任何数据面代码。

---

## 3. 本会话的测量与数字（判据看这里）

* **27 捕获 A/B（行为不变）**：`ab_replay_bins.sh`
  （A = `5f` 的二进制 + `/tmp/mmse_head.metallib`，B = 当前树 + 树里的 kernels）
  ⇒ **27/27 四个 dump 逐字节相同、0 缺 dump、配对断言通过**。
  （这条同时把 5g 的"与旧二进制逐字节相同"在新构建上复核了一遍。
  注意两边的 crossings 行**本来就不同**：A 是 1.00 读 + 1.00 写/跳（5f 的 epochs 上传），
  B 是 2.00 读 + 1.00 写/跳（P1 站点照出的那条往返）——**这是预期的，不是差异**）
* **单测**：新用例在 `ctest -L support` 里过（562/562）；**`ctest -L phy` 172/172 不变**
  （新测试 label 是 `support`，不进 phy 计数）
* **反证**：把 `records_phase_segments()` 恒真（fused 分支不可达）⇒ 新用例**变红**
* **Ubuntu（`ENABLE_FLOW_PROBES=OFF`）**：构建 rc=0；`ctest -j12` = **7618/7618，0 failed**
  （71.9 s；测试数 **7617 → 7618**，多的就是新用例）。新用例在 Linux 上**按设计跳过并给出原因**
  （`compiled_out_without_flow_probes (Skipped)`）——这条本身也验证了"OFF 构建不是静默通过"。
* **观察到的偶发**：`ctest -L phy` 有一次 `port_channel_estimator_metal_mmse_unit_test` **失败**，
  之后 12 次（含单独重跑 8 次、全套 1 次、GPU 子集 3 次）全过、无法复现；
  该测试与本次改动无关（不包含探针头文件）。**记在这里，不当作已解释。**

---

## 4. ★ 要跑的腿（唯一欠的判据）

二进制已经是对应 `7d968cfb84` 的（戳已更新、`gnb` 已重链），**不需要再构建**：

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu probe-iq2llr
# 手机侧照旧 ping / iperf3；Ctrl-C（不要 kill）停止，退出时会打印所有系列
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh \
     doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_probe-iq2llr_*.log.stderr
```

**这一腿的唯一新东西是一条线**（没有数据面改动 ⇒ 契约与 CRC 应与 `5g-epochs` 同量级）。
腿报告现在会打 `-- latency (shutdown series)`，其中应有：

```
[ul_pipeline]     samples=… mean=…         ← IQ → CRC=OK（老系列，与模式无关）
[ul_gpu_pipeline] samples=… mean=…         ← 新：IQ 进 GPU → LLR 出 GPU
[ul_ldpc_decode]  samples=… mean=…         ← LLR 之后的部分
[ul_fapi_mac]     samples=… mean=…
```

**要盯的四点**：

| # | 看什么 | 期望 |
|---|---|---|
| 1 | `[ul_gpu_pipeline]` **这一行存在**（不是 "no samples recorded"）| 出现 |
| 2 | 它的 `samples=` 与 `[ul_ldpc_decode]` **同量级且 ≥ 它** | 因为它在**每次解码尝试**记录，而后者只在 CRC 通过时记录（§20.3 第 2 条）。5g 腿 CRC 79.19% ⇒ 预期多约 20% |
| 3 | `mean` **小于** `[ul_pipeline]` 的 mean，差值 = LLR 之后的部分 | 5g 腿：`[ul_pipeline]` mean 2768.0 µs、`[ul_ldpc_decode]` mean 13.0 µs、`[ul_fapi_mac]` mean 2.3 µs ⇒ 预期 `[ul_gpu_pipeline]` ≈ **2750 µs** |
| 4 | 与 `[ul_gpu_lane]` 一起读：`residency` 远小于这个窗口是**正常的** | 窗口起点是 `ul_process()` 顶部按 `last_rx_timestamp` 派生的时间戳（整 slot 块时对应**上一个块**的到达），所以窗口里有"等这一 slot 的样本到齐"的一段；5g 腿 residency 856.8 µs vs `[ul_pipeline]` 2768 µs |

**同时确认没有回归**：契约 **8/8 MET**、`0 RF failures`、`crc OK/KO` 与 `5g-epochs` 同量级
（79.19%）、`[ul_gpu_lane] lanes` 的 dropped/carried = 0。

**判据不成立时**（例如整行缺失）：先查 `ENABLE_FLOW_PROBES` 是否真的是 ON、
再查 `phy_pipeline_mode_registry` 是否发布了 `gpu`（探针只在 `mode=gpu` 记录与打印）。

---

## 5. 下一步的题目

### 5.1 S13-P2：消掉那条每跳往返（下一步做这个）

**现象**（handoff-8 §4 / `wip/S13_fallback_coverage.md` §5b）：**默认路径**每个跳都有一次
`宿主读设备网格 → 原样写回 gpu_rx_pilots`（432 B 的跳上 = 2.00 读 + 1.00 写/跳，
其中 1 读是 replay 工具自己的回读）。

**本轮读代码得到的落点（可直接照做）**：

1. **消写**：`gpu_rx_pilots`（布局 `[npt][nof_cdm_groups][npf]` complex）由 **K0-a 的
   `mmse_pilots_lse`** 顺手写出即可——它**已经在读同一批 RE**
   （`mmse_pilots_read_grid()` 返回的那个 `float2 rx`），只需多一个输出 buffer + 一次 store；
   注意**只让偶数 layer 写**（同一个 CDM 组的两个 layer 读的是同一个 RE）。
   ⚠ 该 kernel **是逐元素的**（没有长累加），所以按 §19.6.3 的规则它**可以**加代码；
   但它旁边的 `mmse_pilots_cfo` **是**长累加 ⇒ **绝不要动那个 kernel**。
2. **消读**：宿主现在为了两件事抽导频——
   (a) **EPRE**（`epre += average_power(rx) * size`，最后 `/nof_dmrs_pilots`，是**上报值**）；
   (b) **回退**（设备没覆盖这一跳时它自己算 LSE/CFO）。
   ⇒ 设备能覆盖这一跳时（`stage_produces_ls_pilots()` 为真）宿主**整段抽取+EPRE 都不该跑**；
   EPRE 按 5a 的 rsrp 那样由**设备发布**（设备侧已有归约导频功率的
   `ocudu_mmse_pilots_power.metal`——先确认它的定义与宿主 `epre` 是否同一个量，
   **不同就得在设备侧补一个同定义的归约**，不能拿一个"差不多"的量顶）。
   ⚠ 落点难点：抽取循环在 **`stage_args` 构造之前**（`rx_pilots` 是 `args` 的成员），
   所以需要一个**更早的虚钩子**（现有 `stage_produces_ls_pilots(args)` 拿不到 args 就答不了）。
3. **不能碰**：`mmse_noise`（K4，`ocudu_mmse_reformat.metal`）**是归约、发布噪声方差**
   ——按 §19.6.3，**不要给它新算术**（例如"从 lse 反推 y"）；y 必须由 K0-a 直接从网格产出。
4. **判据**：**带着 P1 的站点**，默认旋钮下契约回到 `0.00 read(s) + 0.00 write(s)`/跳
   且分项表**空**；27 捕获四个 dump 逐字节不变（K0-a 的 LSE 是线性的，但**必须实测**）；
   `ctest -L phy` 172/172；然后**一条腿**（契约 8/8）。
5. 回退路径（`OCUDU_CE_CPU_LS=1`、几何被拒）**必须仍然可用且可见**：那时宿主抽网格是
   **合法的回退**，P1 的站点会把它数出来（契约变红是**预期**，不是回归）。

### 5.2 S13-P1 剩余（P2 之后或并行）

* **回退门的拒绝计数**：`device_ls_refused`（几何被拒，区别于 engine 调用失败）、
  `device_y_refused`（`record_device_y_stage` 每个 `return false` 分支分类）、`device_corr_refused`、
  `ta_refused`、`sigma2_refused`、`k3_refused`；打印进 `[metal_stats] mmse_ce` 行；
* **另两个盲点的站点**：`stage_engine_group()` 里 A/R_hp 的宿主 `memcpy`（写）、
  y staging 的逐导频读+写（`ls_pilot()` 读设备 LSE → 写 `gpu_y`，含 pad 行 memset）；
* **判据（P1 自证）**：用现成旋钮强制每条回退（`OCUDU_CE_DEV_Y=0`、`DEV_TA=0`、`DEV_SIGMA2=0`、
  `CORR_DEV=0`、`CPU_LS=1`）⇒ 对应计数**必须动**、契约变红；关掉旋钮 ⇒ 回到 0。

---

## 6. 硬纪律（继承 handoff-8 §6，★ 是本会话新增）

1. **GPU 挂死**：kernel 每个循环上界必须编译期常量、索引夹紧、不读 `[[threads_per_threadgroup]]`、
   barrier 前不早返回。跑任何 dispatch 前限时（`/tmp/limited_run.sh`）+ 跑完查 `recoveryCount`。
2. **陈旧产物**：改 `.metal` 先 `rm -f *.metallib`；改完库要重建"你要跑的那个二进制"；
   跑腿前 `build/hashes.h` 戳必须 == HEAD（**本轮已做**：`touch build/hashes.h` + 重建 `gnb`）。
3. **metallib 是运行时文件**：两个二进制默认读同一个文件 ⇒ 比较两个 build 必须各自钉住 kernels。
4. **给"输出出自长累加"的 kernel 加新算术会动发布位**（CFO 1 ulp、K4 2 ulp；设计文档 §19.6.3）
   ⇒ 这类 kernel 收参数，不要给它新代码。**P2 里 K4（`mmse_noise`）正是这一类。**
5. **git tag 不会随 push 走**：打里程碑一律 `git tag -a` + `git push origin <tag>`。
6. **测量要能被证伪**：插仪表先证明"行为不变"（27 捕获 dump 逐字节），再用旋钮**强制触发**
   它要量的路径。**★ 探针本身也要能证伪**：本轮把 fused 分支改成不可达 ⇒ 新单测必须变红（做了）。
7. **★ `gtest_discover_tests` 把每个用例注册成独立进程**：跨用例共享状态（进程级单例、
   只能发布一次的 mode）的测试**必须放在同一个用例里**——本轮第一版拆成两个用例，
   直接跑二进制通过、`ctest -L support` 变红。
8. "XX 非空"当代理判断危险；换实现后重审每个代理判断。
9. CRC 要按分配宽度分层看；采样连续性坏掉的腿，其数字一律不能当证据。

---

## 7. 常用命令

```bash
# ---- 构建 / 单测（macOS）----
touch build/hashes.h && cmake --build build --target gnb    # 跑腿前
cd build && ctest -L phy                                    # 172（含 8 个 Metal 用例）
ctest -R "metal"                                            # 9 个 GPU 用例（~15 s）
ctest -L support                                            # 含新的 ul_pipeline_probe_test

# ---- 离线 A/B：两个二进制 + 各自 kernels ----
AB_METALLIB_A=/tmp/mmse_head.metallib AB_METALLIB_B=/tmp/mmse_fix2.metallib \
  bash doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh /tmp/replay_head \
       build/lib/phy/upper/channel_processors/metal/ul_chain_replay
bash doc_chinese/phy_pipeline_gpu/wip/ab_ta.sh              # TA 三臂（同一二进制的旋钮 A/B）

# ---- 空中腿（需要手机；Ctrl-C 停，不要 kill）----
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu probe-iq2llr
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh \
     doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_probe-iq2llr_*.log.stderr

# ---- Ubuntu（免密）----
ssh jwang@192.168.100.131 'cd ~/work/ocudu && git pull --ff-only origin apple-silicon && \
  cmake --build build -j12 && cd build && ctest -j12'

# ---- 看腿顺序 ----
# [epoch_impl]/[epoch_check] 与 [ta_impl]/[eq_impl] 自证 → radio sample continuity / host sample assembly
# → 契约 8 项 + 写侧分项表 → [ul_gpu_lane] busy split → 延迟系列（含 [ul_gpu_pipeline]）→ CRC（按宽度分层）
# → RF failures
```

备份：`bash doc_chinese/phy_pipeline_gpu/wip/backup_worktree.sh <label>`。

---

## 8. 诚实清单

| # | 项 | 现状 |
|---|---|---|
| 1 | **`[ul_gpu_pipeline]` 的空中判据** | ⚠️ **只有离线判据**（单测 + 反证 + 27 捕获不变）。**腿待跑**（§4）|
| 2 | 用户新测量的第 2 条（IQ → CRC=OK）| ✅ 本来就有（`[ul_pipeline]`，与模式无关）；本会话把它**加进腿报告** |
| 3 | `port_channel_estimator_metal_mmse_unit_test` 的一次偶发失败 | ⚠️ 观察到 1 次、12 次复跑全过、**无法复现**；与本次改动无关（该二进制不含探针）。**未解释** |
| 4 | 契约的"零" | ⚠️ 仍只在**被审计的站点**上成立；P1 发现的每跳往返**未消除**（P2 的题目）|
| 5 | S13-P2 / P1 剩余 | 未做；路线见 §5 |
| 6 | Ubuntu 对新 HEAD 的验证 | ✅ **已完成**：构建 rc=0；`ctest -j12` **7618/7618，0 failed**（71.9 s），测试数 7617→7618（新增用例；`ENABLE_FLOW_PROBES=OFF` ⇒ 它按设计跳过）|
| 7 | 稀疏 RB 掩码 / 变换 >2048 | 仍回退宿主（适用范围问题，不是正在发生的错误）|
| 8 | `nof_layers > 1` | 只在单测里验证过 |
| 9 | TA 的 ~35 µs 尾延迟 | 接受（设计文档 §18.7 有 fence 备选）|

---

## 9. 本会话踩过的坑

| # | 坑 | 症状 | 正解 |
|---|---|---|---|
| 1 | **`gtest_discover_tests` 每个用例一个进程** | 两个用例共享进程级 mode 单例：直接跑二进制通过、`ctest` 变红 | 跨用例有状态依赖时**合并成一个用例**（§6.7）|
| 2 | **`-R "metal_unit_test"` 匹配不到 `..._metal_mmse_unit_test`** | 以为跑了 8 个 GPU 用例，实际 6 个（handoff-8 §7 的注释也这么写）| 用 `ctest -R "metal"`（9 个）或显式列全 |
| 3 | `ctest` 失败后 `LastTestsFailed.log` 会被**下一次成功运行删掉** | 想回看失败详情时文件已不在 | 失败当下就 `--output-on-failure` 存下来 |

---

## 10. 关键文件

**本会话新增 / 改动**
* `include/ocudu/support/executors/ul_pipeline_probe.h` — `[ul_gpu_pipeline]` 的记录与打印
* `tests/unittests/support/executors/ul_pipeline_probe_test.cpp`（+ 同目录 `CMakeLists.txt`）— 探针自证 + 反证
* `doc_chinese/phy_pipeline_gpu/gpu_phy_pipeline_design_and_implementation.md` — **§20**（口径/判据/缺什么）+ §5 批次表三行
* `doc_chinese/phy_pipeline_gpu/wip/leg_report.sh` — `-- latency (shutdown series)` 段

**P2 会碰的**
* `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_pilots.metal`（`mmse_pilots_lse` 加输出）
* `.../ocudu_metal_mmse_engine.{h,mm}`（`pilots_stage` 的新 buffer）
* `.../port_channel_estimator_metal_mmse_impl.{h,cpp}`（`stage_device_noise_inputs()` 跳过拷贝；新的早钩子）
* `.../port_channel_estimator_average_impl.{h,cpp}`（抽取+EPRE 循环的门控）
* `.../ocudu_mmse_pilots_power.metal`（EPRE 的设备侧发布：**先核对定义是否与宿主一致**）
