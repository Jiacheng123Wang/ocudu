# A6 实现方案 —— 采集字段补齐（★ 工作文档）

> **对应功课**：主规划 §9.2 的 **A6**（必做），`memo_09` §2.1 / §13.1。
> **目标**：让 **P1 批语料**不再因为缺字段而必须重飞。
> **状态**：🟡 **A6a 部分落地（`record.jsonl` 已写）** ｜ 版本：v1.2 ｜ 日期：2026-10-08
>
> **v1.2 变更（2026-10-08）**：★ **A6a 的第一部分已实作** ——
> `record.jsonl` 自描述 sidecar 已接在**始终编译在内**的 G-5 钩子上
> （`pusch_processor_impl.cpp`，与 `rx_meta.csv` 同一个钩子、同一个环境变量 `OCUDU_HELENA_DUMP_DIR`）。
> ★★ **并新增两个原方案里没有、但被 `memo_08` §16 证明为关键的字段**：
> **`grid.bwp_start_rb` 与 `grid.bwp_size_rb`** —— 它们是**绝对坐标系的原点**，
> 没有它们**DM-RS 参考序列根本无法重建**（这正是 H-C 的根因）。
> ★ 同批写入的还有 **`grid.dims` 自描述**（针对 §1.2 那个"注释与代码不一致"的问题）、
> **`grid.k0_units`**（明说 k0 是绝对子载波）与 **`lineage.binary_stamp`**。
> ⬜ **尚未做**：`label` 段（由 DD-label 钩子写、按 slot 配对）、`measurements` 段（A6b）、
> `dmrs.coordinates_file` / `reference_symbols_file`。
> ⚠ **验证边界（必须如实说）**：★ **离线回放工具 `ul_chain_replay` 有自己的 dump 实现，
> 不走 `pusch_processor_impl` 的钩子** ⇒ **本 sidecar 在离线台上跑不到**，
> 只能在**真实链**上验证。★ 已做的独立验证是**把格式串抽出来实际渲染并用 JSON 解析器检查**（合法、字段齐全）；
> ★ **真实链上的首次验证仍待一次飞行**。
>
> **v1.1 变更**：用户裁定——① **JSONL 的 key 直接采用平台契约的字段名**；
> ② **A6b 允许设备→主机取回**（尤其仅为语料采集时），但**生产路径应尽可能少穿越**；
> 判据是**最终综合性能**（主要考量是时延，若有其它架构收益也可计入）。
> 据此新增 **§5bis 穿越政策**（复用既有的 `scoped_debug_touches` 机制）。

---

## 0. 范围

| 做 | 不做 |
|---|---|
| 补 3 个**标量**字段（`dmrs_type`、`nof_cdm_groups_without_data`、`n_rapid`） | 不改网格采集本身（已有且够用） |
| 补**布局自描述**（`memo_09` §7 纪律 1） | 不建新的采集设施（复用 G-5 钩子） |
| 补**血缘字段**（stamp / 配置 hash / 站点） | 不引入新依赖 |
| 为 **per-RE 后均衡噪声** 设计一个**独立的 opt-in 钩子**（A6b） | 不把昂贵采集放进热路径 |

---

## 1. ★★ 现场核查的三个发现（决定了方案形态）

### 1.1 ★ 缺的字段**已经全部在作用域内**——补它们是"几乎免费"的

`lib/phy/upper/channel_processors/pusch/pusch_processor_impl.cpp`：

| 行 | 内容 |
|---|---|
| `:486-497` | `demod_config` **刚刚被填好**，其中 `:490` `demod_config.dmrs_type = demux_config.dmrs;`、`:491` `demod_config.nof_cdm_groups_without_data = ulsch_config.nof_cdm_groups_without_data;`、`:497` `demod_config.n_rapid = pdu.n_rapid;` |
| `:501` | G-5 dump 钩子开始（`OCUDU_HELENA_DUMP_DIR`） |
| `:522-550` | 写 `rx_meta.csv`（16 列） |

⇒ ★ **`dmrs_type` / `nof_cdm_groups_without_data` / `n_rapid` 三个字段，在写 CSV 的那一行就在手边。**
不需要新的 plumbing、不需要新的接口、不需要改调用链。

### 1.2 ★★ 注释与代码不一致——**必须靠"自描述"而不是"约定"**

