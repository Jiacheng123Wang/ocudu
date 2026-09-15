# Session handover #4（2026-09-14，本会话结束时的快照，**只写不改**）

> 本文件是**快照**：写下之后不再回填/修改。后续进展写进新的 handoff 或活文档
> `s2_full_chain_design.md`（新增 **§48.51 / §48.52**）。
>
> 上一份快照是 `session_handoff_2026-09-14-3.md`（HEAD `d01fac7a69`）。
> 命名规则：handoff 按**日期 + 当日序号**，每个日期从 `-1` 开始；本文件是当日的 `-4`。

---

## 0. 一句话现状

**HEAD = `e4d5e68975`**（`apple-silicon`，已 push）。本会话做了**一件事**：
把 §48.50(d) 定的**第一步 `ch_re` gather** 落地 —— 均衡器的 y 输入从"主机逐端口逐符号 memcpy"
改成 **GPU 一次 `gather_ch_re` dispatch 从设备网格取**。三个本地门禁已过（980 抓包 LLR 逐字节一致、
`ctest -L phy` 162/162、120 连跑无崩溃）；**只剩上机腿 b17**。

树状态：干净（只有用户自己的 `configs/*` 与脚本/CSV）。文档是 `doc_chinese/` 下（git-ignored）。

---

## 1. 提交

| commit | 内容 | 结果 |
|---|---|---|
| `e4d5e68975` | **S-7f-1：`ch_re` gather**（详见活文档 §48.51） | ✅ 980 抓包 A/B 逐字节一致、162/162、120 连跑 0 崩溃 |

---

## 2. 关键结论（可直接复用）

### 2.1 本次的收益与代价（本地读数）

- **设备侧**：`[metal_stats] equalizer ch_re device=11 host=0`（每 slot 11 个数据符号各 1 次 gather）。
- **dispatch**：5 → **9**（每 run 多 1 次 gather；一个 slot 4 个 run，因为 DM-RS 把 run 切开了）。
- **GPU busy**：`eq_demap` 54.0 → **76.8µs/lane**（+22.8µs，就是替主机 memcpy 付的钱）。
- **主机侧省下的**：burst staging ≈140µs/slot 的主体（§48.47(d) 的反推）——**必须上机用
  `[ul_equalization_demod]` / `[ul_pipeline]` 验证净收益**。

### 2.2 两个新坑（**已经写进活文档 §48.51(b)，别再踩**）

1. **同一个 `MTLBuffer` 的 `setBuffer:offset:` 字节切片不可靠**。taps/entries 放同一次分配、
   entries 用 `offset=taps_bytes` 绑 ⇒ kernel 读到**别人的字节**（前 2 条 entry 是垃圾），
   症状 = "每个 run 的前 2 个 RE 输出 0 / LLR 为 0"。**每张表各自一个 MTLBuffer** 立刻正确。
   （与 `eq_make_h_starts` 注释里记的是同一件事，这是第二次咬人。）
2. **大对象不要顺手做成成员**。`ch_gather_desc` 有 ~8.6 KB（3300 entry 表），做成
   `pusch_demodulator_impl` 的成员后对象尺寸一变，replay 工具开始**间歇性段错误**
   （`rx_buffer_impl::get_codeblock_data_bits`，约 2–4%/次）。基线 240 连跑 0 崩溃、我的版本
   120 连跑 2–4 次 ⇒ 不是原有抖动。改成 `unique_ptr` 成员后 **120 连跑 0 崩溃**。

### 2.3 一个**既有**缺陷（不是本步引入，选它当 A/B 之前要先修）

`OCUDU_EQ_DEFER_ENCODE=1`（批量 burst 逃生口）下 `channel_equalizer_metal_unit_test` 的
**"batched deferred burst" 项在基线上也 FAIL**（同一项：`submit_group` 两边都 OK）。
默认路径全绿。

---

## 3. 门禁状态

| 门禁 | 状态 | 证据 |
|---|---|---|
| ① LLR 逐字节一致 | ✅ | 980/980（`--metal` vs `OCUDU_EQ_GATHER=0`，同一构建）；host 腿另与基线逐字节一致 |
| ② `ctest -L phy` | ✅ | 162/162 |
| ②' 稳定性 | ✅ | 120 抓包连跑 0 崩溃（基线同条件亦 0） |
| ③ `[ul_gpu_lane]` gap | ⏳ 上机 | 本地 `busy` 升是预期的 |
| ④ 上机不退化 | ⏳ 上机 | 见 §4 |

---

## 4. 下一步（**已定，可立即开工**）

