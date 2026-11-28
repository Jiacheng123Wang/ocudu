# dump 工作流 memo（**追加式**：每次会话追加一节）

> **用途**：记录「采集语料 → dump → 建库」这条线上**实测出来的事实、做过的决定、踩过的坑**，
> 让下一个人（或下一次会话）不必重新推导。
> **怎么用**：**只追加，不改写历史**。每条结论后面必须跟证据（命令 / 数字 / 文件行号）。
> 新结论与旧结论冲突时，在新条目里写明「修正 §X」，旧条目保留。
> **每次会话结束前追加一节**（模板见 §0.2）。
> 相关文档：`corpus_to_training_to_measurement_db_manual.md`（方法论，§6 dump / §7 建库）、
> `single_leg_dump_runbook.md`（从 tar.gz 解包到 dump 的操作步骤）。

---

## 0. 使用约定

### 0.1 已知的几条硬事实（改之前先看这里）

| 事实 | 证据 |
|---|---|
| dump ＝ **经典 CPU 链的判决**，工具内钉死 `--phy_pipeline cpu --pusch_receiver_backend classic` | `dump_tb.sh:66` |
| 二进制**过期会静默丢列**（`iterations_max` 整列为空），不报错 | 2026-11-26 的 `ul_chain_replay.cpp` 改动 + `dump_s2_amc_i6` 的 index |
| LDPC 迭代上限 = **6**（= 空中腿 `pusch_dec_max_iterations`）| `ul_chain_replay.cpp:100-112`，运行时会打印 `6 decoder iteration(s)` |
| `index.csv` 的 `iterations` 是**该单元单独回放**的结果；重传单元（`new_data=0`）在回放里**拿不到软缓冲**（`cannot reserve a receive buffer`），迭代列为空 | `ul_chain_replay.cpp:1719-1727` |
| 14 核上 dump 的并发上限 = **12 个进程**（一单元一进程）| 实测 83–86 单元/秒，负载已饱和 |
| 一条 50 000 接收的腿 ≈ 9.6 GB 原始 + 5.8 GB `stage/` 草稿 + 150 MiB 契约产物 | 实测 1103i / 1103j |
| harq 样本的比特**只存在于** `<leg>/tb_live/`（空中链软合并解出），离线逐单元回放永远解不出 | 1103i/1103j：donor 100% live-only |
| **能复现软合并的离线路径**：`ul_chain_replay --also … --harq-chain`（一条链一个进程）| 2026-11-27 实验，10/10 链在重传跳 crc=OK（§1.7）|

### 0.2 新条目的写法（照抄这个骨架）

```markdown
## N. YYYY-MM-DD HH:MM 标题（一句话说清做了什么/发现了什么）

### 背景
（为什么做这件事，一句话）

### 做了什么 / 实测
| 项 | 值 | 怎么测的 |
|---|---|---|

### 结论（可直接引用）
1. …

### 遗留 / 待办
- [ ] …

### 涉及文件
- `path:line`（改了什么）
```

---

## 1. 2026-11-27 单腿 dump 收尾 ＋ 合并迭代数的记录缺口

### 1.1 背景

要把两条已采集腿（`s2_amc_iperf_1103i`、`s2_amc_iperf_1103j`）从 211 的 tar.gz 一直做到
`tb_<leg>/{index.csv, units.txt, tb/}`，并把这条流程写成可复现的 runbook。

### 1.2 做了什么 / 实测

| 项 | 值 | 怎么测的 |
|---|---|---|
| 解包（4.05 GB → 9.6 GB / 29 万文件）| **2 min 13 s**（1103i）、2 min 21 s（1103j）| `ssh 211 cat tar.gz \| tar -xzf - -C corpus --exclude '._*'` |
| dump（50 000 单元，`--jobs 12`）| **9 min 55 s / 9 min 59 s**，83–86 单元/秒 | `dump_tb.sh` 时间戳 |
| 1103i 结果 | 36 747 crc_ok（73.5%），`tb/` 36 747 个 / 144 MiB | 三条判据脚本 |
| 1103j 结果 | 38 740 crc_ok（77.5%），`tb/` 38 740 个 / 151 MiB | 同上 |
| 与历史 dump 一致性 | 1103i：50 000 单元 **6 列全同**；1103j：同样全同 | 逐单元比对 `index.csv` |
| 历史目录多出的 3 个 CSV | `index_self/index_harq/index_harq_only.csv` = **建库第一步** `harq_backfill.py` 写进同一目录的（`build_mother_set.sh:87` 的 `--out "$DUMP"`）| 文件 mtime + 行数对账（36 747 / 5 259 / 50 000）|

**harq 历史文件夹 `tb/` 42 718 个的构成**：36 747（dump 产出）＋ 5 971（backfill 拷回的 donor 比特）。

### 1.3 解包校验的硬指标（换腿时按类别推）

| 类别 | 1103i | 1103j |
|---|---|---|
| 顶层条目 | 200 002（= 50 000×4 + `tb_live/` + `logs/`）| 同 |
| 文件总数 / 字节 | 293 510 / 9 576 543 376 | 297 770 / 9 628 143 902 |
| `tb_live/` 文件 | 93 507 | 97 767 |
| `._*`（AppleDouble）| 解包时应为 0 | 0 |
| tar 成员 | 587 021（3 目录 + 293 510 文件 + 293 508 `._`）| 595 541（同构）|

### 1.4 self / harq 两群体的区别（★ 训练相关）