| 位置 | 说法 |
|---|---|
| C++ 注释（`:499-500`） | *"ordered **[port][symbol][re]** as float32 (re,im) pairs"* |
| C++ 代码（`:514-519`） | 外层 `for l`（符号）、内层 `for port` ⇒ 实际是 **`[symbol][port][re]`** |
| Python 读者（`ai_train/build_labels.py:89-91`） | *"rx grid file order: **[sym][port][sc]** (single-port …)"*，代码 `rx_grid.reshape(n_syms, n_ports, n_sc)[:, 0]` |

⇒ ★★ **注释是错的，代码和 Python 读者是对的。** 这件事**至今没暴露**，
因为现有语料**基本全是单端口**（此时两种顺序等价；`memo_04` §2 实测 `n_layers=1`）。

★★ **这正是 `memo_09` §7 纪律 1 的实证**：
> "每条记录自带布局标识（元素类型/维度顺序/stride 写进 `meta.json`），**不靠全局约定**——约定会随代码漂移。"

⇒ **本方案的第一优先事项就是把布局写进记录**，而不是去修注释（注释修了还会再漂）。

### 1.3 per-RE 后均衡噪声在**另一个模块**，且它是最贵的一项

- 该量在 `pusch_demodulator_impl` 内部：`state.nv`（`pusch_demodulator_impl.cpp:541` 一带，
  `span<float>(temp_eq_noise_vars).subspan(...)`），布局 `[RE][layer]`；
- ★ 在 G-5 钩子所在的位置**拿不到**它（那是 PDU 层，噪声在解调器内部）；
- ★ 而它恰恰是**主机 SINR 的唯一来源**（`memo_06` §4bis.1）⇒ 采集价值高；
- ★ 但它**每 RE 一个 float**，体积与网格同量级，且可能需要把设备侧缓冲取回主机。

⇒ ★ **必须拆成 A6a / A6b**：标量字段随主钩子走（便宜），per-RE 噪声走**独立的 opt-in 钩子**。

---

## 2. 方案：**加法式 JSONL sidecar**（三个选项的取舍）

| 选项 | 做法 | 评价 |
|---|---|---|
| **① 扩展现有 CSV** | 往 `rx_meta.csv` 后面追加列 | ❌ **会打破现有解析器**（`build_labels.py:155` 按固定元组解包）；位置列无法自描述 |
| **② ★ 加法式 sidecar（推荐）** | **保持 `rx_meta.csv` 原样**，另写 `record.jsonl`（每记录一行 JSON），含**全部字段 + 布局描述 + 血缘** | ✅ 向后兼容；✅ 自描述；✅ 网格只写一次（贵的那部分不重复）；✅ 符合 `memo_09` §7"字段只增不改" |
| **③ 新建设施** | 新开 `OCUDU_CORPUS_DIR`，整套重写 | ❌ 重复网格写入；❌ 维护两套；❌ 收益不成比例 |

### 2.1 为什么 JSONL 而不是"再来一个 CSV"

1. ★ **自描述**：JSON 可携带嵌套的 `layout` 对象（元素类型/维度顺序/stride），CSV 不能；
2. ★ **加法友好**：加字段不改变已有行的解析（CSV 加列会改变列数）；
3. ★ **与 `index.parquet` 的关系**：JSONL 是**每记录的原始**，`index.parquet` 是它的**汇总**
   （`memo_09` §6）——JSONL 落盘、parquet 由脚本生成。

### 2.1bis ★ 已裁决：**key 采用平台契约的字段名**

用户裁定（2026-10-08）：**按建议执行**。理由与收益：

| 收益 | 说明 |
|---|---|
| ★ **三方对照** | 同一套名字可与 `memo_06` §1 的 `ocudu_dapp_receiver_input_v1`（`rx_grid` / `pusch` / `dmrs`）以及 NVIDIA `neural_rx.onnx` 的输入名（`rx_slot_*` / `h_hat_*` / `dmrs_ofdm_pos` / `dmrs_subcarrier_pos`）逐项对照 |
| ★ **训练/部署同构** | 模型输入张量的名字在**采集、训练、部署**三处保持一致，减少一次"改名"引入的错误 |
| ★ **迁移成本低** | 将来若采用平台 ABI（主规划 §1.3 路线 B），记录格式**不需要改** |

★ 具体：顶层键用契约的 `rx_grid` / `pusch` / `dmrs`，字段名尽量取契约中的拼写
（如 `dmrs_type` → 契约里是 `type`，**以契约为准**并在记录里注明来源）。

### 2.2 记录形状（草案）

