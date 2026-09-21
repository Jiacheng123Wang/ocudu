# 交接（入口） — S17：**三条线索两条否证 + D1 已定价、可行性已确认；下一步 = D1 第 1 步（DFT 交出缓冲的接口）**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **不要读 `session_handoff_2026-09-20-6.md` 与 `-7.md`**（它们是 S15/S16 早期的状态：那时 K1 路 A 还没做、
> `[ul_time_frequency]` 的谜题还没解、D1 还没定价——**本文件已包含全部结论**）。
> **技术细节全在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md`**，
> 本会话新增的是 **§5.8.28 – §5.8.31** 与 **§5.9 – §5.9.4**（**§5.9.4 是 D1 的开工依据**）。

---

## 1. 一句话状态

**工作树 HEAD = `c4f609ac3f`**（S15 的 `ddc09868e4` + **本会话 28 个提交**）。
**开机第一件事就是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`，坑 35）。
工作树只剩**用户自己的两个 config**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。
**28 个提交全部只在本地，没有推送。**

**门（本会话未改门，末次复跑全过）**：
```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py             # 47 捕获 0 问题
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8
ctest --test-dir build -R metal                                   # 9/9
ctest --test-dir build -R "ul_pipeline_probe|puxch|lower_phy"     # 14/14（含新增的探针断言）
```
**逐字节网 `neutral_vs_baseline.sh` 仍是"信息"不是门**（当前 131 字节，与 S15 同数 ⇒ 出厂路径未动）。

---

## 2. ★★ 本会话三条线索的结论（**全是否证性的，但都给出了机制**）

### 线索一：**K1 已到头**（否证了"还能优化"）

| 测量 | 读数 |
|---|---|
| 栅栏 vs 同步 | `mem_threadgroup` **207.0 µs** vs `mem_none` **207.1 µs**（**1.000×**）⇒ **barrier 的内存栅栏免费**，成本全在**同步**本身 |
| K1 的形状 | **已经是 1 barrier/pivot**（第一道 barrier 同时干"归一化主元行"和"发布给消元用"）|
| 三版"更少 barrier"的重写 | **全部算错**（n=54 全 NaN / 相对误差 3.8e5），错因各不相同（§5.8.28 ③）|
| 几何 / 线程数 | 默认 (64,16) 最优；**线程少反而慢 2.4×**（32×4）|

**⇒ 在"逐元素运算次序不变"前提下，115 道 barrier 里没有一道是白放的。K1 记为"到头"。**

### 线索二：**收包粒度不是杠杆**（否证了用户最初的"桶 vs 杯子"假设）

| 发现 | 结论 |
|---|---|
| **`[ul_time_frequency]` ~1000 µs 之谜** | **不是 bug**：探针起点被移到 `receive()` 之前（`690b086679`），于是**包含了等样本**。同设备历史读数是 **15.5 µs**（旧口径，`pipeline_audit_2026-09.md` §6.2 已加不可比警告）|
| 收包粒度腿对 | **每槽等待 997.9 vs 997.4 µs（六次复现）**，端到端 **−1.6%** |
| **机制**（逐槽时间轴）| **前端没有"提前做完"——它在等最后一个样点**：`rxwait ~350` + `t2f ~1080` = `pipeline ~1150`（自洽）；其中 ~500 µs 等后半槽，**一个槽的 CPU FFT 只有 ~230 µs 量级** |

**⇒ 在"上层按槽消费网格"的前提下，收包粒度买不到端到端延迟。**

### 线索三：**D1 已定价 + 可行性确认**（✅ 这条是要继续做的）

| 问题 | 答案 |
|---|---|
| D1 要消什么 | **1.83 次 DFT 提交/跳**（一跳合计 ≈ 2.83 次提交：DFT 1.83 + 车道 1.00）|
| 宿主还被占多少 | **每槽被 DFT 阻塞 ~507 µs**（`[ul_dft_wait]`），但这是**次要**的（用户裁定 A 优先）|
| 跨队列栅栏值多少 | **0**（同队列臂 `[ul_dft_wait]` 506.8 → 505.8，一字未动）|
| **合并的代价** | **实测为零**：`dft residency` **420.2 → 419.6 µs**（同队列），**重叠不丢** |
| **可行性** | ✅ **一跳之内宿主不再需要读设备数据** ⇒ 一条 GPU 链可达（详见 §4）|

