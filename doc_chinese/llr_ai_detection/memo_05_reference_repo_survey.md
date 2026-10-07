# 参考 repo 代码调研（memo 05）

> 调研对象（用户指定）：
> ① `gitlab.com/ocudu/work_groups/wg2_ai_ran`（OCUDU dApp 平台）
> ② `https://github.com/NVIDIA/aerial-cuda-accelerated-ran.git`（NVIDIA CUDA-Accelerated RAN）
>
> 目的：**分析对我们工作的启示——哪些可以重用、哪些需要修改**；
> 并在 high level 上保证**可扩展性与可移植性**（参照 `metal_vs_cuda_architecture.md`
> 的双重异构 / 读者属地 / lane 而非 backend / ANE 是车道内的环节 等原则）。
>
> 版本：v0.9（初稿，三路并行深挖进行中）｜ 日期：2026-10-08

---

## 0. Clone 状态（★ 有一个需要你处理）

| repo | 结果 | 说明 |
|---|---|---|
| `aerial-cuda-accelerated-ran` | ✅ **已 clone** 到 `~/dev/aerial-cuda-accelerated-ran`（110 MB / 4015 文件） | `github.com` 的 **git 协议被墙**（curl 443 超时），改走 **`codeload.github.com` 的 tarball** 成功 |
| `wg2_ai_ran` | ❌ **无法访问** | HTTPS 与 SSH 都返回 *"could not be found or you don't have permission to view it"*；公共 API 返回 **404**。SSH 身份是 **`@JiachengWang`**（已认证成功），但该账号**不在 WG2 组里**。公开的 `ocudu` 组下只有 8 个仓库（`ocudu`、`ocudu_tools`、`ocudu_docs` …），**没有 wg2_ai_ran** |

⇒ **需要你做的**：把 `@JiachengWang` 加入 `ocudu/work_groups/wg2_ai_ran`（或提供访问令牌）。
论文说平台/SDK/quickstart 是 BSD-3-Clause-Clear 的 **preview release**，但那只是**许可**，
**可见性仍是组内私有**。在拿到之前，本备忘对 ① 的分析只能基于两篇论文的正文（已逐字直读）。

---

## 1. ★★★ 最重要的发现：aerial 里带着**两个训练好的模型**和**完整的 ML 设计流程**

这不只是一份 CUDA 代码库——它是**我们这件事的工业参考实现**，而且模型权重就在仓库里。

### 1.1 `pyaerial/models/` 里有两个 ONNX

| 文件 | 大小 | 参数量（本次解析） | 是什么 |
|---|---|---|---|
| `llrnet.onnx` | **1759 B** | **184** | LLRNet：per-RE MLP，`Linear(2→16) + ReLU + Linear(16→8)` |
| `neural_rx.onnx` | 664 KB | **145 232** | **神经接收机**：CGNN（卷积图网络），**7 输入 / 2 输出** |

★ `neural_rx.onnx` 的 **145 232 ≈ 1.45e5**，与调研里"NVIDIA 合规实时接收机的实时模型只有 1.4e5 权重"
**完全吻合**（`memo_03` §2.6 引用的尺寸上界）。⇒ 我们现在的尺寸预算有了**第一手的权重文件**做锚点。

### 1.2 ★★★ `neural_rx` 的精确张量规格（这就是"深度 3"的工业形态）

来自 `pyaerial/notebooks/example_neural_receiver.ipynb`（可运行例子，逐字）：

> "the neural network is used to **replace channel estimation, noise and interference estimation and
> channel equalization, and thus outputs log-likelihood ratios directly**. … **Also, the neural receiver
> takes LS channel estimates as inputs in addition to the received PUSCH slot.**"

**输入（7 个）**：

| 张量 | 形状（不含 batch） | dtype | 含义 |
|---|---|---|---|
| `rx_slot_real` | `(3276, 12, 4)` | fp32 | 接收网格实部 `[子载波, 符号, 端口]` |
| `rx_slot_imag` | `(3276, 12, 4)` | fp32 | 接收网格虚部 |
| `h_hat_real` | `(4914, 1, 4)` | fp32 | **LS 信道估计**实部 `[DM-RS RE, 层, 端口]` |
| `h_hat_imag` | `(4914, 1, 4)` | fp32 | LS 估计虚部 |
| `active_dmrs_ports` | `(1,)` | fp32 | 激活的 DM-RS 端口数 |
| **`dmrs_ofdm_pos`** | `(3,)` | **int32** | ★ **DM-RS 的 OFDM 符号位置** |
| **`dmrs_subcarrier_pos`** | `(6,)` | **int32** | ★ **DM-RS 在 PRB 内的子载波位置**（例：`[[0,2,4,6,8,10]]`） |

