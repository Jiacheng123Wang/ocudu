# 交接 — 批次 5g 收尾 + 里程碑 tag + S13 起步（**发现契约漏计了一条每跳往返**）

> 上一份：`session_handoff_2026-09-19-7.md`（5g 之前的状态；本文件是**新的入口**）。
> 常驻设计文档：`gpu_phy_pipeline_design_and_implementation.md`
> —— 批次表 §5、阶段总结 §18（5b+5c+5d）、写侧 §19（5e+5f+5g，**§19.6.2b 有一条勘误块，务必先读**）。
> 本轮计划文件：`wip/S13_fallback_coverage.md`（S13 的方案 + **§5b 是 P1 的第一个结果**）。

---

## 1. 一句话状态 + 下一步

**HEAD = `b3f72deadb`**（工作树干净；已推送；远端 = 本地）。Ubuntu `~/work/ocudu` 已 pull 到同一提交，
**构建 rc=0、`ctest` = 7617/7617 全过（70.4 s，测试数与改动前完全一致）**
—— 包括 P1 新加在**基类**里的那个虚钩子（默认空实现，CPU 车道不受影响，这一点已在这台机器上验证）。

| 项 | 状态 |
|---|---|
| 5g（epochs 搬上设备）| ✅ 完成（`45fa002d6c`）：**写侧 0/跳**，离线 27 捕获四 dump 逐字节相同，单测 140/140 |
| 里程碑 tag | ✅ `gpu_phy_iq2llr_zero_data_crossings`（annotated `16780f90d6` → `6603a13539`），已推送、两台机器一致 |
| 空中腿 `5g-epochs` | ✅ 跑过：契约 8/8、0 RF failure、CRC 79.19%、0 gaps |
| **但**：P1 发现契约**漏计**了一条**每跳**的设备→宿主读 + 宿主→设备写 | ⚠️ 已插仪表并**实测**（§4）；文档已加勘误；**tag 的那句话要等 P2 之后才真正成立** |

**下一步（按优先级，三件，详见各自小节）**：

1. **★ 用户新加的测量（§5）**：`[ul_gpu_pipeline]` = **IQ 进 GPU → LLR 出 GPU**（只对 `mode=gpu`）。
   设计已定、**未实现**；同一条 `[ul_pipeline]`（IQ→CRC=OK）**已经存在**（§5.2 有腿上的数字）。
2. **S13-P2（§4.6）**：消掉那条每跳往返（设备自己建 `gpu_rx_pilots`；EPRE 按 5a 的 rsrp 那样由设备发布）。
   **判据**：带着新插的站点，契约回到 `0.00 read + 0.00 write`/跳。
3. **S13-P1 剩余（§4.7）**：回退门的计数（`device_ls_refused`/`device_y_refused`/…）+ §2 里另外两个站点
   （A/R_hp 的宿主写、y staging 的逐导频读+写）。

---

## 2. 本会话做了什么（提交清单，全部已推送）

| 提交 | 内容 |
|---|---|
| `45fa002d6c` | **5g**：`symbol_start_epochs` 不再上传（写侧归零）。新头文件 `ocudu_mmse_epochs.h`（宿主 C++ 与 MSL 共用、带精确性证明）；K0-a 的 `apply_cfo`/`sigma2` 在设备自算，**K4 与 CFO 估计 kernel 收参数**（原因见设计文档 §19.6.3）；加了 GPU 探针 kernel `mmse_epoch_probe`（全定义域 140/140）|
| `531b9e7790` | 把 `configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml` 入库（PUCCH 六个资源值 + `max_consecutive_kos=300` + **AMF 地址**——最后一条是环境项，换机器要改）|
| `1721098690` | **doc_chinese/ 入库**（策略反转：文档 tracked、工作产物 gitignore 在 `doc_chinese/.gitignore`）；`lib/`、`tests/`、`configs/` 里 11 份中文文档 `git mv` 进 `doc_chinese/{metal_ldpc,metal_ce,ai_ce,macos_compat_refactor,test_environment}/` 并更新全部引用；4 个 ai_train 脚本 + 1 个 config 注释译成英文（命令逐行核对未变）+ 2 个指针 README |
| `8c87761441` | **6 个 Metal 单测 + 2 个深模式变体注册进 ctest**（`phy` label；`port_channel_estimator_metal_mmse_unit_test[_ta_chain]`、`channel_equalizer_metal_unit_test[_defer_encode]`、`dft_processor_metal_unit_test`、`ofdm_demodulator_metal_batch_test`、`demodulation_mapper_metal_unit_test`、`ldpc_metal_unit_test`）|
| `6603a13539` | 交接 memo §7 的命令块改为以 `ctest -L phy` 为主 |
| `a33808cae9` | **S13 方案**（`wip/S13_fallback_coverage.md`）：回退路径的可见性与收窄 + 判据 |
| `b3f72deadb` | **S13-P1 第一批仪表** + 三处文档勘误（见 §4）|

