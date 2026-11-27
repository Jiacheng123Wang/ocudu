# 单腿 dump runbook：从 211 的语料 tar.gz 到「可解码的 TB ＋ 索引」

> **用途**：给定一条采集腿的归档 `<leg>.tar.gz`，在一台空闲的 Mac 上，从 **解包** 一路做到
> `<out>/{index.csv, units.txt, tb/}`，并用**三条判据**验收通过。
> **本文是这条流水线的操作入口**；方法论、坑的出处与后续步骤见
> `depth3_llr_ai/corpus_to_training_to_measurement_db_manual.md` §6（dump）、§7（建库）、
> §16.4（删除后的恢复路径）、§22/§23（归档与流式读归档）。
> **实测日期**：2026-11-27（本机 Apple M4 Pro，14 核／64 GB／macOS，T7 APFS）。
> **实测腿**：`s2_amc_iperf_1103i`（50 000 个接收，iPerf 上行，AMC）。
> ★ 本文出现的**每个数字都是那一天实测**；数字随腿而变，**判据不变**。
> 下文用两个占位符：`<leg>` ＝ 腿名，`<out>` ＝ dump 输出目录（约定 `.../tb_<leg>`）。

---

## 0. 一分钟版（复制即可跑）

```bash
# ── 变量区：换腿只改第一行 ──────────────────────────────────────────────
LEG=s2_amc_iperf_1103i
REPO=/Users/jiachengwang/dev/ocudu
CORPUS=/Volumes/T7_Corpus/corpus          # 解包目标（语料根）
OUT=$CORPUS/tb_$LEG                       # dump 输出：一条腿一个目录
BIN=$REPO/build/lib/phy/upper/channel_processors/ul_chain_replay_cpu
TOOL=$REPO/lib/phy/upper/ai_signal_processors/depth3/ai_train/dump_tb.sh
REMOTE=jwang@192.168.31.211
TARBALL=/mnt/corpus/s2_amc_corpus_tarballs/$LEG.tar.gz

# ① 二进制：先确认是新的（过期会静默丢列），再谈 dump
make -C $REPO/build -j14 ul_chain_replay_cpu          # 已最新则几秒空转
# ② 解包（流式，不落 4 GB 中间文件；排除 macOS 的 ._ 元数据成员）
mkdir -p $CORPUS
ssh $REMOTE "cat $TARBALL" | tar -xzf - -C $CORPUS --exclude '._*'
# ③ 解包校验（完整四条见 §4；本腿文件数应为 293 510）
find $CORPUS/$LEG -type f | wc -l
# ④ dump（12 路并行；50 000 单元约 12 分钟，含 self 与 harq 两段；默认 LDPC cap 20，参考配置加
#    OCUDU_REPLAY_DEC_ITERATIONS=6；见 §5.5 与 §5.7）
cd $REPO && bash $TOOL --corpus $CORPUS --out $OUT --jobs 12 --binary $BIN $CORPUS/$LEG
# ⑤ 三条判据（见 §6）
python3 - "$OUT" <<'PY'
import sys, os
out = sys.argv[1]
us = [l.strip() for l in open(os.path.join(out, "units.txt")) if l.strip()]
with_bin = sum(1 for u in us if os.path.exists(u[:-4] + ".bin"))
rows = sum(1 for _ in open(os.path.join(out, "index.csv"))) - 1
ok = sum(1 for l in open(os.path.join(out, "index.csv")) if ",OK," in l)
print(f"units={len(us)} with_bin={with_bin} index_rows={rows} crc_ok={ok}")
import csv
lab = [r["label_from"] for r in csv.DictReader(open(os.path.join(out, "index_harq.csv")))]
print("self+harq:", sum(1 for x in lab if x in ("self", "harq")),
      "tb:", len(os.listdir(os.path.join(out, "tb"))),
      "ko:", sum(1 for _ in open(os.path.join(out, "index_ko.csv"))) - 1)
PY
# ⑥ 收尾：草稿由工具自动删（--keep-scratch 才保留）；产物共九样（含 dump_meta.txt）
ls $OUT; find $OUT -name .DS_Store -delete
```

---

## 1. 角色、路径与容量

### 1.1 三处路径

| 角色 | 位置 | 内容 |
|---|---|---|
| **归档机 211** | `jwang@192.168.31.211:/mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz` | ★ 原始采集归档，**一条腿一个包**（该目录 102 个包：95 条 ping ＋ 7 条 iperf，合计 340 GB）|
| 归档机 211（旁路） | `/mnt/corpus/s2_amc_tb_tarballs/tb_<leg>.tar.gz` | ★ **dump 的归档**（本腿现有的是 2026-11-06 的 22 MB 旧版；新流程产出 19.4 MiB，见 §7.2）|
| **本机代码仓** | `/Users/jiachengwang/dev/ocudu` | dump 工具与 replay 二进制源码 |
| **本机语料盘** | `/Volumes/T7_Corpus/corpus` | 解包目标 ＋ dump 输出（T7 为 APFS，2026-11-27 实测余量 676 GiB）|

约定：`<out>` 永远是 `.../tb_<leg>`，**而不是语料根目录**。`build_mother_set.sh` 正是按
`DUMP=$DUMP_ROOT/tb_$LEG` 找 dump（`build_mother_set.sh:65`），
`CAP=$CORPUS_ROOT/$LEG` 找捕获（`:66`）。
位置可以换：手册 §6 的示例放在训练机的 `~/ai_depth3/dump_s2/tb_<leg>`；
**本文按本次实际执行放在语料盘** `/Volumes/T7_Corpus/corpus/tb_<leg>`（那里有 ≥ 600 GiB 余量，
且与捕获同盘，`build_mother_set.sh` 只要把 `DUMP_ROOT` 指过去即可，见 §7.3）。

### 1.2 一条腿的容量预算（实测）

| 阶段 | 占用 | 说明 |
|---|---|---|
| 归档 tar.gz（网络上） | **4.05 GB** | 本腿 4 053 908 427 B（gzip 把 9.58 GB 内容压到 4.05 GB，2.37×）|
| 解包后原始语料 | **9.6 GB / 293 510 文件** | 9 576 543 376 B，见 §4 |
| dump 草稿 `stage/` | 运行中**每单元即删**（常驻几 MB）| `--keep-scratch` 时才会累积到 **5.8 GB** |
| dump 草稿 `rows/` | **195 MB**（run 期间）| 拼 `index.csv` 用；run 结束自动删 |
| dump 契约产物 | **≈ 185 MiB** | `tb/` 172 MiB（self ＋ harq）＋ `index.csv` 2.2 ＋ `units.txt` 3.4 ＋ 三个群体索引 5.2 ＋ `chain_iters.csv` 0.44 |
| **峰值合计** | **≈ 10 GB/腿** | 所以开跑前 **T7 至少留 12 GiB**（草稿不再累积）|

