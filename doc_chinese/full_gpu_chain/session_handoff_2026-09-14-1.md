# Session handover #7（2026-09-14，本会话结束时的快照，**只写不改**）

> 本文件是**快照**：写下之后不再回填/修改。后续进展写进新的 handoff 或活文档
> `s2_full_chain_design.md`（§48.x 起）与 `full_chain_gpu_uma_zero_copy_refactor_plan.md`（§10.8）。
>
> 上一份快照是 `session_handoff_2026-09-13-6.md`（HEAD `d9f741b31a`）。

---

## 0. 一句话现状

**HEAD = `2efa704850`**（`apple-silicon`，已 push）。**S-7e（批量均衡）收口完成**：
两个真根因找到并修好，批量的实网缺陷消失，**批量已翻为默认**（`OCUDU_EQ_DEFER_ENCODE=0` 为逃生口）。
实网已确认（`bc2331efc1`，`/tmp/gnb_b9.log`）：RRC setup 完成、PDU Session + GTP-U 建立、**ping 通**、
PUSCH BLER **418/1619 = 25.8%**（前 200 次 32.5% → 后 200 次 25.5%，无恶化），
均衡 dispatches 17809 → **6476**（整链 22 → 15）。

树状态：`ctest -L phy` **162/162**；工作树只有用户自己的 `configs/*` 与未跟踪的
`bler_metal_bg1_z64_*.csv`、`scripts/switch_gnb_plmn.sh`（**不要动**）。

---

## 1. 阶段目标与硬规则（用户已明确）

- 阶段目标：**融合优化 LDPC 之前的 Metal GPU PHY pipeline**。**本阶段不做 LDPC 优化**。
- 提交信息/注释**不得引用 `doc_chinese/`**；前缀 `GPU-PHY S-<id>:`（构建修复用 `build:`）。
- 后端选择只走 `expert_phy` CLI；env 只用于 debug。TX/DL 保持 CPU。每完成一步：提交 + push。
- gNB 必须 `sudo`；**不要用 `| tee`**（会丢退出期探针）；每条腿自己的 `log.filename`；
  跑的时候 agent 完全不动；**OTA 一律由用户跑，一次给一条命令**。
- **文档规则（用户本轮明确）**：handoff 是**会话转移快照，只写不改**；新进展写进活文档
  `s2_full_chain_design.md` / `full_chain_gpu_uma_zero_copy_refactor_plan.md` /
  `soft_hard_decoupled_execution_plan.md`，必要时**新建文件**。

---

## 2. 本会话的提交（接着 `d9f741b31a`）

| commit | 内容 | 关键数字 |
|---|---|---|
| `8f2c3502ca` | S-7e-b2：批量诊断计数、per-engine burst 状态、flush 交接 | `[metal_stats] eq_batch` 上报 |
| `54aeefccc5` | S-7e-b3：按实网形状探设备切片组 | 探针 Pattern H |
| `ec35877066` | S-7e-b2：deferred 组移交探针 | `eq_handoff_probe`（未复现实网缺陷） |
| `76d3fa934e` | S-7e-b2：**按分配身份识别** wrap 缓存 | 并发测试 20/20（此前 1 mismatch/run） |
| `ae9b8ec32c` | S-7e-b2：**批量翻默认** | 触发实网回归，随即回退 |
| `bb1b9ae515` | S-7e-b2：**回退为逐符号默认** | 实网 95% BLER 的教训 |
| `69ab9d3fec` | S-7e-b2：批量也读设备上的估计 | 症状未变（98% BLER）——真因不在这里 |
| **`bc2331efc1`** | **S-7e-b3：按每符号起始表读估计 + 基准只应用一次 + 两条编码路径合并** | 离线 **3732 → 0 字节**；实网 BLER 98% → **25.8%**、attach+ping 通 |
| **`2efa704850`** | **S-7e-b4：批量翻为默认** | dispatch 22→15；`ctest -L phy` 162/162 |

---

## 3. 本会话的关键结论（可直接复用，不必重测）

### 3.1 批量均衡的原缺陷：**每符号起始偏移不均匀**（真根因）

信道估计器按 DM-RS 图样给每个符号发布一个切片：**携带 DM-RS 的符号数据 RE 更少**，于是相邻起点
按 **72、108、72、72、72、108…** 跳（实网 25 PRB 观测：`0 72 180 252 324 396 504 576 648 756 828`）。
批量核原先按**单一元素 stride**（`sym * st.h_stride`）读 ⇒ **从 run 内第 3 个符号起读到别的符号的估计**。
实网表现：PUSCH BLER 10%→95%、RLF、无法 attach。

### 3.2 第二个缺陷：**run 首个估计的基准被应用两次**