其它：tag `gpu_phy_iq2llr_zero_data_crossings` 推送 + Ubuntu `git fetch --tags`；
用户删掉的 3 项残留（`scripts/switch_gnb_plmn.sh`、3 个 BLER CSV、`Testing/`）已删。

---

## 3. 本会话的测量与数字（判据看这里）

**空中腿 `5g-epochs_0919_1945`（`45fa002d6c`，手机 ping+iperf3，~73 s，3576 次 PUSCH）**：

```
契约 8/8 MET；0 RF failures / 69549 slots；973602 symbols
CRC 2832/3576 = 79.19%（对照 5e 79.24%）；最忙一分钟 3549 次 / 79.6%（5e 3672 / 80.1%）
分层（24 PRB 数据层 97.20% vs 5e 97.95%；25 PRB 97.09% vs 89.78%；宽度 2 两腿都 0.00%）
busy split ch_est 118.7 / ch_wt 379.9 / eq_demap 101.1 µs/lane；lanes 3576，cbs/lane 3.38，
    residency 856.8 / busy 599.7 / gap 257.1 µs
[ul_pipeline]    samples=2832 mean=2768.0us median=2700.0us p95=4005.0us p99=4993.0us   ← IQ→CRC=OK
[ul_ldpc_decode] samples=2832 mean=13.0us   median=10.0us   p95=24.0us   p99=79.0us
```

**离线**：27 捕获 A/B（`wip/ab_replay_bins.sh`，两边 metallib 各自钉住）四 dump 逐字节相同；
`S12 epoch: 140 of 140 values bit-identical`（GPU 探针，2 种 CP × 5 numerology × 14 符号）。

**测试**：macOS `ctest -L phy` = **172/172**（原 164，+8）；Ubuntu `ctest` = **7617/7617，65 s**
（注册前后完全一致：Linux 上 `ENABLE_METAL_*=OFF`，那些 `add_test` 行根本不被处理）。

---

## 4. ★ P1 的发现：契约漏计了一条**每跳**的往返（这是本会话最重要的事）

### 4.1 现象（已实测，不是推测）

给"宿主抽接收导频"这条路径补上站点后，**默认路径**（不是回退！）每个跳都有一次往返：

```
1 捕获、默认旋钮：  2 host read(s) (4464 B) + 1 host write(s) (432 B) over 1 device hop
                    = 2.00 read(s) + 1.00 write(s) per hop      （其中 1 读是 replay 工具自己回读 h）
    ce: rx pilots staged (host)      1 call(s),   432 bytes
```

### 4.2 机理

* 基类 `compute_hop_submit()` **无条件**地从资源网格抽本跳的接收 DM-RS 导频
  （注释原文："it is unconditional: only the least-squares pilots and the CFO can be taken over by the stage"）；
* 在 gpu 车道里，那个网格是**前端 DFT 写在设备上的**（统一内存，S-7b）⇒ 宿主读它 =
  契约定义的 "the host TAKING device-produced data away"（和已经计数了的 LLR 读、h unpack 同一个约定）；
* 紧接着 `stage_device_noise_inputs()` 把这些值**原样写回** `gpu_rx_pilots`（噪声 kernel 要读）⇒
  又一次 "handing device-DERIVED data back"。

### 4.3 影响面（要如实说）

* 这条路径**早于 5a–5g**（是上游基类代码），所以设计文档里**所有**"读 0.00/跳"的说法（5c 起每一次）
  都要读作"**在被审计的站点上**为 0"；
* **dump 不受影响**：插仪表不改行为，27 捕获 A/B 仍 0 差异（本轮实测）；
* 没有任何测试断言穿越数字（已确认），CE/等化器单测仍 `All tests PASSED`/`ALL OK`；
* 已加勘误：设计文档 **§19.6.2b 顶部**、交接 -7 顶部、`S13_fallback_coverage.md` §5b。

### 4.4 已插的仪表（`b3f72deadb`，行为中性）

1. 基类新增 `virtual void account_host_grid_read(unsigned nof_re, bool device_written)`（默认空实现
   —— **CPU 车道不能计**，那里网格是宿主内存）；
