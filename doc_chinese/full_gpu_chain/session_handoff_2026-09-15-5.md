# S-7f-5w 交接（2026-09-15 深夜）——device sigma2：离线验证已完成，只差真语料上的门 + OTA

> 上一份交接：`session_handoff_2026-09-15-4.md`（S-7f-5v 的 `33eee2cdfd`，已 OTA 通过）。
> 本章的详细证据在 `s2_full_chain_design.md` **§48.135**（含全部原始输出与推理链）。

## 0. 一句话状态

S-7f-5w（把每个 hop 的噪声方差搬进 K0-a 的 command buffer）**代码完成并已离线验证**：
L1 10/10 PASS、合成捕获上的 5 个门全 PASS、CE 单元测试 PASS、`ctest -L phy` 162/163。
**尚未 commit，尚未重建 `gnb`**（磁盘上的 `gnb` 仍是 `33eee2cdfd` 的产物）。
唯一缺的是**真捕获语料上的门**（`combos` 只能真语料），之后即可 commit → 重建 → OTA。

## 1. 这次改动包含了什么（8 个文件，785 行）

| 文件 | 内容 |
|---|---|
| `ocudu_mmse_pilots.metal` | 两个新内核 `mmse_pilots_fd_smooth`(TG=128) / `mmse_pilots_sigma2`(TG=256) |
| `ocudu_metal_mmse_engine.{h,mm}` | `pilots_stage` 新字段 + 两次 dispatch（同一个 CB）+ `sigma2_done` 契约 + `[metal_stats] device_sigma2` |
| `port_channel_estimator_metal_mmse_impl.{h,cpp}` | 新阶段的接线、`OCUDU_CE_DEV_SIGMA2` 开关、`[sigma2_check]` 探针、**`cfo_ref` 修复** |
| `port_channel_estimator_helpers.{h,cpp}` | `get_fd_smoothing_filter()` |
| `capture_gates.sh` | 新模式 `sig2` + 发现名单走文件（见下） |

**WIP 补丁已存**：`wip/s7f5w_sigma2_2026-09-15.patch`
（1111 行，sha256 `ac05ad0bd0633e3c…`；基线 commit `33eee2cdfd`）。
—— 上次 `git checkout --` 丢掉过未提交的内核，**改文件前先刷新这个补丁**。

## 2. 本轮抓到的 4 个真缺陷（都已有实测证据）

1. **MSL 的 `float2 * float2` 是逐分量乘**，不是复数乘（与 `std::complex` 不同）。
   两处改成显式 `mmse_cmul` 后 `npt=2` 从"全部不匹配"变成 **0.00e+00**。
   **新条目：MSL 里凡复数运算都要显式写复数乘。**
2. **漏了调用方的 `1/beta`**：宿主平滑的是已乘过 `1/beta` 的导频 ⇒ 加 `inv_beta` 参数。
3. **静默路径**：`sigma2_ok==false` 时引擎仍返回 true，调用方却按"指针非空"判定 ⇒ 会读到没写过的标量。
   修：`pilots_stage::sigma2_done` + 越界一次性 `[E]` 日志；反向探针 `L1_REFUSE` 实测 PASS。
4. **`DEV_SIGMA2` 的 A/B 曾经比的是两个不同的 CFO**（本轮的真正发现）：
   K0-a 把 `pilots_lse_view` 覆盖成 device 的 LSE 并施加 **device 自己的 CFO**，
   而 `estimate_sigma2()` 用宿主的 `args.cfo_hop` 旋转残差 ⇒ 两个估计器不同时凭空出现 1.4% 差异
   （4 层捕获）。修：`cfo_ref = device_ls_valid ? gpu_ls_cfo[0] : args.cfo_hop`。修后 6.5e-08~1.3e-07。

另外修掉一个**门脚本自己制造的假红**：跨 shard 拼 `BAD` 时空表变成 `" "` ⇒ 四个全绿的门打印
`MISMATCH:  `（空）并 rc=1。现在发现名单走文件。

## 3. 语料：真语料没了，但**合成捕获**顶上来了

重启清空了 `/tmp`，980 个真捕获全丢（仓库内无副本）。捕获格式完全可重建，
所以写了 **`lib/phy/upper/signal_processors/channel_estimator/metal/make_synthetic_capture.py`**（已进仓库，`/tmp` 不可靠）（`key=value` + port×14 符号×`bwp*12` 的 complex64；
可指定 PRB / DM-RS 符号 / 层数 / 端口数）。**注意还要给每个前缀补一个 `_ce.txt` 占位文件**，
门是按 `*_ce.txt` 发现捕获的。

**合成语料能验证什么**：门比的是"同一输入下 device 与 host 是否一致"，与数据真不真实无关
（而且它抓到了上面第 4 条真缺陷）。**不能**覆盖的只有：真实几何/噪声分布，以及解出 OK 的那一类捕获
（合成导频是随机的，所以三个捕获全是 `crc=KO`）。

`combos` 已改成**自基线**：10 种开关组合必须在**同一捕获**上复现默认组合的决策
（crc 相同、|dSINR| ≤ 0.2 dB）；三个旧捕获降级为**可选绝对锚点**（在就比、不在就打印"未检查"）。
所以它现在在**新录的语料上也能跑**（本轮之前它会因为找不到那三个 slot/rnti 而硬失败）。

**重新录制真语料**（被动录制，不是测试；手机附着跑一会儿流量即可）：

```
OCUDU_UL_DUMP=/tmp/iq1 OCUDU_UL_DUMP_COUNT=<N> ./gnb <config>      # 另一路用 /tmp/iq2
```

## 4. 下一步（顺序不要换）

1. **真语料到位后**：`capture_gates.sh` 依次跑 `sig2` / `k0dm` / `k0d` / `ydev` / `k1` / `combos`
   （**串行**；replay 工具在并行下会给出错误结果，脚本内有串行复检）。
2. commit（含 §48.135 的文档）。
3. 重建 `gnb` 并记录新 stamp。
4. 写 OTA 交接 → 用户上机。

## 5. 上机判据（沿用 `33eee2cdfd` 那一轮，用于对比）

- `[metal_stats]`：`device_sigma2=N/总 hop 数`（必须 >0 且与 hop 数同量级）、`device_corr_builds`、
  `device_y_writes`、`y_write_fail=0`；`cbs/lane=3.00`。
- 0 crash / 0 zero-copy warning；`[mmse_time]` 的 `sigma2=` 一项应从 ~3.3 µs/hop 掉到 ~0。
- 手机侧：attach + ping + iperf3 上行吞吐不低于上一轮（**性能不得退化到影响附着/吞吐**）。
- 失败率用**墙钟 slot 数**做分母（`elapsed × 1000`），不用 `[ul_pipeline] samples`。

## 6. 纪律（每一条都是付过代价的）

- **判定几何是否越界前先读代码**：内核常数与宿主真上限逐条对齐（§48.135(d) 的表）。
- **绿色的空转测试不是证据**：CE 单元测试的 `device_sigma2=0`，它对新阶段什么也没测。
- **GPU 内核必须对任意参数都终止**（挂死 = 强制断电，`kill -9` 与 `reboot` 都救不了）。
- 任何 GPU 运行都要**限时**（后台 + 定时 `kill -9`），跑完查 `ioreg` 的
  `Device Utilization %` / `recoveryCount`。
- 用户上机测量期间**不要跑任何重活**。
- 门脚本改一行也要**双向验证**："打印 MISMATCH 却没有内容"比没有门更糟。