| 项 | 1103i | 1103j |
|---|---|---|
| self / harq / none | 36 747 / 5 971 / 7 282 | 38 740 / 6 053 / 5 207 |
| harq 中首传（进训练集）| 5 259（`index_harq_only.csv`）| 4 909 |
| self 的 `iterations_max` 均值（1..6 直方）| 1.75（17 791/13 776/3 137/1 153/575/**315**）| 1.71（20 431/13 128/2 764/1 331/676/**410**）|
| harq 首传的 `iterations_max` | **100% = 6** | **100% = 6** |
| harq 占该调制可训练样本 | QPSK 1.3% / 16QAM 3.6% / 64QAM 19.0% / **256QAM 65.2%** | 0.3% / 3.1% / 23.3% / **79.3%** |
| 同调制配平后 SINR（self → harq）| 64QAM 20.9 → 11.2 dB；256QAM 26.1 → 18.6 dB | 64QAM 20.3 → 13.1；256QAM 25.0 → 16.5 |
| 同调制配平后授权宽（self → harq）| 256QAM **7.7 → 27.4 PRB** | 256QAM **5.1 → 25.5 PRB** |
| donor 来源 | **100% live-only**（离线 index 里 crc=OK 的 donor 数 = 0）| 100% live-only |

**结论**：harq 群体是「各调制的低 SNR 尾巴 ＋ 更宽的授权」，且**是 256QAM 训练样本的主要来源**；
`safe` 的难度标签（`iterations_max`）只在 self 上有区分度，harq 内部恒为 6。

### 1.5 发现的缺口（★ 本次最重要）

`iterations/iterations_max` 对 harq 样本来说**记的是首传那次失败尝试**（= 迭代上限 6），
而**真正决定标签的那次解码**是 donor 的合并解码（crc=OK）。空中链确实算出了它的迭代数，
但当时的 `dd_meta.csv` 只有 6 列（`idx,ts_us,tb_bits,nof_cb,frame,slot`），**这个数被丢掉了**。

### 1.6 代码改动（已提交工作区，未 commit）

| 文件 | 改动 |
|---|---|
| `lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.cpp` | `dd_meta.csv` 追加 `iter_mean,iter_max`（8 列）；`0,0` 表示无观测（解码被跳过）。前 6 列位置不变 |
| `lib/phy/upper/ai_signal_processors/depth3/ai_train/harq_backfill.py` | 解析第 7/8 列；harq 行的 `iterations/iterations_max` 改写为**标签那次解码**的值；首传失败值另存 `own_iterations/own_iterations_max`；未知留空（不填 6）|
| 同上 `split_leg_by_modulation.py` | 分腿时沿用源 `dd_meta` 表头（原先硬编码 6 列）|
| `single_leg_dump_runbook.md` §7.3 | 补语义说明 |

**验证**：`ctest -R pusch_processor` 3/3 通过；单码块 `dd_meta` = `…,16147,1,1`；多码块单元
`dd_meta` = `…,19767,4,5`（nof_cb=2），与 `index.csv` 的 4/5 一致；`harq_backfill` 旧 6 列路径
（harq 行留空、`own_*=6/6`）与新 8 列路径（合成数据，harq 行取 donor 值）都跑通，标签计数不变。

### 1.7 链式回放实验（★ 决定了旧语料能不能补）

问题：旧语料没记录合并迭代数，重跑现有 dump 能不能得到？
**不能**：`dump_tb.sh` 一单元一进程，无软缓冲 ⇒ 只会再得到 6/6（两条腿重 dump 与历史逐字段全同，
harq 行仍是 6/6）。

但 **`ul_chain_replay` 的 `--also … --harq-chain` 会在一个进程里连续回放一条 HARQ 链、共享受缓冲**。
2026-11-27 在 1103i 上随机抽 10 条链实测：**10/10 在重传那一跳 crc=OK**，迭代 2–5
（例：`cap_23904_7047` 首传 KO 6/6 → `cap_23905_7067` OK **2/2**；`cap_45085_587` → `cap_45086_609` OK **3/5**）。
这 10 个"重传跳"在逐单元 dump 里全是 `crc=no`、且都在 `tb_live` 里。

**接口现成**：链式回放加 `OCUDU_REPLAY_CSV=<csv>` 逐跳输出
`capture,slot,rnti,modulation,nof_prb,sinr_db,crc,iterations,iterations_max,tbs_bytes`。

**含义**：旧语料**不需要重新采集**即可补出合并迭代数——需要的是"链式 dump"（一条链一个进程）。
新采集走 `dd_meta` 的 8 列（空中链权威值）；旧语料走链式回放重建（bits 有 CRC 保证同块，
迭代数无法与"当时的值"逐位对照）。

### 1.8 遗留 / 待办

- [ ] 实现 `dump_harq_chains.py`（按链回放 → 输出 `unit,donor,iterations,iterations_max,…`），
      并让 `harq_backfill.py --chain-iters` 消费它（优先级：`dd_meta` > 链式回放 > donor 自解）
- [ ] 在 1103i/1103j 上补出 harq 合并迭代数并复核标签不变
- [ ] 全部 63 条腿补齐（先抽 1 条验证，再批处理）
- [ ] 空中链日志（`<leg>/logs/*.log`）里有 51 450 条 `iter=`（**只有均值**），行首坐标是 `SFN.slot`
      每 10.24 s 回绕，仅作旁证，不做主路径

### 1.9 涉及文件

- `lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_tb.sh`（§6 流程本体）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/harq_backfill.py`
- `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp`（`--harq-chain`、`OCUDU_REPLAY_CSV`）
- `lib/phy/upper/channel_processors/pusch/pusch_decoder_impl.cpp`（`dd_meta.csv` 写点）
- `doc_chinese/depth3_llr_ai/single_leg_dump_runbook.md`

---

## 2. 2026-11-27 08:20 链式 dump 落地：harq 合并迭代数补出来了

### 背景

§1.5 的缺口（harq 样本的难度标签其实是"首传失败值 6"）需要一个工具把**合并解码那次**的迭代数补出来。
§1.7 的 10 条链实验证明离线链回放能复现软合并，本节把它做成工具并在两条腿上跑完。

### 做了什么

| 产物 | 说明 |
|---|---|
| `dump_harq_chains.py`（新增，`ai_train/`）| 一条 HARQ 链一个进程（`--also … --harq-chain`），读 `OCUDU_REPLAY_CSV` 的逐跳行，取**第一个 crc=OK 的跳**的 `iterations/iterations_max`；可选 `--live-tb` 与空中链的比特逐字节比对；`--jobs` 并行 |
| `harq_backfill.py --chain-iters <csv>` | 优先级：live `dd_meta`（权威）> 链式回放 > donor 自解；harq 行的 `iterations/iterations_max` 用该值，首传值留在 `own_*` |
| `harq_backfill.py` 重构 | `load_units()` / `build_chains()` 变成模块函数，两个工具**共用同一套分链规则**；重构前后 50 000 单元的 label/donor **0 差异** |

### 实测（两条腿，`--jobs 12`）

| 项 | 1103i | 1103j |
|---|---|---|
| 候选链（首传失败且有后继）| 5 655 | 4 947 |
| 链回放解码成功 | 5 236（92.6%）| 4 894（98.9%）|
| 耗时 | **35.5 s** | 49.6 s |
| 比特与 `tb_live` 逐字节一致 | 5 225 match / 11 no-live-row | 4 879 match / 15 no-live-row |
| 补上合并迭代数的 harq 行 | **5 235 / 5 259（99.5%）** | **4 891 / 4 909（99.6%）** |
| label/donor 变化 | **0** | — |
| 仅靠链回放可新增的 label | 1 个（"none"→"harq"）| 3 个 |

### 结论（★ 可直接引用：self vs harq 的真实难度）

| 项 | 1103i self | 1103i harq（合并后）| 1103j self | 1103j harq（合并后）|
|---|---|---|---|---|
| `iterations_max` 均值 | 1.75 | **3.10** | 1.71 | **3.21** |
| 直方 1/2/3/4/5/6 | 17 791/13 776/3 137/1 153/575/315 | **108/1 687/1 805/1 023/415/197** | 20 431/13 128/2 764/1 331/676/410 | **79/1 469/1 668/958/471/246** |
| `iterations` 均值 | 1.67 | 2.44 | 1.60 | 2.40 |

按调制（harq 合并后，`iterations_max` 均值 / 撞 6 的个数）：

| 腿 | QPSK | 16QAM | 64QAM | 256QAM |
|---|---|---|---|---|
| 1103i | 2.65 / 10（n=276）| 2.66 / 3（253）| 3.06 / 51（1 650）| **3.21 / 133（3 056）** |
| 1103j | 2.28 / 0（61）| 2.51 / 0（396）| 2.75 / 21（1 890）| **3.68 / 225（2 544）** |

1. **harq 群体比 self 硬一倍**：合并后仍需 3.1/3.2 轮，而 self 是 1.7 轮；且 harq 有 197/246 个样本
   **合并后仍要跑满 6 轮才通过**——这些是"再差一点就丢"的真临界样本。旧口径把它们全部记成 6/6，
   和 self 的 6 混在一起，难度维无法区分。
2. **256QAM 是 harq 的主力也是最硬的**：n=3 056/2 544，均值 3.21/3.68，撞 6 的有 133/225 个。
3. QPSK/16QAM 的 harq 虽然只有 61–276 个，但它们处在 **−6 dB 量级**（§1.4），合并后也要 2.6/2.3 轮，
   不是"低调制就一定简单"。

### ★ 新发现的坑：replay 的 `dd_meta` 里 frame 是进程相对值

`extended_frame()`（`lib/phy/upper/channel_processors/pusch/ul_capture_frame.h`）的进位基数是**进程级**状态，
所以：**空中链**（起飞就在跑）写出的 frame = 10023 量级，与捕获名一致；**replay**（进程新起）写出的
frame = 原始 SFN（如 807）。两者**不能跨进程按 frame 匹配**。若将来要用 replay 的 dump 做对账，只能按 slot
（同一条链内唯一）或按 capture 名匹配。工具里已经按 slot 匹配并在注释里写明。

### 遗留 / 待办

- [ ] 其余 61 条腿批量补（按 35–50 s/腿，约 40 分钟）；先确认这些腿的语料在 T7 上是否已解包
- [ ] `owner` 之外的消费方：`build_mother_set.sh` 是否要默认接 `--chain-iters`（现在需显式传）
- [ ] 1103i 的 dump 目录还留着 `stage/`（5.8 GB）与 `rows/`，按 runbook §7.1 可删
- [ ] 考虑给 `dump_tb.sh` 增加一个"链模式"开关，把 `chain_iters` 作为 dump 的标准第四件产物

### 涉及文件

- `lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_harq_chains.py`（新增）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/harq_backfill.py`（`--chain-iters` + 两函数抽出）
- `~/ai_depth3/chain_iters/chain_iters_s2_amc_iperf_1103{i,j}.csv`（产物，故意**不**放进 dump 目录）

---

## 3. 2026-11-27 09:05 标准 dump 改为两群体一次产全（self ＋ harq，比特统一进 tb/）

### 背景

§2 的链式回放还是个外挂工具。它其实应该是 dump 的**内在部分**：事后重放没有"必须先解 RV0 才知道要不要发 HARQ"
这个约束，链就在磁盘上，一条链一个进程合并解码就能直接给出 harq 样本。所以标准 dump 从"只出 self"改成"self ＋ harq"。

### 做了什么

| 文件 | 改动 |
|---|---|
| `dump_tb.sh` | 新增第二段：逐单元回放后自动跑链回放（`--tb-out "$OUT/tb"`）＋ 打标；`--no-harq` 可关；结束时打印 `populations: self=… harq=…; tb now holds N blocks` |
| `dump_harq_chains.py` | 新增 `--tb-out DIR`：把恢复出来的块写成 `<DIR>/<首传>.bits`（**与 self 同一目录同一命名**），CSV 加 `tb_written` 列 |
| `harq_backfill.py` | ① 链回放的比特**能当标签来源**（没有 `tb_live` 的腿也能出 harq）；② 自动读 `<out>/chain_iters.csv`，`build_mother_set.sh` 重跑不会再抹掉合并迭代数 |

### 实测（1103i / 1103j，都是"跑完即完整"）

| 项 | 1103i | 1103j |
|---|---|---|
| 链回放：候选 / 解码 | 5 655 / 5 236（92.6%）| 4 947 / 4 894（98.9%）|
| 打标结果（live 优先 + 链回放兜底）| self 36 747，harq 首传 **5 260**，harq 全标 5 975 | self 38 740，harq 首传 **4 912**，harq 全标 6 065 |
| 比旧的 live-only 打标多出的样本 | +1 个首传（+4 个全标）| +3 个首传（+12 个全标）|
| `tb/` 文件数 | **42 722**（原 36 747；历史混合目录是 42 718）| **44 805**（原 38 740）|
| `tb/` 体积 | 172 MiB | 180 MiB |
| 合并迭代数覆盖 | 5 236 / 5 260 首传有值 | 4 894 / 4 912 |

产物（dump 目录一屏）：
```
index.csv  units.txt  tb/               # 契约（tb/ 现在同时装 self 与 harq）
index_self.csv  index_harq.csv  index_harq_only.csv   # 群体索引（5.6 MiB）
chain_iters.csv                                      # 链回放审计（0.44 MiB）
```

### ★ 对"省掉 RV0 撞墙解码"的核实（结论要写准）

- **首传的 RV0 判决不能省**：它决定样本属于 self 还是 harq，也决定链要不要继续解。没有它就没有群体划分。
- **重传单元的独立解码本来就是免费的**：第 1 段里它们 `cannot reserve a receive buffer` 直接退出，**不进 LDPC**。
  所以"省掉无用功"在现状里已经成立。
- **真正的重复**是第 2 段又把 5 655 条链的首传解了一遍（≈35 s/腿，占 10 分钟 dump 的 5%）。
  若要做到零重复，需要 chain-first 单遍架构（一条链一个进程、逐跳产行），属于后续优化。

### 遗留 / 待办

- [ ] 其余 61 条腿按新标准重跑（有 dump 的腿只需补第二段：链回放 + 打标 ≈ 40 s/腿）
- [ ] 归档清单更新进 `single_leg_dump_runbook.md`（已改 §7.2）
- [ ] 可选：chain-first 单遍 dump（省掉 5% 的重复解码）
- [ ] 1103i 的 `stage/`（5.8 GB）与 `rows/` 仍在，按 runbook §7.1 可删

### 涉及文件

- `lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_tb.sh`（第二段）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_harq_chains.py`（`--tb-out`）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/harq_backfill.py`（链比特当标签来源、自动读 chain_iters）
- `doc_chinese/depth3_llr_ai/single_leg_dump_runbook.md`（§1.2 / §5.2 / §5.5 新节 / §6.2 / §7.2 / §7.3）

> 本节与 §1/§2 的代码改动已提交并推送：`15c36de374`（2026-11-27，`origin/apple-silicon`）。

---

## 4. 2026-11-27 09:25 删掉旧产物，用新流程从零重跑 1103i

### 做了什么

| 步 | 动作 |
|---|---|
| 1 | 删 `tb_s2_amc_iperf_1103i`（6.2 GB，含 `stage/`）与 `tb_s2_amc_iperf_1103j`（193 MB）两个旧 dump 目录，并删掉 `~/ai_depth3/chain_iters/` 的旧副本；**原始语料 `s2_amc_iperf_1103{i,j}/` 未动** |
| 2 | 一条命令跑新标准流程：`dump_tb.sh --out …/tb_s2_amc_iperf_1103i --jobs 12 …`（自动含链回放＋打标）|
| 3 | 按 runbook §6 判据验收，§7.1 收尾 |

### 实测

| 项 | 值 |
|---|---|
| 总耗时 | **11 min 8 s**（逐单元 50 000 单元 ＋ 链回放 5 655 条链 ＋ 打标）|
| 判据 | `units=50000 with_bin=50000 index_rows=50000 crc_ok=36747` |
| 群体 | self 36 747 / harq 首传 5 260 / harq 全标 5 975 |
| `tb/` | 42 722 个（= self＋harq 全标），172 MiB |
| 链回放 | 5 655 候选 → 5 236 解码（92.6%），比特 `match=5 225 / no-live-row=11` |
| 合并迭代数覆盖 | 5 236 / 5 260 |
| 合并 `iterations_max` 直方 | 1:108 2:1 687 3:1 806 4:1 023 5:415 **6:197** |
| `index.csv` sha256 | `5c060c01…ba5bbd` —— 与旧流程（逐单元 dump）**逐字节相同** |
| 最终目录 | 185 MiB，七样：`index.csv` `units.txt` `tb/` `index_self.csv` `index_harq.csv` `index_harq_only.csv` `chain_iters.csv` |

### 结论

1. 新流程可重复：`index.csv` 与旧流程逐字节一致 ⇒ self 半程没有被改动；
2. harq 半程在原目录里一次产全（比特在 `tb/`），不再需要事后拼装；
3. 每次"从零 dump"现在就是 11 分钟/腿，产出即完整。

### 遗留 / 待办

- [ ] 1103j 用同样的命令重跑（尚未执行）
- [ ] 其余腿批量（有语料的 11 分钟/腿；已有 dump 的只需补第二段 ≈ 40 s/腿）

---

## 5. 2026-11-27 09:45 runbook 按新流程改写完毕

`single_leg_dump_runbook.md` 按"标准 dump 一次产 self＋harq"重写，所有数字取自 §4 的**干净重跑**（不是拼装结果）：

| 节 | 改动 |
|---|---|
| §0 | ④ 时间改为 11 分钟、⑤ 判据加 `self+harq / tb` 两行、⑥ 收尾改成"七个契约产物" |
| §1.2 | 容量 ≈185 MiB（tb 172 ＋ 索引 5.2 ＋ chain_iters 0.44）|
| §5.2 | 参数表补 `--no-harq`；`--limit` 注明"链也会被截断"；产物表 7 项＋3 个草稿 |
| §5.3 | 拆成三段耗时：逐单元 ≈10 min ＋ 链回放 ≈40 s ＋ 打标 ≈20 s |
| §5.4 | 冒烟更新：2 000 单元 27.7 s（260 候选 / 258 解码 / `tb/` 1 739）|
| §5.5 | 「dump 的两个群体」保留并更新实测（5 655 → 5 236，`tb/` 36 747 → 42 722）|
| §6.2 | 判据改为 `tb/` = `label_from ∈ {self,harq}` 的行数；`tb > crc_ok` 属正常 |
| §6.3/§6.4 | `tb/` 172 MiB；比特复核分成"vs 历史 dump"与"vs tb_live"两组 |
| §7.1 | 七个产物 ＋ 体积表（`tb/` 172 MiB、索引 5.2 MiB、`chain_iters` 0.44 MiB）|
| §7.2 | 归档实测 **19.4 MiB / 20 333 635 B / 42 730 成员**（11.6 s）|
| §7.3 | 重写：dump 自己就出三索引＋harq 比特；建库重跑自动读 `chain_iters.csv`；旧腿补齐的两条命令 |
| §8 | 总账 ≈14 分钟（含解包）、峰值 ≈16 GB |
| §9 | 4/9 改写，新增 13（`bits_check` 两种取值的区别）、14（replay 的 frame 是进程相对值）、15（Ctrl-C 断链回放）、16（多目录不跑 harq 段）|
| §10 | 指纹更新：`tb` 42 722 / 172 MiB、`chain_iters` sha256、合并直方、两段耗时 |

### 结论

runbook 现在与代码（`dump_tb.sh` 两段、`dump_harq_chains.py --tb-out`、`harq_backfill.py --chain-iters`）
**一一对应**，照抄即可复现 1103i 的 11 min 8 s / 42 722 块 / 5 236 条合并解码。

---

## 6. 2026-11-27 09:55 新增 `index_ko.csv`：没有 label 的样本成为别处解码器的工作清单

### 背景

`index_self` / `index_harq_only` 只覆盖"CPU 经典链拿到了 label"的样本。剩下那批既没解出、也没有 donor 的接收，
以前只以 `label_from=none` 的形式散在 `index_harq.csv` 里 —— 想换 GPU/NPU 后段捞样本时没有一份干净的清单。

### 做了什么

`harq_backfill.py` 多写一个 `index_ko.csv`（标准 dump 因此自动带出）：

| 项 | 值 |
|---|---|
| 定义 | `label_from=none` 的行，与 `index_self.csv` / `index_harq_only.csv` **互斥** |
| 列 | `index.csv` 的 8 列 ＋ `new_data, rv, harq_id, rnti, reason` |
| `reason` | `decode-failed`（进了 LDPC、6 轮没过）／`no-buffer`（重传，harness 拒绝预留软缓冲，没进 LDPC）|
| 1103i 实测 | **7 278 行** ＝ `decode-failed` **457**（`new_data=1`）＋ `no-buffer` 6 821（`new_data=0`）；435 560 B |
| 分区校验 | self 36 747 ＋ harq 全标 5 975 ＋ ko 7 278 = **50 000**（harq 首传 5 260 ＋ harq 中间成员 715）|

`dump_tb.sh` 的收尾行现在打印 `ko=N (ko = no label; work list for another decoder)`。

### 用途与边界

- **457 条 `decode-failed`**：确实撞了 LDPC 的 6 轮上限，是最值得用更强后段（更多轮次、GPU/NPU 解码器、
  AI 接收机）重试的一批；解出 CRC 后按 self 的方式写入 `tb/<unit>.bits` ＋ `crc=OK` 行即可进建库。
- **6 821 条 `no-buffer`**：都是重传。harness 按 `new_data` 预留软缓冲 ⇒ 单跑直接被拒；**当前单靠逐单元回放解不了**，
  要走链回放（已有）或给 harness 加"强制新缓冲"模式（未做）。

### 文档

- runbook：§5.2 产物表加 `index_ko`；新增 **§5.6**（定义、字段、怎么换后段用、两条边界）；§6.2 加分区校验 (c)；
  §7.1/§7.2 归档改八样（19.5 MiB / 20 400 698 B / 42 732 成员）；§10 指纹加 `index_ko` 行。

### 待办

- [ ] 若要在本机试 GPU/NPU 后段：用 `metal/ul_chain_replay` ＋ `--phy_pipeline gpu|npu`，
      对 457 条 `decode-failed` 逐个重试（约几分钟），看能翻回来多少
- [ ] 若需要覆盖 `no-buffer` 那 6 821 条：给 harness 加"忽略 new_data 的强制新缓冲"开关（要谨慎，会改变语义）

---

## 7. 2026-11-27 10:05 **修正 §6**：换后段也能产出 harq 样本，分类看 label 来源而不是后端

### 修正内容

§6 里写的「解出 CRC 后按 self 的方式进建库（它不是 harq —— 没有任何合并发生）」**范围说窄了**。
正确说法：

| label 怎么来的 | 归类 | 说明 |
|---|---|---|
| 这次接收**自己**解出（单跑）| 等价 self（`label_source=own`）| 与 CPU 规则一致 |
| 只能靠**同一条 HARQ 链上别的接收**（软合并）解出 | 等价 harq（`label_source=chain`）| 标签挂在链首传上才是 harq 样本 |
| 重传被**强制新缓冲**单跑解出 | **既不是 self 也不是 harq** | `own` 标签 ＋ `first_transmission=0`，是当前流水线从未产生过的新群体 |

也就是说：**任何后段（metal / npu-ai / 别的 LDPC）逻辑上都和 CPU 一样，可能补出 self，也可能补出 harq，
也可能一条都补不出来**（全被 CPU 先解了）。原来那句话只对"单跑解出首传"这一种情形成立。

### 由此必须加的三列（harvest 落盘时）

`decoder`（cpu-classic / metal / npu-ai …）、`label_source`（own / chain）、`first_transmission`（`new_data=1 && rv=0`）。

理由：`label_from=self` 只说明"这次接收自己解出来了"，**不说明参考链解得出来**；
把 harvest 解出的 own 样本混进经典 self 群体，会让"参考链能解"的基线虚高（与 §17/§18 的泄漏规则同一类事故）。

### 现状

- 代码里 `label_from = self iff crc==OK`，**没有** `new_data` 条件；因为目前没有任何重传被单跑解出，
  `index_self.csv` 事实上只含首传 —— 但这是"事实"，不是契约。加 harvest 时必须靠新列/新文件把这条语义钉住。
- runbook §5.6 已按上表改写（含 457 / 6 821 两类各自的出路）。

---

## 8. 2026-11-27 10:15 新旧 dump 对照（1103i）：数据一致，差异只有三处且都可解释

对照对象：新 `~/ai_depth3/corpus/tb_s2_amc_iperf_1103i`（= `/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i` 的逐字节副本，
七个文件 sha256 全同）vs 旧 `~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103i`。

| 项 | 结果 |
|---|---|
| `index.csv` | 50 000 行、unit 集合相同；**7 个共有列逐单元 0 差异**；新多一列 `iterations_max` |
| `index_self.csv` | 36 747 行，共有列 0 差异（新多 `iterations_max`）|
| `index_harq_only.csv` | 旧 5 259 / 新 5 260；`tb_bits/crc/sinr_db/slot/tbs_bytes` 0 差异；`iterations` 有 5 234 行不同 = **预期改进**（旧＝首传失败值，新＝合并解码值）|
| `index_ko.csv` | 新 7 278 ＝ 新 `none`；旧 `none` 7 282；差的 4 个正是新捞回的链 |
| `tb/` | 旧 42 718、新 42 722；**共同 42 718 个逐文件 md5 全同**（self 36 747 ＋ harq 5 971）；只多 4 个文件 |
| `units.txt` | 50 000 个同名单元；仅路径前缀不同（旧指向 `~/ai_depth3/corpus`）|

**差异三处**：① 新增 `iterations_max` 列；② 新增 `index_ko.csv`；③ harq 从 5 971 增到 5 975 —— 多出来的 4 个是**同一条链**
（rnti 17929 / harq_id 7，首传 `cap_41736_15527_17929`，donor `cap_41791_16637_17929`，第 5 跳解码 iter 3/3），
它的 4 个成员（1 首传 ＋ 3 重传）比特完全相同（md5 `7ca4e416d3`，1 857 B）。这条链**在 live dump 里没有解码记录**
（`bits_check=no-live-row`），所以旧流程只能留成 `none`；链回放解出了它。CRC 通过 ⇒ 标签可信，但没有空中链的字节级旁证。

另：新 `index_harq_only.csv` 有 **24 行**合并迭代数为空 —— 那些链的链回放没解出来、而旧语料的 `dd_meta` 又没有迭代列，
所以数值未知（留空，不填 6）。

---

## 9. 2026-11-27 10:20 **修正 §8**：`~/ai_depth3/corpus` 是符号链接，不是副本

§8 写的「新 `~/ai_depth3/corpus/tb_s2_amc_iperf_1103i`（= T7 的逐字节副本）」**说法错了**：

```
$ ls -ld ~/ai_depth3/corpus
lrwxr-xr-x  ... /Users/jiachengwang/ai_depth3/corpus -> /Volumes/T7_Corpus/corpus

$ stat -f "%i %N" ~/ai_depth3/corpus/tb_s2_amc_iperf_1103i /Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i
107949500 .../ai_depth3/corpus/tb_s2_amc_iperf_1103i
107949500 /Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i
```

同一个 inode ⇒ **同一个物理目录，两条路径**，磁盘上只有一份 dump（185 MiB），没有"两份副本"可言（§8 里那句
"要不要删掉重复副本"的提议也随之作废）。

对 §8 的对照结论**没有影响**：对照的另一侧 `~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103i` 是
**本机磁盘上的真实目录**（inode 不同、2026-11-04 生成、7 列、42 718 块），与 T7 这份（2026-11-27、8 列、42 722 块）
确实是两棵不同的树。

顺带两条实用事实：

- **旧 dump 的 `units.txt` 仍然可用**：它写的是 `~/ai_depth3/corpus/...` 路径，经符号链接解析到
  `/Volumes/T7_Corpus/corpus/...`，同一份采集（抽第一个单元验证：文件存在）。
- **新的 dump 用 `/Volumes/T7_Corpus/corpus` 作为 `--corpus`**，所以它的 `units.txt` 与旧的只是路径拼写不同；
  两条路径都指向同一批文件，下游按哪个都能取到，但**同一批产物里不要混用两种拼写**（会白算"两个语料根"）。

---

## 10. 2026-11-27 10:35 草稿改为随跑随删（`--keep-scratch` 可保留）

### 背景

`stage/`（5.8 GB/腿）、`rows/`（195 MB）以前留到人工收尾才删。它们对下游没有用处（§6 已核实：全仓无消费者），
留着只是占盘，还让每条腿的峰值到 16 GB。

### 改动（`dump_tb.sh`）

| 时机 | 动作 |
|---|---|
| 每单元写完 `rows/<unit>.row` 与 `tb/<unit>.bits` 之后 | `rm -rf stage/<unit>`（原来只删 `raw/<unit>`）|
| 整个 run 结束（含 harq 第二段）| `rm -rf stage/ rows/ raw/`，并打印 `scratch removed (…; pass --keep-scratch to keep them)` |
| `--keep-scratch` | 两处都跳过，产物保留，供事后排查 |

`rows/` 必须留到 run 末尾（`index.csv` 是它拼起来的），所以它在**最后**删。

### 验证

| 跑法 | 结果 |
|---|---|
| `--limit 200`（默认）| 产物只剩八样；`self+harq=189 = tb 189`、`ko=11`；日志出现 `scratch removed` |
| `--limit 50 --keep-scratch` | `stage/` 保留（180 个条目、6.3 MB）、`rows/` 50 行、`raw/` 在 |

### 影响

- **峰值 16 GB → ≈10 GB/腿**（语料 9.6 ＋ 契约 0.19 ＋ run 期间 rows 0.2），开跑前的最低余量从 20 GiB 降到 **12 GiB**；
- 正常 run 的 dump 目录**天生就是归档形态**（八样），不再依赖人工收尾；
- run 中途挂掉时草稿会留下（有利于续跑与事后排查）——这是有意的。

### 文档

runbook：§0 ⑥、§1.2 容量、§3.2 空间检查、§5.2 参数与草稿行、§5.3 监控（stage 不再增长）、§7.1 收尾（改为核对）、
§8 总账、§9 新增坑 17（想查现场要加 `--keep-scratch`）。

---

## 11. 2026-11-27 10:40 用 cap 20 重跑 1103i：+852 个有 ground truth 的样本

### 做法

1. `dump_meta.txt` 进产物（记录 `ldpc_iterations`、binary、jobs、corpus、起止时间、各群体计数）—— 判断一份 dump 是
   "参考配置"还是"更强的解码器"就看这一行；
2. 删掉 cap-6 那版 dump（185 MiB），用 `OCUDU_REPLAY_DEC_ITERATIONS=20` 重跑同一条命令；
3. 两段（逐单元 ＋ 链回放）都继承这个变量，二进制 banner 打印 `20 decoder iteration(s)`。

### 实测（1103i，cap 6 → cap 20）

| 项 | cap 6 | cap 20 | 差 |
|---|---|---|---|
| `crc_ok`（self）| 36 747 | **37 599** | **+852** |
| └ 原本 harq（借标签）→ self | — | **824** | 比特 824/824 逐字节相同 |
| └ 原本 ko → self | — | **28** | 新增样本 |
| cap6 OK → cap20 KO | — | **0** | 早停 ⇒ 单调 |
| harq 首传 | 5 260 | 4 436 | −824 |
| ko | 7 278 | 7 251 | −27（其中 429 个首传全部撞 20）|
| `tb/` | 42 722 | 42 749 | +27 |
| 运行 | 11 min 8 s | **12 min 39 s** | +1.5 min |
| 链回放 | 5 655 候选 / 5 236 解出 | 4 831 / 4 425（match 4 081、no-live-row **344**、0 mismatch）| 更多链在 20 轮下解出，无 live 旁证的从 11 涨到 344 |

新样本的 `iterations_max` 分布：7:205 8:148 9:111 10:95 11:74 12:42 13:38 14:30 15:30 16:22 17:18 18:17 19:14 20:8。

### 结论

1. **+852 个有 ground truth 的样本（+2.3%）**，其中 824 个从"借 donor 标签"升级成"自己解出"——难度标签因此更干净；
2. **无回归**（cap6 OK 的样本在 cap20 全部仍 OK）；
3. **harq 群体变纯**：能自己解的那批挪去 self，留下的 4 436 才是真正必须合并的；
4. ★ **这份 dump 不是"参考链判决"**：852 个样本空中链（cap 6）解不出。门控/尺子/验收对比必须用 cap 6 的 dump；
   `dump_meta.txt` 的 `ldpc_iterations=20` 是唯一标记，**不要把它当参考 dump 用**。
5. 再加轮次仍有余量（20 轮处还有 8 个刚够，429 个首传撞满 20），但更值得试的是换后段（§5.6 的 `index_ko` 工作清单）。

### 文档

runbook：新增 **§5.7 解码强度**（含上表、三条要点、怎么避免误用）；§5.2 产物表加 `dump_meta.txt`；§7.1/§7.2 九样
（cap-20 归档实测 **19.6 MiB / 20 590 860 B / 42 759 成员**）；§8 加 cap-20 耗时行；§10 标注"这是 cap 6 那一版"。

---

## 12. 2026-11-27 10:50 cap 20 → 40 实测：成本几乎为零，但**收益也是零**

### 疑问

LDPC 有 early termination，是否可以把 cap 提到 40、用很小的代价换更多样本？

### 实测（1103i，cap 20 的 dump 之上）

| 实验 | 结果 |
|---|---|
| 已收敛单元（1 轮解出）在 cap 6/20/40 | 都报 `iterations=1`，20 次平均 **0.50 s 用户时间**（≈25 ms/次，全是进程启动与 I/O）→ **early termination 生效，加 cap 零成本** |
| 7–20 轮才解出的单元 | cap 20 与 cap 40 都是 `iterations_max=15`（同一单元）→ 同样零成本 |
| **撞墙单元**（cap 20 仍 KO 的 429 个首传）| 在 cap 40 下复跑：**0 / 429 解出**；单次 25 ms 与 cap 20 无差别（这些 TB 很小：均值 388 B、397/429 ≤ 500 B）|
| **失败链**（cap 20 的 406 条 `no-decode` 链）| 整链回放在 cap 40 下：**decoded 仍是 4 425 / 4 831**，与 cap 20 完全相同（26.5 s）|

### 为什么没有收益：不是迭代不够，是信号不够

| 群体 | n | SINR 均值 | 中位 | p10 |
|---|---|---|---|---|
| 撞墙 429（cap40 仍 KO）| 429 | **−19.17 dB** | −21.69 | −25.35 |
| 7–20 轮解出的 852 | 852 | **+13.28 dB** | 15.17 | 1.61 |

那 429 个接收的 SINR 在 −19 dB 量级 —— 基本是噪声，再加 20 轮也解不出；而 cap 20 捞到的 852 个在 +13 dB，
只是离收敛差了 1–14 轮。**cap 的膝盖在 20 之前就已经过了**。

### 结论

1. **成本判断对**：early termination 让已收敛单元零成本，撞墙单元也几乎零成本（TB 太小）→ 提到 40 不心疼；
2. **收益判断错**：1103i 上 20→40 **一个样本都不多**；
3. 想要更多样本，杠杆不在迭代数：① 换后段（§5.6 `index_ko` 的 429 条撞墙首传＋6 822 条重传）；
   ② 给 harness 加"强制新缓冲"把 6 822 条重传纳入；③ 再采集；
4. 现有 cap-20 dump **不需要重跑成 40**；旋钮保留，`dump_meta.txt` 继续记录实际用的 cap。

---

## 13. 2026-11-27 11:00 政策：dump 默认 cap 20；1103j 已按 20 跑完并与 cap6 对照

### 政策

`dump_tb.sh` 现在 `export OCUDU_REPLAY_DEC_ITERATIONS=${…:-20}`（两段都继承，banner 与 `dump_meta.txt` 都记录）。
**要参考判决（与空中链可比）必须显式 `=6`**。

### 1103j 实测（cap 6 → cap 20，`--jobs 12`）

| 项 | cap 6（历史）| cap 20（本次）| 差 |
|---|---|---|---|
| `crc_ok`（self）| 38 740 | **39 611** | **+871** |
| ├ harq（借标签）→ self | — | 841（比特 841/841 相同）| 借 → 自有 |
| └ 无标签 → self | — | 30 | 新增 |
| ★ 无标签 → harq（链在 20 轮下解出）| — | **72** | 1103i 只有 4 |
| cap6 OK → cap20 KO | — | 0 | 无回归 |
| harq 首传 | 4 909 | 4 089 | −820 |
| 无标签 | 5 207 | 5 107 | −100 |
| `tb/` | 44 805 | 44 893 | +88 |
| 运行 | — | **11 min 56 s** | |
| 链回放 | 4 947 / 4 894 | 4 106 / 4 081（match 3 622、no-live-row 459、**0 mismatch**）| |
| 仍 KO 的首传（撞 20）| — | **38** | 1103i 是 429 |

新样本 `iterations_max`：7:222 8:149 9:101 10:79 11:58 12:62 13:41 14:36 15:30 16:22 17:23 18:15 19:21 20:12。

### 与 cap6 的逐项对照（对照对象：`~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103j`）

| 项 | 结果 |
|---|---|
| `index.csv` | 50 000 行、unit 集合相同；`sinr_db`/`slot`/`tbs_bytes` **0 差异**；`crc`/`tb_bits` 只在 871 个翻转单元上不同；`iterations` 有 4 870 行不同（KO 行的值从 6 变 20，属预期）|
| `tb/` | 旧 44 793、新 44 893；**共同 44 791 个逐字节相同、0 不一致**；只在旧 2 个（harq→none），只在新 102 个（30 self ＋ 72 harq）|
| `index_ko.csv` | 5 107 ＝ 5 069 `no-buffer` ＋ 38 `decode-failed`（全部撞 20）|

### 结论

1. 两腿合计：**+1 723 个有 ground truth 的样本**（+852 ＋ +871，约 +2.0% 的可训练集）；
2. 借标签→自有标签共 1 665 个，比特全部逐字节一致 ⇒ 标签质量提升而非替换；
3. 1103j 的 72 个"无标签→harq"说明加轮次也能**补出合并样本**（不只补 self）；
4. cap 40 无收益的结论（§12）在 1103j 上同样适用：仅 38 个首传还撞墙，SINR 极低；
5. 参考判决库仍需 cap 6 的 dump —— 历史目录 `~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103{j}` 就是（7 列索引、无 `iterations_max`）。

### 文档

runbook：§0 ④、§5.1、**§5.7 重写**（默认 20 / 参考 6、两腿对照表、新样本分布、cap40 无收益）、§8 耗时行。

---

## 14. 2026-11-27 11:30 批量：211 上其余 100 条 s2_amc 腿（解包 → dump → 对照 cap6）

### 做法（一条腿一条腿，后台低优先级）

两个脚本（都在 `~/ai_depth3/`，与 `dump_1103.sh` 等运行脚本同处；**未入仓**，批跑结束后再决定是否移入 repository）：

| 脚本 | 作用 |
|---|---|
| `run_s2_amc_batch.sh` | 对 211 上每条腿：从 tar.gz 流式解包 → `nice -n 19 dump_tb.sh`（默认 cap 20）→ 与 `~/ai_depth3/dump_s2_amc` 的 cap-6 dump 对照 → 删原始采集保 dump。可续跑（`dump_meta.txt` 有 `finished=` 即跳过；半成品 `mv` 到一边重做）|
| `compare_cap6.py` | 对照并出一行 CSV：units / crc_ok / self / harq / ko / tb、KO→OK（flips）、OK→KO（regressions，必须 0）、`sinr_db/slot/tbs_bytes` 差异（必须 0）、共有比特数与 **md5 不一致数**（必须 0）、只在新/只在旧的比特数。判定 PASS / CHECK / noref |

- 启动方式：`nohup nice -n 19 bash run_s2_amc_batch.sh >> logs_s2_amc_batch/batch.out 2>&1 &`（整棵树继承 nice 19；
  按手册 §21 只用 `nice`，不用 `taskpolicy -b`）；
- `--jobs 12`：与实测天花板一致（每进程内部还会开 2 个线程，14 核上 12 进程已饱和）；
- 每条腿的日志：`~/ai_depth3/logs_s2_amc_batch/<leg>.log`（dump）与 `<leg>.compare.txt`；
- 汇总表：`~/ai_depth3/s2_amc_batch_report.csv`；进程日志：`~/ai_depth3/logs_s2_amc_batch/batch.out`。

### 磁盘策略

一条腿解包 9.6–13 GB，dump 只有 ~185 MB，T7 剩 660 GiB ⇒ **dump 全留（100 × 0.19 ≈ 19 GB），原始采集做完即删**。
代价：以后建库要重新解包该腿（211 上 2–4 分钟）。已存在的 `s2_amc_iperf_1103i/j` 是操作者放在盘上的，不删。

### 首条腿（1103k）实测 —— 流程验证通过

| 项 | 值 |
|---|---|
| 耗时 | 721 s（12 min，含链回放）|
| 群体 | self 38 268 / harq 3 428 / ko 7 270 / tb 42 730 |
| 对照 | cap6 crc_ok 37 616 → **38 268（+652）**；**OK→KO = 0**；`sinr/slot/tbs` 差异 **0**；共有比特 42 650 个 **md5 全同（0 不一致）**；只在新 80、只在旧 6 |
| 判定 | **PASS** |

### 监控

```bash
tail -f ~/ai_depth3/logs_s2_amc_batch/batch.out          # 每条腿的起止与对照结论
awk -F, '{print $1,$2,$5,$10,$11,$14}' ~/ai_depth3/s2_amc_batch_report.csv   # 汇总
```

### 待办

- [ ] 100 条腿跑完（约 24–25 h）；`status=CHECK` 的腿逐条看 `*.compare.txt`
- [ ] 全部完成后：把两个运行脚本移入 repo 或写入 runbook §附录；按 `index_ko` 汇总全局可捞样本

---

## 15. 2026-11-27 12:50 **修正 §14 的磁盘策略**：raw 不再自动删，改为 20 GiB 等待守卫

### 背景

§14 的批处理在每条腿 dump＋对照后**自动删除**解包的原始采集（为在 660 GiB 里跑完 102 条腿）。用户指出：
**建库（母集）需要 raw** —— 每个样本的 X/H/r/sigma2/… 都是在建库那一刻从 raw 提取的，删了就没法建。

### 新策略（`run_s2_amc_batch.sh` 已改）

| 项 | 旧 | 新 |
|---|---|---|
| dump＋对照完成后 | `rm -rf <corpus>/<leg>` | **保留**；由操作者在"该腿建库完成后"手动删 |
| 空间不足 | 直接 `exit 3` 中止 | **等待**：`FREE_FLOOR_GIB=20`，每 300 s 打印一次 `WAITING: N GiB free … floor 20 GiB`，直到腾出空间 |
| 每腿开始前 | 检查 ≥12 GiB，不足即中止 | 同上去等待 |

这样批处理与并行的建库工作流**可以同时跑**：建库吃一条腿的 raw，操作者删掉已建完的腿，批处理自动被放行。

### 已执行的补救

- 已删 raw 的 3 条腿 **1103k / 1103l / 1103m 正在重新解包**（`/tmp/reextract.sh`，nice 19）；
- 批处理已按新脚本重启，当前在解包 `1103o`；已完成的 1103i/j/k/l/m/n 会被跳过；
- 已完成记录补进报告：1103n 的对照行（+813，PASS）。

### 守卫验证

把 `FREE_FLOOR_GIB` 临时设成 100000 跑一条腿，脚本只打印：
`[12:44:48] WAITING: 659 GiB free on /Volumes/T7_Corpus/corpus, floor 100000 GiB (delete the captures of a leg that has been built)`
然后 sleep，不启动 dump、不中止 —— 符合预期。

### ★ 本次踩到的两个操作坑（都真实发生）

1. **`pkill -f <pattern>` 会杀掉自己的 ssh 会话**：远端命令里若含同样的路径字符串，pkill 会匹配到自身（`bash -c` 的 cmdline）。
   两次把正在排查的 ssh 会话直接杀掉。正确写法：把 pkill 放进一个**不含 pattern 的脚本文件**再执行，或 kill 具体 PID。
2. **`timeout` 在 macOS 不存在**：守卫测试用 `timeout 8 …` 直接失败；改为后台跑 + 睡 + `kill <pid>`。
   另外后台测试脚本如果只 `kill` 了外层 wrapper，**子进程会残留**（guard 测试的 driver 就留了一条，后来按 PID 杀掉）。

### 状态（截至 12:50）

| 项 | 状态 |
|---|---|
| T7 | 659 GiB free；已保留 raw：1103i/j/k（k 正在重解包）/n/o（o 正在解包）|
| 已 dump 完 | 1103i/j（cap20）、1103k +652、1103l +855、1103m +1349、1103n +813（均 PASS）|
| 批处理 | 运行中（nice 19，jobs 12），下一条 1103o |
| 211 w2 | `w2_harq_1027` dump 进行中（14 331 / 23 975），cap-6 参考已解包待对照 |

---

## 16. 2026-11-27 14:30 w2 首批 10 腿开跑（保留 raw，跑完退出等建库）

### 决策

用户要求：**保留原始采集**（建库要读），先 dump **10 腿**，完成后退出等建库。于是：
- 211 上新建 `~/run_w2_batch.sh`（nice 19、`--jobs 6` ≈ 12 线程、空间守卫 floor 100 GiB，**不删任何 raw**）；
- 跑完 10 条腿自动结束，不自动接续；原始采集与 dump 都留在 `/mnt/corpus/w2_tb_tarballs/`。

### 选的 10 腿（覆盖 4 调制 × ping/iperf，压缩总量 18 GB）

`16QAM_ping_1029b/c/d/e`、`QPSK_ping_clip_1029g`、`64QAM_ping_1030a`、`256QAM_iperf_1030d`、
`mcs06_ping500_1027`、`mcs12_ping500_1027`、`mcs20_ping100_1027`。

### ★ 实测压缩比远高于 s2：一条腿 ≈ 26–36 GB raw

| 腿 | tar.gz | 解包后 | 压缩比 | 单元数 |
|---|---|---|---|---|
| w2_harq_1027 | 1.12 GB | 36 GB | 32× | 23 975 |
| 16QAM_ping_1029b | 0.46 GB | **26 GB** | **56×** | **8 960** |

⇒ 10 腿 raw 预计 **≈ 1.0 TB**（17.95 GB × ~56），加 dump（5.8 GB/腿）≈ 1.06 TB；211 现余 1.6 TB，够但需盯：
守卫在 free < 100 GiB 时**等待**（不删），到时要么腾空间、要么停批。

### 每腿流程

解包**完成**后才启动 dump（1027 曾因并行解包只抓到 18 214/23 975 个单元）→ dump（默认 cap 20）→
解包该腿的 cap-6 参考 → `compare_cap6.py` 判定 → 保留 raw → 下一条。

### 已修复的脚本 bug

`df -g` 是 macOS 语法，Ubuntu 上直接报 `invalid option -- 'g'`，守卫读到空值会永久等待；
改成 `df -BG --output=avail /mnt/corpus | tr -dc '0-9'`。首次启动因此空转 5 分钟，已改正并重启（14:18）。

### 监控

```bash
ssh jwang@192.168.31.211 'tail -f /tmp/w2_batch.out'                     # 每条腿的起止、units、对照结论
ssh jwang@192.168.31.211 'column -s, -t ~/w2_batch_report.csv'          # 汇总（与 s2 同一套列）
```

---

## 17. 2026-11-27 21:00 `iterations=0` 的根因与三层修复（dump 侧截断，母集侧夹紧）

### 背景

母集审计发现少量样本 `iterations=0` 而 `iterations_max≥1`。0 在 dump 里的约定是"没解出来"（KO），
而这两个字段来自同一次解码 —— 同一列里出现了两种含义，必须收敛。

### 做了什么 / 实测

| 项 | 值 | 怎么测的 |
|---|---|---|
| 根因 | `ul_chain_replay.cpp` 的 `nof_iters = static_cast<unsigned>(stats.get_mean())`：**float 均值被截断**，真实均值落在 [0,1) 就记成 0 | 读代码；6 个样本在 `chain_iters.csv` 里就是 `iterations=0, iterations_max=2` |
| 出现位置 | **只在 harq 链解码**；`index.csv` 的逐单元解码（首传）没有 0 | 扫全部 dump 的 `index.csv`（0 个）与 `index_harq_only.csv`（有）|
| 规模 | 已建成的 13 条腿共 **3 481** 个：1103l 714、1103m 812、1103n 652、1103o 600、1103i 168、1103j 224、1103k 252；1102b–f 每条 4–28 | 扫母集 npz 的 `iterations` |
| A/B（1102g，4 679 条链）| 旧二进制 `iterations=0` **25 个 → 新二进制 0 个**；变化：0→1 ×25、1→2 ×23、**2→3 ×859**、3→4 ×30、4→5 ×16、5→6 ×4；`iterations_max` **不变** | 旧二进制副本与新二进制各跑一遍 `dump_harq_chains.py` |
| 修复①（dump 源码）| `lround` 取代截断，且 CRC-OK 时两字段下限为 1；**20:57:54 重链接** | 1102s 的 `dump_meta.txt`：`binary_mtime=2026-11-27 20:57:54` |
| 修复②（建库）| CRC-OK 行的 0 夹到 1（`-1` 不动）| 覆盖修源码之前已 dump 的腿 |
| 修复③（历史 npz）| `clamp_iterations.py` 用 `zip` 就地替换两个成员 | 32 个文件 / 3 481 个值 / **19 秒**；`X` 的 md5 前后一致 |

### 结论（可直接引用）

1. ★ **2026-11-27 20:57:54 之前产的 dump**，其 harq 行的 `iterations=0` 表示"均值不足 1 轮"，**不是** KO。
   判据是 `dump_meta.txt` 的 `binary_mtime`；1102s 起的 dump 没有这个问题。
2. ★ **受影响的 dump 不重跑**（1102b–r 中已 dump 的 17 条）：母集侧已夹紧，重新 dump 约 2 小时只换回
   0.01–1% 的数值语义，不划算。
3. ★ **四舍五入是有意改变口径**：932/4 679 条链的 `iterations` +1（2→3 占 859），其中 46 条跨进"临界"
   （3→4 30 条、4→5 16 条）。以后与更早的 dump 比 `iterations` 分布时，要知道这个台阶。
4. ★ 建库监控不需要为 replay 的改动重启（它不调用 replay 二进制）；但为了用上第②层夹紧，
   21:00:02 重启过一次，1102g 用新规则从头重建。

### 遗留 / 待办

- [ ] 任何"跨 dump 版本"的分析，先看 `dump_meta.txt` 的 `binary_mtime`
- [ ] 1102g 重建完成后确认 `iterations=0` 为 0（它的 `chain_iters.csv` 里仍有 0，由第②层兜住）

### 涉及文件

- `lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp`（`lround` ＋ CRC-OK 下限）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/build_depth3_dataset.py`（建库夹紧）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/clamp_iterations.py`（历史 npz 修复，新增）
- `doc_chinese/depth3_llr_ai/mother_set_build_runbook.md` §3.1 / §5.3 / §9 坑 #22

---

## 18. 2026-11-28 08:15 w2 解包与建库暂停：exFAT＋SMR 盘不适合几十万小文件

### 背景

w2 的解包（`run_w2_batch.sh`）在建库 watcher 上线后仍极慢，`w2_harq_64QAM_ping_1030a` 跑了 13 小时 23 分
才到归档的 64%，最终被 SIGKILL（批处理记 `EXTRACT FAILED` 并跳下一条腿）。经排查是存储介质问题，
不是流程或脚本问题。生产方与建库 watcher 均已停止，等换快盘再续。

### 实测

| 项 | 值 | 怎么测的 |
|---|---|---|
| 目标文件系统 | `/mnt/corpus` = **exFAT**，块大小 **262 144 B（256 KB）** | `stat -f /mnt/corpus` |
| 物理盘 | **TOSHIBA MQ04ABB400**，2.5″、5400 rpm、**SMR**（`ROTA=1`） | `lsblk -d -o NAME,ROTA,SIZE,MODEL` |
| 归档真实内容 | 压缩 4.87 GB → **解压 17.2 GB**（du 报 99 GB 是 256 KB 簇的分配膨胀） | `gzip -l` |
| 解包速率 | **~5–14 个文件/秒、0.2 MB/s**；`user +0 / sys 88%`、`iowait 85%` ⇒ 卡在内核写路径，不是解压 | `/proc/PID/stat`、`/proc/PID/io`、`vmstat` |
| 进度 | gzip 读到 3.11/4.87 GB（64%）用了 12 h 50 m ⇒ 全程约 20 h | `/proc/<gzip>/fdinfo/<fd>` 的 `pos` |
| 建库（读同一块盘）| `w2_harq_1027` 23 415 样本 **210 min**（0.54 s/样本）；`w2_harq_16QAM_ping_1029b` 7 323 样本 **258 min**（2.1 s/样本）| watcher 进度表 |

### 结论

1. ★ **exFAT 的 256 KB 簇 + SMR 机械盘 + 每条腿 60 万个小文件**是根因；换 ext4/NVMe 或把解包放到
   Mac 的 APFS 上，同一批归档是分钟级而不是小时级。
2. ★ 建库本身没有问题：暂停前两条腿都 `findings: none`、manifest 齐全、30 738 样本 `iterations=0` 为 0。
3. ★ 解包被 SIGKILL 的 `1030a` 没有 dump，半成品目录还在（~60 万文件 / 17.2 GB 内容），恢复时要重解。

### 遗留 / 待办

- [ ] 把解包改到 NVMe（`/home`，ext4，一次一条腿）或搬到 Mac，再恢复 w2 的解包→dump→建库
- [ ] 恢复建库：`w2_tb_tarballs/` 里已有 4 条未建的 dump（1029c/d/e/g），watcher 会自动捡起
- [ ] `1030a` 重解；`1030d`、mcs06/12/20 与归档里其余 w2 腿都还没解包
- [x] 2026-11-28 08:20 把 `1030a` 的半成品目录**改名**为 `w2_harq_64QAM_ping_1030a.partial`：
      `run_w2_batch.sh` 的判据是 `[ -d "$src" ]` 即"已解包"，留着原名会让它拿 64% 的树去 dump
      （该脚本注释里 1027 就犯过：只看到 18 214/23 975 个单元）。改名可逆，`.partial` 约 30 万项、
      17.2 GB 内容，确认无用后可删

### 涉及文件

- `/mnt/corpus/...`（数据，不动）
- `~/run_w2_batch.sh`（生产方，另一个工作流，已停）
- `lib/phy/upper/ai_signal_processors/depth3/ai_train/watch_dumps_build_w2.sh`（建库 watcher，已停）

---

## 19. 2026-11-28 s2 母集回填 `snr_nominal`：统一到 dump 的 `sinr_db`

### 背景

w2 那边发现 `snr_nominal` 原先取自采集的 `_ce.txt snr=`（另一个估计器，0.01–2.4% 的样本会算出负 dB，
p5 差 −9 dB），已改为取 dump 的 `sinr_db`（`build_depth3_dataset.py` 的 `candidate_units` 多带一列）。
按同一口径，本机 s2 母集也做一次回填。

### 做法

`refill_snr.py --out /Volumes/Eaget_HD/datasets_full_s2_amc --dump-root /Volumes/T7_Corpus/corpus`：
按 `_progress.csv` 里 `ok` 的腿逐文件重算 `snr_nominal`（单元 → dump 的 `index_self.csv` /
`index_harq_only.csv` 的 `sinr_db`），用 zip 成员替换写回（与 `clamp_iterations.py` 同法，X/H 不解压、
字节不变）。

### 结果

| 项 | 值 |
|---|---|
| 文件 | **275 / 276**（`1121ac_256QAM_harq` 本来就在 1 ULP 内，0 变化）|
| 改写样本值 | **1 481 862** |
| 耗时 | **12 分 52 秒**（68 GB 的 zip 重写）|
| 幂等性 | 再跑一次 dry-run：**0 变化** |
| 逐样本一致性 | 抽查 `1103i`（41 987 样本）、`1121ac`（46 522）：与 dump 最大差 **0.0000 dB** |

★ **负 SINR 变多不是错误**：`1103i` 回填后有 3 595 个负值（8.6%），因为 **dump 本身**就记了负 SINR ——
其中 **3 558 个是 QPSK**（中位 −3.0 dB），迭代均值 2.62（非负样本 2.08），量级与"边缘 QPSK 用户在灵敏度附近
用多轮迭代解出"一致；16QAM/64QAM 的负值只有 15/22 个（迭代均值 6.4，确实难）。旧的 `_ce.txt` 口径把这些
QPSK 记成正的，所以回填后负值数从 662 涨到 3 595。

### watcher

**不需要重启**：`build_mother_set.sh` 每个 (调制, 群体) 现起 `build_depth3_dataset.py`，改的是磁盘上的源码。
回填期间 watcher 正常切到 `1121d`（11:30:29），它从第一个 pair 起就是新口径。

### 遗留

- [ ] 若要按 SNR 分箱，仍以 dump 的 `sinr_db` 为准（npz 的 `snr_nominal` 现在与它一致，但只是副本）
- [x] 负 SINR 样本**不过滤**（2026-11-28 决定）：二维分箱里 `snr <= 0` 本来就是一格，过滤反而会
      把那一格掏空；母集口径保持"每个可解出的 TB 都要"。★ 分箱时注意 `snr <= 0` 是**一个**桶，
      不是异常值 —— `1103i` 的那 3 595 个 QPSK 负 SINR 样本就落在这一格里