2. Metal impl 覆写它 → `count_host_read(nof_re × sizeof(cbf16_t))`（仅当网格设备常驻）；
3. `stage_device_noise_inputs()` 末尾 → `count_host_write_site("ce: rx pilots staged (host)", …)`。

### 4.5 修法（S13-P2 的题目）

* **消写**：让设备自己从网格建 `gpu_rx_pilots`（K0-a 的 LSE kernel 已经在读同一张网格）；
* **消读**：宿主需要 `rx_pilots` 只为 (a) **EPRE**（一个上报值——设备侧 `mmse_pilots_power` 已经在归约
  同一批导频的功率，可以像 5a 的 rsrp 那样由设备发布）与 (b) 宿主回退路径（设备覆盖不到时才跑）；
* **判据**：**带着 4.4 的站点**，契约回到 `0.00 read(s) + 0.00 write(s)`/跳，且 27 捕获四 dump 逐字节不变、
  单测全过、空中腿契约 8/8。做完可以再打一个 tag（**当前 tag 的那句话在 P2 之前不成立**）。

### 4.6 P1 剩余（未做）

* 回退门的**拒绝计数**（现在只有一部分可见）：`device_ls_refused`（几何被拒，区别于 engine 调用失败）、
  `device_y_refused`（`record_device_y_stage` 的每个 return false 分支分类）、`device_corr_refused`、
  `ta_refused`、`sigma2_refused`、`k3_refused`；打印进 `[metal_stats]` 家族；
* S13 §2 里另外两个盲点的站点：**A/R_hp 宿主 staging 的写**（`stage_engine_group` 的 `memcpy` 进
  `gpu_a`/`gpu_r_hp`）、**y staging 的逐导频读+写**（`ls_pilot()` 读设备 LSE → 写 `gpu_y`，含 pad 行 memset）；
* 判据（P1 的自证）：用现成旋钮**强制每条回退**（`OCUDU_CE_DEV_Y=0`、`DEV_TA=0`、`DEV_SIGMA2=0`、
  `CORR_DEV=0`、`CPU_LS=1`）→ 对应计数必须动、契约变红；关掉旋钮 → 回到 0 与（P2 之前的）现状。

---

## 5. ★ 用户新增的测量需求（设计已定，**未实现**）

### 5.1 需求

1. **IQ 进 GPU → LLR 出 GPU 的总时间**，**只对 `--expert_phy.phy_pipeline gpu`**
   （cpu/cpu_gpu 已有分模块测量）；
2. 类似 cpu/cpu_gpu 的 **`[ul_pipeline]`：IQ samples → MAC PDU CRC=OK**，
   "实际上就是前面的那个测量加上 LDPC 的时间"。

### 5.2 现状：**第 2 条已经存在**（用户可能没看到它在 gpu 模式下也打）

同一腿的日志里：

```
[ul_pipeline]    samples=2832 mean=2768.0us median=2700.0us p95=4005.0us p99=4993.0us
[ul_ldpc_decode] samples=2832 mean=13.0us   median=10.0us   p95=24.0us   p99=79.0us
```

`include/ocudu/support/executors/ul_pipeline_probe.h`：编译期开关 **`OCUDU_FLOW_PROBES`**；
`record_start()` 在 `lower_phy_baseband_processor` 收到一个 slot 的 IQ 后调用，`record_end_crc_ok()` 在
CRC 通过时调用 ⇒ 模式无关，gpu 模式自然也有。**所以第 2 条要做的是"确认 + 写进文档"**，
必要时把腿报告里的解读写清（它与 `[ul_gpu_lane]`/`busy` 的差就是宿主+队列的部分）。

### 5.3 第 1 条的实现路线（下一个会话可直接照做）

* **起点与终点就在同一个探针里**：`ul_pipeline_probe` 已经按 slot 配对保存了
  `start`（IQ 到达）与 `eqdem_end`（等化+解调结束 = **LLR 就绪**）⇒
  `iq_to_llr = eqdem_end − start` 已经在数据里，只是**没有单独上报**；
* 做法：在该探针里再加一个累加器 + 一条打印（与 `[ul_pipeline]` 同样的统计格式，名字建议
  `[ul_gpu_pipeline]`），**打印与否按 `phy_pipeline_mode` 门控**（`include/ocudu/phy/phy_pipeline_mode.h`：
  `cpu` / `cpu_gpu` / `gpu`；探针不知道模式，需要由 pusch processor 侧告知一次，或读同一处 mode 源）；
