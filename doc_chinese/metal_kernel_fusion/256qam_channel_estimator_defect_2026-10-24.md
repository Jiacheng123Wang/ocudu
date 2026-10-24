# Metal 通道估计器在 256QAM 上的缺陷（2026-10-24，来自 LLR AI 检测工作流的对照测量）

> 这份报告的**来源**：`llr_ai_detection` 工作流在排查"上行 256QAM 高码率块为何总是失败"时，需要一条可信的
> 经典参考链，于是用 `ul_chain_replay` 把同一批真实捕获分别喂给 **CPU 链**、**Metal 链**和 **AI 臂**。
> CPU 链能解、Metal 链解不出的比例远超预期 ⇒ 报告给 `metal_kernel_fusion`。
> 全部原始数据与脚本在 `doc_chinese/llr_ai_detection/work_tmp/` 下，可复现。

## 1. 结论（一句话）

**Metal 接收链在 256QAM 上只解出 CPU 参考链的六分之一：13% 对 83%。缺陷定位在
**Metal 通道估计器（CE）**，不在解调器、也不在 LDPC 解码器。**

## 2. 测量方法

| 项 | 内容 |
|---|---|
| 工具 | `build/.../ul_chain_replay`（同一工具，四种链路开关） |
| 捕获 | 真实上行捕获，`OCUDU_UL_DUMP` 落盘的网格（`C_*.bin` ＋ `_ce.txt`） |
| 样本 | 256QAM 捕获中 `_ce.txt` 记录 SNR 在 **24–32 dB** 的 **1 438** 条，**等距抽 60 条** |
| 判据 | 每条回放一次的 **CRC 结果**（OK / KO） |
| 开关 | `--cpu`（全 CPU）、`--metal`（全 Metal）、`--metal-cpu-demod`（Metal CE ＋ CPU 解调）、`--metal-cpu-ldpc`（Metal CE＋解调 ＋ CPU LDPC） |

## 3. 结果

| 配置 | OK / 60 | 比例 |
|---|---|---|
| **`--cpu`（全 CPU 参考）** | **50** | **83%** |
| `--metal`（全 Metal） | **8** | **13%** |
| `--metal-cpu-demod`（Metal CE，其余 CPU） | **8** | **13%** |
| `--metal-cpu-ldpc`（Metal CE＋解调，LDPC 用 CPU） | **8** | **13%** |

**⇒ 三次把"Metal 的其余部分"换成 CPU 都没有改善；只要 **CE 是 Metal**，结果就是 8/60。
⇒ **缺陷在 Metal CE**。**

## 4. 影响

1. **任何走 Metal 链的上行 256QAM 都会大量失败**（现场的空口腿 `mode=gpu` 正是这条链）；
2. **外环链路自适应（OLLA）会被这些失败一路压到下限**：本工作流实测 OLLA 长期停在 **−5 dB**（其下限），
   于是整条 MCS 曲线左移 5 dB，低阶调制被连带压得过度保守；
3. **CPU 参考链不受影响**；
4. **AI 臂不受影响**：它在纯运行下**自己**做估计／均衡／解调（其 256QAM 成绩为 48/60，与 CPU 参考的 50/60 基本持平）；
5. **训练数据的采集**：网格是原始输入，**不受影响**；且训练只用 `crc=OK` 的样本、标签取自**发送比特**，
   所以既不会污染数据、也不需要重新采集。但 `_ce.txt` 里的 **SNR 是 Metal CE 算的**，在 256QAM 上可能有偏
   ⇒ **凡是拿回放语料的 `snr_nominal` 做绝对分析的（包括本工作流的分箱），都要注意这一点**，不要跨来源比较绝对值。

## 5. 复现

```bash
cd ~/dev/ocudu
# 单条捕获，三种链路
build/lib/phy/upper/channel_processors/metal/ul_chain_replay <stem> --out /tmp/x --cpu
build/lib/phy/upper/channel_processors/metal/ul_chain_replay <stem> --out /tmp/y --metal
build/lib/phy/upper/channel_processors/metal/ul_chain_replay <stem> --out /tmp/z --metal-cpu-demod
# 批量（60 条 × 4 配置）
python3 doc_chinese/llr_ai_detection/work_tmp/probe/…    # 见 qam256_3way.log / qam256_bisect.log
```

## 6. 建议的下一步（给 `metal_kernel_fusion`）

1. **在 Metal CE 内部二分**：把 CE 的子步骤（最小二乘估计、时域平均、CFO 补偿、相位跟踪、插值）逐一切回参考实现，
   直到 8/60 变回 50/60 ⇒ 定位到具体子步骤；
2. **按调制分层**：同一批捕获用 QPSK/16QAM/64QAM 各跑一遍 ⇒ 判断是"256QAM 专属"还是"高阶调制随阶数恶化"；
3. **按 SNR 分层**：24–32 dB 内分成四档 ⇒ 判断是"高 SNR 才错"（更像数值范围/饱和）还是"全程都错"（更像算法）。

## 7. 保留

- 样本 **n=60**、**单一语料**（来自含失败的采集腿）、**只覆盖 256QAM**；方向可靠，**比例需扩大**；
- 四条路径共用同一个 **LDPC 解码器实现**（CPU）时仍为 8/60 ⇒ 排除解码器；
- 判据是 **CRC**，不是 BLER 的精确值。