---

## 2. 步骤 0：代码与二进制（**唯一容易漏、又静默出错的一步**）

### 2.1 为什么必须先做

dump 的语义是「**经典接收机的判决**」：`dump_tb.sh` 把工具内部钉死成
`--phy_pipeline cpu --pusch_receiver_backend classic`，输出就是 CPU 经典链的 CRC 判决。
二进制**过期**时不会报错，只会安静地少一列或按旧逻辑落盘：

> **真实事故（本仓 2026-11-26）**：`ul_chain_replay.cpp` 增加了「最差码块的 LDPC 迭代数」
> `iterations_max`，而 `~/ai_depth3/dump_s2_amc_i6/tb_s2_amc_iperf_1103i/index.csv` 是旧二进制产的，
> 这一列**整列为空**（只有 7 列有值）——难度维（临界样本）直接不可用。
> 本次 dump 前检查发现：二进制 **10-27** 构建，源码 **11-26** 修改 ⇒ 重编。

### 2.2 检查与重建

```bash
cd /Users/jiachengwang/dev/ocudu
git log -1 --format='%h %ad %s' --date=iso          # 本仓 HEAD: 3ce0f15a9a 2026-11-26
stat -f 'binary: %Sm' -t '%Y-%m-%d %H:%M' build/lib/phy/upper/channel_processors/ul_chain_replay_cpu
stat -f 'source: %Sm' -t '%Y-%m-%d %H:%M' lib/phy/upper/channel_processors/metal/test/ul_chain_replay.cpp

# 重建（增量；本次只重编 exe 源文件，实测 12.7 s）
make -C build -j14 ul_chain_replay_cpu
```

判据：**源码比二进制新就重编**；不确定时直接 `make`（已最新时是几秒空转，代价可忽略）。
`build/` 是 `Unix Makefiles` / `CMAKE_BUILD_TYPE=Release`，目标名就是 `ul_chain_replay_cpu`。

### 2.3 功能探针（证明「新二进制在，且吐出 `iterations_max`」）

```bash
U=$CORPUS/s2_amc_iperf_1103i/cap_10023_16147_17922       # 本腿任意一个单元
mkdir -p /tmp/probe_tb
OCUDU_HELENA_DUMP_DIR=/tmp/probe_tb OMP_NUM_THREADS=1 \
  build/lib/phy/upper/channel_processors/ul_chain_replay_cpu "$U" \
  --phy_pipeline cpu --pusch_receiver_backend classic --out /tmp/probe_stage \
  | grep '\[0\]'
ls -l /tmp/probe_tb/tb_*.bits
rm -rf /tmp/probe_tb /tmp/probe_stage
```

**本腿实测预期**（逐字可对照）：

```
  [0] tbs=217 slot=16147 rnti=17922 QPSK: crc=OK iterations=1 iterations_max=1 sinr=7.90 dB epre=0.00 dB rsrp=0.00 dB
tb_00000000_tbs1736.bits   217 字节   （= 1736 bit = tbs 217 B × 8）
```

看不到 `iterations_max=` ⇒ 二进制还是旧的，**停下来重编**。

---

## 3. 步骤 1：从 211 取归档并解包

### 3.1 命名与布局

- 包名 = 腿名 ＋ `.tar.gz`：`/mnt/corpus/s2_amc_corpus_tarballs/s2_amc_iperf_1103i.tar.gz`
  （先确认包在不在、多大：`ssh jwang@192.168.31.211 'ls -l /mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz'`）
- **tar 内顶层目录就是腿名**（`s2_amc_iperf_1103i/...`），所以解到语料根即可，不需要 `--strip-components`
- 包里有 **macOS 元数据成员** `._<name>`（AppleDouble，各 163 B 级）：本腿 **293 508 个**，
  它们在 `*.txt` 通配里长得像单元、在清点里会把数字翻倍（手册 §16.2）。解包时直接排除

### 3.2 推荐做法：流式解包（不落 4 GB 中间文件）

```bash
df -h /Volumes/T7_Corpus          # ★ 至少 12 GiB 可用（本腿峰值 ≈ 10 GB；草稿每单元即删）
mkdir -p /Volumes/T7_Corpus/corpus
ssh jwang@192.168.31.211 "cat /mnt/corpus/s2_amc_corpus_tarballs/s2_amc_iperf_1103i.tar.gz" \
  | tar -xzf - -C /Volumes/T7_Corpus/corpus --exclude '._*'
```

**实测**：整条命令 **2 分 12.7 秒**（LAN 实测 103 MB/s ⇒ 4 GB 传输约 40 s，其余是解压＋把 9.6 GB
写成 29 万个小文件到 APFS）。macOS 的 `bsdtar` 从 stdin 读 gzip 没问题。

> ★ 目标目录**必须不存在**（或确认是空的再解）。已有一份就换个名字，不要在旧目录上叠着解：
> 叠解出的混合目录既不能归因，也数不清文件（和手册 §4 那句「不要在同一个目录上续采」同理）。

### 3.3 备选 A：先拷到本地再解（网络不稳、想留一份 tar 时）

```bash
scp jwang@192.168.31.211:/mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz /Volumes/T7_Corpus/
tar -xzf /Volumes/T7_Corpus/<leg>.tar.gz -C /Volumes/T7_Corpus/corpus --exclude '._*'
rm -f /Volumes/T7_Corpus/<leg>.tar.gz        # 不想留就删
```

### 3.4 备选 B：在 211 上解、再 rsync 回来（手册 §16.4 的历史路径）

```bash
ssh jwang@192.168.31.211 'mkdir -p /mnt/corpus/restore && tar -C /mnt/corpus/restore -xzf \
     /mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz --exclude "._*"'
rsync -a jwang@192.168.31.211:/mnt/corpus/restore/<leg>/ /Volumes/T7_Corpus/corpus/<leg>/
```

> ⚠ 手册 §16.4 写的是 `/mnt/corpus/s2_corpus_tarballs/<leg>.tar`（**旧批次的 `.tar`**）。
> 本批（AMC，2026-11-05 归档）是 **`.tar.gz`，在 `s2_amc_corpus_tarballs/`**。路径别抄错。

### 3.5 完整性：包本身对不对

```bash
# 可选，磁盘受限：完整读一遍，rc=0 才算没截断（本腿实测 1 分 55 秒）
ssh jwang@192.168.31.211 'tar -tzf /mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz > /dev/null; echo rc=$?'

# 可选：整包 sha256（4 GB、磁盘受限，约 2 分钟；本腿 2026-11-27 实测见 §10）
ssh jwang@192.168.31.211 'sha256sum /mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz'
```

