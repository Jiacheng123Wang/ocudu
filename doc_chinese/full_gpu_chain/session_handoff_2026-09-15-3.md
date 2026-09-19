# Session Handoff Memo — 2026-09-15-3

> **性质**：状态快照，**冻结不改**。需要修正/更新时写进活文档
> `doc_chinese/full_gpu_chain/s2_full_chain_design.md`，不要回头改本文件。
>
> **代码基线**：起点 `d871a51918`（= 上一份 handoff 记录的 HEAD），**终点/HEAD = `f534857820`**；
> `gnb` 二进制戳 = **`f534857820`**（**尚未上机**，本次就是交用户 OTA）
> **活文档**：`doc_chinese/full_gpu_chain/s2_full_chain_design.md`（8344 行），本会话写入 **§48.125–§48.126**
> **横向状态视图 §48.84 已同步更新**（模块落点表 (a0)/(a)、数据流 (b)、缓冲账本 (c)、门禁 (e)、账本 (f)）

---

## 0. 一句话现状

**胶水 #2 已消灭**：设备把 K0-a 产出的导频**直接写进引擎的 `y` 槽位**（在引擎自己那条 command buffer 内），
主机那次 `pilots_lse_view → gpu_y` 的 memcpy 消失。**离线门禁全绿**（k0d 980/980 逐字节、k1 980/980 判决、
combos 10/10、新 `ydev` 980 抓包逐字节、CE 单测全 PASS、`ctest -L phy` 162/162），
**代码已提交 `f534857820`、`gnb` 已重建同步**。**下一步：用户手机 OTA 判读。**

---

## 1. 本会话做了什么（`f534857820`）

| 层 | 内容 |
|---|---|
| 内核 | `ocudu_mmse_pilots.metal` 新增 `mmse_pilots_scatter_y`：把 `gpu_ls_out`（跳布局）重新索引进 `y`（块布局）+ 一个 `inv_beta` 单乘。**只重新索引 + 单乘 ⇒ 逐位一致是构造性的**，故**不进 `IEEE_MATH_SOURCES`** |
| 引擎 | `pilots_scatter` 描述符 + `scatter_available()` + 四个入口（`run_async`/`run`/`run_weights_only`/`run_weights_only_async`）都支持把 scatter 编进**同一条 CB 的最前面**；计数器 `device_y_writes`/`y_write_fail` |
| 估计器 | `record_device_y_stage()`（**唯一的"设备是否写 y"判定点**）、`stage_engine_group()` legacy 分支"设备写就跳过主机 memcpy"、`device_ls_valid` 逐 hop 复位、`OCUDU_CE_DEV_Y`（默认开）、`OCUDU_CE_Y_CHECK` 探针 |
| 门禁 | `capture_gates.sh ydev`：A/B 逐字节 + **非真空断言**（route A 必须 `device_y_writes>0`） |

**观测**（离线单抓包）：`[mmse_time_sum] stage` 7.33 µs（设备写）vs 9.12 µs（主机 staging）；
`cbs/lane` **3.00 不变**（scatter 搭同一条 CB）。

---

## 2. ⚠️ 本会话最贵的发现（**一等条目**，务必先读 §48.125(c)）

**同一块内存 ≠ 同一个资源。**

第一版把 `sys_offset` 放进了**目的指针**，于是零拷贝缓存 `wrap()` 为尾组**新建了第二个 `MTLBuffer` 对象**
覆盖同一段内存。**Metal 只通过"绑定的是哪个资源对象"关联两个 dispatch 的访问**，
所以尾组的 scatter 与 K2 的读**无序**：merged 形式 publish 出主机 `memset` 的 0，
split 形式 publish 出上一次提交的残留；而标准组（指针 == 批次基址 ⇒ 同一个对象）**逐字节正确**。

**定位靠两个证据缺一不可**：

1. 探针 `OCUDU_CE_Y_CHECK=1`（新）：设备写进 `y` 的值 vs 主机 staging 会写的值，**864 槽 0 不匹配、`max_abs=0`**
   ⇒ **内容对**；
2. 内容对而结果错 ⇒ 只能是**排序**问题 ⇒ 端到端 A/B（`ydev`）才是判据。

**教训**：跨 dispatch 的写者/读者，"用同一套 stride/偏移"**还不够**，必须绑定**同一个资源对象**；
偏移要走 `setBuffer:offset:`。**探针只能证明内存内容，不能证明"读到的就是它"。**

