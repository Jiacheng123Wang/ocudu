# 交接（入口） — S23：**`s39`：MISS 等待把延迟治好了（中位 4797→1962 µs）、认领率 59%→77%、数据不比对照差；代价是一次 13 s 停顿（355 个上行时隙被丢）⇒ 下一步：把"宿主等"换成"GPU 等"**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **本会话（S23）的就一件事**：读 `s39` 腿对 + 把 MISS 路径的等待从**宿主**搬到**设备**（见 §3，未做，只有方案与判据）。设计文档 **§5.9.23** 是完整读数。
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

## 3. ★★★ 下一步：**把 MISS 的等待从宿主搬到 GPU**（`s39` 的停顿就是宿主等待造成的）

**`s39` 判了**（设计文档 §5.9.23）：开工告警 0、`grid_shared=14474==hops`、**`grid_miss_waited=3387` 精确等于
"跳数−`taken`"**、**`grid_miss_timeouts=0`**；端到端中位 **4797→1962 µs**（+2.8 ms 消失）、认领率 **59%→77%**、
扫掠 7079→4520、提交/跳 2.76→**2.10**；按调制分层 CRC **不比对照差**（16QAM 99.97%、64QAM 63%、256QAM 44%）。

**代价（新事故）**：06:11:07–06:11:20 **一次 13 s 停顿**——`UL processor is busy` **355** 条（对照 21）、
实时失败 356（对照 25）、最坏端到端 162 ms；丢的时隙**是连续的**（160.2/160.3/161.2/161.3…）⇒
不是单个处理器忙，是**整链停顿后 FAPI 成批迟到**。
**机制假设**：一个 MISS 的跳在 `grid_ready_hook::wait()` 里**占着车道（PUSCH 池）线程**，而它等的块由**另一个车道阶段**提交
⇒ 池子被等待者占满时没人能推进到"提交"⇒ 级联。（s38 无等待 9 ms/27；s39 有等待 13 s/355。）

**修法（照 §5.9.23 ⑤ 的四步做，都在两个文件里）**

| 步 | 内容 |
|---|---|
| 1 | `shared_burst::grid_production_generation(storage, slot)`：返回该 (存储,槽) 的生产代际；未认领先兜底提交（照 `ensure_grid_produced()`），已认领返回它的 `generation`，无记录返回 0 |
| 2 | `shared_queue::grid_ready_encode_wait(cb, generation)`：`[cb encodeWaitForEvent:s.grid_event value:generation]`（0 空操作；同文件已有 `fence_event`/`stage_fence_event` 两处同样写法）|
| 3 | MISS 时记 `e->pending_grid_wait`，**下一个 `begin_stage()` 打开缓冲后第一件事**编码该等待再清掉 |
| 4 | 计数 `grid_miss_devwaited`；**删掉宿主等待**（`grid_miss_waited` 那一段）|

**判据（下一对腿 `s40`）**：`grid_miss_devwaited > 0`、**`UL processor is busy` 回到 ~20**、
`[ul_pipeline]` 中位仍 ~2 ms 且**最坏值回到 ~10 ms**、CRC 按调制分层不劣化、`timeouts==0`。
**这条也更符合裁定 ⑦**（GPU 一旦发动就跑完，CPU 不参与"等"）。

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

**`s39` 判了：MISS 等待是对的**（`grid_miss_waited` 精确、`timeouts=0`、端到端中位回到 1962 µs、
认领率 59%→77%、提交/跳 2.76→2.10、按调制分层的 CRC 不比对照差），
**但"用宿主线程等"制造了一次 13 s 停顿**（355 个上行时隙被丢、`UL processor is busy` 355 vs 21）——
一个等待者占着车道线程，而它等的块由另一个车道阶段提交。
**下一步只有一件事：把这次等待编码进 GPU**（`encodeWaitForEvent` on `shared_queue::grid_event`，四步见 §3），
判据是 `UL processor is busy` 回到 ~20 而中位延迟不变。**在它判过之前不要 tag。**