flush 既把 `h_run_binding.offset` 加进 Metal 绑定偏移，又把它作为 `p.h_offset` 传给核
（核的 `load_h` 会再加一次）⇒ **凡首个估计不在 0 的 run**（除第一组外全部）都从错位置读。
修法：**在缓冲区基址绑定**，由 `p.h_offset` 承载首个起点（与表的基准一致）。

### 3.3 三个把诊断带偏的坑（**下次务必先排除**）

1. **`metallib` 没跟着源码重编**：一个 marker 实验（往核里塞毒值 `123456`）用 `finally` 还原了
   `.metal`，但 `ocudu_equalizer.metallib` 留在污染态，**之后好几轮读数全部不可信**。
   → 改 `.metal` 后必须确认 metallib 真的重编（`touch` 源文件强制），并留意输出里是否出现毒值。
2. **自制回读探针写错槽位**：核的 `eq`/`nv` 指针会按 `sym * stride` 前进，而解析按紧凑布局读，
   于是"读到全 0"这类结论反复出现且都是假的。
3. **探针自身的分片 bug**：用 `setBuffer:offset:` 把**一个共享表切片**给多个 dispatch 时，
   各 dispatch 读到的是**邻居的切片**（表现为"每个 run 只有第一个符号对"）。
   → 改成**每个 dispatch 一个独立的表 buffer**（几十字节，成本可忽略）后消失。

### 3.4 本地判据（秒级，不需要 air）

```bash
B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
OCUDU_UL_DUMP=/tmp/a $B /tmp/iq1_5480_17921 --metal --out /tmp/a
OCUDU_EQ_DEFER_ENCODE=0 OCUDU_UL_DUMP=/tmp/b $B /tmp/iq1_5480_17921 --metal --out /tmp/b
cmp /tmp/a_5480_17921_llr.bin /tmp/b_5480_17921_llr.bin    # 必须 0 差异
```
测试向量 `/tmp/iq1_*`（187 个 reception，来自 `/tmp/gnb_cap.log`）**不要删**。

### 3.5 受控探针（新增，长期保留）

`lib/phy/upper/channel_processors/metal/test/eq_batch_kernel_probe.cpp` —— 在探针自己掌控的页对齐
缓冲上做两条编码的 A/B，把"数据来源/布局"彻底移出调试：
```bash
./build/lib/phy/upper/channel_processors/metal/eq_batch_kernel_probe 11 72 28672 --dmrs 16384
# 期望：OK: the batched dispatch matches the per-symbol dispatch on every symbol
```

### 3.6 实网数字的可比性（**别误读**）

b8 的 22.2%（逐符号）与 b9 的 25.8%（批量）来自**不同运行**，空中条件/调度/重传时机都不同，
**不能**据此断言"批量差 3.6 个百分点"。可断言的是：修复后的批量**健康**（attach 完成、ping 通、
无 RLF、BLER 与逐符号同量级且不随时间恶化），修复前是 95–98% 且完全无法 attach。
**算术等价**的严格证据是离线回放的 **0 字节差异**。要严格比实网 BLER，需在同一次会话里
用 `OCUDU_EQ_DEFER_ENCODE` 切换做两段统计。

---

## 4. 待办 / 下一步

1. **默认腿（b10）待用户确认**：默认已翻为批量，但实网数据来自**显式 `OCUDU_EQ_DEFER_ENCODE=1`**
   的 b9 腿。命令（无需任何 env）：
   ```bash
   cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
     --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal \
     --expert_phy.pusch_dft_type metal --log.filename /tmp/gnb_b10.log
   ```
   **注意**：配置文件里**没有** `expert_phy` 段（历史上一直靠命令行传），漏掉这几个参数会掉回 CPU 路径。
   启动后先核对 `Built in Release mode using commit 2efa704850` 与
   `[phy_pipeline] ... equalizer=metal`。跑完 Ctrl-C（`[metal_stats]` 只在退出时打印）。
2. **下一个 handoff 的候选方向**（本会话未展开，按优先级）：把 pre-LDPC 链的剩余 dispatch/等待合并
   （`[mmse_time_sum]` 显示 `defer_wait=410us`、`gpu_wait=143us` 仍是主要等待来源）。
3. `s2_full_chain_design.md` §48.31/§48.32 的旧结论（"CE 未写完/可见性"）**已作废**，
   §48.33 是正确结论；再读那两节时不要采信其解释。

---

## 5. 环境与操作要点（沿用）

- 构建：`cmake --build build --target gnb ul_chain_replay eq_batch_kernel_probe -j 8`。
- **改 `.metal` 后务必确认 metallib 重编**（见 §3.3 第 1 条）。
- **提交自检**：`git rev-parse --short=10 HEAD` 必须等于
  `strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$'`，否则二进制是旧的（别用 `/tmp` 里旧日志判断）。
- 用户配置（**不要动**）：`configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml`（AMF 192.168.31.250、
  `clock_source=internal`、`tx_gain: 70`、`rx_gain: 55`、`max_ue_mcs: 20`）。