---

## 3. ★ 用户的裁定（**长期有效，本会话新增两条**）

| # | 裁定 |
|---|---|
| ① | `mode=gpu` 不允许宿主兜底 |
| ② | dump 不算 CPU in the loop |
| ③ | 控制面融合分阶段做、**每阶段一次 OTA**（腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）|
| ④ | **`merged` 保留为默认** |
| ⑤ | 顺序：~~先 K1 路 A~~（已做完）→ **DFT/D1** |
| **⑥** | **★ 新增：排序标准是"CPU 还剩几个参与点"（个数），不是耗时**。所以 **A（提交数）优先于 B（宿主空等）**；B 是"CPU 被占着"，等 CPU 真正靠边站之后再优化 |
| **⑦** | **★ 新增：判据是"GPU 一旦发动，就能不停顿地跑完 IQ → LLR"**；中途若还需要 CPU 才能继续，那一点**就算参与点**。"只要 GPU 一旦发动，就可以暂时不关心 CPU 在干什么" |

---

## 4. ★★★ D1 的开工依据（**这一节是本次交接的核心**）

### 4.1 目标形态（用户的判据 ⑦ 的具体化）

```
今天：[DFT 缓冲(前端队列) commit] ──宿主等 ~507µs──→ [网格] → 车道缓冲打开 → 权重 → 均衡 → commit
目标：[一条缓冲] DFT 14 个变换 → 提取 → 权重 → 均衡 → 解映射 → 【一次 commit】→ GPU 一口气跑到 LLR
```

**⇒ 提交数 2.83 → 1.00/跳。**

### 4.2 ★ 可行性已逐点核实：**一跳之内宿主不再需要读设备数据**

| 检查 | 结果 | 依据 |
|---|---|---|
| 权重是宿主算的吗？ | ❌ **不是** | `gpu_invert` 为真时**设备自己求逆**（K1）；`gauss_jordan_invert` 只在 `!gpu_invert` 的**回退**路径 |
| 提取结果要回宿主吗？ | ❌ **不要** | `hold_for_weights` 的承诺原话：*"Everything else the host consumes from the extraction … is read at the hop's COMPLETION"*；`stage_engine_group()` 改为**从设备暂存 y** |
| 宿主缩放量 | ❌ **默认关** | `OCUDU_CE_HOST_SCALARS` 未设即 false；且它一开，`hold_for_weights` **自动关闭**（互斥）|
| `ls_pilot()` 的设备读 | 只发生在**回退** | 设备 LSE 无效，或 `OCUDU_CE_DEV_SIGMA2=0` |

**⇒ 一跳的宿主参与点只剩"DFT 提交"与"车道提交"两处，中间没有同步点。**

### 4.3 现成的机制（**不需要新发明**）

| 需要的能力 | 现成的 | 位置 |
|---|---|---|
| DFT 已经"一条缓冲收一整槽变换" | `open_cb`/`open_enc`（`OCUDU_DFT_OPEN_BLOCK` 默认开）| `ocudu_dft_metal_engine.mm:672 begin_block()` |
| "接手别人的命令缓冲继续编" | **`shared_burst::adopt(cb)`** | `ocudu_metal_burst.mm:205` |
| 阶段间 buffer 级栅栏 | `stage_pipeline()` 切 kernel 时插 `memoryBarrierWithScope:MTLBuffer` | `ocudu_metal_mmse_engine.mm` |
| 车道缓冲由估计器提取打开 | `pilots_stage::hold_for_weights` | `port_channel_estimator_metal_mmse_impl.cpp:1637` |

### 4.4 ⚠ 两条必须先处理的约束

1. **`adopt()` 不为被接管的缓冲编码前端/后端栅栏**（它的注释：*"its command-buffer-level FENCES …
   stay as the stages inside it encoded them"*）⇒ **DFT 阶段内部必须自己编码它需要的栅栏**，
   否则就是 **P0 的签名（读到没人 signal 的代际 ⇒ 静默错数据）**。§5.8.26 ② 记过同一形状的缺陷。
