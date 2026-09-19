# S-7f-5w 上机交接（2026-09-15，stamp `0e8a7c8a91`）

> 快照，不再修改；后续更新写进 `s2_full_chain_design.md`（§48.135 / §48.136）。
> 上一份：`session_handoff_2026-09-15-5.md`（当时尚未 commit）。

## 1. 状态

- commit **`0e8a7c8a91`** = "the noise variance is computed in K0-a's own command buffer"。
- `gnb` 已重建，stamp **`0e8a7c8a91`**（旧 `33eee2cdfd` 已不在二进制内）。
- **六个门在 237 个真捕获上全部 PASS**：`sig2`（判据=原始 sigma2，串行复检后 3.15e-06，容差 1e-4）、
  `k0dm`（233 逐字节相同 + 4 条串行复检清除）、`k0d`（237 逐字节相同，167 条确实走了 device build）、
  `ydev`（227 相同 + 10 清除）、`k1`（237 决策相同）、`combos`（10 组合 × 3 捕获一致）。
- 两条曾在并行期报出的"决策差异"经**串行复跑各 3 遍**确认是污染，不是缺陷。

## 2. 这一腿要观察什么

**新出现的量（第一次上机）**

- `[metal_stats]` 里的 **`device_sigma2=N`**：应该 > 0，且与 hop 数同量级
  （它是新阶段的空转探测器；若为 0，说明几何被内核契约拒了 —— 同时会有
  `[PHY] [E] MMSE engine: device noise variance skipped …` 一行点名几何）。
- `[mmse_time_sum]` 里的 **`sigma2=`**：应从 ~3.3 µs/hop 掉到 **~0**（host 不再算它）。
- 若开了 `OCUDU_CE_SIGMA2_CHECK=1`：`[sigma2_check] dev=… host=… rel=… device=1`，`rel` 应在 1e-6 量级。

**不许退化的量（与 `33eee2cdfd` 那一腿逐项对比）**

| 量 | `33eee2cdfd` 的读数 |
|---|---|
| `device_corr_builds` | 9413/9415 = 99.98% |
| `device_y_writes` / `y_write_fail` | >0 / **0** |
| `corr` / `stage` | 11.0 → 0.0 µs / 2.46 → 0.06 µs |
| `mean total` | 35.3 → 21.2 µs（应再降 ~3 µs）|
| `cbs/lane` | **3.00** |
| crash / zero-copy warning | **0 / 0** |
| 手机侧 | attach + ping + iperf3 上行 ≥ 9.78 MB 那一腿的吞吐 |

失败率分母仍然用**墙钟 slot 数**（`elapsed × 1000`），**不要**用 `[ul_pipeline] samples`。
参考：`33eee2cdfd` 是 14/117858 = 0.0119%/slot。

## 3. 启动命令

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_s7f5w.log
```

console **不要重定向**（`[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]` 在 stderr）。

**可选 A/B（同一次上机，值一条腿）**：`OCUDU_CE_DEV_SIGMA2=0` 让 host 回到 `estimate_sigma2()`：

```bash
sudo -E env OCUDU_CE_DEV_SIGMA2=0 ./build/apps/gnb/gnb …（同上）
```

这条 A/B 现在才是**真 A/B**：参考实现已改成用与它所平滑的那份 LSE 匹配的 CFO
（§48.135(i)），所以"只差 sigma2"这一件事。对比 `[mmse_time_sum] sigma2=` 与吞吐即可。

## 4. 请回传

1. 完整 console（含 `[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]`）。
2. `/tmp/gnb_s7f5w.log`。
3. 手机侧：attach 是否正常、ping 丢包、iperf3 上行吞吐（与 9.78 MB 那一腿同口径）。
4. 若出现任何 `[PHY] [E] MMSE engine: … skipped` 或 `[mmse_ce] device LSE build failed`，原样回传。

## 5. 纪律提醒

- 上机期间**不要**并行跑我的离线活（重活会抢 GPU，也会污染测量）。
- 若 GPU 挂死：`kill -9` 只能释放进程、**释放不了 GPU**；`reboot` 也可能挂 ⇒ 只能强制断电。
  所以任何 GPU 运行都要限时（后台 + 定时 `kill -9`），跑完查 `ioreg -r -c IOAccelerator -d 1`
  的 `Device Utilization %` 与 `recoveryCount`。
- 万一这一腿失败：`git revert 0e8a7c8a91` 即可回到 `33eee2cdfd` 的行为
  （逃生开关 `OCUDU_CE_DEV_SIGMA2=0` 也能单独关掉新阶段，不必回滚整笔提交）。
