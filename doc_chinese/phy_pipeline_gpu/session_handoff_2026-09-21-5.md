# 交接（入口） — S21：**D1 的拦路石不是"网格每槽复用"，是【网格的 MTLBuffer 对象在跳缓冲里不唯一】**（已修，待腿）

> **本文件是新会话的唯一入口**：读完它就能开工。
> **⚠ 本文件【撤回】`session_handoff_2026-09-21-4.md` 的 §4.1 第 14/15 行**（那份说"网格是一个对象、每槽复用"，
> 并据此把"给槽自己的网格"定为下一步——**证据读错了，不是修法**）。`-4.md` 保留不删。
> **技术细节全在常驻设计文档**：本会话的是 **§5.9.19（已撤回 ①③④）** 与 **§5.9.20（★ 真障碍 + 修法 + 腿判据）**。

---

## 0. 一句话状态

**工作树 HEAD = （见下方 `git log -1`）**，改动：`ocudu_metal_mmse_engine.mm` 一个文件（+自检计数）。
**开机第一件事仍是自查戳记**：
```bash
git rev-parse --short=10 HEAD && grep build_info build/hashes.h
```
**两个短哈希必须相同**（不同就 `touch build/hashes.h && cmake --build build --target gnb`）。
工作树只剩**用户自己的两个 config**，**别动**。**提交全部只在本地，没有推送。**

**门（改后复跑，全绿）**：`value_net` **47/0**、`--self-test` **8/8**、`ctest -R metal` **10/10**、
`ctest -R "ul_pipeline_probe|puxch|lower_phy"` **5/5**、`neutral_vs_baseline.sh` **131 differing-bytes**（同数）。

---

## 1. ★★ 本会话的结论（代码，逐条可查）

| 事实 | 出处 |
|---|---|
| 网格**不是**每槽复用的一个对象：每个 `uplink_processor_impl` 一个，每小区 **`nof_ul_rg`(=20) 个**处理器轮转 | `upper_phy_factories.cpp:1019`、`processor_pool_helpers.h:43-56` ⇒ 复用周期 **~20 次 UL 请求（10 ms）** |
| 前端写网格：**武装后走进程级缓存**（消费者用的那一个）| `ocudu_dft_metal_engine.mm:700-707` `wrap_grid()` → `shared_queue::wrap_no_copy()` |
| 均衡器读网格：**同一个进程级缓存** ✅ | `ocudu_equalizer_metal_engine.mm:432-441` |
| **信道估计器读网格：引擎私有缓存** ❌ | `ocudu_metal_mmse_engine.mm` `build_pilots_lse()` 的 `e->wrap(s.grid, s.grid_bytes)` |
| 该提取 dispatch 就编在**被认领的那条缓冲里**（前端写网格的 dispatch 之后）| `begin_stage_on_handed()` + `[enc setBuffer:grid_buf.buf …]` |

**⇒ Metal 只按 `MTLBuffer` 对象关联两次访问**：同对象才自动插屏障，**两个对象盖同一段内存 ⇒ 它们之间没有任何顺序**
（本会话早先用 `wip/metal_alias_order.mm` 量过：同对象 200/200 PASS、别名对 200/200 FAIL，两个方向都如此）。
**⇒ 于是估计器的 DMRS 提取会读到"前端还没写"的网格 —— 上一帧同一地址的内容。**

**它一次解释掉 `s36` 候选臂的全部读数**：DMRS 读旧 ⇒ **信道估计错但自洽** ⇒ SINR 看着好（20–38 dB）而 CRC 全错；
旧内容为空处 ⇒ `sinr=inf`；半写 ⇒ −20…−66 dB；同一 HARQ 重传时而好（`rv=0` 连发 KO/KO/OK）⇒ 纯竞态。
**PUCCH 的 −14 dB 不是独立缺陷**：那是 UE 没接上 ⇒ 多为 DTX。
**交出机制本身是好的**（`handed=3934 taken=847 fallback=2767 late=320 not_found=318 timeouts=0 keepalives=55076/55076`）。

**⇒ 为什么两条离线判据都没看见它**：机制单测**恰好量到了这条陷阱**，但那条臂是**负对照**
（`dft_release_adopt_metal_test` 第 2 条臂 `private`：判词原话 *"the reader must read the PRE-WRITE content instead"*
——**生产里估计器就是那第 2 条臂**）；`ofdm_demodulator_metal_batch_test` 的武装节用**宿主**读网格（有栅栏）⇒ 0/17808。

---

## 2. ★ 改动（本会话已做，待腿判）