---

## 4. 步骤 2：解包后校验（四条硬指标）

```bash
L=/Volumes/T7_Corpus/corpus/s2_amc_iperf_1103i
ls $L | wc -l                                   # 顶层条目（含 tb_live/ 与 logs/ 两个目录项）
find $L -type f | wc -l                          # 文件总数
find $L -type f -exec stat -f %z {} + | awk '{s+=$1} END{print s}'   # 总字节
find $L -name '._*' | wc -l                      # macOS 元数据残留（应为 0）
```

**本腿实测（照此对表）**：

| 指标 | 实测值 | 推导 |
|---|---|---|
| 顶层条目 | **200 002** | 200 000 个捕获文件 ＋ `tb_live/` ＋ `logs/`（`ls` 把目录算一条）|
| 文件总数 | **293 510** | 200 000 ＋ `tb_live` 93 507 ＋ `logs` 3 |
| 总字节 | **9 576 543 376** | 与 tar 内「真实成员」字节合计**逐字节相等**（见下）|
| `._` 文件 | **0** | `--exclude '._*'` 生效 |
| 单元数 | **50 000** | 200 000 / 4（每单元 4 个文件：`<u>.txt`、`<u>.bin`、`<u>_h.bin`、`<u>_ce.txt`）|

按类别（本腿）：

| 类别 | 个数 | 字节 |
|---|---|---|
| `<u>.txt`（PDU 侧车，dump 只吃这个做单元清单）| 50 000 | 31 314 444 |
| `<u>.bin`（接收网格）| 50 000 | 3 427 200 000 |
| `<u>_h.bin`（同一网格形状的信道估计 H，complex64，14×612 RE ＝ 68 544 B）| 50 000 | 3 427 200 000 |
| `<u>_ce.txt`（CE/SNR 侧车，**dump 不用**）| 50 000 | 6 320 587 |
| `tb_live/*`（采集时 live dump 的 TB，§7 建库的 harq donor 会用到）| 93 507 | 2 574 830 738 |
| `logs/*` | 3 | 109 677 607 |

> ★ **和 tar 对账**：本腿 tar 有 **587 021 个成员** ＝ 3 个目录 ＋ 293 510 个真实文件 ＋ 293 508 个 `._`；
> 真实成员字节合计 **9 576 543 376**，与解包后磁盘清点**完全相等**。
> （tar 物理大小 4.05 GB ≠ 内容 9.58 GB，是 gzip 压缩，不是缺数据。）

抽样比对（本机已有同一腿另一份时最省事，一次就能证明字节一致）：

```bash
md5 -q $L/cap_10023_16147_17922.bin     # 本腿实测 5dedb2827d06a298f83e4b99bfc6662f
head -3 $L/cap_10023_16147_17922.txt    # 必须是 slot=16147 / scs_khz=30 / cp=normal 起头
```

---

## 5. 步骤 3：跑 dump

### 5.1 命令（本腿实跑的那条）

```bash
cd /Users/jiachengwang/dev/ocudu
LIB=lib/phy/upper/ai_signal_processors/depth3/ai_train

bash $LIB/dump_tb.sh \
  --corpus /Volumes/T7_Corpus/corpus \
  --out    /Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i \
  --jobs   12 \
  --binary build/lib/phy/upper/channel_processors/ul_chain_replay_cpu \
  /Volumes/T7_Corpus/corpus/s2_amc_iperf_1103i
```

建议在命令末尾接 `2>&1 | tee /tmp/dump_${LEG}.log`：结束时会打印
`attempted N units, M decoded CRC-OK`、链回放的 `decoded=…` 与最终 `populations: self=… harq=…` 三类行，事后核对用。

**默认含 harq 半程**（`--no-harq` 可关）：第一条通道逐单元回放（self），随后对首传失败的 HARQ 链整链回放
（一条链一个进程，软缓冲共享 ⇒ 合并解码），把恢复出来的 TB 写进同一个 `tb/`，并把合并解码的迭代数写进
`index_harq*.csv`。见 §5.5。

**默认 LDPC 迭代上限 20**（2026-11-27 起；脚本内 `export OCUDU_REPLAY_DEC_ITERATIONS=${…:-20}`，两段都继承）。
它是"标签采集"强度，不是空中值：要参考判决就显式 `OCUDU_REPLAY_DEC_ITERATIONS=6`。理由、实测与误用警告见 §5.7。

### 5.2 参数说明

| 参数 | 作用 | 要点 |
|---|---|---|
| `--corpus DIR` | 语料根 | **仅在没给腿目录时用于默认值**；单元清单来自位置参数 |
| `--out DIR` | 输出目录 | ★ **一条腿一个 `tb_<leg>`**；给根目录会把多条腿混在一起 |
| `--jobs N` | **N 个单元同时在飞**，每单元一个进程 | 每进程单线程（脚本内导出 `OMP_NUM_THREADS=1`）；★ 14 核上 **12** 是实测上限（手册 §6：12 个并发进程已到负载天花板）|
| `--binary PATH` | replay 二进制 | 优先 `ul_chain_replay_cpu`（纯 CPU 经典链）；脚本另钉死 `--phy_pipeline cpu --pusch_receiver_backend classic` |
| `--limit N` | 只跑排序后的前 N 个单元 | 冒烟用，见 §5.4；★ 链回放也只用这 N 个单元，链条被切断，数字不代表本腿 |
| `--no-harq` | 关掉第二段（harq 半程）| 只要逐单元结果时用；此时 `tb/` 只装 self，`index_*` 三个文件不生成 |
| `--keep-scratch` | 保留 `stage/`、`rows/`、`raw/` | 只在需要事后排查某单元时用；正常跑不要加（会多占 ~6 GB）|
| 位置参数 | **腿目录**（可多条） | 脚本用 `find -maxdepth 1 -name '*.txt' ! -name '*_ce.txt' ! -name '._*'` 建单元清单 |

产物结构（全部在 `--out` 下）：

