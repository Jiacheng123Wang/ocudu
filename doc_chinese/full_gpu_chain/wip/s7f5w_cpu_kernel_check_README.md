# S-7f-5w 的 CPU 侧内核对照（把 MSL 内核语义与真实宿主实现对拍）

**为什么有它**：GPU 内核写错时，症状往往是"结果偏一点"或"整机卡死"（§48.131/§48.133），
代价极高。`check.cpp` 把 `ocudu_mmse_pilots.metal` 里两个新内核**逐行转写成 C++**，
与**真实宿主实现**（直接编 `port_channel_estimator_helpers.cpp` 的 `apply_fd_smoothing()` +
`estimate_noise()`）对同一份合成输入比对。**全程不碰 GPU。**

**它抓到过什么（真事）**：第一版内核里 `base = s*nof_layers*nof_pilots + i` **漏了 `l0*nof_pilots`**，
于是"第 2 个 CDM 对"永远读第 0/1 层 —— 1 层/2 层（含上机配置）看不出来，**4 层分配就错 2.4%**。
CPU 对照一眼定位（逐对能量：第一对吻合、第二对差 4.9%）。**这就是先做 CPU 对照的价值。**

**重跑配方**（macOS，无需 GPU；`CE` 指向 estimator 目录）：

```bash
cd /Users/jiachengwang/dev/ocudu
CE=$PWD/lib/phy/upper/signal_processors/channel_estimator
clang++ -std=c++17 -O1 -I include -I lib -I external/fmt/include -I "$CE" \
  -o /tmp/sigma2_check/check doc_chinese/full_gpu_chain/wip/s7f5w_cpu_kernel_check.cpp \
  "$CE/port_channel_estimator_helpers.cpp" lib/ocuduvec/*.cpp build/external/fmt/libfmt.a
/tmp/sigma2_check/check          # 期望：CPU-SIDE ALGORITHM CHECK: PASS
```

**覆盖的 9 个用例**：air 25PRB/3sym/comb6/1lay（含/不含 CFO 补偿）、2 层（1 个 CDM 对）、
**4 层（2 个 CDM 对）**、2 与 4 个 DM-RS 符号、单 PRB（`nv = 全部导频` 的特例）、
comb 4（stride 3）、comb 3（stride 4）。

**判据**：虚拟导频 / 平滑后导频 / sigma2 的相对差都要 < 1e-4（实测 ≤ 3.7e-07，虚拟导频那项
在随机边缘数据上可到 ~2e-05，属正常外推放大）。

**已知局限**：它验证的是**转写体**，不是 MSL 文本本身 —— 两者可能漂移。
所以 GPU 侧的第一步（L1）仍然要跑，用来验证真正的 MSL 与接线。