| 改动 | 位置 |
|---|---|
| 新增 `mmse_engine_impl::wrap_grid()`：网格的读映射走**进程级缓存并保留 offset**（不能走 `wrap_shared()`——它会丢掉非零 offset，绑错地址）| `ocudu_metal_mmse_engine.mm` |
| `build_pilots_lse()` 的网格映射改用它（一行 + 注释说明"别人的输入"必须共用对象）| 同文件 |
| 新增计数 `grid_shared=` / `grid_failed=`，接在 `[metal_stats] mmse_ce` 行尾 | 同文件 |

**⚠ 诚实说明**：这个**修法本身没有新增离线门**，直接判据就是下面的 `grid_shared == hops`（空口腿读它）。
机制那一层（别名对无序）早已被机制单测的第 1/2 条臂离线判过。

---

## 3. ★★★ 下一步：**上腿对**（`gpu` 模式；交出已武装）

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s37-d1-gridobj-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s37-d1-gridobj OCUDU_DFT_RELEASE_BLOCK=1
```

**判读（按顺序）**

| 先看 | 期望 |
|---|---|
| **`grid_shared == hops`**（`[metal_stats] mmse_ce … grid_shared=NNN grid_failed=0`，与 `calls=` 同量级）| **硬判据**：每一次跳的网格读都绑在生产者写过的那一个对象上 |
| `crc=OK/KO`（**按调制分层**）| 候选臂**不再劣化**（这是上次唯一真正抓住问题的读数）|
| `sinr` 分布 | 回到对照臂形状（**不再有"高 sinr + CRC 全错"**）|
| UE | 能接入并跑 ping/iperf3（本配置是 n1 5 MHz FDD bridge；手机 WiFi 关掉）|
| `[metal_stats] dft handover` | `handed>0`、`taken>0`、**`timeouts==0`**、`keepalives` 两侧相等 |
| `Real-time failures` / `RF late` | 不劣化（先看这一条）|

**★ 若候选臂仍坏**：先比 `grid_shared`——若它 `== hops` 而数据仍错，则"对象唯一性"这条已排除，
下一个嫌疑是**前端那条写**本身（`wrap_grid()` 在武装时与均衡器的映射是否同一对象，可用 `OCUDU_CE_WRAP_MAP=1` 打出来比对）。

---

## 4. 未解 / 开放项（按建议顺序）

1. **上腿对判 `grid_shared`**（第 3 节）；
2. 前端栅栏代际与被交出块的归属（§5.9.4 ⑤-1）；`wait_all()` 不覆盖交出去的块（§5.9.4 ⑤-2）；
3. **给离线补一个能跑完整 D1 路径的 harness**（`ul_chain_replay --dft` 目前 `return 0`；本机没有 TD 语料）；
4. `wrap_shared()` 丢非零 offset（9 个调用点，静默错址）；
5. `merged` 下 `burst dispatches` 的 `channel_estimator=0` 是纯计数缺口；
6. `ul_pipeline_probe_test.one_report_shape_per_pipeline_mode` 的时序脆断言（~1/10）；
7. §18.7 的 TA fence 实验仍未做。

## 5. 环境与流程（不变）

* 构建：`-DENABLE_METAL_STATS=ON -DENABLE_FLOW_PROBES=ON -DENABLE_CE_TIME=ON -DENABLE_UL_CAPTURE=ON`；
  **新增文件/目标后要 `cmake -S . -B build`**；
* **腿**：`touch build/hashes.h && cmake --build build --target gnb`（戳记必须 == HEAD），
  `sudo -E bash …/run_leg.sh <mode> <label> [OCUDU_*=…]`——**sudo 由用户执行**（把命令以纯文本贴给用户）；
  **Ctrl+C 停**；判读 `bash …/leg_report.sh <log>`；
* **★ 上腿之前关掉手机 WiFi**；**判读前先看 `Real-time failures`**；**CRC 按调制分层比**；
* **★ 离线绝对 µs 不可信**；只报比值/份额。

---

## 6. 一句话给新会话

**D1 的拦路石是"一跳一条命令缓冲"里 `MTLBuffer` 对象的唯一性被破坏：估计器读网格绑的是自己的私有对象，
而前端写的是进程级共享对象 ⇒ 两者之间没有任何顺序 ⇒ DMRS 提取读到上一帧的网格（"高 SINR + CRC 全错"就是这个签名）。
修法是一行 + 一个新计数；判据是空口腿的 `grid_shared == hops` 与 CRC 分层。**