```json
{
  "schema": "ocudu-corpus-record/1",
  "idx": 12345,
  "t_us": 111675509035,
  "slot": 4579, "frame": 0, "scs_khz": 30,

  "grid": {
    "file": "rx_00012345_prb51.f32",
    "elem": "cf32", "endian": "little",
    "dims": ["symbol", "port", "subcarrier"],
    "extent": [14, 2, 612], "stride": [2*612*8, 612*8, 8],
    "k0": 0, "bwp_start_rb": 0, "bwp_size_rb": 51,
    "note": "dims recorded from the writer, not from a comment"
  },

  "pusch": {
    "rnti": 17921, "n_id": 1, "n_rapid": null,
    "scid": 0, "data_scid": 0, "rv": 0, "new_data": 1,
    "mcs_index": 13, "mcs_table": 0, "modulation": "64QAM",
    "target_code_rate": 308, "nof_tx_layers": 1, "rx_ports": [0],
    "start_symbol_index": 0, "nof_symbols": 14, "nof_prb": 51,
    "dc_position": null, "enable_transform_precoding": false,
    "tbs_lbrm": 0, "harq_id": 0
  },

  "dmrs": {
    "sym_mask": 2180, "type": 1,
    "nof_cdm_groups_without_data": 1,
    "scrambling_id": 1, "n_scid": 0,
    "coordinates_file": "dmrs_coords_00012345.u16",
    "reference_symbols_file": "dmrs_refs_00012345.cf32"
  },

  "label": { "crc": "ok", "tb_file": "tb_00012345.bits", "nof_codeblocks": 1 },

  "measurements": {
    "per_port": [{"port": 0, "noise_variance": 1.2e-4, "snr": 16.8,
                  "rsrp": -80.1, "epre": -77.3, "ta_us": 0.12, "cfo_hz": -3.5}]
  },

  "lineage": {
    "binary_stamp": "676bb40ca9", "config_hash": "…", "cell_id": 41,
    "site": "oaiue", "ue": "…", "leg": "aillr00N-…", "intent_id": "ci-0007"
  }
}
```

★ **`grid.dims` 由写者从代码里取**（而不是抄注释）——见 §3.1。

---

## 3. 字段清单与取值来源（逐字段给代码位置）

### 3.1 A 组：网格（★ 新增布局自描述）

| 字段 | 来源 |
|---|---|
| 文件、`elem`、`endian` | 常量（写者确定） |
| ★ **`dims` / `extent` / `stride`** | ★ **必须由写循环的顺序生成**（`pusch_processor_impl.cpp:513-519` 的循环嵌套顺序），**不是抄注释** |
| `k0` / `bwp_*` | `:508` `rb_mask.find_lowest()*12`、`pdu.bwp_start_rb`、`pdu.bwp_size_rb` |

### 3.2 B 组：几何（★ 缺两项）

| 字段 | 来源 | 现状 |
|---|---|---|
| `sym_mask` | `:531-534`（已有 16 列里的 `dmrs_sym_mask`） | ✅ |
| ★ **`type`** | ★ `demod_config.dmrs_type`（`:490`） | ❌ **新增** |
| ★ **`nof_cdm_groups_without_data`** | ★ `demod_config.nof_cdm_groups_without_data`（`:491`） | ❌ **新增** |
| `scrambling_id` / `n_scid` | `:537-541`（已有） | ✅ |
| **导频坐标 / 参考符号** | 可由 `sym_mask` + `type` + `nof_cdm_groups` + `scrambling_id` **离线重算** | ⚠ **但重算依赖二进制版本** ⇒ 至少存**生成它们的 stamp** |

★ **决策点**：坐标/参考符号是**存**还是**重算**？
- 存：体积小（51 PRB × 6 × 3 符号 ≈ 918 个导频 × (4B 坐标 + 8B 复值) ≈ 11 KB/条），**免依赖**；
- 重算：省空间，但**版本相关**。
⇒ ★ **建议存**（与 `memo_09` §4.2 的"不可恢复"判定一致：配置能推出来，但**推导器会变**）。

### 3.3 C 组：PDU（★ 缺一项）

`rnti` / `n_id` / `scid` / `data_scid` / `rv` / `new_data` / `modulation` / `n_id` / `nof_tx_layers` /
`rx_ports` / `nof_symbols` / `nof_prb` **已有**（`rx_meta.csv` 16 列）。
新增：★ **`n_rapid`**（`demod_config.n_rapid`，`:497`，`std::optional<unsigned>` ⇒ 空则 `null`）、
`mcs_index` / `mcs_table` / `target_code_rate` / `harq_id` / `dc_position` / `enable_transform_precoding`。