| 产物 | 性质 |
|---|---|
| `index.csv` | ★ 契约：表头 ＋ 每单元一行，8 列 `unit,tb_bits,crc,sinr_db,iterations,iterations_max,slot,tbs_bytes`（按 `units.txt` 排序拼装，可复现）|
| `units.txt` | ★ 契约：单元清单（50 000 行绝对路径，`LC_ALL=C` 排序）|
| `tb/<unit>.bits` | ★ 契约：**每个可训练样本一个 TB 比特文件**：self ＝ 自己解出的；harq ＝ 合并解码解出的（同一目录、同一命名）|
| `index_self.csv` / `index_harq_only.csv` / `index_harq.csv` | ★ 契约：按群体拆开的索引（self / harq 首传 / 全量 + `label_from`）|
| `index_ko.csv` | ★ 契约：**没有 label 的样本清单**（`label_from=none`，与 self / harq 互斥）；字段见 §5.6，是换后段解码器捞样本的工作清单 |
| `chain_iters.csv` | ★ 契约（审计用）：每条候选链的恢复结果 —— `hop`、`decoded_unit`、`iterations/iterations_max`、`bits_check` |
| `dump_meta.txt` | ★ 契约（**出处**）：`ldpc_iterations`、`binary`、`binary_mtime`、`jobs`、`corpus`、`legs`、起止时间与各群体计数。**判断这份 dump 是"参考配置"还是"更强的解码器"就看这一行** |
| `rows/<unit>.row` | 草稿：`index.csv` 的一行；**run 结束自动删** |
| `stage/<unit>/` | 草稿：逐单元 staged 捕获；**该单元写完行与比特即删**（约 3.4 文件/单元，5.8 GB/腿）|
| `raw/<unit>/` | 草稿：单元级 scratch，每单元跑完即删；目录本身在收尾时删 |
| （以上三样） | ★ 默认**不留在产物里**；`--keep-scratch` 才保留，用于事后排查 |

### 5.3 实测耗时与节奏

| 项 | 实测（2026-11-27 干净重跑）|
|---|---|
| **整条流程**（50 000 单元 ＋ 链回放 ＋ 打标）| **11 分 8 秒** |
| 其中：逐单元回放 | ≈ 10 分钟（**≈ 83 单元/秒**，12 路 ⇒ 每单元约 0.145 s）|
| 其中：链回放（5 655 条链）| **≈ 40 秒**（一条链一个进程）|
| 其中：打标（三个索引 ＋ 拷比特）| ≈ 20 秒 |
| 负载 | 1 分钟负载均值峰值 33（含 T7 的 I/O 等待）；吞吐已饱和，**不要再加 jobs** |

> ★ **同时跑多条腿时，限制的是「总进程数」不是「每条腿的 jobs」**：手册 §6 的实测上限是
> **12 个并发进程**（当时的形状是 4 条腿 × 3 worker，负载 17.8/14）。本 runbook 单腿用 12；
> 若要 2 条腿并跑，就各给 6。

跑起来之后每 30 秒看一眼（数字要涨）：

```bash
O=/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i
while :; do echo "$(date +%H:%M:%S) rows=$(ls $O/rows | wc -l) tb=$(ls $O/tb | wc -l)"; sleep 30; done
```

> 草稿默认**随跑随删**，所以 `stage/` 不涨（`--keep-scratch` 时才会涨到 ~5.8 GB）；
> 能看到的进度只有 `rows/` 与 `tb/` 两个数。

### 5.4 冒烟：先用 `--limit 2000` 试一条腿的 1/25

```bash
bash $LIB/dump_tb.sh --corpus /Volumes/T7_Corpus/corpus --out /tmp/tb_smoke \
  --limit 2000 --jobs 12 --binary build/lib/phy/upper/channel_processors/ul_chain_replay_cpu \
  /Volumes/T7_Corpus/corpus/s2_amc_iperf_1103i
```

**实测**：2 000 单元逐单元段 **23.2 秒**；带 harq 段时整条 **27.7 秒**（1 694 条链里挑出 260 条候选、
解出 258 条、写出 258 个 harq 比特，`tb/` 1 739 个块）。其 `index.csv` 的 2 000 行与全量 dump 的同名行
**逐字段完全相同 0 处不符**（`--limit` 取的是排序后的前 N 个，正好是全量结果的前缀 ⇒ 可用于 A/B 对照）。

> ★ 冒烟阶段链是**被截断的**（只拿到前 2 000 个单元），所以候选数/解出数不能拿来判断本腿的 harq 规模；
> 冒烟只用于验证「流程能跑通、`index.csv` 是全量的前缀」。

### 5.5 dump 的两个群体：self 与 harq（`--no-harq` 可关）

空中实链必须把每个 RV0 都解到 LDPC 6 轮撞墙才能知道要不要发 HARQ；**事后重放不用**：整条链在磁盘上，
一个进程按顺序回放、软缓冲共享，合并解码一次就给出标签。所以标准 dump 是两段：

| 段 | 做法 | 产物 |
|---|---|---|
| self | 逐单元回放（RV0，`--jobs` 并行，一单元一进程）| `index.csv` 的 50 000 行；`tb/<self>.bits` |
| harq | 只挑「首传失败且有后继」的链，**一条链一个进程**（`--also … --harq-chain`），取第一个 crc=OK 的跳 | `tb/<首传>.bits`（合并解码的块）；`chain_iters.csv`；`index_self/index_harq/index_harq_only.csv` |

要点：

- **合并解码的迭代数**才是 harq 样本的难度；首传那次撞墙的 6 轮留在 `own_iterations/own_iterations_max`。
- 首传的 RV0 判决**不能省**：它决定这个样本属于 self 还是 harq（也决定链要不要往下解）。
  能省的是重传单元的独立解码：它们在第 1 段里根本进不了解码器（`cannot reserve a receive buffer`），
  在第 2 段里只作为合并的输入出现。
- 有 `<leg>/tb_live/`（空中链的 live dump）时，链回放的比特会与之**逐字节比对**；一致才落盘。
  没有 `tb_live` 也能出 harq（比特来自链回放本身，CRC 保证同块），只是少了那层验证。
- 本腿实测（2026-11-27 干净重跑）：候选链 **5 655** → 解码 **5 236（92.6%）** → `tb/` 从 36 747 增到
  **42 722**；比特与 `tb_live` 比对 `match=5 225 / no-live-row=11`。

> ★ **重跑要先删旧目录**：`dump_tb.sh` 每次会截断 `index.csv`/`units.txt`，但**不会**清 `tb/`。
> 旧目录里残留的 `.bits` 会让「`tb/` = self＋harq」这条判据假性失败（数出来比群体多）。
> 正确做法：`rm -rf <out>` 再跑（历史驱动 `dump_1103.sh` 也是把半成品 `mv` 到一边，不在原地续跑）。

### 5.6 `index_ko.csv`：CPU 链拿不到 label 的样本清单（给别的后段用）

`index_ko.csv` 是 `index_self.csv` / `index_harq_only.csv` 的**补集**：所有 `label_from=none` 的单元 —— 经典 CPU
链没有解出来、也没有任何重传把它捞回来的接收。三个群体加 harq 的中间成员恰好把 50 000 个单元分完。

