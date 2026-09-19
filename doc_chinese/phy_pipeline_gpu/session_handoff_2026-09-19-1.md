# session_handoff_2026-09-19-1 — IQ→LLR 全 GPU 化（`phy_pipeline_mode::gpu`）

> **读这一份就可以开工。** 分支 `apple-silicon`，最后提交 `76fc488385`（已推送）。
> 详细过程文档在同目录 `doc_chinese/phy_pipeline_gpu/wip/`，本文是入口与索引。
>
> **本文是 `session_handoff_<YYYY-MM-DD>-<序号>.md` 的第 1 份（2026-09-19）**，由 `README.md` 索引。
> **同名规则**：同一天再次交接就写 `-2`、`-3`……**新会话永远读序号最大的那一份**；
> 旧的那几份**保留不删**——它们记录了当时的判断，包括后来被推翻的那些（本线的教训之一就是
> 判断会被推翻，而推翻的过程本身是有用的证据）。
>
> **写下一份时**：以本份为骨架，更新 §2（状态与基线数字）、§5（剩余任务）、§9（历史提交）、
> §10（开工清单），并在开头加一段"**相对上一份的变化**"——**特别是被更正/撤回的结论**。

---

## 0. 30 秒版本

**目标**：`--phy_pipeline gpu` 模式下，**一旦数据流进入 GPU，CPU 只能做两件事——提交命令缓冲、在出口等待**。
此外任何**逐跳的宿主计算或数据搬运**都是缺陷，不管它搬的是样本、标量还是索引表，也不管它叫"数据"还是"控制"。

**进展**：写侧从 **36 次 / 4984 字节每跳** 降到 **2.65 次 / 882 字节每跳**（批次 0、1 已完成并验证）。
**剩下的大头是读侧**：**3.10 读 / 1420 字节每跳**，而且它带着一次**宿主计算**——`dev → host → dev` 的完整往返（批次 2）。

**下一步就是批次 2**：把信道统计量搬到设备侧（§6）。

---

## 1. 判据（一切裁决的标准，不要重新发明）

> **数据流进入 GPU 之前，CPU 做什么都属于装配，允许；一旦流开始，CPU 只能在出口等，
> 中间的每一次接触都算——无论它搬的是样本、标量还是索引表。**
>
> 主要目的：**一旦 GPU in the loop，CPU 就该靠边站。如果在中间某个时候还是需要 CPU（无论是什么），
> 那把数据流从 CPU 挪到 GPU 的意义就不大。**
>
> 补充裁定（用户原话）：**"即使是'指挥'/控制，也是一次 CPU in the loop，不是真正的 CPU offloading。
> 真正的 CPU offloading 是 CPU 彻底甩手不管。"**

**唯一物理上不可移除的**：Metal 命令缓冲必须由 CPU `commit`。但那是"提交 + 在出口等"，
**关键是提交不需要 CPU 知道任何逐跳的内容**。一旦提交前宿主必须算点什么，就是 in the loop。

**⇒ 由此得出的设计原则**：能预先算的在装配阶段算完；不能预先算的**搬到设备侧算**；
**不允许**"宿主在中间算完再传给设备"。

---

## 2. 当前状态

### 2.1 已完成的批次

| 批次 | 提交 | 内容 | 稳态效果 |
|---|---|---|---|
| **0** | `929fc4a3d5` | 均衡器的两张宿主建表（`h_starts`、gather tables）改成**按内容缓存、只写一次** | 写 36.00 → 30.60/跳 |
| **1** | `76fc488385` | `gpu_epochs`（符号起始时刻，**只是 (CP,SCS) 的函数**）改成**只在变化时上传** | 写 30.60 → **2.65**/跳 |

**累计**（相对批次 0 之前）：写 **36.00 → 2.65/跳**、字节 **4984 → 882/跳（−82%）**；读**未动**。

### 2.2 当前稳态实测（这是后续每一批的基线）