**输出（2 个）**：`output_1` = **LLR**（notebook 声明 `(8,1,3276,12)` = `[Qm, 层, 子载波, 符号]`，
8 = 256QAM 的比特数）；`output_2` = 另一路读出（图里有 `readout_ch_est` 分支）。

★ **尺寸自洽验证**：3276 = 273 PRB × 12 ✓（4096 FFT − 410 − 410 保护 = 3276）；
**4914 = 3 × 1638 = 3 个 DM-RS 符号 × (3276/2)** ✓ ——即 **type-1 DM-RS 的 comb-2 图案**，
`dmrs_subcarrier_pos = {0,2,4,6,8,10}` 正是每 PRB 6 个导频子载波 ✓。

### 1.3 ★★★ 这三条直接印证/修正了我们的规划

| # | 发现 | 对我们的意义 |
|---|---|---|
| 1 | ★ **输入就是"网格 + 几何"**：网格 I/Q + **DM-RS 符号位置** + **DM-RS 子载波位置**（整数索引张量） | **逐项对应用户描述的形态**，也与 OCUDU dApp Class A 契约的 *"explicit DM-RS pilot tensor with bounded coordinates, compact ordered data-RE indices"* 一致 ⇒ **我们不是自创** |
| 2 | ★★ **它不是从零学估计**：`h_hat` 是**经典 LS 估计**，作为**输入**喂进去 | ★ **修正我规划里的一个隐含假设**。深度 3 不等于"网络必须自己学会估计"——工业做法是**把导频上的 LS 估计喂给它**，让它学"插值 + 噪声估计 + 均衡 + 解映射"的联合。这大幅降低了学习难度，也解释了为什么 1.45e5 参数就够 |
| 3 | ★ **一个网络、多个读出**：图里有 `readout_ll_rs`（LLR）与 `readout_ch_est`（信道估计）两条分支 | **印证规划 §2.3 的"一个 trunk、多个头"**——不是我们发明的，是工业界在用的形状 |
| 4 | **经典 L1 不被取代**：神经接收机跑在 **TensorRT** 里，**在 cuPHY 之外**（pyAerial + TRT），LLR 再交回 cuPHY 的 derate-match/LDPC | 与 dApp 论文"conventional path is never displaced"同构 ⇒ **我们的"经典路径始终在链上"是对的做法** |

### 1.4 ★ LLRNet：一个 184 参数的 per-RE MLP（及其与我们的差异）

`llrnet_model_training.ipynb` 逐字给出模型类：

```python
class LLRNet(nn.Module):
    def __init__(self, mod_order=2):
        self.fc1 = nn.Linear(2, 16)     # 输入 = 均衡后符号的 (I, Q)
        self.fc2 = nn.Linear(16, 8)     # 输出 = 最多 8 个 LLR（256QAM）
    def forward(self, x):
        return self.fc2(torch.relu(self.fc1(x)))

def loss_fn(predictions, llr, mod_order):
    """MSE loss between predicted and target LLRs."""
    mse = torch.mean((predictions[:, :mod_order] - llr) ** 2)
```

★ 三个可复用的工程技巧：

1. **一个模型服务所有调制阶数**：输出固定 8 路，用 `predictions[:, :mod_order]` 切片
   （QPSK→2、16QAM→4、64QAM→6、256QAM→8）。
2. ★ **它是蒸馏**：训练目标 `llr` 是**经典 demapper 的输出**——由
   `llrnet_dataset_generation.ipynb` 跑经典链（CE + 噪声/干扰估计 + 均衡 + 解映射）生成。
   ⇒ ★ **这正是 `memo_04` §3 的"路线 2"**。而我们的"路线 1"（CRC-OK TB → **真值比特** → BCE）
   **比 NVIDIA 的参考流程更强**，因为它不受教师上限约束。**这一点是我们的方法优势，应写进规划。**
