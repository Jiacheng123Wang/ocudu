# S1 — 解锁 `--phy_pipeline gpu`

**提交**：`c69167dfdb`（已推送；Ubuntu `BUILD_RC=0` / `ctest -L phy` 164/164）
**改动**：`apps/units/flexible_o_du/o_du_low/du_low_config_validator.cpp` —— 删掉 `gpu` 分支里那句
无条件的 `"not implemented yet"` + `return false`，**保留**第一层
`check_phy_pipeline_lane_available()`。
**类别**：A 类（编排/配置）。**不碰数据路径。**

---

## 1. 为什么删它是正确的

`phy_pipeline_mode.h` 已把 `gpu` 定义成一条**可检验的性质**（"只有两次 host↔device 数据穿越"），
而 `resolve_phy_pipeline()` 的 `gpu` 分支解析出的后端是
`phy_pipeline_lane_defaults`（`metal` / `metal_mmse` / `metal`）+ `device_grid=true`
——**与模块级腿脚本已经显式传的完全一致**。

**⇒ 选这个模式改变的是"编排声称"，不是"数据路径"。**
而**声称要靠测量来验证，不是靠拒绝运行来保证**。运行时契约
（`phy_pipeline_contract.h`）才是执行者。

**它也是"差距量不出来"的原因**：旧腿脚本写死 `cpu_gpu`，而该模式的定义就是
"每个模块边界保留一次穿越"——模式选不出来，就没有任何一条腿能陈述目标要的性质，
契约在那个模式下也就无物可查。

---

## 2. 判据与实测（**全部在真跑路径上**，不用 `--dryrun`）

| 输入 | 改动前 | 改动后 |
|---|---|---|
| `--phy_pipeline gpu` | 退出：*"… is not implemented yet; use --phy_pipeline cpu_gpu …"* | **通过校验并打开电台** ✔ |
| `--phy_pipeline cpu` | 通过校验、打开电台 | **不变** ✔ |
| `--phy_pipeline cpu_gpu` | 通过校验、打开电台 | **不变** ✔ |
| `gpu` + `--expert_phy.pusch_channel_estimator_algo cpu` | — | **被 validator 拒** ✔ |
| `gpu` + `--expert_phy.pusch_channel_equalizer_backend cpu` | — | **被 validator 拒** ✔ |
| `cpu` + `--expert_phy.pusch_dft_type metal` | 被拒 | **仍被拒** ✔ |
| `gpu` + `--expert_phy.pusch_dft_type auto` | — | 接受（车道自选后端，符合定义）✔ |
| `--phy_pipeline gpux` | 被 CLI schema 拒 | **仍被拒**，并列出 `[auto,cpu,cpu_gpu,gpu]` ✔ |
| 未知的 channel-estimator 值 | 被 CLI schema 拒 | **仍被拒**，并列出合法值 ✔ |

**离线门**：三张网 `AB_ALL_RC=0`（严格 27/27 逐字节、有界 26/27 最差 8 字节 **flips 0**、CPU 27/27）、
七门 `ALL_GATES_RC=0`、Metal 自测 6/6、`ctest -L phy`、两条配置构建门 RC=0。

---

## 3. 顺带修掉：**我自己新脚本里的一个缺陷**

`wip/run_leg.sh` 第一版把"每个模块的 backend 旋钮"**全部留给模式去解析**。对 `gpu` 是对的，
但对 `cpu_gpu` **是错的**：`resolve_phy_pipeline()` 的 `cpu_gpu` 分支在旋钮为 `auto` 时
把它们解析成 **CPU** 后端（`out.dft = "cpu"` 等）——**那样 `cpu_gpu` 这条臂会变成 CPU 流水线，
拿它跟自己对拍**。

已修：旋钮**只为 `cpu_gpu` 传**（那正是该模式的定义），`gpu` 与 `cpu` 不传。
理由写进脚本注释，并已实测三条臂都能起来。

**⇒ 这条也写进纪律**：A/B 的两条臂必须**各自陈述它们声称的模式**，
否则比较的是"同一件东西"。

---

## 4. 上机（手机 OTA，ping/iperf3）：两条臂都跑通，**`mode=gpu` 有史以来第一次上机**

两条腿都由用户跑（`sudo -E bash wip/run_leg.sh <mode> <label>`），二进制版本串都是 `c69167dfdb`。

| | `gnb_cpu_gpu_baseline_0918_1911` | `gnb_gpu_s1_0918_1913` |
|---|---|---|
| 契约首行 | **`mode=cpu_gpu`** | **`mode=gpu`** ← 首次 |
| 契约 | **7/7 MET** | **7/7 MET** |
| `host sample assembly` | `945462 of 945462 … 0 copied (mode=cpu_gpu)` → OK | `664706 of 664706 … 0 copied (mode=gpu)` → OK |
| `Real-time failures` | 2 / 67539 = **0.003%**（预算 0.0116%）| **0 / 47485** |
| crc OK/KO | 818 / 247 | 760 / 105 |
| `lanes` / `cbs/lane` | 1065 / **3.00 (max 3)** | 865 / **3.00 (max 3)** |
| `lane fence` | signals=2130 = **2×lanes** | signals=1730 = **2×lanes** |
| `device_sigma2` | 1065 **== lanes** | 865 **== lanes** |
| busy split（`ch_est`/`ch_wt`/`eq_demap`）| 93.1 / 320.3 / 75.2 µs（19/66/15%）| 95.9 / 337.2 / 76.0 µs（19/66/15%）|

**基线那 2 次失败是 `Real-time failure in RF: underflow`**（同一毫秒内的两条，即一次电台/USB 事件），
**不是 PHY/GPU 失败**；而且它落在**未改动的 `cpu_gpu` 臂**上（S1 只删了 `gpu` 分支里的拒绝，
`cpu_gpu` 永远不进那个分支）。

**⇒ 两条臂在结构上完全一致**：同样的 lane 结构（`cbs/lane=3.00`、fence 每 lane 2 次）、
同样的"设备在干活"计数器、同样的 busy 比例。
**这正是 S1 应有的结果**：它是 A 类（编排/配置）改动，
**解锁模式没有关掉任何差距——它只是让那条差距第一次可以被陈述和测量。**

**⚠ 两条腿的 crc KO 率（247 vs 105）不可比**：它们是两次独立的 OTA 会话（时长、时隙数、
手机位置都不同），不是受控对比。**不要从这里读出任何结论。**

## 5. 本步暴露、留给下一步的

1. **`lane_fused` 仍然只被打印**（`du_low_config_translator.cpp:48` 是唯一消费点），
   所以 `gpu` 模式现在能跑，但**编排上还没有任何东西按它分支**。两条臂跑的是同一条流水线。
2. **契约里没有任何一条检查在数"还剩几次穿越"** ⇒ 目标仍然不可测。**这是 S2**：
   两条臂的契约都"7/7 全过"，却**没有一条在说"只剩两次穿越"**——目标的性质今天无人检验。
3. 二进制版本串已重打（`touch build/hashes.h && cmake --build`），两条腿的 boot line 都是 `c69167dfdb`。

## 6. 回退

`git revert c69167dfdb`。回退后 `gpu` 重新被拒，其余一切不变。
