> **⚠ 已被 `session_handoff_2026-09-21-4.md` 撤回 §4 的结论**：腿跑了，**候选臂否证了这条接线**（上行 CRC 全错）。
> 本文件保留为当时的判断记录；**腿法与结论不要再用**。

# 交接（入口） — S19：**D1 第 2 步的接线做完了（默认关闭、逐字节零影响）；下一步是空口腿——因为"DFT 交出 + 车道认领"这条组合离线跑不了**

> **本文件已作废**：新会话读 `session_handoff_2026-09-21-4.md`。
> **不要读 `session_handoff_2026-09-21-1.md`**（S17 的状态：那时 D1 第 1 步还没做）。
> `session_handoff_2026-09-21-2.md`（S18 前半）记的是**第 1 步**；**本文件已包含两者的结论**。
> **技术细节全在常驻设计文档 `gpu_phy_pipeline_design_and_implementation.md`**，
> 本会话新增的是 **§5.9.5**（第 1 步 + 对象同一性那条硬约束）与 **§5.9.6**（第 2 步的接线与守卫）。

---

## 0. 相对上一份（`-2.md`）的变化

| 项 | 变化 |
|---|---|
| **D1 第 2 步** | ✅ **接线做完了**（`-2.md` 说"可以开工"）：跨线程登记处 + C++ 桥 + 低层 PHY 交出 + 上层 PHY 认领，**默认关闭** |
| **★ 新增的三条"允许交出"的显式条件** | 缺任何一条就是 **P0 的签名**（deposit 没人取 ⇒ 网格没人写 ⇒ 静默错数据）。见 §2.2 |
| **★ 新增的未验项** | **"DFT 真的交出、车道真的认领"这条组合，离线跑不了**（`ul_chain_replay --dft` 写完网格就 `return 0`；本机没有 TD 语料）⇒ **空口腿是唯一判据** |
| **新仪表** | `[metal_stats] dft handover handed= taken= dropped= (armed=)`：**武装了就一定打这一行**（`handed=0` 是发现，不是"没接线"）|
| **门** | 全部同数：`value_net` 47/0、`ctest -R metal` **10/10**、`neutral_vs_baseline` **131** |

---

## 1. 一句话状态