3. **验收指标是三条 BLER 曲线**：`aerial`（经典）/ `llrnet` / **`logmap`（Log-MAP）**，
   全部**过真实 LDPC 译码器**。
   ★ **Log-MAP 曲线就是我们的 P0-c"genie 上界"**——工业界的做法与我规划里的 P0 完全一致。

### 1.5 ★ 完整的 ML 设计流程（四本 notebook，可直接借方法学）

```
example_simulated_dataset.ipynb      ← ① 造源数据（仿真 or 真实 OTA 采集）
        │                              逐 slot 扫 信道模型 × SNR × MCS，落 parquet（类 SCF FAPI 格式）
        ▼
llrnet_dataset_generation.ipynb      ← ② 用【经典链】生成 (输入, 目标) 对：
        │                             输入 = 均衡后符号；目标 = 经典 demapper 的 LLR
        ▼
llrnet_model_training.ipynb          ← ③ PyTorch 训练（MSE）→ ONNX → trtexec → TRT engine
        │                             测试同时用 Aerial test vector 与合成数据
        ▼
example_neural_receiver.ipynb        ← ④ 深度 3：神经网络替换 CE+噪声+均衡，直接出 LLR，
                                       在 pyAerial 里接上 derate-match / LDPC / CRC，画 BLER
```

★ **四个可移植的方法学要点**：
- 数据集**逐参数化扫**（信道模型 × SNR × MCS），并明确记录每个样本的 `snr`；
- 数据记录用 **parquet 元数据 + 每条记录一个 pickle**（`rx_iq_data_filename` / `user_data_filename`）；
- **导出链**：PyTorch → ONNX（`dynamic_axes` 标 batch）→ `trtexec`；
- **评测链**：经典 / 学习 / Log-MAP 三线对比，指标是 coded BLER。

---

## 2. cuPHY 侧的 LLR 契约（代码级，★ 含对我初稿的更正）

> 本节来自对实际代码的逐行核对（`cuPHY/`，SDK 26-2-cubb）。

### 2.1 ★ 更正一：cuPHY **没有 `RANGE_LIMIT`**，也没有逐调制 LLR 标度

全仓 grep `RANGE_LIMIT|range_limit|llr_scale|LLR_SCALE` **零命中**。
NVIDIA 的软解映射器用的是**解析公式**，唯一的标度来自星座归一化常数与噪声方差：

```
LLR = 2 · A · z · (1/σ²_PAM),   其中 σ²_PAM = σ²_QAM / 2
A(QPSK)=1/√2, A(16QAM)=1/√10, A(64QAM)=1/√42, A(256QAM)=1/√170
```
（`soft_demapper.cuh:396,1125,1148,1174`；`soft_demapper.cu:173-174` 做 `inv_PAM = 2·inv_QAM`。）
BPSK/QPSK 走直接算术，16/64/256QAM 走 **mipmapped texture LUT**（逐比特最小距离查表后乘 `noiseInv`）。

★ **我们仓库的 `range_limit`（BPSK/QPSK 24、16/64/256QAM 20，`memo_01` §2）是我们自己的设计**，
不是业界通用做法。两者是**不同的 LLR 标度哲学**：我们"先算再裁到固定范围"，他们"按噪声方差自然标度、不裁"。
⇒ 对我们的意义：**模型的输出标度必须对准我们的 `range_limit`**（`memo_01` §2 的标度校准），
而不是对准某个"业界标准"，因为不存在这样的标准。

### 2.2 ★ 更正二：**出货的 LDPC SPI 只接受 fp16**，默认 clamp = **32.0**

`cuPHY/src/cuphy/ldpc/ldpc_cb_kernel_api.h:99,109` 逐字：**"Only ::CUPHY_R_16F is supported"**，
并在 `ldpc_cb_kernel_spi.cpp:63-66` 强制（否则 `INVALID_ARGUMENT`）。
`clamp_value` 必须有限且 >0（`:68-71`），**默认 `32.0f`**（`ldpc/ldpc_api.hpp:111`）。

