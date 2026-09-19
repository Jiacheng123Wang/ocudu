# S-7f-5y 上机交接（2026-09-15，stamp `3e30b22444`）

> 快照，不再修改；后续更新写进 `s2_full_chain_design.md`（§48.145）。
> 上一份：`session_handoff_2026-09-15-7.md`（`4624fb7def`，已上机 PASS、G5 关闭）。

## 1. 这一腿改了什么（一句话）

host pre-stage 每跳重算一份 LS 导频，而设备（K0-a）随后把同一块缓冲**覆盖**掉 ⇒ 那是"算了就扔"。
现在 stage 在跑之前回答"这批导频我来算"，基类据此**跳过** pre-stage；CFO 由设备随导频一起返回。
顺带修掉一处只有跳过才会暴露的缺陷：`args.cfo_hop` 同时是**噪声归约的 CFO 补偿输入**，
不跳过时它由 pre-stage 填、跳过时成了 `nullopt`（详见 §48.145(d)）。

- commit **`3e30b22444`**，4 文件 206+/58−（其中 2 个是**共享基类** `port_channel_estimator_average_impl`）。
- `gnb` 已重建，stamp **`3e30b22444`**（旧 `4624fb7def` 已不在二进制内）。
- 离线证据：**跳过 vs 不跳过 24/24 捕获 LLR 逐字节相同**；soak `replaces=0`、32/32 CRC OK；
  六个门全 PASS 且 `combos` 表与改前**逐项相同**；L1 10/10 + 拒绝探针；CE 单测 PASSED；`ctest -L phy` 162/163。

## 2. 这一腿要观察什么

**本腿的预期是"结果不变、少一点宿主活"**——它删的是死输出，所以**没有**要出现的新读数；
要确认的是**没有回归**。

| 量 | `4624fb7def` 的读数 | 本腿期望 |
|---|---|---|
| `[mmse_time_sum] pre` | 0.71 µs | 略降（死的那部分离线占 3.31 → 2.77 µs = −16%）|
| `[mmse_time_sum] mean total` | 16.7 µs | 不退化 |
| `[metal_stats] wrap replaces / failures` | **0 / 0** | **0 / 0**（G5 不许回来）|
| `device_sigma2` / `hops_gpu` | 10460 / 10460 | 都是 hops（100%）|
| `device_corr_builds` / `corr_build_fail` | 10459 / 0 | >0 / **0** |
| `device_y_writes` / `y_write_fail` | 14696 / **0** | >0 / **0** |
| `cbs/lane` / `dropped` | 3.00 / 0 | 3.00 / 0 |
| `ch_est device` / `ch_re device` 的 `host` | 115060 / 0 | 同形，`host=0` |
| 失败率 | 4 / 97403 = **0.00411%** | 同量级（预算 0.1148%）|
| 上行 `[ul_mac_pdu_size] total` | 8.94 MB / 97.4 s | 不低 |
| 手机侧 | attach + ping + iperf3 正常 | 同（**这次仍可用 gNB 侧 PDU 总量代替**，见 §4）|

**不许出现的行**（出现即回传原文）

1. `[W] [mmse_ce] device LSE build failed: running the host pre-stage for this hop`
   —— 新增的冷路径告警。它出现说明 K0-a 在本腿真的失败过（那才是要查的事）。
2. `[E] Metal demapper: a run reaches … bytes but only … are left in the allocation at …`
3. `MMSE engine: zero-copy cache hit with a larger request … re-wrapping the buffer`

## 3. 启动命令（比上一腿多一行 `info`）

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level info --log.filename /tmp/gnb_s7f5y.log
```

`info` 在这条链上只多**5 行一次性记录**（我核实过：全部由 `path_logged`/静态标志/构造期守卫），
热路径零成本、不扰动测量。其中 3 行是这一腿想要的独立佐证：

- `Metal demapper: inputs … , LLRs written in place, engine no-copy wrap OK`（LLR 真在原地写）
- `Metal equalizer: outputs written in place, engine no-copy wrap OK`
- `PUSCH: estimates read device of the demodulation (OCUDU_CE_CPU_CE=0)`

`debug` **不要常开**：CE 的 `[mmse_time] prb=… gpu=…` 是每跳一行，会灌满并扰动测量。

**没有新开关**；语义变化只有一条：`OCUDU_CE_LS_CHECK=1` 现在**同时**会让宿主 pre-stage 照跑
（它本来就要两侧都有）。逃生路径仍是 `OCUDU_CE_CPU_LS=1`（整条回宿主）与
`git revert 3e30b22444`。

## 4. 请回传

1. 完整 console（含 `[metal_stats]` / `[mmse_time_sum]` / `[ul_gpu_lane]`）。
2. `/tmp/gnb_s7f5y.log`（这次是 `info` 级）。
3. 手机侧：**可以不做**。上一腿的口径已证明 gNB 侧 `[ul_mac_pdu_size] total` 与"attach + iperf3 真的跑通"
   等价（PDU 是 CRC 通过后才交付的）；本腿若要顺手记一行 ping 丢包 + iperf3 吞吐更好，
   但不值得为此单独重跑一遍。
4. 上面第 2 节列的三行"不许出现"的日志，若出现请原样回传。

## 5. 纪律提醒

- 上机期间**不要**并行跑离线活（重活会抢 GPU，也会污染测量）。
- 若 GPU 挂死：`kill -9` 只能释放进程、**释放不了 GPU**；`reboot` 也可能挂 ⇒ 只能强制断电。
  所以任何 GPU 运行都要限时（后台 + 定时 `kill -9`），跑完查 `ioreg -r -c IOAccelerator -d 1`
  的 `Device Utilization %` 与 `recoveryCount`。
