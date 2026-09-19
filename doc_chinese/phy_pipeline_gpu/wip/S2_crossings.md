# S2 — 让目标的"穿越次数"可测

**提交**：`ca9760b104`（已推送；Ubuntu `BUILD_RC=0` / `ctest -L phy` 164/164；二进制版本串已重打）
**改动**：新增 `include/ocudu/phy/phy_pipeline_crossings.h`（计数器 + 一条契约检查）；
估计器里 4 处设备标量读取点各加一次 `count_host_read()`，设备跳加一次 `count_device_hop()`。
**类别**：A 类（只读计数器 + 报告行）。**不碰数据路径**。

---

## 1. 为什么必须有这一步

`phy_pipeline_mode.h` 把 `gpu` 的目标写成**穿越次数**（"只有两次 host↔device 数据穿越：
IQ 上传、LLR 下载"）。**但没有任何东西在量它。**

契约原有 7 条检查数的是**样本**（`host sample assembly`）、**网格**（`zero-copy wraps`）、
**一次补偿往返**（`cfo compensation`）——它们**在每一条腿上都全过**，
**包括 `mode=gpu` 的头两条腿**，而那时估计器**每跳仍在读三个设备标量并把结果写回核参数**。

**⇒ 一个"声称了却没被测量"的性质，只能被断言，不能被达到**；而被测量的那个性质（样本拷贝）
在 24 条腿之前就已经达成并保持住了。

---

## 2. 口径

> **一次穿越** = 宿主**取走**设备产出的数据，**或**把**由设备数据算出来的东西交回**设备。
> **不计数**：提交、fence、选后端（**控制**，不是数据）；
> **也不计数**：IQ 上传与 LLR 下载（模式**明确允许**的那两次）。

**计数点就在读取处**，一次读一次自增——**不是靠减法推断出来的**。
（本 session 已有两次"用差值归因"得出错误结论的先例：§48.194(g-nonies) 与 (g-vicies)。）

## 3. 判据的归属

| 模式 | 计数 | 判定 |
|---|---|---|
| `cpu` | 打印（预期 0 / 0 跳）| **不适用**（没有设备参与）|
| `cpu_gpu` | **打印真实次数** | **不适用**——该模式的定义就是"每个模块边界保留一次穿越"，它在这里没有做任何声称 |
| **`gpu`** | 打印真实次数 | **判定**：必须为 **0** |

**计数在所有模式都打印**，因为那是这条线要推到零的数字；把它藏在还没达到的模式里，
正是这条检查要纠正的错误。

---

## 4. 实测

### 4.1 设备路线上量到的就是那个差距

`ul_chain_replay`（`--metal`，1 跳）：

```
[phy_pipeline]   host device data crossings: 3 host read(s) of device-produced data over 1 device hop(s)
                 = 3.00 per hop; the fused lane (mode=gpu) allows 0 … -> not applicable
```

**3.00/跳** —— 与 `00_goal_and_gap.md` §3.2 的分析**逐个对上**（CFO / σ² / power sum 三次回读）。

### 4.2 两条判定分支都验过（用一次性的本地实验，已回退）

gNB 当时用不了（**AMF 不可达**，CU-CP 在 PHY 注册任何检查之前就中止），
所以在 replay 工具里**临时** publish 一个模式来验证，验完即回退（`git status` 干净）：

```
mode=gpu     -> "… 3.00 per hop … -> FAILED"
                "[phy_pipeline] contract NOT MET: 1 of 3 applicable checks failed (mode=gpu)"
mode=cpu_gpu -> "… 3.00 per hop … -> not applicable"
                "[phy_pipeline] contract MET (2 of 4 checks applicable)"
```

**契约失败不会中止**（`report_phy_pipeline_contract` 的注释明写："the report runs at exit"）：
声称靠**可见**来执行，不靠杀掉进程。

### 4.3 离线门

`AB_ALL_RC=0`（严格 27/27 逐字节、有界 26/27 最差 8 字节 **flips 0**、CPU 27/27）、
`ALL_GATES_RC=0`（七门）、Metal 自测 6/6、`ctest -L phy`、两条配置构建门 RC=0。

### 4.4 上机（手机 OTA）：**目标第一次被实测，数字正是 3.00/跳**

腿 `gnb_gpu_s2_0918_1929`（`mode=gpu`，用户跑）：

```
[phy_pipeline] contract (mode=gpu):
[phy_pipeline]   radio sample continuity: 0 gaps over 64764 blocks …                                          -> OK
[phy_pipeline]   dft radio inputs: 197988 of 197989 transforms read the radio buffer                          -> OK
[phy_pipeline]   zero-copy wraps: 66641 hits, 25409 creates, 0 replaces, 0 failures, 0 misaligned            -> OK
[phy_pipeline]   ce device estimates: 34914 device, 0 host                                                    -> OK
[phy_pipeline]   host device data crossings: 9522 host read(s) of device-produced data over 3174 device
                 hop(s) = 3.00 per hop; the fused lane (mode=gpu) allows 0 …                                 -> FAILED
[phy_pipeline]   cfo compensation: 0 round trips over 906612 symbols, 0 commands, offset 0.000 Hz             -> OK
[phy_pipeline]   baseband metrics: 0 symbols measured for 906612 processed (metrics disabled)                 -> OK
[phy_pipeline]   host sample assembly: 906612 of 906612 symbols read where the radio put them, 0 copied …     -> OK
[phy_pipeline] contract NOT MET: 1 of 8 applicable checks failed (mode=gpu)
```

其它量：`Real-time failures 0 / 64764`、crc `2685 / 489`、`lanes=3174`、`cbs/lane=3.00 (max 3)`、
`lane fence signals=6348 = 2×lanes`、`device_sigma2=3174 == lanes`、
busy split `113.9 / 333.6 / 74.2 µs`（22/64/14%）。

**⇒ 两个关键观察：**

1. **`9522 / 3174 = 3.00` 与离线预言逐个对上**（CFO / σ² / power sum 三次回读）。口径可信。
2. **对比 S1 腿**（同一模式、同一二进制家族，只是还没有这条检查）：
   那时契约写的是 **`contract MET (7 of 7)`** —— **目标没达到，契约却说全过**。
   现在它写 **`NOT MET: 1 of 8`** 并把数字指出来。**这正是 S2 的全部意义。**

**⚠ 顺带修掉 reader 自己的一个 bug**：`wip/leg_report.sh` 的 block 终止符只匹配 `contract MET`，
**不匹配 `contract NOT MET`** ⇒ 契约**失败**的腿（恰恰是最该看的那种）会打印空白。
已修，并加了注释说明。

---

## 5. 本步暴露、留给下一步的

1. **目标第一次有了机器可读的数字：`3.00 次/跳`。** 下一步（S3）就是把它降到 3.00 → 0.00。
2. **S3 的假设需要 A/B 证实**：核在 `sigma2_from_device` 置位时不读 `p.sigma2`
   （`ocudu_mmse_corr.metal:151`），所以宿主读回来的东西**对 GPU 是死的**。
   **判据 = 旋钮开关，发布数据逐字节相同。**
3. **尚未计入的穿越**：宿主建相关矩阵（`:1440/:1720/:1858/:1953`，视路线）。它今天**没有**被这条检查计数
   ——因为它是"宿主自己造数据"而非"读设备产出"。**S4 处理它时要把口径想清楚**
   （它是 CPU 在数据流里，但不是本检查定义的那种穿越）。
4. `[ul_host] metrics` 的语义仍未确认（`00_goal_and_gap.md` §7 第 5 项）。

---

## 6. 回退

`git revert ca9760b104`。回退后契约回到 7 条检查。