⇒ **更正我初稿的暗示**：虽然 `cuphyDataType_t` 里有 fp8(E4M3/E5M2)、独立解映射器与 ldpc2 loader
里也确实有 fp8 路径（`ldpc2_llr_loader_fp8.cuh:70-86`），但**出货的 PUSCH LDPC SPI 拒绝 fp8**。
所以"fp8 LLR"在本 repo 里是**存在但未启用**的代码路径。

**clamp 校验规则**（`cuPHY/examples/error_correction/ldpc_clamp_validation.hpp:31-54`，
测试 `cuPHY/test/error_correction/test_ldpc_clamp_validation.cpp`）：

| LLR 类型 | max finite | 规则 |
|---|---|---|
| fp8 E4M3 | 448.0 | `0 < clamp_value < 448`（**严格小于**） |
| fp8 E5M2 | 57344.0 | `< 57344` |
| **fp16（出货唯一可用）** | **65504.0** | `< 65504` |
| fp32 | 65504.0 | `< 65504` |

★ clamp **在 LDPC kernel 内部 load/stage LLR 时施加**，不是独立 kernel。
（`cuphy.h:3440` 那句 "Clamp buffer kernel (launched last)" 属于 **rate matching**：
`de_rate_matching_clamp_buffer` 对 HARQ buffer 裁 **±10000**，`rate_matching.hpp:24`。
我初稿把它误挂在 LDPC 上。）

### 2.3 ★ LLR buffer 布局：与我们**同约定**，但他们恒定 pad 到 8

解映射器输出张量：`(CUPHY_R_16F, 8, NUM_LAYERS, NF, NUM_DATA_SYMS)`，
**bit 是最快变化维 ⇒ `[re][bit]`**（`pusch_rx.cpp:10841`，`QAM_STRIDE 8` 恒分配、
**QPSK/16QAM/64QAM 也 pad 到 8**，`cuphy.h:146`）。

★ 对照我们：`memo_01` §1 的契约是 `llrs[re*Qm + b]`——**同样是 RE-major / bit-fastest**，
**但我们不 pad**。⇒ 布局约定一致，**差别只在 stride**，这是接入时要注意的一个具体点。

**解扰位置**：NVIDIA 把**解扰融合进 de-rate-matching kernel**（符号位 XOR，
`rate_matching.cu:543-566`），独立的 `descrambling.cu` 模块**从不被 PUSCH Rx 调用**（legacy）。
★ 对照我们：解扰在**解调器内部**（`pusch_demodulator_impl.cpp:730`）就做了。
**同一个数学，不同的位置**——而位置决定了"模型输出加扰域还是解扰域"（`memo_01` §3）。

### 2.4 ★★ 更正三：cuPHY 内部**没有任何神经/学习模块**

- `grep -i neural cuPHY/` → **零命中**。
- TensorRT 确实在 `cuPHY/src/cuphy/trt_engine/`，但**只接信道估计**：
  `ch_est/trtengine_chest.{hpp,cpp}` + `ch_est::IModule` + `createPuschRxChEst`
  （`chest_factory.hpp:33-58`），接口是 **DMRS-LS 估计 → 信道估计**，**不是 LLR**。
- 因此**唯一干净的替换点是函数级**，不是插件：
  `cuphy_i::soft_demap(...)`（`soft_demapper.hpp:48-55`）或融合的 device helper
  `ch_eq_soft_demapper_tex(...)`（`channel_eq.cu:3513-3521`），由**编译期**宏
  `EQ_SOFT_DEMAP_USE_TEX` 选择（`channel_eq.cu:68`）——**没有运行期钩子**。

★★ **这条对我们非常重要，而且它是一盆冷水**：
**`neural_rx.onnx` 那个"深度 3 神经接收机"是 pyAerial 里的一个示例，跑在 cuPHY 之外，
并没有集成进生产的 L1 路径。** 所以"工业界已经在生产里这么做了"**不成立**；
准确的说法是"**工业界已经把这条路的模型、数据流程、评测方法都做出来并开源了，
但把它放进生产 L1 这件事，他们用插件化的 CE（而不是神经接收机）来做**"。
⇒ 这**加强**了本工作流的价值主张（`memo_03` §6 的空白清单仍然成立），
同时也给了我们一个**更现实的对照**：他们的生产化程度低于论文给人的印象。

### 2.5 均衡—解映射是**融合**的，且噪声输入是"偏差校正后的残差方差逆"

