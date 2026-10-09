
---

## 14. ★★★ S-1 已实作并验证：**纯旁路，逐位不变**（2026-10-08）

> ★ 范围与安全原则见 §13。★ 本节记 S-1 的**实作、验证、以及它抓到的两个真陷阱**。

### 14.1 ★★★ 交付物

| ★ 件 | ★ 内容 |
|---|---|
| ★★ **`pusch_depth3_receiver.h`** | ★★ **深度 3 的接缝接口**：`estimate()` + `demodulate()` 两个入口，★ 以及 `kind()` / `is_identity()` |
| ★★ **`pusch_depth3_receiver_impl.{h,cpp}`** | ★★ **两个实现**：`_classic` 与 **`_ai_identity`**（★ 同一条委派，★ 不同的 `kind()`）|
| ★ **`pusch_processor_impl.{h,cpp}`** | ★ 两处接缝调用改走 `get_depth3()`；★ 新增 `set_depth3()` 与 `configure()` 后的 `get_dependencies()` |
| ★ **`processor_factories.cpp`** | ★ 按 `pusch_receiver_backend` **构造臂**；★ AI 臂**构造时打印** S-1 身份声明 |
| ★★ **`ul_chain_replay`** | ★★ 新增 **`OCUDU_REPLAY_RECEIVER=ai`** ⇒ ★ **离线 A/B 变成可能** |
| ★ **`upper_phy_factories.cpp`** | ★ 闸门接受 `classic` \| `ai`；★ 其余取值仍 fatal |

### 14.2 ★★★ 验证结果（★ 核心判据达成）

★★ 同一捕获、同一构建、**只差一个环境变量**，★ 比较**四件产物**：

| ★ 捕获 | ★ `.bin` | ★ `_h.bin` | ★ `_llr.bin` | ★ `_ce.txt` | ★ 结论 |
|---|---|---|---|---|---|
| ★ `C_1007_17921` | ✅ | ✅ | ✅ | ✅ | ★★ **逐位相同** |
| ★ `C_1008_17921` | ✅ | ✅ | ✅ | ✅ | ★★ **逐位相同** |
| ★ `C_1009_17921` | ✅ | ✅ | ✅ | ✅ | ★★ **逐位相同** |

★★ **且两臂的 `crc` / `iterations` / `sinr` / `rsrp` / `epre` 完全一致**
（★ 例：★ `crc=KO iterations=2 sinr=23.19 dB epre=-2.00 dB rsrp=-1.96 dB`）。

★★★ **并且已证明 AI 臂【确实走了它自己的路】**（★ 否则这个证明没有意义）：
★ 只有 AI 臂会打印那两条标记 ——
```
[pusch_receiver] backend=ai: STAGE S-1 IDENTITY. ...
depth-3 receiver arm: ai (OCUDU_REPLAY_RECEIVER)
```
★ 经典臂 **0 次**打印。

★★★ **⇒ S-1 的三个目的全部达成**：
★★ **① 接缝数据齐全**（★ 两个入口都拿到了需要的输入）；
★★ **② 开关是惰性的**（★ 关掉时逐位不变）；
★★ **③ AI 臂的失效不影响链路** —— ★★ **此时它什么都不做，但通路已建立**。

### 14.3 ★★★ S-1 抓到的**两个真陷阱**（★ 这才是 S-1 最值钱的产出）

#### ★★★ 陷阱 1：`dependencies` 在接缝处**已经被 move 掉**

★★ 原代码在 `process_estimate()` 里持有 `dmrs_pusch_estimator& estimator = dependencies->get_estimator();`
—— ★ **一个引用，★ 它能挺过 `std::move(dependencies)`**。
★★★ 而我把接缝写成 `dependencies->get_depth3()`，★ **但 `dependencies` 在同一函数稍早处
已被 move 进 `configure()`** ⇒ ★★ **它在那里是 `nullptr`** ⇒ ★ **段错误/断言**。
★★ **症状极具误导性**：★ 崩在 `process()` 里、★ 在 validator 之后、★ 而在**任何断点之前** ——
★ 定位它花了很多轮（★ 逐个插桩才找到 `deps after move = 0x0`）。
★★★ **修法**：★ **给 notifier 加 `get_dependencies()`**，★ 因为 notifier **在整次接收期间持有它**。
★★ **教训（★ 可迁移）**：★★★ **`std::move` 之后，调用方的句柄就是空的；
★ 而一个"引用"局部量不会** —— ★★ **替换一个引用时，最容易漏掉的正是它的生命周期。**