2. **`wait_all()` drain 的是 front-end 链**（`ocudu_dft_metal_engine.mm:1064` 附近）⇒
   DFT 若常驻车道队列，**它必须一起改**，否则退出时的 drain 会漏掉 DFT 的工作。

### 4.5 ★ 实施步骤（**每步可单独验证；建议一次会话做一步**）

| 步 | 做什么 | 判据 |
|---|---|---|
| **1** | **DFT 引擎提供"交出未提交的缓冲"**（新的 `release_block()` / detach），**默认不启用** | **不启用时出厂路径零影响**：`value_net` 47/0、`ctest -R metal` 9/9 |
| **2** | **让车道在槽末（样本到齐）打开缓冲**，把 DFT 编进去，再让提取/权重/均衡接着编（`adopt`）| `dft commits` **→ 0**；`cbs/lane` 仍 **1.00**；契约 `ce device estimates`/`equalizer ch_re` **host=0/0**；CRC 不劣化 |
| **3** | `wait_all()` 与栅栏归属跟进 | 退出无悬挂；CRC 不劣化 |

**⇒ 第 1 步是自包含的，可以从它开工。** 第 2 步是跨三处的编排改动
（`lower_phy` 槽末 + 估计器 `hold_for_weights` + DFT 引擎），**必须"改一处、验一处"**。

### 4.6 腿法（**⚠ 模式必须是 `cpu_gpu`**）

```bash
# 对照（Metal DFT + 默认）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu s30-d1-step1
# 候选（加上你要验的旋钮；旋钮作为【位置参数】传，别放 sudo 前面）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh cpu_gpu s30-d1-step1b OCUDU_*=…
```

**判读前先看 `[ul_dft_wait]` 是否有样本**——`no samples` 说明**模式又选错了**（见 §5.2）。

---

## 5. ★★ 本会话踩过的坑（**新会话最可能重犯的，全部已记档**）

### 5.1 我（上一会话）在这条仪表上花了 **7 次空口腿**才让它可用

根因**全是实现缺陷**，而且是**同一族**——"逻辑上成立"代替了"在真实形态下真的触发"：

| 轮 | 缺陷 |
|---|---|
| s18 | 哨兵用 `time_point{}` 判"缺失"（**本平台 `high_resolution_clock` 就是 `steady_clock`，纪元是【开机】**，默认值**不是哨兵**）|
| s19 | 用**块尺寸**而非块末 ⇒ 半 slot 块永不触发 |
| s20 | 用"块末落在 slot 边界" ⇒ **流偏 7 样点就永不触发**（偏移恰好等于配的符号数）|
| s21–22 | 从"帧内槽号"反推位置、与**绝对**时间戳比较（**混帧**；实测 `sfn0=0`，即恒等变换）|
| s23 | **有界 map 装满后拒绝新键 ⇒ 留下最旧**（运行开头的接入期）；**同一函数里残留的旧辅助函数**又犯了 s18 的错 |
| s24 | 以"样本集齐"为键 ⇒ 预算全花在 **idle 槽**上（FFT 对 idle 槽也跑）|
| s25 | 早退 `what != ldpc_start → return` 挡在写条目前 ⇒ **CRC 列永远 NaN**（而其他列全对）|

**四条教训（与坑 4 同族，务必遵守）**：

1. **哨兵值必须是不可能真实出现的值**——在纪元是开机时刻的时钟上，`time_point{}` 是**普通值**；
2. **判定类改动必须先构造出"应触发"的真实形态再验**（此处的真实形态：**流偏移、块 3840/545、跨界块**）；
3. **有界缓存要写"淘汰最旧"并测溢出**——我写的第一个淘汰断言**只加 32 项对上限 512**，
   **因错误的原因通过**（**空洞断言比没有断言更糟**，它给出虚假的信心）；
4. **诊断要打"原始量"，不要只打派生量**——`t2f` 的 71680 看着合理，**是两列原始时刻（差 72 s）一眼定案**。

### 5.2 ★ `cpu` 模式下 D1 的对象【不存在】

`phy_pipeline_mode::cpu` **强制所有后端走 CPU**（`du_low_phy_pipeline.h:163-172`：任何非 CPU 的
`--pusch_dft_type` 直接**报冲突拒绝启动**）⇒ **没有 Metal DFT、没有前端队列、没有 `dft commits`**。

