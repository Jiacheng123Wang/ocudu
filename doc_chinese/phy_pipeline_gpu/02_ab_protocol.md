# 02 — `gpu` / `cpu_gpu` / `cpu` 三模式共存的 A/B 协议

---

## 1. 为什么三模式天然共存

| 机制 | 位置 | 说明 |
|---|---|---|
| 模式枚举 | `include/ocudu/phy/phy_pipeline_mode.h` | `cpu` / `cpu_gpu` / `gpu` |
| 模式→后端解析 | `du_low_phy_pipeline.h`（`resolve_phy_pipeline`）| 每个模式自行决定后端与 `device_grid`，并做冲突检查 |
| 模式→校验 | `du_low_config_validator.cpp` | 冲突在工厂建立之前就被拒绝 |
| 模式→运行时探针 | `phy_pipeline_mode_registry` | 探针据此决定某条检查是否适用 |

**⇒ 模式之间没有共享的可变数据路径状态**，A/B 不需要额外机制。

### 1.1 唯一的限制：模式是**进程级**的

`phy_pipeline_mode_registry` 是进程级单例，`resolve_phy_pipeline()` 在启动时解析一次。
**⇒ 切换模式必须重启 gNB；A/B 是逐进程的，不是逐跳的。**
（这一点与 `OCUDU_CE_LANE_ORDER` 那类**逐跳**旋钮不同——后者可以在一个进程里 A/B，
但本线的判据不建立在它上面。）

---

## 2. 两类改动，两类判据（**不要混用**）

| 类型 | 例子 | 发布数据的期望 | 判据 |
|---|---|---|---|
| **A. 编排类**（谁提交/谁等待/谁 fence/谁持有缓冲）| 本 session 已落地的 fence、轮转槽位；S1 解锁模式 | **逐字节相同** | **严格网 27/27 逐字节 + 有界网 flips 0** |
| **B. 算术搬家类**（把某个计算从宿主搬到设备）| 将来的"统计量上设备"；K0-a 的比值 | **末位可能不同**（设备的浮点除法不保证正确舍入，K0-a 已实测 2²⁰ 对里 28.4% 不同）| **有界网：差异字节 ≤ 预算且 LLR 判决翻转 = 0**，外加一条专门的设备/宿主一致性门 |

**⇒ 先判定改动属于哪一类，再选判据。** 把 B 类当 A 类判会永远红；把 A 类当 B 类判会放过真缺陷。

---

## 3. 最强度量：`gpu` 与 `cpu_gpu` 的**同后端**对比

`gpu` 模式解析出的后端是 `metal` / `metal_mmse` / `metal` + `device_grid=true`——
**与旧腿脚本显式传的完全一致**。因此：

> **对纯编排类改动，`--phy_pipeline gpu` 与 `--phy_pipeline cpu_gpu`
> 必须发布逐字节相同的 LLR / h / ce。**

这是整条线**最重要的一条判据**：它把"目标是编排性质的、不是数值性质的"变成可执行的检查。
**任何让这两者产生差异的改动，要么归入 B 类并给出门，要么就是错的。**

`cpu` 与另外两者**不**做逐字节对比（`cpu` 用 average 估计器、`cpu_gpu` 用 `metal_mmse`，
数值本来就不同）。`cpu` 的作用是**反面参照**：证明冲突检查与契约的 not-applicable 分支都还活着。

---

## 4. A/B 的执行协议

### 4.1 离线（每条腿都跑，串行）

用旧目录的三张网脚本**只读地**调用（它们是纯比较工具，不含模式假设）：

```
ab_strict.sh   → 严格网（OCUDU_CE_CPU_LS=1）：27/27 逐字节
ab_tol.sh      → 有界网（设备路线）：26/27、最差 8 字节、flips 0
ab_cpu.sh      → CPU 网：27/27 逐字节
```

**⚠ 不要用 `ab_fused_lane.sh` 的红绿当判据**：它 flaky（基线 **2/24** 轮、改动后 **3/28** 轮出现假不匹配，
Fisher p≈1.0），因为它比的是**默认跑 vs 显式 `event` 跑——同一条代码路径**。

### 4.2 空口（需要模式切换，因此逐进程）

```
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu <label>
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu     <label>
```

**跑腿之前**：`ps aux | grep [g]nb` 必须为空（遗留进程会让该腿作废）。
**跑腿之后**读四项：`[phy_pipeline] contract` 块、`[ul_host]`、`[metal_stats]`、
`[ul_gpu_lane]`（若有）。

### 4.3 模式的**负面**用例（防止校验被削弱）

每次动校验层都要跑：

| 输入 | 期望 |
|---|---|
| `--phy_pipeline cpu` + `--expert_phy.pusch_dft_type metal` | **被拒绝**（模式与后端冲突）|
| `--phy_pipeline gpu` + 一个 CPU 后端 | **被拒绝** |
| `--phy_pipeline <拼错的模式>` | **被拒绝**并列出合法值 |
| 不给 `--phy_pipeline`（`auto`）| 解析为 `cpu` 或 `cpu_gpu`，**绝不解析为 `gpu`** |

**必须用真跑路径**：`--dryrun` **静默忽略未知选项**，只有真跑才报
`arguments were not expected`；无 SDR 时走到 `CU-CP failed to connect to AMF` 即代表校验已过。

---

## 5. 每一步的交付格式

每一步在 `wip/` 下落一份 `S<N>_<短名>.md`，包含：

1. **改动**（文件:行）与**属于 A 类还是 B 类**；
2. **判据**（三选一：逐字节 / 契约变化 / A/B 可分辨）；
3. **实测结果**（贴原文，不转述）；
4. **回退方式**（通常是 `git revert <sha>`）；
5. **本步暴露的、留给下一步的东西**。

---

## 6. 已知的环境纪律（沿用旧目录里仍然有效的部分）

1. **同一台机器上不许同时跑两套 GPU 门**（并发会造成假不匹配）。
2. **门必须在 metallib 构建结束之后跑**；`/tmp/build_{nostats,nometal}` 会**原地重编** `.metal`。
3. **`cmake --build build`（`all`）不含** `ul_chain_replay` 与几个 Metal 自测
   ⇒ 改完 `lib/` 必须**显式** `--target` 它们，否则跑的是"新 metallib + 旧宿主"。
   （旧目录的 `run_leg_gates.sh` 已把这一步做成 STEP 0。）
4. `configs/*.yml` 是用户本地、不入库；`doc_chinese/` 是 gitignored。
