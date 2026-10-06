# `dispatch_cost_probe` —— dispatch / barrier / 命令缓冲成本的微基准（本工作流的 H1 判据）

**为什么需要它**：`OCUDU_LANE_ABLATE*` 只换 kernel **本体**、**不改提交与屏障结构** ✗ ⇒ 它量的是**工作量**，
永远量不出 **dispatch 成本** ✗；而上一工作流的"每次提交 300–400 µs"是在**空队列**上量的 ✗
（那是驱动提交窗口，不是 GPU 侧 launch/barrier）⇒ 两者都不是本工作流要的那个数 ✓。

**它是什么**：一个独立的 Metal 程序（**不需要电台、不进仓库构建系统** ✓），
同一个 device / queue / 缓冲，比较 **N 次 dispatch 编在一条命令缓冲里** vs **一次 dispatch 做 N 倍活** ✓，
并用**仓库同款口径**读时间：设备侧用 `GPUStartTime/GPUEndTime`（completion handler 里读，commit 前装 ✓，
与腿上 `[ul_gpu_lane] busy` 同源 ✓），宿主侧用 `commit → waitUntilCompleted` 的墙钟 ✓。

## 构建与运行

```bash
xcrun clang++ -std=c++17 -fobjc-arc -O2 -framework Metal -framework Foundation \
    dispatch_cost_probe.mm -o /tmp/dispatch_cost_probe && /tmp/dispatch_cost_probe
```
几何固定为**一跳**：`51 RB × 12 子载波 × 14 符号 = 8568 线程`，256/线程组 ✓（与均衡/解调的派发一致 ✓）。

## 怎么读（2026-10-06 实测，Apple M4 Pro；原始输出见 `run_2026-10-06_m4pro.log`）

| 臂 | 结论 |
|---|---|
| **1** N 次独立 trivial dispatch（一条 CB）| 每次 dispatch 的**边际设备成本 ≈ 3.7 µs** ✓（N=1: 6.9 → N=16: 59.8 µs ✓）|
| **2** N 次**有依赖**的 dispatch（阶段间模式）| 边际 ≈ **1.2 µs/阶段** ✓（依赖链不吃额外的 launch 成本 ✓）|
| **3** 一次 dispatch 做 N 倍活（融合形状）| N=1: 2.5 → N=16: 4.5 µs ✓ ⇒ **多做 16 倍活只多 2 µs** ✓✓ |
| **4** 宿主侧：1 条 CB×N dispatch vs N 条 CB×1 dispatch | 1 条 CB：**88–106 µs 基本不随 N 变** ✓；N 条 CB：**84 → 256 µs** ✓ ⇒ **命令缓冲数才是宿主成本** ✓ |
| **5** 网格大小（8.5k / 548k / 1M 线程）| 6.6 / 14.5 / 14.5 µs ✓ ⇒ **成本是每次 dispatch 的，不是每线程的** ✓ |

## ★ H1 判决（**推翻** ✗）

**每次 dispatch ≈ 3.7 µs，相对一跳 ~480 µs 的 lane busy 只有 ~0.8 %** ✓
⇒ **4 次 dispatch 变 1 次只值 ~11 µs** ✗ ⇒ **融合的收益不在"dispatch 计数"上** ✓。
（上一工作流的 4.3 µs/dispatch 与此吻合 ✓；p305 与 M0 的 `max_in_flight=1` 说明**命令缓冲数**早已是 1/跳 ✓，
宿主那 300–400 µs 的空队列窗口也不在这里 ✓。）

**它把工作流指向哪里** ✓：ARM 3 显示**融合形状吸收工作量几乎免费** ✓（16 倍活 +2 µs ✓）、
ARM 5 显示**这个网格规模下 GPU 基本闲着** ✓ ⇒ 一跳那 465 µs 必须由**别的东西**解释：
每个 RE 的实际算法/访存安排 ✗、冗余 gather ✗、或者 fence/守护结构 ✓。
⇒ **融合的价值是"给更好的调度提供容器"** ✓，不是"省下 launch" ✗ —— M1 的验收重点据此重排 ✓。