**工作树 HEAD = `7fa657794c`**（S18 前半的 `c7107bc0ed` + **本会话 1 个代码提交**；快照提交跟在它后面）。
**开机第一件事就是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`）。
工作树只剩**用户自己的两个 config**（`gnb_rf_b200_fdd_n1_5mhz_bridge.yml`、`gnb_rf_b200_tdd_n78_20mhz.yml`），**别动**。
**提交全部只在本地，没有推送。**

**门（本会话末次复跑）**：

```bash
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py             # 47 捕获 0 问题
python3 doc_chinese/phy_pipeline_gpu/wip/value_net.py --self-test # 8/8
ctest --test-dir build -R metal                                   # 10/10
ctest --test-dir build -R "ul_pipeline_probe|puxch|lower_phy"     # 5/5（⚠ §5.3 的已知偶发）
bash doc_chinese/phy_pipeline_gpu/wip/neutral_vs_baseline.sh      # 131 differing-bytes（= S15/S17/S18 同数）
```

---

## 2. ★★ 本会话做了什么

### 2.1 四处改动，一处一个决定

| 处 | 改动 |
|---|---|
| **`shared_burst`** | **跨线程登记处**：`deposit_released(grid_base, cb)` / `take_released(grid_base)` / `handed_stats()`。**按资源网格的存储基址为键**、**有界（8 条、淘汰最旧、计数）**、mutex 保护（**两端是两个线程**）|
| **DFT 侧** | `dft_processor::release_block(const void* grid_base)`（纯 C++ 桥，默认 false）→ `dft_processor_metal` → 引擎：结束 encoder、**不提交**、deposit |
| **低层 PHY** | `ofdm_demodulator_impl::finish_symbol()`：槽末**交出**而不是 `end_block()`，**并且不再等** |
| **上层 PHY** | `mmse_engine::set_hop_grid(grid_base)`（适配器**每跳**一次）+ `build_pilots_lse()` 认领并 adopt |

**键为什么是"网格基址"**：两端本来就各自握着它（写侧/读侧的 `get_device_view().base`，**同一个 `rg_buffer.get_data()`**），
所以"这一跳认领的缓冲"**按构造**等于"写了它正在读的那份网格的缓冲"。

### 2.2 ★★ "什么时候允许交出"= 三条必须显式成立的条件（`handover_allowed()`）

| 条件 | 为什么 |
|---|---|
| `wait_per_slot`（= `device_write && grid_consumed_on_device`）| 配置**声明**过网格由设备消费（前端栅栏路线用的同一句声明）|
| **没有宿主网格读取臂**（`OCUDU_CE_CPU_CE` / `OCUDU_CE_CPU_LS` / `OCUDU_CE_LS_CHECK`）| 这些臂让**宿主**读网格，而交出去的缓冲在跳末之前没人提交 ⇒ 宿主读到没人写过的内存 |
| **`phy_pipeline_strict_enabled()`**（`mode=gpu`，或 `OCUDU_GPU_STRICT=1`）| 设备服务不了的跳在 `gpu` 下**失败**（响），而不是被宿主悄悄兜底 |

**⇒ 缺任何一条 ⇒ 回到"提交 + 等待"**（`cpu_gpu`、以及**不发布模式的离线回放**都拿不到这条路径；
`OCUDU_GPU_STRICT=1` 是离线覆盖口）。

**认领是每条路线都做的**（不只融合路线）：缓冲里装着接收链的变换，**任何**路线不认领 = 那份网格永远没人写。
区别只在**谁提交**：`merged`/`burst` 由车道提交（D1 要的那一次），其余由提取自己的 `end_stage()` 提交并等待。

### 2.3 已验（离线）

| 判据 | 结果 |
|---|---|
| `value_net` / `ctest -R metal` / `neutral_vs_baseline` | **47/0**、**10/10**、**131（同数）** |
| **机制单测（走真入口）** | `release_block(grid_base)` → `take_released(grid_base)` → `adopt()`：**shared 20/20 读到网格**、**private 20/20 读到写前内容（陷阱复现）**、`handed=40 taken=40 dropped=0` |
| **★ 旋钮武装 + 无 deposit 的回放 A/B** | 网格捕获模式不跑 DFT ⇒ 上层认领**每跳都走到、空手而归**，5 个 dump **逐字节相同** |
| **守卫负例** | `ofdm_demodulator_metal_batch_test`（**宿主读网格**的配置）武装 + strict：**ALL OK**，`handed=0 (armed=1)` |

### 2.4 ⚠ 未验（**这就是下一步**）

1. **"DFT 交出 + 车道认领"这条组合离线跑不了**：`ul_chain_replay --dft` 写完网格就 `return 0`（不进上层链），
   而**本机没有 TD 语料**；网格捕获模式又根本不跑 DFT。⇒ **只能空口腿判。**
2. 跨线程时序（deposit 早于 take）依赖**已有的**网格交接 happens-before——**同一条假设前端栅栏路线已经在用**，
   但本步没有单独证过。
3. 真机上的提交数与延迟（§4.4）。

**残余风险（写下来了，别靠记忆）**：设备服务不了的一跳（现实来源：**稀疏 RB 分配**的 `ls_geometry`）
在 strict 下会**失败**，但宿主的 LS 预级**仍会先读一次网格** ⇒ 读到未写内容；因为那次 grant 随后失败、结果被丢弃，
**不会变成错的 LLR**。非 strict 时不会发生（守卫直接拒绝交出）。

---

## 3. ★ 用户的裁定（**长期有效，原样带过来**）

| # | 裁定 |
|---|---|
| ① | `mode=gpu` 不允许宿主兜底 |
| ② | dump 不算 CPU in the loop |
| ③ | 控制面融合分阶段做、**每阶段一次 OTA**（腿通过就 `git tag -a` 并推送，不过就回滚到上一个 tag）|
| ④ | **`merged` 保留为默认** |
| ⑤ | 顺序：~~K1 路 A~~（做完）→ **DFT/D1**（第 1、2 步接线已做完，**差空口腿**）|
| ⑥ | **排序标准是"CPU 还剩几个参与点"（个数），不是耗时**；A（提交数）优先于 B（宿主空等）|
| ⑦ | **"GPU 一旦发动，就能不停顿地跑完 IQ → LLR"**；中途若还需要 CPU，那一点就算参与点 |

---

## 4. ★★★ 下一步：**空口腿（一次 OTA）**

### 4.1 腿法：**两臂都必须是 `gpu` 模式**（旋钮作为**位置参数**传）

```bash
# 对照（gpu 模式，默认：槽末提交 + 宿主等待）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s32-d1-step2-base
# 候选（交出 + 认领）
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s32-d1-step2 OCUDU_DFT_RELEASE_BLOCK=1
```

**⇒ 一共 2 次腿（一个腿对），同一个二进制、同一次部署，唯一自由变量是 `OCUDU_DFT_RELEASE_BLOCK`。**

#### ⚠⚠ 为什么候选臂**不能**是 `cpu_gpu`（这一条是本交接文件原先写错、已更正的地方）

守卫（§2.2）要求 **`phy_pipeline_strict_enabled()`**，而它读的是**已发布的流水线模式**：

| 模式 | `strict` | 结果 |
|---|---|---|
| `cpu` | ✗ | `cpu` 下 **Metal DFT 根本不存在**，交出对象不存在 |
| **`cpu_gpu`** | **✗** | **守卫直接拒绝 ⇒ `handed=0` ⇒ 这一腿什么都没判**（而且看起来"跑完了"）|
| **`gpu`** | **✓** | 守卫通过（`device_resource_grid on` ⇒ `grid_consumed_on_device` ⇒ `wait_per_slot` 也通过）|

**⇒ `cpu_gpu` + 旋钮 = 白跑一条腿**（这正是本线反复踩的"判据没触发却看起来正常"）。
（`cpu_gpu` + `OCUDU_GPU_STRICT=1` 技术上能让守卫通过，但它**同时打开了严格策略**——
设备服务不了的跳从"宿主兜底"变成"grant 失败"——**那就是第二个自由变量**，腿对的结论会没法归因。不用它。）

#### ✅ `gpu` 模式是现成的、而且已经验证过

最近一条 `gpu` 腿 `gnb_gpu_s14p3-merged_0920_1847`（commit `a55742c5f8`，就是 P3 翻默认那次）：

| 读数 | 值 |
|---|---|
| 契约 | **8 of 8 checks applicable → MET**（`mode=gpu`）|
| `ce device estimates` | **166122 device, 0 host** → host=0 ✅ |
| `host device data crossings` | **0 read + 0 write / 15102 hop** ✅ |
| `[ul_gpu_lane]` | `lanes=15102 **cbs/lane=1.00 (max=1)**` ✅ |
| **`[metal_stats] dft commits`** | **27668 / 15102 跳 = 1.83 次/跳** ← **这就是 D1 要消掉的那 1.83** |
| `[metal_stats] dft transforms` | 387339（14 变换/提交 ⇒ 块批处理在跑）|
| CRC | 14124 OK / 978 KO |

**⇒ 它同时是"对照臂长什么样"的参照，也是"判据会触发"的证明**：候选臂上 `dft commits` 应当**塌下来**（只剩 warm-up 等极少数），
而 `dft handover` 行应当出现 `handed ≈ 跳数`。

### 4.2 判读（**先看仪表有没有触发，再看数字**）

| 看什么 | 对照臂（`gpu`） | 候选臂（`gpu` + 旋钮）| 说明 |
|---|---|---|---|
| **`[dft_release]` 横幅** | 无 | **有** | 旋钮真被引擎看到（`grep dft_release <log>`）|
| **`[metal_stats] dft handover ... (armed=)`** | **无这一行** | **必须有，且 `armed=1`** | 没有这一行 ⇒ 旋钮没进去 |
| **`handed`** | —— | **> 0** | **`handed=0` 是发现**（守卫拒绝？没有槽走到末符号？），**不是"没接线"** |
| **`taken`** | —— | **> 0，且与"跳数"同量级** | 认领真的发生了 |
| **`superseded`** | —— | **允许非 0** | **无害且预期**：同一个地址又被 deposit（网格经池子回来了）⇒ 说明那个槽**没有跳**、那份网格没人读。**参照数字见下** |
| **`evicted`** | —— | **必须 0** | 超过 8 条未认领 ⇒ **积压**（消费者落后于生产者）⇒ 这才是可疑的那一半 |
| **`outstanding`** | —— | 小（≤ 在飞的槽数）| 退出时还没人认领的 |
| **`[metal_stats] dft commits`** | **1.83/跳**（≈27668）| **塌下来**（≈0，只剩 warm-up）| 这就是本步的目标本身 |
| `[metal_stats] dft waits` | ≈27668 | 大幅下降 | 宿主不再等 |
| **`[metal_stats] dft ... released_waits`** | 0 | **必须 0** | 非 0 ⇒ 宿主仍在等交出去的槽位（接线漏了一处）|
| **`[ul_gpu_lane] cbs/lane`** | **1.00** | **仍 1.00** | 一跳一条缓冲 |
| **契约**（8/8）| MET | **仍 MET**，尤其 `ce device estimates` **host=0**、`host device data crossings` 0 | 没有宿主消费者漏网 |
| **CRC（按调制分层比）** | 14124/978 量级 | **不劣化** | **最重要的一条** |
| `Real-time failures` | 1 / 71788 slots | 不劣化 | **判读前先看它** |

#### ★ 关于 `superseded` 为什么"允许非 0"（**别把它当成失败**）

参照腿里 **DFT 块 ≈ 27667，跳只 ≈ 15102（1.83 块/跳）**——**块比跳多**。
多出来的那些块的网格**没有跳去读**（那一槽没有成功的上行接收），它们经网格池被下一个槽复用 ⇒ **同一个地址再次 deposit ⇒ `superseded++`**。

**这是可以证明无害的**：网格池只有在**持有者放手之后**才会把地址还回来，而持有者正是那个（还没跑的）跳——
所以"地址回来了"本身就说明**那个槽没有消费者**，那份网格没人读。
**⇒ 判据不是"superseded==0"，而是 `evicted==0` + `released_waits==0` + 契约 host=0 + CRC 不劣化。**
（`superseded` 与 `evicted` 分成两个数就是为了这个：一个数是预期结果，另一个是缺陷，混在一起就没法读。）

### 4.3 判读入口

```bash
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh <log>
grep -E "dft_release|handover|dft commits|released_waits|cbs/lane" <log>
```

### 4.4 开工前要确认的事

0. **模式 = `gpu`**（理由与证据见 §4.1；**不要用 `cpu_gpu`**）；
1. **上腿前关掉手机 WiFi**；**先跑对照臂**，空臂宽度 ≥ 要判的效应 ⇒ 该次读数作废；
2. **只报比值/份额，不报绝对 µs**；`last_gpu_wait_us()` 在单发下是队列跨度不是 kernel 时间；
3. **臂的顺序**：先对照后候选（同一二进制；两次之间不要改任何东西，包括 config）。

### 4.5 之后（D1 第 3 步）

| 步 | 做什么 | 判据 |
|---|---|---|
| **3** | **`wait_all()` 与栅栏归属跟进** | 退出无悬挂；CRC 不劣化 |
| 3a | 交出去的块**不发前端栅栏代际**（§5.9.4 ⑤-1）：代际还要不要推进、`front_end_wait()` 会不会等一个没人 signal 的代际 | 结构性 |
| 3b | `wait_all()` drain 的是**前端链**，交出去的块在后端队列由车道提交（§5.9.4 ⑤-2）| 退出时无悬挂 |

---

## 5. ★★ 本会话踩过的坑

### 5.1 仪表"没接线"和"接线了但没触发"长得一模一样

`[metal_stats] dft handover` 最早写在 `ocudu_metal_burst.mm` 的 atexit 里——
**那个目标文件在离线二进制里根本没被链进来**（没人引用 `shared_burst` 的符号就不会被拉进静态库），
于是"武装了旋钮、但守卫拒绝"的运行**什么都不打**，看起来和"旋钮没设"完全一样。
**⇒ 改成由 DFT 引擎（只要能有 Metal DFT 就一定在链里）打印，并且"武装了就一定打这一行"。**
**教训（与 §5.1 的四条教训同族）：诊断的【存在性】本身要有判据——否则它只会告诉你它自己没跑。**

### 5.2 一个"看起来更激进"的选择反而更安全

"只在融合路线认领"读起来更保守，实际是**错的**：非融合路线不认领 ⇒ deposit 没人提交 ⇒ **网格永远没人写**。
**⇒ 每条路线都认领，区别只交给既有的"谁提交"机制。**
**教训：把"更保守"当成"更安全"之前，先问一句"那么谁来做那件被我省掉的事"。**

### 5.3 `ul_pipeline_probe_test.one_report_shape_per_pipeline_mode` 已知偶发

`ctest -R "ul_pipeline_probe|puxch|lower_phy"` 里它**偶发失败**（S18 前半观测到 2 次，此后 40+ 次通过）；
**该二进制不链接任何 Metal 库**（`link.txt` 里 metal 出现 0 次），且它的断言是 `sleep_for(7ms)` 后
要求 `mean_us ≈ 7000 ± 1000`——**时序脆，不是回归**。碰到时把它改成对**记录值**的断言。

---

## 6. 工具、旋钮与仪表

| 名字 | 说明 |
|---|---|
| **`OCUDU_DFT_RELEASE_BLOCK=1`** | **交出路径的旋钮（默认关）**。武装时：块建在**后端队列**、网格走**进程级 wrap**、`release_block()` 才肯交出。**还要过 §2.2 的三条守卫** |
| **`[metal_stats] dft handover handed= taken= superseded= evicted= outstanding= (armed=)`** | **新增**。**武装了就一定打这一行**；`evicted` 必须 0（`superseded` 允许非 0，见 §4.2）|
| **`OCUDU_GPU_STRICT=1`** | 离线/腿上的 strict 覆盖口（离线回放**故意不发布模式**，没它就永远判不了融合路线）|
| **`[metal_stats] dft ... released= released_waits=`** | 交出的块数 / **给不出的等待**（后者必须 0）|
| `OCUDU_DFT_OPEN_BLOCK=0` | DFT"每槽 14 变换共用一条缓冲"关掉（默认开）|
| `OCUDU_CE_LANE_ORDER` | `merged`（默认）/ `event` / `burst` / `host_wait` |
| **`wip/neutral_vs_baseline.sh`** | **信息网**：逐字节对比归档基线（235 dump，走真的 Metal DFT 路）。**它比 `value_net` 更能证明"出厂路径没动"** |
| `wip/value_net.py` / `wip/run_leg.sh` / `wip/leg_report.sh` | 门 / 跑腿 / 单腿判读 |

**★ 上腿前的三条自查（长期有效）**：
1. **模式对吗**（D1 的腿**必须 `gpu`**：`cpu` 下 Metal DFT 不存在，`cpu_gpu` 下守卫拒绝交出；见 §4.1）；
2. **判据会触发吗**（`[ul_dft_wait]` 有样本？`handover` 行在？`handed>0`？）；
3. **开关武装了吗**（依赖 `OCUDU_UL_FRONTEND_FENCE` 的臂必须设 1）。

---

## 7. 本会话的腿

**没有空口腿**——第 2 步把能离线验的都验了（门 + 机制单测 + 逐字节网 + 武装/无 deposit 的 A/B + 守卫负例），
**剩下的一条只能上腿**（§2.4-1）。

---

## 8. 未解 / 开放项（**按建议顺序**）

1. **★ 空口腿**（§4）——**判"交出 + 认领"这条组合**；
2. **D1 第 3 步**（§4.5：前端栅栏代际归属、`wait_all()`）；
3. **给离线补一个能跑完整 D1 路径的 harness**：`ul_chain_replay --dft` 目前 `return 0` 不进上层链，
   而且**本机没有 TD 语料**（`*_td.txt` 一个都没有）——补上它，以后每一步都能离线判；
4. `shared_queue::wrap_no_copy()` 非零偏移被 `wrap_shared()` 丢掉（静默错址，9 个调用点）；
5. §5.8.12 那条旧读数需要重读（栅栏默认没武装 ⇒ 很可能是假阴性）；
6. `merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口；
7. `merged` 的 +471 µs 里 ~340 µs 排队与 64QAM CRC 变差的单变量隔离未做；
8. §18.7 的 TA fence 实验仍未做；TA 链 = 80.1 µs；
9. K1 那条"每块列收尾 barrier"的 6% 理论移除：已推理为安全但**未上腿**；
10. `ul_pipeline_probe_test` 的时序脆断言（§5.3）。