`channel_eq.cu` 是**一个 equalize+demap kernel**。逐 RE 噪声输入是
**逆残差误差方差对角 `tReeDiagInv`**，带偏差校正：`λ = 1/(1−Ree)` 上限 `MAX_BIAS_LIMIT=10`；
`reeInv = 1/(λ·ree)` 饱和于 `MAX_ERROR_PRECISION=10000`（`channel_eq.cu:856-880`）。

★ 这与我们的"后均衡噪声方差"（`eq_noise_vars`，`memo_01` §1.3）是**同一个量**，
而且他们**做了偏差校正**——这是一个我们目前没有的细节，值得记入待办。

---

## 2bis. ★ E3 / data lake：与 OCUDU dApp **同族但不同线**（代码级）

### 2bis.1 NVIDIA 的 E3 是 **ZeroMQ + JSON**，不是 SCTP/APER

- `cuPHY-CP/data_lake/e3_agent.{hpp,cpp}`：三个 ZMQ socket（REP/PUB/SUB）绑
  `tcp://*:<port>`，默认 **5555/5556/5557**。RAN id `NVIDIA_L1`，RAN function 2 = KPM。
- 过程名与 OCUDU 同族（`setupRequest`/`subscriptionRequest`/`indicationMessage`/`messageAck`/`release`，
  键名 `dAppIdentifier`/`ranFunctionIdentifier`/`periodicity` 默认 **100000 µs**）。
- ★ 而 OCUDU dApp 论文自己的 Table IV 就列着："NVIDIA cuSense = **JSON**，transport **ZMQ REQ/REP and
  PUB/SUB**，payload out-of-band shared memory"——**与代码完全一致**。
⇒ **同族接口、不同 wire**（OCUDU = ASN.1 APER over SCTP/36423 + FlatBuffers E3DP/38472）。
**不 wire-compatible，但 dApp 侧流程可移植。**

### 2bis.2 ★ data lake 是**主机内存、拷贝式双缓冲环**——不是零拷贝

cuPHY 先产出**主机侧**结果（D2H 由 `cpuCopyOn` 控制，`cuphy_api.h:685`），`DataLake` 再 memcpy 进环。
**没有设备指针、没有 CUDA-IPC/NVSHMEM**。
发布描述符是 `SharedMemoryHeader`（`e3_agent.hpp:401-423`）：POSIX SHM `/e3_ran_buffers`，
0666，默认 ≈0.73 GB，`access_pattern:"double_buffer"`。

★★ **更正**：OCUDU 论文说 NVIDIA 的方案是 *"publishing RAN data through shared-memory descriptors to
**GPU inference backends**"* ——**只对了一半**：
E3 manager + SHM descriptors ✓；**"Triton" 全仓零命中 ✗**；**GPU 推理后端 ✗**
（SHM 是**主机 POSIX SHM**，负载靠 memcpy）。那句话描述的是**dApp 容器那一侧**（其引文 [8]/[15]），
**不是本 repo**。

### 2bis.3 ★★ NVIDIA 的 E3 **没有回写通路**

`controlIdentifierList` 恒为 `[]`、`controlGrantedList` 恒为 `[]`、
`handleControlMessage` 直接 ack **"control actions not implemented"**。
⇒ 它是**单向遥测**：没有 in-process C ABI、**没有 Class A/B/C**、**没有 100/150 µs 契约**、
没有准入边界、没有回退、没有调度器钩子。

★★ **结论**：**OCUDU 的 dApp 平台与 NVIDIA 的 E3 不是一回事**，前者**明显更靠前**
（三大时序契约 + 签名包 + 生命周期 + 回退 + lane breaker）。
⇒ 我们要对标的**是 OCUDU 的契约**，而 NVIDIA 这边只能提供**"单向遥测 + SHM 描述符环"**这一层的参考。
这也**再次印证** `memo_03` §2.2 的判断：**外部观察者路线对神经接收机是不可表达的**
（NVIDIA 自己的实现就是这个限制的活例子——它的 E3 连回写都没有）。

### 2bis.4 ★ 最值得"抄形状"的一件东西：`e3agent-standalone`

