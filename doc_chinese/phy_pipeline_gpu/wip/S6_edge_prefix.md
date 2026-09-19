# S6（乙）— 把 edge 的构造并进**引擎自己那条命令缓冲**

**状态**：**已实施，默认关闭**（`OCUDU_CE_EDGE_FUSE=0`）。默认路线**逐字节未变**（见 §4 的三张网）。
**结论**：机制正确、可用，但它**没有**像预期那样顺手修掉甲（S4）：设备侧 edge 构造的两个形态
（独立 CB / 融合前缀）**都与宿主的构造不同**，而且**互相也不同**。

---

## 1. 实施内容

| 位置 | 改动 |
|---|---|
| `ocudu_metal_mmse_engine.mm` `encode_run`（声明 `:1485`、定义 `:1628`）| 新增末位参数 `const corr_stage* corr_edge`；在既有 `corr` 块之后加一段同形状的块（`encode_corr` → `mmse_stats_corr_build()` → barrier）；`first_pipe = corr_a_pipe` 的条件改为 `corr \|\| corr_edge` |
| 同文件 `run_async`（声明 `.h:557`）| 新增末位参数 `corr_edge = nullptr`，透传给 `encode_run` |
| 同文件其余 `encode_run` 调用点（同步 `run`、`run_weights_only*`）| 传 `nullptr`（它们的语义不需要第二个前缀）|
| 估计器 `build_slots_on_device` | 新增出参 `corr_stage* fused_corr = nullptr`；**仅当 `fused_corr != nullptr && gpu_invert`** 时不提交独立 CB，改为回填 stage 并把 `nof_systems` 一并写入 |
| 估计器 `engine_run` | 新增末位参数 `corr_edge = nullptr`，传给 `run_async` |
| 估计器合并分支 | 新增 `std::optional<corr_stage> edge_corr`，在 `gpu_invert && edge_fuse_enabled()` 时取址交给 `build_slots_on_device`，并作为 `corr_edge` 传入合并的那一次 `engine_run` |

新增 A/B 开关 `OCUDU_CE_EDGE_FUSE`（`edge_fuse_enabled()`，**默认 0**）。做成开关而不是默认开启，
理由写在函数注释里：两种形态必须逐字节相同，而**判据必须能同时看到两边**——否则一个"算得不一样"
的融合形态与一次胜利无法区分。

### 1.1 实施中真实踩到的一个坑（已修，并留作注释）

`corr_stage::nof_systems` 由 `correlation_stage()` **不设置**（它是 0，因为那个函数不知道调用者要建几个
system）；独立入口 `build_correlation(corr_std, nof_systems)` 是把它当**参数**收的，所以独立形态一直是对的。
融合形态下 0 的含义是"**整个 batch**"——对一个合并 batch 就是 `2 * nof_layers` 个 **edge 几何**的 system，
**直接覆盖标准组的槽位**（正是 `corr_stage::nof_systems` 自己的注释警告过的那件事）。
实测：`_llr` 差 36527 字节（对照独立形态是 1400）。修法与 `std_corr_prefix->nof_systems = nof_layers`
（`:1560`）一致。

### 1.2 顺带查清的一件事：同步入口 `run()` 没有 `corr`，**不是**缺陷

`mmse_engine::run`（`.mm:1501`，声明 `.h:451`）没有 `corr` 参数，它的 `encode_run` 调用传 `nullptr`。
曾担心"`gpu_invert` 下同步路线会用空槽位"。查调用图后否定：
`engine_run` 只在 `matrix == false && defer == false` 时走到 `run`；而 `defer == !matrix_on` 且**合并分支要求
`!matrix_on`**，所以合并分支恒有 `defer == true` → 恒走 `run_async`。
`matrix == false && defer == false` 即 `!matrix_on && matrix_on`，**不可达**。⇒ 无需改动。

---

## 2. 判据与实测（全部串行、`ab_dumps.sh`）

| 测量 | 结果 |
|---|---|
| 估计器单元测试（含 Test 11 合并/拆分、Test 13 三种 lane 序）| **All tests PASSED** |
| 自检：`""` vs `""` | 27 captures，missing-dumps=**0**，差异=**0** |
| CPU 路线：`OCUDU_CE_CPU_LS=1` vs 同 | 27 captures，差异=**0** |
| **`TAIL_DEV=0` vs `TAIL_DEV=1`（默认，融合关）** | 15/27 不同，**6043** 字节（`_llr` 1400 / `_h` 4295 / `_ce.txt` 348）|
| **隔离：`TAIL_DEV=1` vs `TAIL_DEV=1 EDGE_FUSE=1`** | 15/27 不同，**50590** 字节（`_llr` 35312 / `_h` 14112）|

**第一行是关键的安全判据**：6043 与 S4 记录的**改动前**数字**完全一致** ⇒ `EDGE_FUSE` 默认关闭时，
本步对产品路线**逐字节无影响**，与 §1 的结构论证（`fused_edge` 恒为 `nullptr`）互为印证。

**第二行是本步真正的收获**：融合形态与独立形态**不相等**（50590 字节）。两者跑的是**同一个 kernel、
同一片槽位、同一个几何**，唯一区别是命令缓冲的归属与顺序 ⇒ 这是一个**干净、可复现、单变量**的病灶探针。

---

## 3. 由此更新的甲（S4）结论

原先的怀疑方向是"独立 CB + 超大槽位 + 手写 pad 三者组合"。现在两个形态**都**与宿主的构造不同、
且**彼此**也不同，这**排除**了"命令缓冲的编排方式"这个方向，并把病灶收敛到：

> **设备侧 edge 构造本身给出的矩阵值，与宿主 `build_correlation_matrices()` 给出的不同。**

已排除的（沿用 S4，并本轮在代码上复核）：
- **σ² 来源**：`k0a_ratio_from_device_enabled()` 的 `OCUDU_CE_K0A_RATIO_DEV=0` 确实把
  `device_sigma2_rel` 置 `nullptr`（`:1380`），而 A/B 数字未变 ⇒ 真的排除，不是空门；
- DM-RS 符号表：`dmrs_sym` 与 `edge_dmrs` 来自**同一个** `pattern_symbols.for_each`，`npt` 与
  `edge_dmrs.size()` 同源；
- `nout`/`L` 几何公式、housing pad 循环、`ridge`（两侧同为 `1e-6F`）。

**仍然剩下的嫌疑**（下一轮从这里开始，不要再猜）：
**超大槽位路线本身**——标准组的槽位是精确的（`k0d` 门覆盖了它），而 edge 组的槽位是
`a_stride = L_std > L_e`、`r_stride = nout_std > nout_e`，**从未被任何门覆盖过**。
下一步应当**直接 dump edge 组的槽位字节**（`gpu_a` / `gpu_r_hp` 在 systems `[nof_layers, 2*nof_layers)`
上的区域）在两个形态下逐字节对照——而不是继续对下游 `_h` 做差分。

---

## 4. 与最终目标的关系

| 子句 | 度量 | 现状 |
|---|---|---|
| **A. 只有两次 host↔device 数据穿越** | `phy_pipeline_crossings` | **3.00**/跳（S5 实测可达 0.00，但依赖甲）|
| **B. 一条设备侧流水线（fused）** | `cbs/lane` | **3.00**（默认路线；`TAIL_DEV=1` 时仍 3.28，因为 `EDGE_FUSE` 默认关）|

本步把子句 B 的**机制**做完了并留了判据，但**没有打开**它——因为打开它就会把甲从一个
"只在 `TAIL_DEV=1` 下出现"的问题变成默认路线上的问题。顺序仍然是：**先修甲，再开乙。**