```bash
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/x 2>&1 >/dev/null \
  | grep -a crossings
```

```
3.10 host read(s) (1420 bytes) + 2.65 host write(s) (882 bytes) per hop
  counted by: equalizer, dft, channel_estimator
```

**⚠ 这是下界**：`demapper` **未申报**（未审），不在覆盖范围内（§7 批次 4）。

### 2.3 空口契约状态

`mode=gpu` 的契约**当前是 FAILED**，这是**正确的**——因为读侧还有 3.10/跳。
最后一次空口腿 `gnb_gpu_s5_crossings0_0918_2135`（commit `a656133d70`）曾报 `contract MET (8 of 8)`，
**那个里程碑已被撤回**（见 §8.2，检查的覆盖范围当时只有一个模块）。

---

## 3. 代码地图（改动会落在哪）

```
include/ocudu/phy/phy_pipeline_mode.h              ← 模式定义（"two host<->device data crossings"）
include/ocudu/phy/phy_pipeline_crossings.h         ← 计数器 + 申报制 + 契约检查（本线的度量核心）
lib/phy/upper/signal_processors/channel_estimator/metal/
    port_channel_estimator_metal_mmse_impl.{h,cpp} ← 估计器：unpack_engine_group / stage_engine_group
                                                      / upload_symbol_start_epochs / check_edge_slots
    ocudu_metal_mmse_engine.{h,mm}                 ← 引擎：encode_run / encode_corr / corr_stage
    ocudu_mmse_corr.metal                          ← 相关矩阵 kernel（A / R_hp）
lib/phy/upper/channel_processors/metal/
    ocudu_equalizer_metal_engine.mm                ← 均衡器：eq_cached_table（批次 0）
lib/phy/upper/channel_modulation/metal/
    ocudu_demod_metal_engine.mm                    ← 解调器（**未审**，批次 4）
lib/phy/generic_functions/metal/
    ocudu_dft_metal_engine.mm                      ← DFT：输入的零拷贝 / staging 兜底
```

### 3.1 计数器 API（`phy_pipeline_crossings.h`）

```cpp
count_host_read(bytes = 0)      // 宿主读设备产出的数据；命中缓存时**不要调**
count_host_write(bytes = 0)     // 宿主写设备要读的数据；命中缓存时**不要调**
count_device_hop()              // 分母（每跳一次）
declare_reporter("module-name") // 模块**审过之后**才申报，名字会打印在计数下面
print_reporters(FILE*)          // 命中路径不涉及
```

**硬性要求**：
- **申报 = "我审过了"**。没审过**不许**申报——否则 `0` 会被读成"整条车道"。
- **缓存命中路径不得调用计数器**——那时什么都没写/读，稳态必须是**字面的 0**，不是"很小"。
- **头文件必须保持轻量**：只能用 `<atomic> <cstdint> <cstdio> <cstring> <mutex>`。
  加 `<vector>/<string>/<algorithm>` 会让若干"位于命名空间内"的包含点炸掉
  （`no template named 'basic_ostream'`）。要打字符串就直接 `fprintf`。

---

## 4. 判据工具（每次改动的标准流程）

### 4.1 构建（**必须显式指定 target**）

```bash
cd /Users/jiachengwang/dev/ocudu
cmake --build build --target ul_chain_replay
cmake --build build --target port_channel_estimator_metal_mmse_unit_test
```

**⚠ `cmake --build build`（不带 target）不会重建 `ul_chain_replay` 和 Metal 自测**，
会得到"新 metallib + 旧宿主"的假结果。**这是本线踩过两次的坑。**

### 4.2 稳态计数（**唯一的性能判据**）

```bash
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/x 2>&1 >/dev/null \
  | grep -a "crossings" -A1
```

**⚠ 必须带 `--repeat N`（N≥20）。单跳回放的缓存是冷的，分不出"有缓存"和"没缓存"。**
批次 0 第一次尝试就是因为只用单跳，得出"改了但计数一点没变"的错误结论。