#### ★★ 陷阱 2：**离线工具不在默认构建目标里，★ 我拿旧二进制得出了"验证通过"**

★★★ 我改完 `ul_chain_replay` 的旋钮后跑 A/B，★ **得到"逐位相同"** —— ★ **但那个结论是无效的**，
★ 因为 **`ul_chain_replay` 是一个独立的 `add_executable`，不在默认 `all` 目标里**，
★ 所以 ★★ **我跑的是 10 月 8 日的旧二进制**（★ 那时还没有旋钮）。
★★ **抓到它的方式**：★ **打印的标记一次都没出现** ⇒ ★★ **"仪器沉默"救了我**。
★★★ **修法**：★ `cmake --build build --target ul_chain_replay`。
★★ **教训**：★★★ **一个"验证通过"必须先证明那件工具真的是新的** ——
★ 与 `memo_10` 的"数字之前先全量构建"同族，★ 但更隐蔽：★ **全量构建【不包含】这个目标。**

### 14.4 ★★ 顺带更正的一处既有认知

★★ `du_low_phy_pipeline_test` 里那条 `receiver_ai_falls_back_to_classic_while_unbound` 已**按新语义重写**：
★★ `ai_receiver_bound()` **由 false 变 true**，★ 而**它承载的那个问题已经【转移】了** ——
★ 不再是"请求是否被兑现"，★ 而是 **"有没有东西真的在计算"**（★ 由 `is_identity()` 回答）。
★★★ **测试名改成 `receiver_ai_resolves_to_the_ai_arm_now_that_its_head_exists`**，
★ 并**同时断言两半**（★ 请求被兑现 + 逐模块后端未被改动），
★★ **这样将来任何人都不能悄悄把 AI 臂变回经典链的别名而不让测试变红。**

### 14.5 ★★ 代码改动清单（★ 供 review）

| ★ 文件 | ★ 改动 |
|---|---|
| ★ `lib/.../pusch/pusch_depth3_receiver.h` | ★ **新增**（接缝接口）|
| ★ `lib/.../pusch/pusch_depth3_receiver_impl.{h,cpp}` | ★ **新增**（两个实现 + 工厂函数）|
| ★ `lib/.../pusch/pusch_processor_impl.h` | ★ `concurrent_dependencies` 持有 `receiver`；★ `set_depth3()`；★ notifier 的 `get_dependencies()` |
| ★ `lib/.../pusch/pusch_processor_impl.cpp` | ★ **两处接缝**改走 `get_depth3()`；★ 严格策略的模块interrogation **保持不变**（★ 为了逐位不变）|
| ★ `lib/.../pusch/processor_factories.cpp` | ★ 构造臂 + S-1 声明打印 |
| ★ `lib/.../pusch/CMakeLists.txt` | ★ 加入新 `.cpp` |
| ★ `include/.../pusch/factories.h` | ★ `pusch_processor_factory_sw_configuration` 增 `pusch_receiver_backend` |
| ★ `lib/phy/upper/upper_phy_factories.cpp` | ★ 闸门接受两值 |
| ★ `lib/.../metal/test/ul_chain_replay.cpp` | ★ `OCUDU_REPLAY_RECEIVER` |
| ★ `apps/.../du_low_phy_pipeline.h` | ★ `ai_receiver_bound() -> true`，★ 语义注释重写 |
| ★ `tests/.../du_low_phy_pipeline_test.cpp` | ★ 那条测试按新语义重写 |
