# S-7f-5x 上机交接（2026-09-15，stamp `4624fb7def`）

> 快照，不再修改；后续更新写进 `s2_full_chain_design.md`（§48.143）。
> 上一份：`session_handoff_2026-09-15-6.md`（`0e8a7c8a91`，已上机 PASS）。
> 本文档与 §48.142 的**配方不同**：那一版的前提被实测推翻，真因见 §48.143。

## 1. 这一腿改了什么（一句话）

`llr_span_bytes()` 把**字节步长**也乘了调制阶数，导致 demapper 每跳为 LLR 缓冲申请一个
**大于它自己所在分配**的长度（16QAM 放大 4×、256QAM 放大 8×），shared wrap 缓存因此
**每跳重映射一次**同一段内存。修好单位之后，wrap 长度改取"指针所在分配的剩余整块"。

- commit **`4624fb7def`**，"a demodulation run's LLR length is a byte stride, not a bit count"，
  4 文件 68+/6−（demapper 引擎 ×1、CE 引擎 ×2、CE 实现 ×1）。
- `gnb` 已重建，stamp **`4624fb7def`**（旧 `0e8a7c8a91` 已不在二进制内）。
- 离线证据：soak `wrap replaces` **31 → 0**（`creates 92 → 64`，`failures=0`）；
  六门 237 捕获全 PASS；L1 10/10 + 反向探针 PASS；CE 单测 PASSED；`ctest -L phy` 162/163。

## 2. 这一腿要观察什么

**核心判据（上一腿同一格）**

| 量 | `0e8a7c8a91` 的读数 | 本腿期望 |
|---|---|---|
| `[metal_stats] wrap replaces` | **9409**（≈每跳 1 次）| **≈0** |
| `[metal_stats] wrap creates` | 9500 | 明显下降（≈ −9400）|
| `[metal_stats] wrap failures` | 0 | 0（**不许 >0**）|
| `[mmse_time_sum] submit` | 8.53 µs | 预期下降（那一格里含每跳重映射；**不是**硬判据）|
| `[ul_gpu_lane] busy / gap` | busy 83% ch_est / 17% eq_demap | 不应变差 |

**不许出现的两行**（出现即回传原文）

1. `[E] Metal demapper: a run reaches … bytes but only … bytes are left in the allocation at …`
   —— 新增的一次性点名。它出现说明**还有**某个几何的跨度算错（即本腿没修干净）。
2. `MMSE engine: zero-copy cache hit with a larger request … re-wrapping the buffer`
   —— CE 侧的同类重映射。本轮把 `fd_filter` 也改成按容量 wrap，它应该继续保持 0 次。

**不许退化的量（与 `0e8a7c8a91` 那一腿逐项对比）**

| 量 | 上一腿 |
|---|---|
| `device_corr_builds` / `corr_build_fail` | >0 / **0** |
| `device_y_writes` / `y_write_fail` | >0 / **0** |
| `device_sigma2` / `sigma2=` | 9410=hops / **0.0us** |
| `cbs/lane` / `dropped` | **3.00** / 0 |
| crash / zero-copy warning | **0 / 0** |
| `mean total` | 18.5 µs |
| 手机侧 | attach + ping + iperf3 上行 ≥ 9.78 MB 那一腿的吞吐 |

失败率分母仍然用**墙钟 slot 数**（`elapsed × 1000`），**不要**用 `[ul_pipeline] samples`。
参考：`0e8a7c8a91` 那一腿是 11/140215 = **0.0078%**/slot（预算是 0.1148%）。

## 3. 启动命令

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_s7f5x.log
```

console **不要重定向**（`[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]` 在 stderr）。

**没有新开关**：这次是纯修正，没有 A/B 开关。要 A/B 就用二进制本身：
`git revert 4624fb7def` 回到 `0e8a7c8a91` 的行为（逃生路径也只有这一条）。

## 4. 请回传

1. 完整 console（含 `[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]`）。
2. `/tmp/gnb_s7f5x.log`。
3. 手机侧：attach 是否正常、ping 丢包、iperf3 上行吞吐（与 9.78 MB 那一腿同口径）。
4. 上面第 2 节列的两行"不许出现"的日志，若出现请原样回传。

## 5. 纪律提醒

- 上机期间**不要**并行跑离线活（重活会抢 GPU，也会污染测量）。
- 若 GPU 挂死：`kill -9` 只能释放进程、**释放不了 GPU**；`reboot` 也可能挂 ⇒ 只能强制断电。
  所以任何 GPU 运行都要限时（后台 + 定时 `kill -9`），跑完查 `ioreg -r -c IOAccelerator -d 1`
  的 `Device Utilization %` 与 `recoveryCount`。