`--rotate` 可以在一进程内轮换多个形状（soak across ALLOCATIONS），用于验证分配键缓存不失效。

### 4.3 数据不变（**每批必过**）

```bash
bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh \
  "OCUDU_CE_TAIL_DEV=0 OCUDU_CE_HOST_SCALARS=1" "OCUDU_CE_HOST_SCALARS=1"
# 期望：captures=27  missing-dumps=0  captures-with-differences=0  total-differing-bytes=0
```

**⚠ 两个臂必须钉在同一个 σ² 来源上。** S5（`a656133d70`）之后 `TAIL_DEV=0` vs 默认
**不再是合法的等价参考**（宿主用宿主自己的 σ²，设备用设备的商，`_llr` 会差 12930 字节——
那是**预期差异不是缺陷**）。所以两个臂都要带 `OCUDU_CE_HOST_SCALARS=1`。

### 4.4 跨跳稳定（**批次 0 起新增，必过**）

```bash
W=$(mktemp -d); R=build/lib/phy/upper/channel_processors/metal/ul_chain_replay
C=doc_chinese/work_tmp/corpus/syn004_4
$R $C --metal --repeat 1  --out $W/a >/dev/null 2>&1
$R $C --metal --repeat 20 --out $W/b >/dev/null 2>&1
fa=$(ls $W/a*_ce.txt|head -1); fb=$(ls $W/b*_ce.txt|head -1)
for s in _llr.bin _h.bin .bin _ce.txt; do echo "$s: $(cmp -l "${fa%_ce.txt}$s" "${fb%_ce.txt}$s" 2>/dev/null|wc -l)"; done
```

**任何"按内容/分配做 key 的缓存"都必须过这一关**——单跳门看不到跨跳失效。
（`ul_chain_replay.cpp:217-220` 的注释专门警告过这件事，别忽略。）

### 4.5 单元测试

```bash
./build/lib/phy/upper/channel_processors/metal/../../signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
# 或
./build/lib/phy/upper/signal_processors/channel_estimator/metal/port_channel_estimator_metal_mmse_unit_test
```
期望 `All tests PASSED`（含 Test 11 合并/拆分、Test 13 三种 lane 序）。

### 4.6 空口腿（需要 root + B210 + 手机）

```bash
# 跑之前必须先打戳，否则脚本会 REFUSE
touch build/hashes.h && cmake --build build --target gnb
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <label>
# 跑完后
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh \
  doc_chinese/phy_pipeline_gpu/wip/logs/gnb_gpu_<label>_*.log
```

`run_leg.sh` 会**校验二进制戳记 == HEAD**，不等就 `exit 2` 并打印修复命令。
**为什么**：曾出现"先改、再构建、后提交"，导致二进制是**新代码却带旧 commit 戳**，
报告里的 `commit` 行会骗人。

**每完成一个批次要向用户要一次 OTA（ping/iperf3）确认功能正确**——这是用户定的流程。

---

## 5. 尚未解决的工作（按优先级）

### 5.1 批次 2（**下一步就做这个**）—— 读侧：把信道统计量搬到设备

**证据（代码 `port_channel_estimator_metal_mmse_impl.cpp:1583`）**：

```cpp
const channel_statistics stats = stats_estimator->estimate(stats_in);
```

**当前的往返链，每跳一次**：

```
设备产出 h
  → 宿主回读 h        （unpack_engine_group()，只回读 DM-RS 符号，但每跳都发生）
  → 宿主算 3 个统计量 （fd_hz / tau_rms_s / sigma2）
  → 这 3 个数作为 KERNEL 参数传回设备
  → corr kernel 用它们算 rt_corr / rf_corr
```