| 项 | 本腿实测 |
|---|---|
| 行数 | **7 278**（self 36 747 ＋ harq 首传 5 260 ＋ harq 中间成员 715 ＋ ko 7 278 = 50 000）|
| `decode-failed`（首传，进了 LDPC、6 轮撞墙没过 CRC）| **457**（`new_data=1`）|
| `no-buffer`（重传，harness 按 `new_data` 预留软缓冲失败，**没进 LDPC**）| **6 821**（`new_data=0`）|
| 列 | `index.csv` 的 8 列 ＋ `new_data, rv, harq_id, rnti, reason` |
| 大小 | 435 560 B |

**怎么用**：`index_ko.csv` ＋ `<leg>/`（`.bin`、`.txt`、`_ce.txt`）就是一份工作清单。换一套后段解码器逐个重试 ——
replay 的接口是 `--phy_pipeline cpu|cpu_gpu|gpu|npu`、`--pusch_receiver_backend auto|classic|ai`、
`--pusch_ldpc_decoder_type auto|generic|metal…`（GPU/NPU 用 `metal/ul_chain_replay` 那个二进制），
或换成任何 GPU/NPU 的 LDPC 实现。

**换后端不改变样本的分类规则**：self / harq 由「label 从哪来」决定，和 CPU 那套完全一致 ——

- 这次接收**自己**解出来（单跑） ⇒ `label_source=own`，等价于 self；
- 只能靠**同一条 HARQ 链上别的接收**（软合并）解出来 ⇒ `label_source=chain`，等价于 harq
  （标签挂在该链首传上才是 harq 样本）。

所以同一套后端跑完，既可能多出 self 样本，也可能多出 harq 样本（也可能一个都没有，全被 CPU 解掉了）。
**落盘时必须多带三列**，否则建库和评测分不清：

| 列 | 取值 | 为什么必须 |
|---|---|---|
| `decoder` | `cpu-classic` / `metal` / `npu-ai` / … | `label_from=self` 只说明"这次接收自己解出来了"，不说明"**参考链**也解得出来"；把 harvest 的 own 样本混进经典 self 群体，会让"参考链能解"的基线虚高（§17/§18 的泄漏规则）|
| `label_source` | `own` / `chain` | 决定它属于 self 还是 harq 群体 |
| `first_transmission` | `1` / `0`（`new_data=1 && rv=0`）| 重传被单跑解出时，它既不是经典 self（不是首传）也不是 harq（没有借标签），是当前流水线从未产生过的新群体；混进 `index_self.csv` 会破坏该文件"首传"的隐含语义 |

两条注意：

- `reason=decode-failed` 的 457 条**最值得先试**：它们确实跑过 LDPC（6 轮撞墙），更多轮次或更强的解码器有机会翻过来。
- `reason=no-buffer` 的 6 821 条是**重传**：当前 harness 按 `new_data` 预留软缓冲会直接拒绝，单跑解不了。两条出路：
  ① 走链回放（本流程已做，它们在那里是链条中间成员，按既有规则不进 harq 群体）；
  ② 给 harness 加"强制新缓冲"模式，让它们能被单跑解码 —— 那样解出的是 **`own` 标签、非首传**的新群体
  （上表第三列 `first_transmission=0`），既不是 self 也不是 harq。

### 5.7 解码强度：`OCUDU_REPLAY_DEC_ITERATIONS`（dump 默认 **20**，参考配置 6）

harness 的 LDPC 迭代上限来自环境变量 `OCUDU_REPLAY_DEC_ITERATIONS`（`ul_chain_replay.cpp:148-155`）。**从 2026-11-27 起
`dump_tb.sh` 默认 20**（`export …:-20`，两段都继承），值会打印在 banner（`… N decoder iteration(s)`）并写进
`dump_meta.txt` 的 `ldpc_iterations`。要 **参考配置**（＝空中腿的 `pusch_dec_max_iterations`）就显式 `=6`：

```bash
OCUDU_REPLAY_DEC_ITERATIONS=6 bash $LIB/dump_tb.sh …     # 参考判决（与空中链可比）
bash $LIB/dump_tb.sh …                                   # 默认 20：标签采集
```

**它决定判决，不只是时延**：某个接收在第 7 轮能过、在第 6 轮就是 `crc=KO`。所以**换 cap 就是换一份数据集**，不是换速度。

两条腿的实测（同语料、同二进制、`--jobs 12`，cap 6 → cap 20）：

| 项 | 1103i cap 6 | 1103i cap 20 | 1103j cap 6 | 1103j cap 20 |
|---|---|---|---|---|
| `crc_ok`（＝self）| 36 747 | **37 599**（+852）| 38 740 | **39 611**（+871）|
| ├ 原本 harq（借标签）→ self | — | 824 | — | 841 |
| └ 原本无标签 → self | — | 28 | — | 30 |
| ★ 原本无标签 → harq（链在 20 轮下解出）| — | 4 | — | **72** |
| 反例：cap6 OK → cap20 KO | — | **0** | — | **0** |
| harq 首传 | 5 260 | 4 436 | 4 909 | 4 089 |
| ko（无标签）| 7 278 | 7 251 | 5 207 | 5 107 |
| `tb/` 块数 | 42 722 | 42 749 | 44 805 | 44 893 |
| 运行时间 | 11 min 8 s | **12 min 39 s** | — | **11 min 56 s** |
| 仍在 KO 的首传（全部撞 20）| 457→**429** | — | — | **38** |

新解出样本的 `iterations_max` 分布（7 轮起点，逐轮递减）：

- 1103i：7:205 8:148 9:111 10:95 11:74 12:42 13:38 14:30 15:30 16:22 17:18 18:17 19:14 **20:8**
- 1103j：7:222 8:149 9:101 10:79 11:58 12:62 13:41 14:36 15:30 16:22 17:23 18:15 19:21 **20:12**

三条要点：

1. **"借标签 → 自有标签"的样本，比特逐字节相同**（1103i 824/824、1103j 841/841 的 md5 相等）：同一块，只是不再需要 donor。
2. **harq 群体变"纯"**：能自己解出的那批从 harq 挪去 self，留下的（1103i 4 436 / 1103j 4 089 个首传）才是真正必须靠合并的；
   反过来，1103j 还有 72 个单元因为链在 20 轮下解出而**第一次**拿到 harq 标签。
3. ★ **cap≠6 的 dump 不是"参考链判决"**：多出来的样本空中链（cap 6）解不出。用这种 dump 建的数据集
   **不能**拿去和经典链做门控/尺子/验收对比；`dump_meta.txt` 的 `ldpc_iterations` 就是防误用的标记。做测量时请用 cap 6 的 dump。