---

## 9. 环境与流程

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
* **★ 新增测试/文件之后要 `cmake -S . -B build`**（否则 `--target <新目标>` 会说没有这个目标）；
* **★ 改 `.metal` 后的正确姿势**（本会话没改 .metal）：
  ```bash
  rm -f build/lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse_inv.air{,.d} lib/.../ocudu_mmse.metallib
  cmake --build build --target ocudu_mmse_metallib -j 10
  xcrun metal-nm lib/…/ocudu_mmse.metallib | grep mmse_inv     # 验证 kernel 真的进去了
  ```
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  然后 `sudo -E bash …/run_leg.sh <mode> <label> [OCUDU_*=…]`——**sudo 由用户执行**（把命令以纯文本贴给用户）；
  **Ctrl+C 停**；判读 `bash …/leg_report.sh <log>`；
* **★ 上腿之前关掉手机 WiFi**；**判读前先看 `Real-time failures` 是否为 0**；**CRC 按调制分层比**；
* **★ 离线绝对 µs 不可信**（宿主 GUI 抢 GPU）：**先跑空臂**；**只报比值/份额**；
  **判 ≥40 µs 的效应只能靠空口腿对**。

---

## 10. 一句话给新会话

**D1 第 1、2 步都接线完成、默认关闭、逐字节零影响（131 = 同数），
"DFT 交出 + 车道认领"这条组合已经能从代码一路读到仪表——但它离线跑不了，只能空口腿判。**
**下一步：跑一个 `gpu` 模式的腿对（对照 = `gpu`，候选 = `gpu` + `OCUDU_DFT_RELEASE_BLOCK=1`；
⚠ **不是 `cpu_gpu`**，`cpu_gpu` 下守卫拒绝、`handed=0`，这一腿什么都判不了）。
先看 `handover` 那一行有没有 `armed=1`、`handed>0`、`taken>0`、**`evicted==0`**（`superseded` 允许非 0），
再看 `dft commits` 是否从 1.83/跳塌下来，最后比 CRC。**