**关键代码位置**：
- `unpack_engine_group()` 定义在 `:2845` 附近；**已标计数**（一次调用一次，带元素数）；
- 调用点 `:3412` 附近：`publish_all_grid = unpack_hopping || !gpu_ce_ready`，
  车道路线上为 **false** ⇒ 只回读 `npt` 个 DM-RS 符号（`nof_unpack_symbols = all_symbols ? 14 : unpack_npt`），
  **但它照常执行，不跳过**；
- `materialize_host_grid()`（`:2884`）是按需的全网格回读，同样走 `unpack_engine_group`。

**两条做法**：

| | 内容 | 代价 |
|---|---|---|
| **A（推荐先做）** | 新增 MSL kernel，从估计/导频直接算那三个统计量，结果**留在设备侧** | 需要新 kernel；但**可分、可判**，做完 `dev→host→dev` 链就断了 |
| B | 把统计并进 corr kernel 的序言（那三个数只用来算权重，不必显式算出）| 省一次 dispatch，但改动面更大、更难 A/B |

**判据（三条，缺一不可）**：
1. **读计数 → 0**（现 3.10/跳）；
2. **`_llr`/`_h` 逐字节不变**（27 捕获 + `--repeat 1` vs `20`）；
3. **新 kernel 的输出与宿主 `stats_estimator` 逐值对照**——**这是新 kernel 的正确性判据**，
   不设这条就又是一个空门（本线已有教训，见 §8.1）。

**注意**：`stats_estimator` 是**经典的宿主统计估计器**（`stats_in` 从回读的网格构造）。
新 kernel 要与它逐值对照，先要搞清楚 `stats_in` 是怎么从网格构造的。

### 5.2 批次 3 —— 写侧剩下的 2 次/跳

| 点 | 现状 | 按判据该怎么做 |
|---|---|---|
| `gpu_ls_cfo` 进位 | `gpu_ls_cfo[slot] = gpu_ls_cfo[prev]`（**1 读 + 1 写**，宿主手工步进一个设备可见数组）| **搬到设备侧**：让 kernel 自己进位，或让宿主留影子变量只在真变化时写 |
| `gpu_y` 尾块 memset | 合并跳上清尾组的 pad 块 | **搬到设备侧**：已经有 kernel 在写 y |

**注意**：这两项**不该只减少次数**，按判据应**整个搬到设备**。判据同 §4。

### 5.3 批次 4 —— 审 `demapper` 并申报

**唯一未申报的模块**，因此现在的所有数字**都是下界**。

已知（来自代码与空口证据）：
- 它的 staging 兜底（`ocudu_demod_metal_engine.mm:322`）**没有触发**——空口
  `zero-copy wraps: 0 failures, 0 misaligned`，DFT 的 `wrap_copies=0`；
- 它用共享缓存 `wrap_no_copy`（`:291`），与均衡器/估计器同一套；
- **但它的 host→device 写没有枚举**。

**审的方法**：读 `wrap_length()` / `wrap_buffer()` 的**对齐判定**与调用方 buffer 的**真实对齐情况**
（`alloc_aligned_pages` vs `alloc_aligned`），看车道路线上落进哪个分支。**读代码，不读注释。**

### 5.4 长期项

- **子句 B（融合程度）**：`cbs/lane` **3.11**（曾 3.00）。`OCUDU_CE_EDGE_FUSE=1` 与独立形态仍差
  **51810 字节**，**未解决**。下一个嫌疑：`encode_run` 在 `st.burst` 时**跳过** corr 与 K1 之间那道
  barrier（注释假定"切 pipeline 会顺带插 barrier"——**按判据要求去读代码验证，别信注释**）。
- **契约措辞**：`host device data crossings` 那条现在会打印**申报模块**（覆盖范围），
  但主句仍写 "the fused lane (mode=gpu) allows 0"，容易被读成整车道结论。**考虑改措辞。**

---

## 6. 用户交互约定（重要）

1. **每完成一个批次 → 停下来，要用户做一次 OTA（ping/iperf3）** 确认功能正确。
   用户原话："每当完成一个阶段性的任务（例如总的穿越次数减少一次），要停下来，等我的手机OTA测试"。