**⇒ `s17`–`s26` 那批 `cpu` 腿对"收包粒度/等待"的结论【仍然成立】**（那条链与 DFT 后端无关），
**但它们不能用来判 D1**。**D1 的腿必须用 `cpu_gpu` 或 `gpu`。**
（本会话为此白跑了 `s26` 两条腿。）

### 5.3 前端栅栏是 **opt-in**（默认关闭）

`shared_queue::front_end_fence_enabled()` 读 `OCUDU_UL_FRONTEND_FENCE`，**默认 false**。
默认运行的网格顺序由**宿主每槽一次 `wait_slot()`** 提供。

**⇒ §5.8.12 那次"前端栅栏开关无差别（1392.6 → 1395.2 µs）"极可能是假阴性**：
开关没设，**两臂是同一个未武装配置**。**凡是以该开关为基础的臂都必须显式设为 1。**（开放项 9）

### 5.4 其它

* **`.air` 中间产物会静默陈旧**：改 `.metal` 后若 kernel "在 `strings` 里有、`metal-nm` 里没有"，
  就删 `build/…/ocudu_mmse_inv.air{,.d}` 与 `ocudu_mmse.metallib` 再全量构建（§9）；
* **`run_leg.sh` 的旋钮要作为【位置参数】传**（脚本在 root 进程内 `export`）；写在 `sudo` 前
  在**本机**能透传（有日志为证），但脚本注释记了"`env_reset` 可能剥掉"，**位置参数更保险**；
* **同一非递归互斥量重复加锁会【挂死】而不是失败**（本会话踩过）：探针单测**挂住**比报错更难归因。

---

## 6. 工具、旋钮与仪表（**本会话新增的全部在这里**）

| 名字 | 说明 |
|---|---|
| **`wip/value_net.py`** | **门**：`--env K=V`（可重复）/ `--env-passthrough` / `--self-test` / `--what` / `--quiet` |
| **`[ul_rx_wait]`** | **新增**：收包阻塞多久（**唯一没有配对的序列**，按块计数）。读法：`[ul_time_frequency] − [ul_rx_wait]` = 前端自己花在样本上的时间 |
| **`[ul_dft_wait]`** | **新增**：宿主在 DFT `wait_slot()` 里阻塞多久（**不依赖任何开关**）。**只在 `cpu_gpu`/`gpu` 有样本** |
| **`[ul_slot_trace]`** | **新增**：每槽时间轴（**只记有 PUSCH 的槽**），`OCUDU_UL_SLOT_TRACE=N`（N≤512），**给人眼看的、不进门** |
| **`OCUDU_DFT_BACKEND_QUEUE=1`** | **新增**：DFT 提交到后端队列（**实验**）。⚠ **`wait_all()` 的语义会被它破坏** |
| **`OCUDU_INV_MEMNONE=1`** | **新增**：选 K1m（**栅栏被去掉、结果未定义**）——只用于"栅栏 vs 同步"判定，**已完成，答案=同步** |
| **`[inv_kernel]` 横幅** | **新增**：非默认 K1 kernel 被选中时打 stderr（**报旋钮臂前先看它**，坑 4）|
| `OCUDU_UL_FRONTEND_FENCE=1` | **前端栅栏（opt-in，默认关）**——用它的臂**必须显式设 1** |
| `OCUDU_CE_INV_BARRIERS=K` / `OCUDU_INV_TGX/TGY` / `OCUDU_INV_RL` | K1 探针（**+16.2 µs/道**，背靠背上界）/ 几何（**(64,16) 最优**）/ K1b（**数值错**）。**两个入口都认了** |
| `OCUDU_UL_RX_SYMBOLS=N` | 整符号收包（**实验性、有停机竞态**）。**实测可达 0.5 slot 粒度** |
| `OCUDU_CE_LANE_ORDER` | `merged`（**默认**）/ `event`（P2 回退，一行）|
| `OCUDU_DFT_OPEN_BLOCK=0` | DFT 的"每槽 14 变换共用一条缓冲"关掉（默认开）。**D1 会碰它** |
| `wip/run_leg.sh` / `wip/leg_report.sh` / `wip/k1_barrier_slope.sh` | 跑腿（`sudo -E bash … cpu_gpu <label> [OCUDU_*=…]`）/ 单腿判读 / 腿对模板 |