### 3.4 D 组：标签（★ 加"负样本"）

| 字段 | 来源 | 说明 |
|---|---|---|
| `crc` | `pusch_decoder_impl.cpp:499-527`（TB 写出点） | ★ **只有 CRC-OK 才写 `tb_file`；CRC-KO 也要写一行，标 `crc: "ko"`** |
| `nof_codeblocks` | `dd_meta.csv` 已有 | ✅ |

### 3.5 E 组：per-RE 后均衡噪声 → **A6b（独立钩子）**

| 字段 | 位置 |
|---|---|
| `post_eq_noise.bin` | `pusch_demodulator_impl` 内部 `state.nv`（`[RE][layer]`，F32） |
| 布局描述 | 同 §3.1 的规则 |

### 3.6 G 组：血缘

| 字段 | 来源 |
|---|---|
| `binary_stamp` | 构建期写入的版本宏（★ 已有先例：`metal_kernel_fusion` 的 stamp==HEAD 检查） |
| `config_hash` | 配置 YAML + 相关 env knobs 的哈希 |
| `site` / `cell_id` / `ue` | 配置 |
| `leg` / `intent_id` | 环境变量（由 `fly_leg.sh` 注入，见 `memo_09` §8） |

---

## 4. A6a / A6b 拆分

| | **A6a：标量字段 + 布局 + 血缘** | **A6b：per-RE 后均衡噪声** |
|---|---|---|
| 钩子 | 复用现有 G-5 钩子（`OCUDU_HELENA_DUMP_DIR`） | **新开关**（如 `OCUDU_CORPUS_NV_DIR`） |
| 位置 | `pusch_processor_impl.cpp:501-550` | `pusch_demodulator_impl` 内部 |
| 成本 | ★ **可忽略**（每记录一次 `fprintf` 级） | ★ **与网格同量级**（每 RE 一个 float） |
| 默认 | 可随 G-5 钩子 | ★ **默认关**，且**与性能腿分开飞**（`memo_09` §12 纪律 5） |
| 阻塞 P1 | ★ **是** | 否（per-RE 噪声是**加分项**，不是 P1 的硬阻塞） |

★ **本方案的交付物 = A6a 优先，A6b 只出设计、按需实现。**

---

## 5. 热路径成本分析

| 项 | 现状 | A6a 后 |
|---|---|---|
| 每记录 | 网格 `grid.get` + `fwrite`（**已经是主要成本**） | + 一次 `fprintf`（JSONL 一行） |
| 每槽 | 一次 `fopen`+`fclose`（网格）+ 一次（CSV） | + 一次（JSONL） |
| 判定 | — | ★ **相对网格写入可忽略**；且整段在 `OCUDU_HELENA_DUMP_DIR` 未设时**完全不执行** |

★ **纪律**：A6a **不得**改变 `OCUDU_HELENA_DUMP_DIR` 未设时的任何行为
（`getenv` 检查在最外层，与现状一致）。

---

## 5bis. ★★ 穿越政策（v1.1 新增，回应 A6b 的设备→主机取回）

> 用户裁定（2026-10-08）：**"可以，特别是如果只是为了语料采集的话。
> 但是最后的生产路径应该尽可能少（主要考量是时间延迟，如果能够在架构上有其他方面的收益也可以，
> 根据最终性能综合评估）。"**

### 5bis.1 政策

| 路径 | 穿越 | 判据 |
|---|---|---|
| ★ **采集路径（A6b）** | **允许设备→主机取回** | 采集是**调试/语料**行为，不是生产行为 |
| ★ **生产路径（AI 推理）** | **尽可能少** | ★ **主要考量是时延**；若有其它架构收益，按**最终综合性能**评估 |

★ **这条澄清了一个原则性区分**：`phy_pipeline_crossings` 的"零穿越"是**手段**，不是目的——
目的是**性能（首先是时延）**。所以**采集路径可以付费**，**生产路径不可以**。

### 5bis.2 ★★ 现成机制：`scoped_debug_touches`——**已有的三处采集全在用它**

`include/ocudu/phy/phy_pipeline_crossings.h:218`：

```cpp
class scoped_debug_touches {
  scoped_debug_touches()  { debug_depth() += 1; }
  ~scoped_debug_touches() { debug_depth() -= 1; }
};
static bool in_debug_scope() { return debug_depth() != 0; }
```

