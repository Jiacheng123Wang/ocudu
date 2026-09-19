# S9 — host → device **写**：第一次量出来了，以及一个必须记住的门语义变化

## 1. 结果：`30 host write(s) per hop`（此前完全未测）

```
host device data crossings: 1 host read(s) and 30 host write(s) (548 bytes) of device data
  over 1 device hop(s) = 1.00 read(s) + 30.00 write(s) per hop;
  the fused lane (mode=gpu) allows 0 of each ...
  counted by the module(s) that audited their host <-> device data touches: dft, channel_estimator
  (modules NOT listed are not covered by this number)
```

- 备份/申报的模块：`channel_estimator`（4 读 + 3 写点）、`dft`（staging 拷贝）；
- **未申报**：均衡器、解调器 —— **它们的点没有被审，因此不在这个数字里**；
- 空口上 DFT 的 `wrap_copies=0`（IQ 是就地零拷贝读的），所以那 30 次写主要来自估计器每跳的
  `gpu_epochs`、`gpu_ls_cfo` 进位、以及合并跳上的 `gpu_y` 清零。

## 2. ⚠ 门语义变化：`TAIL_DEV=0` **不再**是等价参考

**S5（`a656133d70`）之后，`OCUDU_CE_TAIL_DEV=0` vs 默认 = 26771 字节不同（`_llr` 12930 / `_h` 13370）。**
这**不是**缺陷，是构造上的必然：S5 之后宿主不再取设备的 σ²，于是"宿主建矩阵"用的是**宿主自己的 σ²**，
而"设备建矩阵"用的是**设备自己的商**，两者不再相同。

**判据**（两臂都用 S5 之前的读行为）：

```
OCUDU_CE_TAIL_DEV=0 OCUDU_CE_HOST_SCALARS=1  vs  OCUDU_CE_HOST_SCALARS=1   ->  0 字节
```

**⇒ S4 的修复完好，HEAD 干净。** 但：

> **`TAIL_DEV=0` vs 默认 这个门，在 S5 之后就作废了。** 它证明 S4 修复时（`7524a9f80c`）是有效的；
> 现在要判 S4 那条路径，必须把两个臂都钉在同一个 σ² 来源上。

**教训（与 S4 那次同类）**：一个门的分母/参考臂会随另一个 commit 改变语义。**每次用旧门之前，先问它现在
到底在比什么**——这次为此白跑了两轮 A/B，并误把 S9 的改动回退了（见下）。

## 3. S9 的代码被误回退了，需要重做

当时看到 26771 就判断"S9 动了数据"并 `git checkout` 回退了三个文件。**那是错的**：diff 里只有计数器调用
和空白，且回退后 A/B 仍为 26771 ⇒ 与 S9 无关。

重做时**直接照抄本轮已写好的形态**即可（都在本文件与 commit message 里有记录）：

1. `phy_pipeline_crossings` 加 `count_host_write(uint64_t bytes = 0)`、`get_host_writes()`、
   `host_write_bytes()`，并在同一条检查里打印**两个方向**；
2. **`declare_reporter(const char*)` + `print_reporters(FILE*)`** —— 打印"哪些模块申报了"，
   让 `OK` 的作用域和 `OK` 一起显示。**这是本次最有价值的产出**：一条只覆盖一个模块却写
   "the fused lane allows 0" 的检查，比没有检查更危险；
3. 估计器 3 个写点（`gpu_epochs` ×2、`gpu_ls_cfo` 进位、`gpu_y` memset）+ `declare_reporter`；
4. DFT 的 staging 拷贝（`newBufferWithBytes`）并入同一计数器 + `declare_reporter`。

### 3.1 两个踩过的坑（避免重复）

- **`phy_pipeline_crossings.h` 必须保持轻量**：加 `<vector>`/`<string>`/`<algorithm>` 会让若干
  "位于命名空间内"的包含点炸掉（`no template named 'basic_ostream'`）。现在只用
  `<atomic> <cstdint> <cstdio> <cstring> <mutex>`，字符串用 `fprintf` 直接打。
- **改文件尾部时务必确认命名空间的闭合大括号还在**：本轮曾把 `} // namespace ocudu` 截掉，
  症状是后续所有头文件报"std 模板不在命名空间内"，我**误诊了两次**（先怪 include 顺序、
  再怪 Objective-C++），实际是括号不配平。**同类错误的正确第一步是检查括号配平，不是猜 include。**

## 4. 还欠的

| 项 | 现状 |
|---|---|
| 均衡器 / 解调器的 host↔device 点 | **未审**（两个模块各有一个 `wrap_no_copy` 包住模块边界的 buffer）|
| 契约那条读检查的措辞 | 仍写 "the fused lane (mode=gpu) allows 0"，**未标明只覆盖估计器** |
| 30 写/跳的逐条消灭 | 未开始 |
| 目标值 | 取决于 IQ 那次上传是否算——空口 `wrap_copies=0` 说明**这一路 IQ 没有拷贝上传**，所以目标可以是 **0** |