**还会不会更多？** cap 20 下仍失败的（1103i 429、1103j 38 个首传）**全部撞在 20**；实测把它们单独提到 **cap 40**
仍然 **0 个解出**，链回放也一个不多（见 memo §12）——它们的 SINR 在 −19 dB 量级，是信号不够，不是迭代不够。
所以继续加轮次没有收益，下一步该换后段（§5.6 的 `index_ko` 工作清单）或再采集。

---

---

## 6. 步骤 4：三条判据（＋两条加固检查）

### 6.1 判据

```bash
python3 - /Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i <<'PY'
import sys, os
out = sys.argv[1]
us = [l.strip() for l in open(os.path.join(out, "units.txt")) if l.strip()]
with_bin = sum(1 for u in us if os.path.exists(u[:-4] + ".bin"))
rows = sum(1 for _ in open(os.path.join(out, "index.csv"))) - 1
ok = sum(1 for l in open(os.path.join(out, "index.csv")) if ",OK," in l)
print(f"units={len(us)} with_bin={with_bin} index_rows={rows} crc_ok={ok}")
PY
```

| # | 判据 | 不等的含义 |
|---|---|---|
| 1 | `index.csv` 行数 ＝ **有 `.bin` 的单元数** | 有单元没跑完／清单与语料不符 |
| 2 | `index.csv` 行数 ＝ `units.txt` 行数 | 拼装丢了行 |
| 3 | 通过 1、2 且 `crc_ok > 0` | 整腿全 KO ⇒ 配置/二进制/解包有问题 |

**本腿实测（照此对表）**：

```
units=50000 with_bin=50000 index_rows=50000 crc_ok=36747
```

### 6.2 加固检查（本次加了经验值，建议保留）

```bash
OUT=/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i
# a) tb/ 文件数 = index_harq.csv 里 label_from ∈ {self, harq} 的行数（每个可训练样本一个比特文件）。
#    ★ 别再和 crc_ok 比：tb/ 里还有合并解码解出的 harq 样本（本腿 crc_ok=36 747，tb=42 722）。
python3 - "$OUT" <<'PY'
import csv, os, sys
o = sys.argv[1]
lab = [r["label_from"] for r in csv.DictReader(open(os.path.join(o, "index_harq.csv")))]
print("self+harq units:", sum(1 for x in lab if x in ("self", "harq")),
      "tb files:", len(os.listdir(os.path.join(o, "tb"))))
PY
# 本腿：self+harq units: 42722 tb files: 42722
# b) 每个 OK 行都有 iterations_max，且 tb_bits == tbs_bytes*8
python3 - <<'PY'
import csv
p="/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i/index.csv"
bad=empty=n=0
for r in csv.DictReader(open(p)):
    if r["crc"]!="OK": continue
    n+=1
    if int(r["tb_bits"])!=int(r["tbs_bytes"])*8: bad+=1
    if r["iterations_max"]=="": empty+=1
print(f"crc_ok={n} tb_bits_mismatch={bad} empty_iterations_max={empty}")
PY
# 本腿：crc_ok=36747 tb_bits_mismatch=0 empty_iterations_max=0
```

```bash
# c) 三个群体互斥且把 50 000 个单元分完；index_ko 的行数等于 label_from=none 的行数
python3 - "$OUT" <<'PY'
import csv, os, sys
o = sys.argv[1]
lab = {r["unit"]: r["label_from"] for r in csv.DictReader(open(os.path.join(o, "index_harq.csv")))}
counts = {k: sum(1 for v in lab.values() if v == k) for k in ("self", "harq", "none")}
ko = sum(1 for _ in open(os.path.join(o, "index_ko.csv"))) - 1
print("self+harq+ko:", sum(counts.values()), "of", len(lab), "| ko rows:", ko, "|", counts)
PY
# 本腿：self+harq+ko: 50000 of 50000 | ko rows: 7278 | {'self': 36747, 'harq': 5975, 'none': 7278}
```

> `iterations_max` 为空 ⇒ 二进制过期（§2）。`tb/` 文件数 **大于** `crc_ok` 是**标准 dump 的正常结果**
> （多出来的是合并解码解出的 harq 样本，判据见上面的 (a)）；只有比 `self+harq` **还多**才是旧残留。

### 6.3 本腿的参考解出率（只用于「像不像」，不是判据）

| 调制 | 捕获 | CRC-OK | 解出率 |
|---|---|---|---|
| QPSK | 23 685 | 21 255 | 89.7% |
| 16QAM | 7 397 | 6 795 | 91.9% |
| 64QAM | 10 642 | 7 062 | 66.4% |
| 256QAM | 8 276 | 1 635 | 19.8% |
| **合计** | **50 000** | **36 747** | **73.5%** |

其他量纲（本腿）：单 TB 4–4 867 B，均值 **709 B**（self 部分）；`tb/` 合计 **172 MiB**（self 144 ＋ harq 28）；
`index.csv` **2 329 532 B / 50 001 行**；`units.txt` **3 520 749 B / 50 000 行**。
若某腿解出率明显低于同调制族的常见值，先查**二进制是否过期**、**解包是否完整**，再怀疑数据。

### 6.4 与历史 dump 对照（可选，但强烈建议做过一次的腿都留个底）

```bash
# 历史目录（本机）：~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103i
python3 - <<'PY'
import csv, os
old=os.path.expanduser("~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103i/index.csv")
new="/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i/index.csv"
o={r["unit"]:r for r in csv.DictReader(open(old))}
n={r["unit"]:r for r in csv.DictReader(open(new))}
cols=["tb_bits","crc","sinr_db","iterations","slot","tbs_bytes"]
print({c: sum(1 for u in n if n[u][c]!=o[u][c]) for c in cols})
PY
# 本腿实测：{'tb_bits': 0, 'crc': 0, 'sinr_db': 0, 'iterations': 0, 'slot': 0, 'tbs_bytes': 0}
```

本次还做了两组比特级复核：① 与历史 dump 的 36 747 个 self 文件**文件名集合完全相同**，抽 300 个比 md5 **0 处不同**；
② 链回放解出的 5 236 个 harq 块与 `<leg>/tb_live/` 的对应比特**逐字节相同**（5 225 match，检查见 `chain_iters.csv` 的 `bits_check`）。

---

## 7. 步骤 5：收尾、归档、交给下一步

### 7.1 收尾（工具已自动做完，这里只核对）

```bash
OUT=/Volumes/T7_Corpus/corpus/tb_s2_amc_iperf_1103i
ls $OUT                                       # 应只有九个契约产物；看到 stage/rows/raw 说明用了 --keep-scratch
rm -rf $OUT/{stage,rows,raw}                  # 仅当用了 --keep-scratch 时才需要手动清
find $OUT -name .DS_Store -delete             # Finder 浏览产生的，不是语料
find $OUT -name '._*' -delete                 # 万一有
du -sh $OUT                                   # 本腿实测：≈185 MiB
```