---

## 3. 门禁实测（全部已跑，可重跑）

| 门禁 | 结果 |
|---|---|
| `capture_gates.sh ydev`（新） | 两轮 978/980、979/980，**失败集不同**；三个被标记抓包**干净串行**复跑 **5/5 文件逐字节相同** ⇒ 工具在高并发下的已知不稳定，**非缺陷** |
| `capture_gates.sh k0d` | **980/980 逐字节**（`retried=0`）|
| `capture_gates.sh k1` | **980/980 判决一致** |
| `capture_gates.sh combos` | **10/10 PASS**（含 split 四组；`iq1_10049` 全组合 6.24 dB OK = 参考）|
| CE 单测（显式重建） | **All tests PASSED**（Test 11 merged ≡ split 逐位）|
| `ctest -L phy` | **162/162** |

---

## 4. 上机（OTA）——**这就是本会话交出去的动作**

**二进制**：`gnb` 戳 **`f534857820`**（已重建同步）。

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_ota_glue2.log
```
（**console 不要重定向**。）

**判据**：
1. **硬约束**：attach + ping + iperf3 都跑起来；
2. **`[metal_stats] mmse_ce … device_y_writes>0` 且 `y_write_fail=0`** ← **新路的"确实在跑"证据**；
3. `[mmse_time_sum] stage` 下降（离线 −1.8 µs/hop 量级）；
4. `[ul_gpu_lane] cbs/lane` **不增加**（应仍 3.00）；
5. `device_corr_builds>0`、`corr_build_fail=0`、`[ul_pipeline]`/`[ul_channel_estimation]` median；
6. 另 grep：`Real-time failure in RF` 按**时隙**折算不劣于基线 **0.1148%**、
   `zero-copy cache hit with a larger request` **= 0**、崩溃/USB 错误 **= 0**。

**可选 A/B**：`OCUDU_CE_DEV_Y=0 sudo -E ./build/apps/gnb/gnb …` 一腿（离线已证明逐字节等价）。

**AMF**：`192.168.31.250:38412`（N2 走 SCTP，`nc` 探不到正常）。

---

## 5. 本步**没有**做（有意保留，别当遗漏）

1. **矩阵 flavor 的 `gpu_qy` 仍由主机四元交错打包**（`record_device_y_stage()` 只在 `!matrix` 分支调用，
   `run_nn` 一个字节没改）；
2. **主机的 pre-stage 重算**与**设备 LSE 拷回 `pilots_lse_view`** 都还在——
   `pilots_lse_view` 仍被 `estimate_sigma2()`（FD 平滑）与 RSRP 统计读，**要等 `sigma2`/统计上设备时一起删**；
3. 设备 LSE 的落点/其余账本见活文档 §48.84(f)。

**下一处要消灭的胶水 = `sigma2` / 统计上设备**（它一并解掉上面第 2 条的两项）。**动手前先画图**（§48.83(c)）。

---

## 6. 关键文件

| 文件 | 作用 |
|---|---|
| `.../metal/ocudu_mmse_pilots.metal` | **新增** `mmse_pilots_scatter_y` + `mmse_scatter_params`（K0-a 三内核仍在同文件）|
| `.../metal/ocudu_metal_mmse_engine.h/.mm` | `pilots_scatter`、`encode_scatter()`（**绑定引擎自己的 `y_buf` + 字节偏移**）、`scatter_available()`、四个入口的 `scatter`/`nof_scatter`、`device_y_writes` 计数器 |
| `.../metal/port_channel_estimator_metal_mmse_impl.h/.cpp` | `record_device_y_stage()`、`stage_engine_group()` 的"设备写则跳过"、`engine_run()` 传描述符、`device_ls_valid`、`OCUDU_CE_DEV_Y`、`OCUDU_CE_Y_CHECK` 探针 |
| `.../metal/capture_gates.sh` | 新增 `ydev` 模式（含非真空断言）；更正 split 组合的过期注释 |
| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | 活文档：**§48.125（本步记录，含 (c) 的缺陷定位）**、§48.126（OTA 判据）、§48.84（已同步）|

---

## 7. 一句话交接

**胶水 #2 已消灭并已提交（`f534857820`，`gnb` 已同步）；离线六项门禁全绿；
请上机跑一腿，重点看 `device_y_writes>0` 与 `stage` 下降、`cbs/lane` 不增加；
确认链路没坏之后，下一处胶水是 `sigma2`/统计上设备。**