> **b17 已跑完**（2026-09-14 19:06 退出）：功能全绿（`ch_re device=307967 host=0`、RF 失败 1、
> RLF 0），**但这一腿的信道比 b16 差 8–10 dB**（SINR 29–31 → 24.8→16.9 dB，`ul_mac_pdu_size`
> 57.5 → 36.6MB），所以**不能判收益也不能判退化**。完整分析见活文档 **§48.53**。
>
> **b18 第一次作废**（19:14）：UHD USB 故障（`LIBUSB_ERROR_PIPE`）⇒ 进程终止 ⇒ 无探针数据（§48.54）。
>
> **b18b 跑通**（19:20，`OCUDU_EQ_GATHER=0` + `--log.all_level warning`）：**功能确认、判据③通过**
> （`ch_re device=0 host=640992`、`burst dispatches` 4/跳、`gap` median/p95/p99 = 0.6/1.1/**1.3µs**），
> 但暴露两件事（完整分析见 **§48.55**）：
> 1. **`--log.all_level` 本身值 ~45µs**：b16(info) 与 b18b(warning) 是**同一条主机路径**，
>    `[ul_equalization_demod]` median 324.6 → **278.0µs**、`[ul_pipeline]` 697 → **652µs**。
>    ⇒ **今后上机一律 `--log.all_level warning`**，跨腿只在同 level 比。
> 2. **设备 gather 在主机侧花掉了它省的钱**：同构建同 level 下 b17(设备) vs b18b(主机)，
>    `[ul_equalization_demod]` median **305.3 vs 278.0µs**、`[ul_pipeline]` **666 vs 652µs** ——
>    设备腿反而慢 ~27µs/slot，而 GPU 侧明明贵了 27µs/lane（`eq_demap` 75.8 vs 48.7）。
>    **根因已定位**：`eq_flush_hook` **每个 run 重建一次 gather 表**（~5.5KB 拷贝 + 2 次
>    `newBufferWithBytes`）× 4 run/slot ≈ 30µs/slot。
>    ⇒ **下一步第一件事：把 gather 表按 hop 建一次、按 run 只切窗口**（§48.55(e)）。

### 4.1 上机腿 b18（**用户执行，一次一条命令**）：同构建 A/B，把设备 gather 关掉

```bash
cd /Users/jiachengwang/dev/ocudu && sudo -E env OCUDU_EQ_GATHER=0 ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.all_level warning --log.filename /tmp/gnb_b18b.log > /tmp/gnb_b18b_console.log 2>&1
```

（`--log.all_level warning` 是**新增**的：三条腿的日志都是 40–60MB，怀疑它把 USB/宿主挤坏了。）
跑 600–1000 个 slot 即可，**Ctrl-C 正常退出**（探针是 `atexit`，异常终止不打印）。

与 b17 比（同一二进制，只差这个环境变量）：
- `[metal_stats] equalizer ch_re`：b17 `device=307967 host=0` → b18 应变成 **`device=0 host≈307967`**；
- `[metal_stats] burst dispatches`（equalizer 侧）：b17 8/跳 → b18 应回到 **4/跳**；
- `[ul_gpu_lane] busy split eq_demap`：b17 75.8µs/lane → b18 应回到 **≈50µs/lane**；
- `[ul_pipeline]` / `[ul_equalization_demod]` / `mod 3` BLER：**一个量级即视为无退化**。
- ⚠️ 最好 **b17 → b18 → b17'** 交替（每腿跑到 Ctrl-C），否则信道漂移会吃掉结论。
- 另盯 `[ul_gpu_lane] gap` 的 **p99**（b17 是 62.3µs，b16 只有 1.0µs）。

### 4.1b 原 b17 命令（保留备查）

```bash
cd /Users/jiachengwang/dev/ocudu && sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml --expert_phy.pusch_channel_estimator_algo metal_mmse --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal --expert_phy.pusch_ldpc_decoder_type auto --log.filename /tmp/gnb_b17.log > /tmp/gnb_b17_console.log 2>&1
```

手机侧：attach 后跑满上行 iperf3，**一直跑到 Ctrl-C**。

**与 b16 基线（§48.46）比**：
- `[ul_equalization_demod]`（b16: 314.0µs）、`[ul_pipeline]`（b16: mean 697.7 / median 697 / p99 875µs）→ **应降**；
- `[ul_gpu_lane] gap`（b16: median 0.6µs）→ **必须仍 ≈0**；`busy split` 的 `eq_demap` 会升（预期）；
- `[metal_stats] equalizer ch_re device≠0 host=0`、`burst dispatches`（b16: 5）→ **9**；
- 按 `nof_prb mod 3` 分组 BLER（b16 聚合 0.69%）→ **不得退化**；
- `grep -c "Real-time failure in RF"` 个位到几十正常。

### 4.2 下一个工程目标：**K0-c/K0-d**

统计量与相关矩阵在设备上算完并**直写引擎槽位**（§48.50(c)）。它一旦成立，"主机需要导频"这条依赖
就断了，**K0-a/b 才能顺带搬走**（否则是 §48.50(a) 的循环依赖）。完整实施要点见 §48.49。

### 4.3 之后的排队（活文档 §48.52 有表）

F2（DFT 进 lane 队列 + 单 CB + 屏障，目标 `cbs/lane → 1`）→ F3（ring D≥2）→ **LDPC 阶段**
（进阶段第一件事：量它的 busy/gap）。

---

## 5. 操作要点（本次新增/仍有效）

- **设备 gather 逃生口**：`OCUDU_EQ_GATHER=0` 恢复主机 gather（默认**开**设备 gather）。A/B 靠它。
- **本地 A/B（秒级，唯一能调 dispatch 的手段）**：
  ```bash
  B=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
  rm -f /tmp/g1* /tmp/g2*                                   # 必须新前缀（追加模式!）
  OCUDU_UL_DUMP=/tmp/g1 $B /tmp/iq2_10041_17923 --metal --out /tmp/g1
  OCUDU_EQ_GATHER=0 OCUDU_UL_DUMP=/tmp/g2 $B /tmp/iq2_10041_17923 --metal --out /tmp/g2
  cmp /tmp/g1_10041_17923_llr.bin /tmp/g2_10041_17923_llr.bin   # 必须 0 差异
  ```
- **抓包语料**：`/tmp/iq1_*`（187 reception）、`/tmp/iq2_*`（793 reception，含 3300/100 子载波、
  3/6/14/23/24 PRB）——**不要删**，本次的 980 门禁就靠它。
- **`OCUDU_EQ_DEFER_ENCODE=1` 不可信**（§2.3），别拿它当 A/B 参照。
- **提交自检**：`git rev-parse --short=10 HEAD` == `strings build/apps/gnb/gnb | grep -E '^[0-9a-f]{10}$'`
  （本次已确认 `e4d5e68975`）。
- 其余（构建、日志、健康判据、CE 单测、用户配置）见 `session_handoff_2026-09-14-3.md` §5。