`cuPHY-CP/e3agent-standalone/` 是一个**独立测试台**：它**编译生产的 `e3_agent.cpp`**，
但用 shim **完全摆脱 cuPHY / CUDA / FAPI / ClickHouse**（`README.md:12`），
并带 `scripts/check_drift.py` **检测镜像与生产源码的漂移**。

★ **这是一个高度可移植的测试模式**，值得我们学：
**把生产代码本身（而不是它的复制品）编进一个无依赖的测试台，并用一个脚本守住漂移。**
我们仓库里 `ul_chain_replay` 与 `lib/phy/metal/*_probe` 已有类似意图，
但"**漂移检查**"这一步我们没有——而这正是"离线测试台与线上代码不同步"这一整类问题的解药。

## 3. 可以重用 / 需要修改（映射到我们的架构原则）

参照 `metal_vs_cuda_architecture.md`：**双重异构**（算力 + 存储）、**读者属地**、
**lane 而非 backend**、**ANE 是车道内的环节**、**路由策略是独立注入的决策对象**。

### 3.1 ✅ 可以重用（方法学 / 设计形状）

| 项 | 性质 | 说明 |
|---|---|---|
| ★ **深度 3 的输入规格** | **直接照抄** | 网格 I/Q + **LS 估计** + **DM-RS 符号位置** + **DM-RS 子载波位置**（整数索引）。这是可移植的**接口契约**，与平台无关 |
| ★ **"一个 trunk 多读出"** | 直接照抄 | `readout_ll_rs` + `readout_ch_est` ⇒ 我们的"主头 LLR + 辅助头信道估计"是工业同形 |
| ★ **LS 估计作为网络输入** | 直接照抄 | 让网络学"插值+噪声估计+均衡+解映射"的联合，而不是从零学估计 |
| ★ **三线 BLER 对比（经典/学习/Log-MAP）** | 直接照抄 | Log-MAP = 我们的 P0-c genie 上界；指标是 coded BLER |
| **一个模型服务多调制阶（输出切片）** | 借用 | 输出固定 8 路，按 `mod_order` 切片 |
| **TrtEngine / TrtTensorPrms 的包装形状** | ★ 借鉴**形状** | "模型文件 + 命名张量描述符（形状/dtype）+ 流"。我们的 `ocudu_coreml_nn_engine` 是同一角色的 Metal/ANE 版。★ **建议把两者的接口对齐**，让路由策略能统一对待 |
| **数据记录格式** | 借用 | parquet 元数据 + 每记录 pickle；逐参数化扫（信道×SNR×MCS）并记 `snr` |
| **导出链 PyTorch→ONNX→引擎** | 借用 | 我们是 PyTorch/TF → **CoreML**（`ai_train/convert_coreml.py` 已有） |
| **LLR clamp 校验规则** | 直接照抄 | clamp **严格小于**类型 max finite（fp16 上界 65504） |
| ★ **LLR buffer 布局** | 已同约定 | 他们 `[re][bit]`、bit 最快变化；我们也是。★ 差别只在**他们恒 pad 到 8**、我们不 pad |
| ★★ **"无依赖测试台 + 漂移检查"** | ★ **强烈建议借鉴** | `e3agent-standalone/` 直接编译**生产源文件**、用 shim 摆脱全部平台依赖，并用 `scripts/check_drift.py` **守住镜像与生产的漂移**。★ 我们缺的正是"漂移检查"这一步 |
| **均衡噪声的偏差校正** | 借鉴 | 他们把逆残差方差做偏差校正（`λ=1/(1−Ree)`，上限 10）。我们目前没有这个细节 |

### 3.2 ⚠️ 需要修改（我们不能照抄的）