保留九样：`index.csv`、`units.txt`、`tb/`、`index_self.csv`、`index_harq.csv`、`index_harq_only.csv`、`index_ko.csv`、
`chain_iters.csv`、`dump_meta.txt`。前三个是 dump 本身，中间五个是群体划分、无 label 清单与链回放审计，
最后一个是出处（解码强度等）。
本腿实测体积：

| 产物 | 体积 |
|---|---|
| `tb/`（42 722 个块）| 172 MiB |
| `index.csv` / `index_self.csv` / `index_harq.csv` / `index_harq_only.csv` | 2.2 / 1.95 / 2.8 / 0.41 MiB |
| `units.txt` / `chain_iters.csv` / `index_ko.csv` / `dump_meta.txt` | 3.4 / 0.43 / 0.42 / < 1 KiB |

### 7.2 归档 dump（可选，但 211 上已有约定）

```bash
cd /Volumes/T7_Corpus/corpus
tar -czf /tmp/tb_<leg>.tar.gz tb_<leg>        # 本腿实测(cap20) 19.6 MiB / 20 590 860 B（42 759 个成员，16.6 s）
scp /tmp/tb_<leg>.tar.gz jwang@192.168.31.211:/mnt/corpus/s2_amc_tb_tarballs/
rm -f /tmp/tb_<leg>.tar.gz
```

> ★ **归档前必须删 `stage/`**（手册 §22：不删，tar.gz 会大两个数量级）。
> ★ 标准 dump 的归档现在就是九样：`index.csv`、`units.txt`、`tb/`、`index_self.csv`、`index_harq.csv`、
> `index_harq_only.csv`、`index_ko.csv`、`chain_iters.csv`、`dump_meta.txt`（1103i cap-20 实测 184 MiB → gzip 后
> **19.6 MiB / 20 590 860 B**）。解包后 `tar -t` 成员数 = 42 749（`tb/`）＋ 8 个文件 ＋ 2 个目录 = **42 759**。

### 7.3 交给建库（**按 `mother_set_build_runbook.md` 执行**）

> ★★ **2026-11-27 起，建库的完整步骤以 `mother_set_build_runbook.md` 为准**（含六项验收、全库批处理、
> `REQUIRE_ITERATIONS=1` 的语义）。下面这段是**旧写法**，保留只因为它说明了输入是怎么被找到的：
> `OUT` 已经改到 `/Volumes/Eaget_HD/datasets_full_s2_amc`，并且**必须**加 `REQUIRE_ITERATIONS=1`。

```bash
export TRAIN=/Users/jiachengwang/dev/ocudu
export DUMP_ROOT=/Volumes/T7_Corpus/corpus     # ⇒ dump 在 $DUMP_ROOT/tb_<leg>
export CORPUS_ROOT=/Volumes/T7_Corpus/corpus   # ⇒ 捕获在 $CORPUS_ROOT/<leg>
export OUT=/Volumes/Eaget_HD/datasets_full_s2_amc   # 母集输出（旧写法是 ~/ai_depth3/datasets_full_s2）
export PY=~/ai_ce_work/venv/bin/python
export JOBS=1 CHUNK=8000 REQUIRE_ITERATIONS=1
bash lib/phy/upper/ai_signal_processors/depth3/ai_train/build_mother_set.sh <leg>
```

> ★ **建库会在 dump 目录上重跑一次打标**：`build_mother_set.sh` 第 1 步调用
> `harq_backfill.py --index $DUMP/index.csv --out $DUMP`。dump 已经生成过这三个索引和 harq 比特，重跑得到
> 一致的产物；关键是它会**自动读** `<out>/chain_iters.csv`，因此不会把合并迭代数抹掉（删了才会）。三点要记住：
> - 真正等价于「dump 判决」的索引是 `index.csv`；`index_harq.csv` 把捞回的行改写成 `crc=OK`（借 donor 标签），
>   **不能**当链路判决用；
> - harq 行的 `iterations`/`iterations_max` 是**标签那次解码**（带软合并）的迭代数；自己首传的失败值在
>   `own_iterations`/`own_iterations_max`（2026-11-27 起由 `dd_meta.csv` 的 `iter_mean,iter_max` 记录）；
> - **本 runbook 之前 dump 的腿**（例如 `~/ai_depth3/dump_s2_amc/*`）没有 harq 比特和 `chain_iters.csv`，
>   用第二段补齐即可（约 40 s/腿）：
>   ```bash
>   dump_harq_chains.py --leg <leg> --index <dump>/index.csv --out <dump>/chain_iters.csv \
>       --tb-out <dump>/tb --live-tb <leg>/tb_live --jobs 12
>   harq_backfill.py --leg <leg> --index <dump>/index.csv --out <dump> --live-tb <leg>/tb_live
>   ```
>   补齐前，这些腿的 harq 行 `iterations` 为空（不是 6）。

---

## 8. 时间与容量总账（本腿 / 本机实测）

| 阶段 | 命令 | 耗时 | 增量占用 |
|---|---|---|---|
| 二进制检查＋重编 | `make -C build -j14 ul_chain_replay_cpu` | 12.7 s（仅 exe 源过期时）| — |
| 解包 | `ssh … cat` 管道喂给 `tar -xzf - --exclude '._*'` | **2 min 13 s** | 9.6 GB |
| 冒烟（可选）| `--limit 2000`（含 harq 段）| 27.7 s | 0.3 GB |
| **dump 全量**（self＋harq，cap 6）| `--jobs 12` | **11 min 8 s** | 0.19 GB 契约（草稿随跑随删）|
| dump 全量（cap 20，默认）| `--jobs 12` | **12 min 39 s**（1103i）/ **11 min 56 s**（1103j）| 0.19 GB（块数按样本数略增）|
| 收尾 | 工具自动 | 秒级 | — |
| **合计（一条腿）** | — | **≈ 14 分钟**（含解包）| 峰值 ≈ 10 GB |

---

## 9. 坑与排查（全部真实发生过）