**★ 上腿前的三条自查（本会话的教训）**：
1. **模式对吗**（D1 必须 `cpu_gpu`/`gpu`）；
2. **判据会触发吗**（如 `[ul_dft_wait]` 必须有样本）；
3. **开关武装了吗**（凡依赖 `OCUDU_UL_FRONTEND_FENCE` 的臂必须设 1）。

---

## 7. 本会话的腿（**判读 D1 时只认 `cpu_gpu` 那四条**）

| 腿 | 模式 | 头条 |
|---|---|---|
| `s27-d1-base` / `-sameq` | `cpu_gpu` | **D1 定价**：`[ul_dft_wait]` **506.6 / 504.4 µs**；跨队列免费 |
| `s29-ov-base` / `-sameq` | `cpu_gpu` | **合并代价为零**：`dft residency` **420.2 / 419.6 µs**（重叠不丢）|
| `s17`…`s25` | `cpu` | 收包粒度腿对 + 时间轴调试（**结论对"等待/粒度"有效，不能判 D1**）|
| `s26-d1-*` | `cpu` | ❌ **白跑**：`cpu` 模式下没有 Metal DFT |

---

## 8. 未解 / 开放项（**按建议顺序**）

1. **★ D1 的三步**（§4.5）——**第 1 步可立刻开工**；
2. **"~500 µs 等后半槽 / ~580 µs 其余"的拆分是推算的**：前端 FFT 的 CPU 成本**从未单独计时**（§5.8.31 ⑩ ④）；
3. **`s25` 的时间轴只覆盖一次运行的 512 个槽**（约 0.5 s），不是全程分布；
4. **`wait_all()` 在 DFT 常驻车道队列后必须改**（§4.4 约束 2）；
5. **§5.8.12 那条旧读数需要重读**（栅栏默认没武装 ⇒ 很可能是假阴性）；
6. **`merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口**；
7. **`merged` 的 +471 µs 里 ~340 µs 排队与 64QAM CRC 变差的单变量隔离**未做；
8. **`shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉**（静默错址，9 个调用点）；
9. **§18.7 的 TA fence 实验仍未做**；TA 链 = 80.1 µs；
10. **K1 那条"每块列收尾 barrier"的 6% 理论移除**：已推理为安全但**未上腿**（§5.8.28 ⑥）。

---

## 9. 环境与流程

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
* **★ 改 `.metal` 后的正确姿势**（本会话踩过静默陈旧）：
  ```bash
  rm -f build/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.air \
        build/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.air.d \
        lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib
  cmake --build build --target ocudu_mmse_metallib -j 10
  xcrun metal-nm lib/…/ocudu_mmse.metallib | grep mmse_inv     # 验证 kernel 真的进去了
  ```
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh <mode> <label> [OCUDU_*=…]`
  —— **sudo 由用户执行**（把命令以纯文本贴给用户）；**Ctrl+C 停**；
  判读 `bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>`；
* **★ 上腿之前关掉手机的 WiFi**；**判读前先看 `Real-time failures` 是否为 0**；**CRC 按调制分层比**；
* **★ 离线绝对 µs 不可信**（宿主 GUI 抢 GPU）：**先跑空臂**，空臂宽度 ≥ 要判的效应 ⇒ 该次读数作废；
  **只报比值/份额**；**判 ≥40 µs 的效应只能靠空口腿对**（`ch_wt` 跨腿 ±8% ≈ 27 µs）；
  `last_gpu_wait_us()` 在单发下是**队列跨度不是 kernel 时间**（1/2/4/8 system 全读 206 µs）。

---

## 10. 一句话给新会话

**三条线索都已收口：K1 到头、收包粒度不是杠杆、D1 已定价且代价为零、可行性已逐点核实。**
**D1 第 1 步（DFT 交出未提交缓冲的接口，默认关闭）可以立刻开工，判据是"不启用时出厂路径零影响"。**
**动第 2 步（跨三处的编排改动）之前，先按 §5.2 的"上腿前三条自查"确认模式与判据。**