2. **判据不成立就不要说成功。** 本线已经三次因为"证据比措辞窄"而误判（§8）。
3. **先补判据再动手。** 没有判据的改动**不该进树**——已回退过一次（批次 0 第一次尝试）。
4. **代码 > 注释。** 用户明确要求："不要只看注释，要直接从代码逻辑进行判断，因为注释的信息不一定准确。"
   本线因此吃过两次亏（§8.3）。
5. 用户会追问"这个和最终目标什么关系"——**回答要落到判据上，不要落到延迟上。**
   **本线的判据是"穿越/中间接触"，不是 µs。**

---

## 7. 踩过的坑（**都还在，别再踩**）

| # | 坑 | 症状 | 正解 |
|---|---|---|---|
| 1 | **陈旧二进制** | `cmake --build build` 不重建 `ul_chain_replay` / Metal 自测 | **显式 `--target`** |
| 2 | **陈旧 commit 戳** | 先改→构建→后提交 ⇒ 二进制是新代码、戳是旧 commit，报告骗人 | `touch build/hashes.h && cmake --build build --target gnb`；`run_leg.sh` 现在会**拒绝** |
| 3 | **门的语义会变** | `TAIL_DEV=0` vs 默认在 S5 之后**不再是合法参考**（差 26771 字节，是**预期**） | 用旧门之前**先问它在比什么**；两臂钉在同一 σ² 来源 |
| 4 | **单跳冷缓存** | 单跳回放分不出"有缓存/没缓存"，得出"改了没变" | **`--repeat 20`** |
| 5 | **括号不配平** | 改文件尾部时截掉 `} // namespace ocudu` ⇒ 后续头文件报 `no template named 'basic_ostream'` | **先检查括号配平**，不要猜 include 顺序（我为此误诊两次） |
| 6 | **头文件过重** | `phy_pipeline_crossings.h` 加 `<vector>/<string>` ⇒ 命名空间内的包含点炸 | 只用 `<atomic> <cstdint> <cstdio> <cstring> <mutex>` |
| 7 | **申报范围 ≠ 实际范围** | 检查只覆盖一个模块却写"the fused lane allows 0" ⇒ 误判里程碑达成 | 看**打印出来的申报模块**；只申报审过的 |
| 8 | **注释当依据** | 引用了 `ocudu_demod_metal_engine.mm` 的注释来论断 staging | **读代码逻辑** |
| 9 | **`ab_dumps.sh` 的参数** | 把 `OCUDU_*` 旋钮写在第三个参数（mode）里 ⇒ 不生效，27/27 无 dump | 旋钮放**第 1 或第 2** 个参数；`missing-dumps` 守卫会抓到 |
| 10 | **离线回放要传不带扩展名的路径** | 传 `foo.bin` ⇒ 报 `cannot read foo.bin.txt` | 传 `foo`（`.bin` 是 IQ，`.txt` 是元数据） |

---

## 8. 三个"证据比措辞窄"的教训（本线的核心教训）

### 8.1 S4 的空门
`k0d` 门的证据是空的：`OCUDU_CE_GPU_INVERT=0` 时 `device_corr_builds` 在**两个臂里都是 0**
（即宿主对宿主），而门自带的覆盖计数器 `REPORT_COUNTER="device_corr_builds"` 没被读。
**⇒ 每个门都要问："它的覆盖计数器说了什么？"**

### 8.2 撤回的里程碑
`0 host read(s) -> OK` 被当成"车道干净"并宣布达成。实际它只是**一个模块里 4 个标量点**的状态；
`unpack_engine_group()` 每跳回读 `gpu_h` **从未被计数**。已在 `S8_contract_met.md` 顶部撤回。
**⇒ 宣布里程碑之前先问：这条检查覆盖了哪些模块、哪些方向？**

