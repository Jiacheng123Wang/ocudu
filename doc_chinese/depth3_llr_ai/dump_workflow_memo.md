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
