# 交接（入口） — S24：**"GPU 等"已实现**（MISS 的次序编码进跳自己的命令缓冲，CPU 不再阻塞）——**下一步就是那一对腿（`s40`）**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **本会话（S24）的就一件事**：把 `s39` 那次 13 s 停顿的根因（**用宿主线程等**）改掉——MISS 的次序改成
> **命令缓冲级的 `encodeWaitForEvent`**（CPU 立刻返回，GPU 自己等）。设计文档 **§5.9.24** = 改动 + 判据；**§5.9.23** = `s39` 读数。
> **`s37` 那一对腿不算数**（候选臂 `handed=0 released=0`，交出在代码里还关着）；`-4.md` 的 §4.1 第 14/15 行已被 §5.9.20 撤回。

> **技术细节全在常驻设计文档**：§5.9.19（已撤回 ①③④）、§5.9.20、§5.9.21、**§5.9.22（`s38` 读数 + MISS 修复）**。

---

## 0. 一句话状态

**工作树 HEAD = `（见 `git log -1`）`**，改动：`ocudu_metal_mmse_engine.mm`（`wrap_grid()` + MISS 路径的 `grid_ready_hook::wait` + 四个计数）、`ofdm_demodulator_impl.cpp`（重新武装 + 开工告警）、设计文档 §5.9.20–§5.9.22、本文件。
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

## 3. ★★★ 下一步：上 `s40` 腿对（判"GPU 等"是否既保住次序、又消掉停顿）

**已实现（本会话，见设计文档 §5.9.24）**：
`shared_queue::grid_ready_encode_wait()`（设备侧等待，`generation==0`/无事件 ⇒ 不编码，绝不挂死缓冲）、
`shared_burst::grid_production_generation()`（与 `ensure_grid_produced()` 共用 `claim_grid_production()`，
未认领先兜底提交）、`shared_burst::set_grid_wait()` + `burst_ensure_open()` 里与另两个 fence 同处消费、
`begin_stage_on_handed()` 的 MISS 只记 `pending_grid_wait`、`begin_stage()` 在**开编码器之前**编码它。
**宿主等待已删除**；计数换成 `grid_devwaited` / `grid_wait_unencoded`。

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s40-d1-devwait-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s40-d1-devwait OCUDU_DFT_RELEASE_BLOCK=1
```

**★ 上腿后第一件事**：`grep -c "will NOT exercise D1" <候选臂日志>` 必须是 0。

| 先看 | 期望 | `s39` 实测（对照）|
|---|---|---|
| 开工告警 | 0 | 0 ✓ |
| `grid_shared == hops` / `grid_failed==0` | 成立 | 14474 / 0 ✓ |
| **`grid_devwaited`** | **> 0**（≈ 跳数 − `taken`）| （当时是宿主等待：3387）|
| **`grid_wait_unencoded`** | **== 0** | — |
| `dft handover` | `handed>0 taken>0 timeouts==0` | 27022/11087/0 ✓ |
| **`UL processor is busy`** | **回到 ~20** | **355** ❌ |
| `[ul_pipeline]` 中位 / **最坏** | ~2 ms / **~10 ms** | 1962 µs / **162 ms** ❌ |
| CRC 按调制分层 + KO 的 sinr 中位 | 与对照同形 | 97.1% OK、无异常 ✓ |

**★ 若 `grid_wait_unencoded` 非 0**：有一条跳的网格读没有任何次序 —— 那是一条真的缺陷，先别继续加功能。

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

**`s39` 把"MISS 要等"这件事判对了（延迟中位回到 1962 µs、认领率 77%、提交/跳 2.10），但"用宿主线程等"制造了一次 13 s 停顿。**
本会话把这次等待**搬进了 GPU**：MISS 时取"那块的生产代际"，把它作为 `encodeWaitForEvent` 编进**跳自己命令缓冲的第一件事**
（CPU 立刻返回），未认领的块仍由取代际的那一步兜底提交。**判据是 `s40`：`grid_devwaited>0`、`grid_wait_unencoded==0`、
`UL processor is busy` 回到 ~20、最坏端到端回到 ~10 ms，而中位延迟与 CRC 分层不变。** 在它判过之前不要 tag。