| 项 | 为什么 | 我们怎么做 |
|---|---|---|
| **数据来源** | NVIDIA 用**仿真**（`FadingChannel` TDL/CDL，TR 38.901）为主 | 我们**同时有真实 OTA 采集**（17.7 万 CRC-OK TB）⇒ 走"仿真 + OTA"双臂（`memo_03` §4.3 的负面证据要求这是受控变量） |
| **监督信号** | LLRNet 用**经典 LLR 做 MSE（蒸馏）** | ★ 我们用**真值比特 + BCE**（`memo_04` 路线 1）——**更强**，无教师上限。保留蒸馏作热身 |
| ★ **LLR 表示** | fp16/fp32/fp8 浮点 | 我们链上是 **int8 均匀量化 + `LLRM/AX=120`**。要么适配（网络输出 float → `quantize()`），要么论证改 LDPC 侧（**代价大，不建议**） |
| **引擎层** | TensorRT + CUDA stream + `cupy` 零拷贝 | **CoreML/ANE + Metal**。★ 注意 `memo_03` §1.5 的约束：**自定义 Metal kernel 不能进 ANE 常驻图** |
| **执行位置** | 神经接收机在 **cuPHY 之外**（pyAerial 进程 + TRT） | 我们**内联**（`estimator.estimate` + `demodulator.demodulate` 两处合并）——因为 dApp 论文证明外部框架对神经接收机 **Inexpressible**（`memo_03` §2.2） |
| **harness 时间尺度** | — | ★ 调研记录的教训：AtlasRAN 曾把"接收机限制"误判为**测试框架的时间尺度膨胀**（`memo_03` §7bis.H）⇒ 我们测时延时必须核对 |
| ★ **替换点的选择机制** | NVIDIA 把解映射器做成**函数级 + 编译期宏**（`EQ_SOFT_DEMAP_USE_TEX`），**没有运行期钩子** | ★ **我们的做法更好，应坚持**：`supports_fused_demapping(...)` 是**运行期谓词**，可以做 A/B、可以进路由策略。**不要为了"像上游"而退化成编译期开关** |
| **"工业界已在生产里做神经接收机"这一印象** | ★ 代码事实：`grep -i neural cuPHY/` **零命中**；TRT **只接信道估计**。`neural_rx.onnx` 是 pyAerial 示例，**在 cuPHY 之外** | ⇒ 我们的价值主张**不受影响**（`memo_03` §6 的空白清单仍成立）。引用时要写清"他们开源了流程与模型，但生产 L1 里没有神经接收机" |

### 3.3 ❌ 明确不借鉴

| 项 | 理由 |
|---|---|
| `device_vector` / `cuda_copy` / `cudaMalloc` 式离散内存模型 | 会把**离散地址空间**引入设计，与 UMA 零拷贝的核心成就冲突（`metal_vs_cuda_architecture.md` §6.4 已判） |
| TensorRT / CUDA stream / NVSHMEM / CUDA-IPC | 平台专有 |
| "永远兜底"的回退语义 | 我们的 **lane 严格性**由模式决定：`cpu_gpu` 兜底、`gpu` 报错（§1.6） |

---

## 4. ★ 可扩展性 / 可移植性（按你的要求，用高层架构原则检验）

★ **核心判断：aerial 给我们的最大价值不是代码，而是"哪些是平台无关的设计决定"的答案。**
把它们按 `metal_vs_cuda_architecture.md` 的框架分三层：

### 4.1 完全可移植的一层（应当抽出来，是将来换平台时唯一保值的东西）

| 概念 | aerial 里的对应物 | 我们应有的形态 |
|---|---|---|
| **推理引擎的抽象** | `TrtEngine` + `TrtTensorPrms`（模型文件 + 命名张量描述符 + 流） | 与我们的 `ocudu_coreml_nn_engine` **统一为同一个"引擎"接口**：`init(model)` / `run(named tensors, stream)` / `last_predict_us()`。★ 这样"换引擎"是**绑定**而不是**改代码** |
| **张量描述符** | `TrtTensorPrms(name, dims, dtype)` | ★ 我们已有 `tensor_desc.h`（`metal_vs_cuda_architecture.md` §6.2 提到它让 GPU 输出可直接作 CoreML 输入）。**把 CoreML/ANE 引擎也接到同一个描述符上** |
| **路由/策略对象** | 无（NVIDIA 是静态配置） | ★ 我们的 `phy_routing_policy`（§6.3）**是唯一适合与上游共享的策略层**，而且**必须能感知 ANE 负载与 `last_predict_us`** |
| **数据集/评测协议** | 三线 BLER + parquet 记录 | 我们的 G 门禁 + `ul_capture` 五件套 |

### 4.2 "形状可移植、实现平台专有"的一层（照形状重写）