### 8.3 两次"注释/推测代替代码"
- 引用解调器注释论 staging（实际未触发）；
- 说"`gpu_epochs` 在嵌套 pilot 循环里"（实际是 14 符号循环）。

---

## 9. 关键历史提交（`apple-silicon`）

| 提交 | 内容 |
|---|---|
| `e1bd5dbd70` | 修复队列探针 |
| `2eca4a7965` | S1 步骤1：CFO 经轮转槽在设备侧读 |
| `e0bbc34c9f` | sigma2 在 8 个块上轮转 |
| `832832c442` | 抽取阶段发出 lane fence 信号；权重 CB 等待 |
| `c69167dfdb` | S1：解锁 `--phy_pipeline gpu` |
| `ca9760b104` | S2：穿越计数器 + 契约检查 |
| `861a4feeb3` | S3：`OCUDU_CE_HOST_SCALARS` A/B |
| `532a231782` | S4：`OCUDU_CE_TAIL_DEV`（当时默认关）|
| `93964256aa` | **S4 根因**：`encode_corr()` 用**打包尺寸**申请映射，kernel 按**槽位行距 `Ls`** 写 ⇒ 块比槽位窄时（edge 组）超出部分全丢。判据：`TAIL_DEV=0` vs `1` 由 6043 字节 → **0** |
| `7524a9f80c` | `TAIL_DEV` 默认打开（设备建 edge 矩阵）|
| `a656133d70` | S5：宿主不再回读那 3 个标量（契约曾 `MET 8/8`，**后已撤回**）|
| `0d88781e22` | 计数写入方向 + **申报制**（打印覆盖范围）|
| `dbe0686fb8` | 计入 `unpack_engine_group` 的宿主回读 + **撤回里程碑** |
| `addf5a436b` | 计入均衡器的两张宿主建表 + 申报 `equalizer` |
| **`929fc4a3d5`** | **批次 0**：均衡器的表按内容缓存、只写一次 |
| **`76fc488385`** | **批次 1**：`gpu_epochs` 只在变化时上传 |

---

## 10. 开工清单（照抄即可）

```bash
cd /Users/jiachengwang/dev/ocudu
git log --oneline -3                      # 应是 76fc488385
git status --short lib/ include/          # 应为空

# 1. 复现基线（确认环境正确）
cmake --build build --target ul_chain_replay
./build/lib/phy/upper/channel_processors/metal/ul_chain_replay \
    doc_chinese/work_tmp/corpus/syn004_4 --metal --repeat 20 --out /tmp/base 2>&1 >/dev/null \
  | grep -a crossings -A1
# 期望：3.10 read(s) (1420 bytes) + 2.65 write(s) (882 bytes) per hop
#       counted by: equalizer, dft, channel_estimator

bash doc_chinese/phy_pipeline_gpu/wip/ab_dumps.sh \
  "OCUDU_CE_TAIL_DEV=0 OCUDU_CE_HOST_SCALARS=1" "OCUDU_CE_HOST_SCALARS=1"
# 期望：0 差异

# 2. 开工批次 2（§5.1）
#    先读 stats_in 是怎么从回读的网格构造的、stats_estimator->estimate() 算什么
#    再决定做法 A 的 kernel 形状
#    三条判据：读计数→0、逐字节不变、新 kernel 与宿主统计量逐值对照
```

**语料**：`doc_chinese/work_tmp/corpus/*.bin`（27 个；同名 `.txt` 是元数据，**传给回放时不要带扩展名**）。

---

## 11. 一句话的现状

> **写侧已经基本清零（36 → 2.65/跳，−82% 字节），剩下 2 次/跳是批次 3 的收尾。
> 真正的障碍是读侧的 3.10/跳，它是一个 `dev → host → dev` 的完整往返——宿主回读信道估计、
> 算统计量、再作为 kernel 参数传回。批次 2 把这段搬到设备侧，就是本线的下一个决定性一步；
> 做完之后 `demapper` 的审计（批次 4）才能把"数字是下界"变成"数字是上界"。**