| # | 现象 | 原因 | 处置 |
|---|---|---|---|
| 1 | `index.csv` 只有 7 列 / `iterations_max` 整列为空 | 二进制过期，没重编 | §2 重编，跑 §2.3 探针确认 |
| 2 | `ls /path/*.txt` 返回 0 或报 `Argument list too long` | 一条腿 20 万文件，glob 撑爆 ARG_MAX | 一律用 `find … -maxdepth 1`（`dump_tb.sh` 内部就是 `find`）|
| 3 | 清点数字是期望的 2 倍 | 解包时带出了 `._*`（AppleDouble，本腿 293 508 个）| 解包加 `--exclude '._*'`；`dump_tb.sh` 的单元清单已排除 `._*`，但目录体积会被撑大 |
| 4 | `tb/` 文件数 ≠ `crc_ok` | **正常**：`tb/` 里还有合并解码解出的 harq 样本 | 判据改成 `tb/` = `index_harq.csv` 里 `label_from ∈ {self,harq}` 的行数（§6.2）；只有 `tb/` 比群体**多**才是旧残留 |
| 4b | `tb/` 比群体多出若干 | 旧目录重跑残留（`dump_tb.sh` 不清 `tb/`）| `rm -rf <out>` 重跑 |
| 5 | `crc_ok` 远低于参考 | 解包不全 / 二进制不对 / 跑错后端 | 先按 §4 对四条硬指标，再跑 `--limit 2000` 与全量前缀对照 |
| 6 | dump 很慢（< 20 单元/秒）| `--jobs` 太小，或 T7 同时在跑别的重活 | 提到 12；确认没有第二个 dump 在抢盘 |
| 7 | 负载飙到 40 还想加 jobs | 12 已是 14 核的实测上限 | 保持 12；瓶颈常在 I/O 不在 CPU |
| 8 | 归档 tar.gz 从 19.5 MiB 变几 GB | 忘了删 `stage/`（5.8 GB）| 归档前 §7.1 |
| 9 | 目录里多了 3 个 `index_*.csv`、`chain_iters.csv` | **正常**：标准 dump 的产物 | 见 §5.2；`index.csv` 才是链路判决，其余是群体划分与审计 |
| 10 | 目录里出现 `.DS_Store` | Finder 浏览 | 归档前删（§7.1）|
| 11 | 解包被网络中断 | ssh 断了 | 文件数不够就**换新目录重解**，不要在半个目录上续解 |
| 12 | 把旧批次路径当成本批 | 手册 §16.4 写的是 `s2_corpus_tarballs/<leg>.tar` | 本批是 `s2_amc_corpus_tarballs/<leg>.tar.gz` |
| 13 | `chain_iters.csv` 里 `bits_check=no-live` 或 `no-live-row` | `no-live` ＝ 没给 `--live-tb`（未请求比对）；`no-live-row` ＝ live dump 里没有这次接收 | **都不是错误**：前者补 `--live-tb` 重跑即可比对，后者这几次接收本来就没进 live dump；解码本身仍由 CRC 保证 |
| 14 | 拿 replay 的 `dd_meta.csv` 的 frame 去对飞行 dump 对不上 | `extended_frame()` 的进位基数是**进程级**：新起的 replay 报的是原始 SFN（如 807），飞行 dump 报的是含所有回绕的计数（如 10 023） | 按 slot（同一条链内唯一）或按 capture 名匹配；工具已这么做，别自己再按 frame 对 |
| 15 | 链回放中途 Ctrl-C，`chain_iters.csv` 不完整、harq 群体变小 | 第二段没跑完就退出 | 删 `<out>/chain_iters.csv` 与 `index_*` 后重跑第二段（不必重跑逐单元段）|
| 16 | 一次给多个腿目录时 harq 段没跑 | `--with-harq` 只在**一个**腿目录时运行（链和打标都必须属于同一条腿）| 一条腿一个 `--out`，分开跑 |
| 17 | 想查某单元的 staged 产物/失败现场，但 `stage/` 已经不在了 | 工具默认随跑随删草稿 | 加 `--keep-scratch` 重跑该范围（可配 `--limit`）；`rows/`、`raw/` 同理 |

---

## 10. 本次实测指纹（2026-11-27，腿 `s2_amc_iperf_1103i`，**cap 6** 那一版）

> ★ 这张表是 **cap 6**（参考配置）的指纹；那份 dump 已于 2026-11-27 10:13 删除，其内容与历史归档
> `~/ai_depth3/dump_s2_amc/tb_s2_amc_iperf_1103i` 的 self 半程逐字节一致。**现在磁盘上的是 cap 20 版**（见 §5.7）。

| 项 | 值 |
|---|---|
| 归档 | `/mnt/corpus/s2_amc_corpus_tarballs/s2_amc_iperf_1103i.tar.gz`，4 053 908 427 B |
| 归档 sha256 | `9254c74118ab52fdb193ca6c98d213697365ed57b1b9986bfb3e99c619ca1bef`（2026-11-27 在 211 上实测，4 GB 整读约 2 分钟）|
| tar 成员 | 587 021 ＝ 3 目录 ＋ 293 510 文件 ＋ 293 508 `._`；真实内容 9 576 543 376 B |
| 解包产物 | 293 510 文件 / 9 576 543 376 B / 0 个 `._` / 9.6 GB |
| 单元数 | 50 000 |
| `index.csv` | 2 329 532 B，50 001 行，sha256 `5c060c019d859b4ffd6f6e943a6cb4c2547a44ad41a421170794734422ba5bbd` |
| `units.txt` | 3 520 749 B，sha256 `d65cd410b705e7033f6deb0a77cc48127e0460b620548fad05ebc84bf46e8a82` |
| `tb/` | **42 722 个** `.bits`，**172 MiB**（self 36 747 ＋ harq 全标 5 975）|
| `index_self.csv` / `index_harq_only.csv` / `index_harq.csv` | 36 747 / **5 260** / 50 000 行（1.95 / 0.41 / 2.8 MiB）|
| `chain_iters.csv` | 5 655 行，449 044 B，sha256 `cab2ab0854591fd62d1a6e65facb2863f2b4e9e38e2ecebe2bf7c54f75f26566` |
| `index_ko.csv` | **7 278 行**（457 `decode-failed` ＋ 6 821 `no-buffer`），435 560 B |
| 链回放 | 5 655 候选 → **5 236 解码（92.6%）**；`bits_check`：match 5 225 / no-live-row 11 |
| 合并 `iterations_max` 直方（harq）| 1:108 2:1 687 3:1 806 4:1 023 5:415 **6:197** |
| 两个群体的均值 | self 1.75 轮 / harq（合并后）3.10 轮 |
| 判据行 | `units=50000 with_bin=50000 index_rows=50000 crc_ok=36747`；`self+harq=42722 tb=42722` |
| 耗时 | 解包 2 min 13 s ＋ **dump 全流程 11 min 8 s** |
| 归档 | 185 MiB → `tar.gz` **20 400 698 B / 42 732 个成员**（11.8 s）|

> 注：换腿时归档 sha256 必须现取现记，不要套用上表：
> `ssh jwang@192.168.31.211 'sha256sum /mnt/corpus/s2_amc_corpus_tarballs/<leg>.tar.gz'`。