语义（同文件 `count_host_read` 的注释）：**总数永远记这一笔；在 DEBUG 作用域内**，
它**同时**被记为 debug ⇒ **被裁决的数字 = total − debug**，两者**永远不会互相矛盾**。
★ `debug_depth()` 是**线程局部**的——"一个线程的采集不得把另一个线程的车道触碰重新分类"。

**已有的使用先例**（这就是 A6b 应当遵循的形状）：

| 位置 | 采什么 |
|---|---|
| `pusch_processor_impl.cpp:212` | CE 采集 |
| `pusch_processor_impl.cpp:347` | CE / h 采集 |
| `pusch_demodulator_impl.cpp:763` | ★ LLR 采集（注释：*"not judged by the crossing contract — see the design document's ruling on debug captures"*） |

⇒ ★ **A6b 的实现要求**：设备→主机的取回**必须包在 `scoped_debug_touches` 里**，
使**被裁决的穿越计数不受采集影响**，而 `total` 与 `debug` 两个数**都可读**。

### 5bis.3 对生产路径的要求（不变）

本节**不放松**生产路径的要求。主规划 §2.3 的"LLR 头 / 噪声头"设计，
以及 §1.1 冻结的"模型输出直接写调用方视图"——**都不因为 A6b 而改变**。
★ 一句话：**采集可以慢，生产不能慢。**

---

## 6. 向后兼容

| 消费者 | 影响 |
|---|---|
| `ai_train/build_labels.py:155`（按 15 元组解包 `rx_meta.csv`） | ✅ **无影响**（CSV 不动） |
| `ai_train/pair_capture.py`、`capture_qa.py` 等 | ✅ 无影响 |
| 现有语料 | ✅ 仍可用（缺 JSONL ⇒ 退化为"按约定解析"，即现状） |
| 新语料 | ✅ 优先读 JSONL；无 JSONL 时回退到 CSV |

★ **纪律**：**新字段只进 JSONL，永远不加进 CSV。**

---

## 7. 验收（写在前面）

| # | 判据 |
|---|---|
| 1 | 单测：给定构造的 PDU，JSONL 的每个字段与 `pdu`/`demod_config` 一致 |
| 2 | ★ **布局自检**：`grid.dims`/`extent`/`stride` 与**实际文件大小**一致（`extent × elem_size == filesize`） |
| 3 | ★ **多端口测试**：至少一条 **2 端口**记录的 round-trip 能还原出正确的 `[port][symbol][sc]` 张量 |
| 4 | ★ 关闭钩子时**零行为变化**（`ab_dumps.sh` 的 0 差异控制臂） |
| 5 | 探针关闭版可编译（`probes_off_syntax_check.sh`） |
| 6 | 一条 leg：JSONL 行数 == 网格文件数 == CSV 行数 |
| 7 | ★ CRC-KO 的接收**确实写了行**且标了 `ko` |

---

## 8. 不做的事（明确）

| 不做 | 理由 |
|---|---|
| 改 `rx_meta.csv` 的列 | 会打破现有解析器（§6） |
| 修那句 C++ 注释然后依赖它 | ★ **注释会再漂**；自描述才是解 |
| 把 per-RE 噪声塞进主钩子 | 会把最贵的一项绑进每次采集 |
| 新建 `OCUDU_CORPUS_DIR` 全新设施 | 重复网格写入，收益不成比例 |
| 采集时"顺便"做特征工程/降采样 | 语料应存**原始量**；处理留到离线 |

---

## 9. 待定 / 需评审

| # | 事项 |
|---|---|
| 1 | ✅ **已裁决**：JSONL 的 key **采用平台契约的字段名**（§2.1bis） |
| 2 | ✅ **已裁决**：A6b **允许**设备→主机取回（尤其仅为语料采集），**生产路径尽可能少**，判据是**最终综合性能**（§5bis）。开关命名待定（建议 `OCUDU_CORPUS_NV_DIR`，与 `OCUDU_HELENA_DUMP_DIR` 同族） |
| 3 | `binary_stamp` 的取法（是否已有现成宏） |
| 4 | `config_hash` 的范围（哪些 env knobs 计入） |
| 5 | 导频坐标/参考符号"存"的体积是否可接受（§3.2，估 ~11 KB/条） |
| 6 | 是否需要同时支持 **T0 仿真**产出同格式记录（`memo_09` §3 的"同一套覆盖维度"） |
