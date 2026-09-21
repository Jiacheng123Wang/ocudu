# 交接（入口） — S26：**`s40` 全过（⑥ 距地板 3%）；⑦ 剩的那一档已按"放宽处理器复用周期"改好（一行），等 `s41` 腿判**

> **本文件是新会话的唯一入口**：读完它就能开工。
> **本会话（S25）的就一件事**：读 `s40` + 更正上一轮对停顿的误判。设计文档 **§5.9.25** = 完整读数与机制。
> 一句话：**"GPU 等"全过**（`grid_devwaited=843`、`grid_wait_unencoded=0`、中位 1851 µs、94% 跳认领、最坏 162→51 ms），
> 但 `UL processor is busy` 是**话务爬到饱和**那一档的事，**对照臂也有（84 vs 武装 716）**。
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

## 3. ★★★ 下一步：上 `s41` 腿对（判"放宽上行处理器复用周期"是否消掉饱和爬坡的丢时隙）

**本会话已实现（设计文档 §5.9.26）**：`du_low_config_translator.cpp` 里
`ul_pipeline_depth = nof_slots_per_frame` → **`3 * nof_slots_per_frame`**（复用周期 10 ms → 30 ms）。
理由：`s40` 的 716 次 "UL processor is busy"（对照 84）发生在**话务爬到饱和**那 11–14 秒，
是"该处理器上一个槽的任务还在飞"；⚠ 同时更正了上一轮的一个推断——`Failed to allocate UL resource grid = 0`
**不能**排除网格引用被占（翻译器先打 "busy" 就 `return` 了）；判 (b) 靠的是反向证据：**对照臂更晚放开网格却拒收得少**。
代价：每处理器一份网格 + payload 池（25 PRB ~34 KB / 51 PRB ~280 KB）；**每时隙提交数不变**。

```bash
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s41-d1-ulsupply-base
sudo -E bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu s41-d1-ulsupply OCUDU_DFT_RELEASE_BLOCK=1
```

**★ 上腿后第一件事**：`grep -c "will NOT exercise D1" <候选臂日志>` 必须是 0。

| 判据 | 期望 | `s40` 实测 |
|---|---|---|
| **`UL processor is busy`** | **回到对照量级（~84 或更低）** | 716（对照 84）❌ |
| `[ul_pipeline]` 中位 / 最坏 | ~1.9 ms / ≤ ~50 ms | 1851 µs / 50.9 ms ✓ |
| 提交 / 上行时隙 | ~**1.03**（不因处理器变多而变）| 1.03 ✓ |
| `grid_devwaited>0`、`grid_wait_unencoded==0`、`timeouts==0` | 成立 | 843 / 0 / 0 ✓ |
| 开工告警 | 0 | 0 ✓ |
| CRC 按调制分层 + KO 的 sinr 中位 | 与对照同形 | 91.5% OK ✓ |

**★ 若丢时隙不降**：归因错，下一件是**结构性**的——给 `uplink_processor_impl` 按槽拥有 PDU 仓库与网格（§5.9.19 那个改动，
理由从"正确性"改成"余量"）。**⇒ 那就先回滚这一行再动结构。**

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

**`s40` 全过**：设备侧等待（`grid_devwaited=843`、`grid_wait_unencoded=0`）、中位 1851 µs、94% 跳认领、
最坏 162→51 ms、CRC 91.5%（对照 84.6%）；**⑥ 提交/上行时隙 1.50 → 1.03（地板 1.00，距地板 3%）**。
**⑦ 只剩饱和爬坡那一档**：`UL processor is busy` 716（对照 84），两个臂都发生在"话务爬到饱和"的 11–14 秒。
**本会话把它改成一行**：上行处理器复用周期 1 帧 → **3 帧**（`nof_ul_rg`）。**`s41` 判它**：
"UL processor is busy" 是否回到 ~84，而提交/时隙仍 ~1.03。若判不过，就回滚这一行，转做**按槽拥有的仓库/网格**那条结构路。