* 诚实要点（写进输出/文档）：
  * 它和 `[ul_pipeline]` **共享同一个起点** ⇒ 两者之差就是 LDPC + CRC 路径；
  * 它是**墙钟**窗口，包含宿主提交/队列的空洞 ⇒ 与 `[ul_gpu_lane] residency/busy/gap` +
    `dft busy` 一起读才能把"GPU 忙"与"宿主在环里"分开；
  * **replay 工具不会打印** `[ul_pipeline]`（本轮查过：0 次，只有 gnb 在退出时 report）⇒
    验证要靠**一腿**（或临时给 replay 也调一次 report）。

---

## 6. 硬纪律（违反过、代价很大；★ 是本会话新增）

1. **GPU 挂死**：kernel 每个循环上界必须编译期常量、索引夹紧、不读 `[[threads_per_threadgroup]]`、
   barrier 前不早返回。跑任何 dispatch 前限时（`/tmp/limited_run.sh`）+ 跑完查 `recoveryCount`。
2. **陈旧产物**：改 `.metal` 先 `rm -f *.metallib`；**改完库要重建"你要跑的那个二进制"**；
   跑腿前 `build/hashes.h` 戳必须 == HEAD。
3. **★ metallib 是运行时文件**：`OCUDU_MMSE_METALLIB_PATH` 是 configure 时写死的源码树路径，
   两个二进制默认读同一个文件 ⇒ 比较两个 build 必须**各自钉住 kernels**
   （`wip/ab_replay_bins.sh` 的 `AB_METALLIB_A/B` + `[epoch_impl]` 配对断言）。
4. **★ 给"输出出自长累加"的 kernel 加新算术会动发布位**（CFO 1 ulp、K4 2 ulp；设计文档 §19.6.3）
   ⇒ 这类 kernel 收参数，不要给它新代码。
5. **★ git tag 不会随 push 走**：`git push` 默认不推 tag，`--follow-tags` 只跟随 **annotated** tag
   ⇒ 打里程碑一律 `git tag -a` + `git push origin <tag>`。
6. **★ 测量要能被证伪**：插仪表先证明"行为不变"（27 捕获 dump 逐字节），再用旋钮**强制触发**它要量的路径，
   确认数字按预期动。S13-P1 就是这样发现"默认路径上还有一条往返"的。
7. "XX 非空"当代理判断危险；换实现后重审每个代理判断。
8. CRC 要按分配宽度分层看；采样连续性坏掉的腿，其数字一律不能当证据。

---

## 7. 常用命令

```bash
# ---- 构建 / 单测（macOS）----
cmake --build build --target gnb                       # 跑腿前 touch build/hashes.h
cd build && ctest -L phy                               # macOS 172 个（含 8 个新注册的 Metal 用例）
ctest -R "metal_unit_test|ofdm_demodulator_metal_batch"  # 只看 GPU 的 8 个（~15 s）

# ---- 离线 A/B：两个二进制 + 各自 kernels ----
AB_METALLIB_A=/tmp/mmse_head.metallib AB_METALLIB_B=/tmp/mmse_fix2.metallib \
  bash doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh /tmp/replay_head \
       build/lib/phy/upper/channel_processors/metal/ul_chain_replay
bash doc_chinese/phy_pipeline_gpu/wip/ab_ta.sh          # TA 三臂（同一二进制的旋钮 A/B）

# ---- 空中腿（需要手机；Ctrl-C 停，不要 kill）----
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label> [OCUDU_*=v …]
# 日志：wip/logs/gnb_gpu_<label>_<date>.log{,.stderr,.stdout}

# ---- Ubuntu（免密）----
ssh jwang@192.168.100.131 'cd ~/work/ocudu && git pull --ff-only origin apple-silicon && \
  cmake --build build -j12 && cd build && ctest -j12'

# ---- 看腿顺序 ----
# [epoch_impl]/[epoch_check] 与 [ta_impl]/[eq_impl] 自证 → radio sample continuity / host sample assembly
# → 契约 8 项 + 写侧分项表 → [ul_gpu_lane] busy split → CRC（按宽度分层）→ RF failures
```

备份：`bash doc_chinese/phy_pipeline_gpu/wip/backup_worktree.sh <label>`
（已有 `2026-09-19_5g-air`、`2026-09-19_5g-done` 等）。

---

## 8. 诚实清单