| 概念 | aerial | 我们 |
|---|---|---|
| 设备常驻张量的发布/订阅 | `cuPHY-CP/data_lake/` 的 descriptor ring + `e3_agent` | ★ 若将来要走 dApp/E3，**形状**可借；但**我们默认内联**，所以这一层现在只作参考 |
| 零拷贝推理绑定 | `cupy` 数组 → TRT（同设备内存） | `MLMultiArray` + `initWithDataPointer`（**已在 AI CE 里跑通**） |
| kernel 级融合 | cuPHY 的 fused PUSCH Rx | 我们的 `merged_hop`（一个 CB 覆盖整跳） |

### 4.3 平台专有、不可移植的一层

CUDA stream / TensorRT plan（★ notebook 自己说 *"TRT engine … are not portable across different
platforms"*）/ CUDA-IPC / NVSHMEM / `cudaMalloc` 内存模型。

★ **可移植性结论**：
**Apple Silicon 是唯一成熟的"算力+存储"双重异构平台，所以我们的抽象必须按"双重异构"来设计，
而不是按"CUDA 的替代品"来设计。** aerial 的 `TrtEngine` 抽象只覆盖了**算力**异构
（把模型丢给某个加速器），**没有覆盖存储异构**（它仍假设离散设备内存 + 显式拷贝）。
⇒ ★ **这正是我们可以做出差异的地方**：把"引擎"接口设计成
**"一个环节声明自己在哪个计算单元上、且它的输入可以是另一个环节的设备侧产物"**
（`metal_vs_cuda_architecture.md` §6.2 的裁定），而不是"给 ANE 也做一个 backend"。

---

## 5. 对规划的直接影响（待并入主规划）

| # | 变更 | 依据 |
|---|---|---|
| 1 | ★ **模型输入契约冻结为**：网格 I/Q + **LS 估计（导频处）** + **DM-RS 符号位置** + **DM-RS 子载波位置** | `neural_rx.onnx` 的 7 输入（§1.2） |
| 2 | ★ **深度 3 不等于"从零学估计"**：经典 LS 估计作为输入 | 同上 |
| 3 | ★ **尺寸锚点从"引用文献"变成"手里有权重文件"**：1.45e5 参数 | §1.1 |
| 4 | ★ **多任务头得到工业印证**（LLR 读出 + 信道估计读出） | §1.3 |
| 5 | ★ **新增可交付**：int8-uniform vs fp16 的 **LLR-vs-精度曲线**（文献空白；★ fp8 在 NVIDIA 出货 SPI 里被拒，参考价值下降） | §2.2 |
| 6 | ★ **clamp 规则**：严格小于类型 max finite；fp16 上界 65504，NVIDIA 默认 clamp = **32.0** | §2.2 |
| 6b | ★ **`RANGE_LIMIT` 是我们的设计，不是业界标准**：cuPHY 全仓无此物，它按噪声方差自然标度且不裁 ⇒ 模型输出标度**只能对准我们自己的 `range_limit`** | §2.1 |
| 6c | ★ **LLR 布局已一致**（`[re][bit]`，bit 最快），差别只在 pad；接入时注意 stride | §2.3 |
| 6d | ★ **均衡噪声要做偏差校正**（`λ=1/(1−Ree)`），这是我们现在没有的 | §2.5 |
| 6e | ★ **测试台纪律**：无依赖测试台 + **漂移检查**——建议引入 `check_drift` 式机制 | §2bis.4 |
| 7 | **数据流程照抄四步**（造数据 → 经典链生成标签 → 训练/导出 → 三线 BLER 评测） | §1.5 |

---

## 6. 未完成 / 待办

- ⏳ 三路并行深挖进行中：cuPHY LLR 数据通路、E3 agent / data lake、pyAerial 的 ML 集成与可移植性台账。
- ❌ `wg2_ai_ran` 需要你授权后才能做代码级调研（现在只有论文级）。
- ⏳ `neural_rx.onnx` 的**两处输出形状在 notebook 与单测之间不一致**
  （notebook: `output_1=(8,1,3276,12)`；`test_algo_trt_engine.py`: `output_1=(2,1,3276,12)`、
  `output_2=(1,3276,12,8)`），需要进一步确认哪一个是权威——
  本备忘暂以**能跑通整链的 notebook** 为准，但这一条必须核实后再写进对外材料。
- ⏳ `readout_ll_rs` 与 `readout_ch_est` 两条分支与 ONNX 输出名的对应关系未最终确定。