| # | 项 | 现状 |
|---|---|---|
| 1 | **契约的"零"** | ⚠️ 只在**被审计的站点**上成立；P1 发现的每跳往返**未消除**（§4）。tag 的表述在 P2 之前不成立，文档已加勘误 |
| 2 | Ubuntu 对 `b3f72deadb` 的验证 | ✅ **已完成**：构建 rc=0；`ctest` **7617/7617 全过**（70.4 s），**测试数与注册/仪表改动前一致**（Metal 目录在 Linux 上不参与构建；基类钩子默认空实现）|
| 3 | 用户新增的两个测量 | 第 2 条已存在（§5.2）；第 1 条**未实现**（§5.3 有路线）|
| 4 | P1 剩余 | 回退门计数 + A/R_hp/y staging 两个站点（§4.6）|
| 5 | S13-P2（消掉往返）| 未做；这是"零穿越"成立的条件 |
| 6 | 稀疏 RB 掩码 / 变换 >2048 | 仍回退宿主（空口实测：分配**全是连续区间**，所以是**适用范围**问题，不是正在发生的错误）|
| 7 | `nof_layers > 1` | 只在单测里验证过 |
| 8 | TA 的 ~35 µs 尾延迟 | 接受（设计文档 §18.7 有 fence 备选）|
| 9 | `doc_chinese` 里的工作产物 | 已 gitignore（`work_tmp/`、`logs/`、`air_logs*/`、`worktree_backups/`、`*.bin`、`*.metallib`）；入库的是 106 个文档/脚本（~2.9 MB）|

---

## 9. 本会话踩过的坑（按价值）

| # | 坑 | 症状 | 正解 |
|---|---|---|---|
| 1 | **python 脚本用了过期的 `s` 缓冲区** | 第二次 `write()` 把第一次的插入覆盖掉，链接时报 `symbol(s) not found` | 每次替换后重新读取文件（或写一个 buffer）|
| 2 | `--out` 指向**不存在的目录** | `ul_capture::capture_h()` 的 `fopen` 失败就提前 return ⇒ 少一次回读，"读"凭空变 0 | 目录必须存在（harness 里 `mkdir -p`）|
| 3 | **新旧二进制混跑同一 metallib** | "旧二进制"其实在跑新 kernels，结论完全错 | 两边各自钉住 metallib + `[epoch_impl]` 配对断言 |
| 4 | **改了 `.metal` 但没重链 metallib** | md5 不变，白跑一轮 A/B | 构建输出里必须看到 `Linking Metal library` |
| 5 | `git checkout <file>` 用来"撤销仪表" | 把该文件的**本轮改动**一起撤销（5g 的 pilots.metal 丢过一次） | 用精确的 patch 反向应用；动手前先 `git diff > 备份.patch` |
| 6 | tag 只在本地 | 两台机器 `git tag` 不一致 | `git tag -a` + `git push origin <tag>`，Ubuntu 侧 `git fetch --tags` |

---

## 10. 关键文件

**本轮新增**
* `lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_epochs.h` — 5g 的宿主/MSL 共用实现
* `doc_chinese/phy_pipeline_gpu/wip/S13_fallback_coverage.md` — S13 方案 + §5b P1 结果
* `doc_chinese/phy_pipeline_gpu/wip/ab_replay_bins.sh` — 两个二进制（各自 kernels）的 27 捕获对比
* `doc_chinese/README.md`、`doc_chinese/.gitignore` — 文档入库的策略与排除规则

**主要改动**
* `ocudu_mmse_pilots.metal` / `ocudu_mmse_reformat.metal` / `ocudu_mmse_pilots_power.metal`（5g：params + epoch 计算 + 探针 kernel）
* `ocudu_metal_mmse_engine.{h,mm}`（`epoch_span`、`dmrs_epochs`、`run_epoch_probe`、镜像结构体 + offset 断言）
* `port_channel_estimator_metal_mmse_impl.{h,cpp}`（`epoch_geometry_of` + `[epoch_impl]`/`[epoch_check]`；**P1：** `account_host_grid_read` 覆写 + `ce: rx pilots staged (host)` 站点）
* `port_channel_estimator_average_impl.{h,cpp}`（**P1：** `account_host_grid_read` 虚钩子，默认空 = CPU 车道不变）
* 5 个 `metal/CMakeLists.txt`（ctest 注册）
* `docs/apple_silicon_heterogeneous_gnb_plan_english.md`、`tests/ci/macos_triage/SUMMARY.md`、`configs/*.yaml`、`lib/.../ai_train/**/*.sh`（文档迁移的引用与英文化）
