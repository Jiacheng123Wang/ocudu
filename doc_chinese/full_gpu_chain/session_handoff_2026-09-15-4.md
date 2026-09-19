# Session Handoff Memo — 2026-09-15-4

> **性质**：状态快照，**冻结不改**。需要修正/更新时写进活文档
> `doc_chinese/full_gpu_chain/s2_full_chain_design.md`，不要回头改本文件。
>
> **代码基线**：起点 `f534857820`（= 上一份 handoff 交出去上机的那一腿），**终点/HEAD = `33eee2cdfd`**；
> `gnb` 二进制戳 = **`33eee2cdfd`**（**尚未上机**，本次就是交用户 OTA）
> **活文档**：`doc_chinese/full_gpu_chain/s2_full_chain_design.md`（8474 行），本轮写入 **§48.127–§48.128**
> （§48.84 横向视图上一轮已同步；本轮只改 CE 内部"谁建矩阵"，落点表未变）

---

## 0. 一句话现状

**上一腿（胶水 #2，`f534857820`）已上机通过**（判据见 §1）；
**本轮把 merged 批次的 `A`/`R_hp` 也搬到设备上建**（K0-d 覆盖率 25.7% → 预期 ≈100%），
**离线六项门禁全绿**，代码已提交 `33eee2cdfd`、`gnb` 已重建同步。**下一步：用户手机 OTA。**

---

## 1. 上一腿（`f534857820`，胶水 #2）上机判读 —— **通过**

| 判据 | 实测 |
|---|---|
| 硬约束（attach/ping/iperf3） | 上行 **7.21 MB**、PDU median 480 B（真流量）|
| **`device_y_writes` / `y_write_fail`** | **21804 / 0**（12512 hops ⇒ 每个 hop 都走了新路）|
| `cbs/lane` | **3.00 不变**（`ch_est` 2.00）⇒ scatter 没加 command buffer |
| `[ul_pipeline]` median | **961 µs**（上腿 977；基线 <1 ms 要求满足）|
| `Real-time failure in RF` | **5 / 12265 = 0.0408%/时隙**（基线 0.1148%，好 **2.8×**）|
| 崩溃 / USB / `zero-copy` 告警 | **0 / 0 / 0** |
| `mean total` / `gpu_path` / `gap` | 35.3 / 19.9 / 109.3 µs |

⚠️ 注意 `stage=2.46 µs` 比上腿的 2.24 略**升**：`stage` 里主要是**标准组 A/R_hp 的主机 staging**，
它的量随 hop 结构（`device_corr_builds` 覆盖率）变化，**不隔离胶水 #2 的效果**——
胶水 #2 的效果由离线同抓包 A/B 证明（`stage` 9.12 → 7.33 µs），以及 `device_y_writes>0`。

---

## 2. 本轮做了什么（`33eee2cdfd`，S-7f-5v）

| 层 | 内容 |
|---|---|
| 选点依据 | 上腿 `[mmse_time_sum]`：`corr` **11.0 µs** + `stage` 2.46 = **13.5 µs/hop**，是 IQ→LLR 链路上**剩下最大的主机计算块**；而 K0-d 内核**早就有**，只是 merged 批次（**上机 74% 的 hop**）走不到 |
| 引擎 | **`corr_stage::nof_systems`**：0 = 整批（原语义），非 0 = **只覆盖本几何的这几个 system**。merged 路径给标准组一个 `nof_systems = nof_layers` 的前缀，**edge 组仍由主机建+staging**（1–3 PRB，~1 µs）|
| 估计器 | 把"谁建标准块"的判定**提到建矩阵之前**（`matrix_on`/`merge_tail`/`device_corr_enabled`/`gpu_invert_std`/`dev_corr_std_merged`/`std_slots_filled`），只在"主机自己会 staging"时才建：`host_builds_std = !((std_slots_filled && !matrix_on) \|\| dev_corr_std_merged)`；几何改为**算术导出**并与设备侧 `correlation_stage()` **assert 对齐** |
| 安全网 | 主机数组被跳过后，"设备建失败→退回主机 staging"这条老路没有数据可退 ⇒ 改成 **`return false`**，由 CPU 块路径（自建矩阵）接手 |
| 门禁 | **新增 `k0dm`**（默认 vs `OCUDU_CE_CORR_DEV=0`，逐字节 + **两侧非真空断言**）；`k0d` 现在**打印覆盖率**；**"串行复核"改成真的串行**（并行阶段结束后统一复核）+ 反向验证 |

**为什么上次尝试会失败**（现在有名字了）：corr 前缀按**批次的 system 数**建矩阵，
于是把标准几何写进了 edge 组自己的槽位。修法就是 `nof_systems` 一个字段的语义。

---

## 3. 门禁实测（全部已跑，可重跑）

| 门禁 | 结果 |
|---|---|
| **`k0dm`（新）** | 980 抓包：**927 逐字节一致 + 9 真空（几何上无标准块）+ 44 个并发假红旗被串行复核清掉** ⇒ PASS |
| `k0d` | **980/980 逐字节**；其中 **`device-built-captures=258/980`**（其余 722 个抓包过去是**静默的主机 vs 主机**）|
| `ydev` | **942 逐字节一致**，38 个假红旗复核清掉 ⇒ PASS |
| `k1` / `combos` | 980/980 判决一致 / 10-10 PASS |
| CE 单测 / `ctest -L phy` | All tests PASSED / 162/162 |
| 离线单抓包 | `corr` **23 → 3 µs**、`stage` 5.75 → 1.42、`mean total` **76 → 50 µs** |

**本轮发现的两个工具/方法问题**（已修）：

1. **门禁的"串行复核"过去不是串行的**（在 shard 内、其他 shard 仍在跑时复核）——
   实测三次运行共 **5 个**假红旗，单独跑全部逐字节相同；本轮第一次重跑时它甚至**掩盖了门禁结论**
   （42 个红旗，无一真实）。现已改为**并行结束后空闲时统一复核**，并做了**反向验证**
   （把 route B 换成真会改字节的开关 → 门禁必须 MISMATCH 且 exit 1 ✔）。
2. **`--out` 是前缀且工具不清理旧文件**：我用了一个与**上一会话撞名**的前缀（`/tmp/v_a`），
   读到的是几小时前的残留，差点当成"两条路不一致"。**复核输出一律用新建空目录。**

---

## 4. 上机（OTA）——**这就是本会话交出去的动作**

**二进制**：`gnb` 戳 **`33eee2cdfd`**（已重建同步）。

```bash
cd /Users/jiachengwang/dev/ocudu
sudo ./build/apps/gnb/gnb -c configs/gnb_rf_b200_fdd_n1_5mhz_bridge.yml \
  --expert_phy.pusch_channel_estimator_algo metal_mmse \
  --expert_phy.pusch_channel_equalizer_backend metal --expert_phy.pusch_dft_type metal \
  --expert_phy.pusch_ldpc_decoder_type auto \
  --log.all_level warning --log.filename /tmp/gnb_ota_glue3.log
```
（**console 不要重定向**。）

**判据（与上腿对比）**：
1. **硬约束**：attach + ping + iperf3；
2. **`device_corr_builds` ≈ hops 数（上腿 3218 → 预期 ≈12500）** ← 本步的"确实在跑"证据；
3. **`[mmse_time_sum] corr` ≈ 0**（上腿 11.0 µs）、`mean total` **≈ 25 µs**（上腿 35.3）；
4. `cbs/lane` **3.00 不变**；`device_y_writes>0`、`y_write_fail=0`；
5. `Real-time failure in RF` 按**时隙**折算不劣于基线 **0.1148%**；
6. 崩溃 / USB 错误 / `zero-copy cache hit with a larger request` **= 0**。

**可选 A/B**：`OCUDU_CE_CORR_DEV=0 sudo -E ./build/apps/gnb/gnb …` 一腿（`device_corr_builds` 应为 0）。

---

## 5. 本步**没有**做（有意保留，别当遗漏）

1. **edge（tail）组仍由主机建 + staging**——它在标准槽位里是另一个几何，
   要设备建就得连"pad 清零 + 单位对角"一起设备化；单独一步做；
2. **窄 hop（`n_std_blocks == 0`）**仍走主机建；
3. **`sigma2` / 统计 / 主机 pre-stage 重算**（`pre` 0.69 + `sigma2` 3.4 µs）——**下一处胶水**：
   它同时解掉"主机重算"与"设备 LSE 拷回"两条；
4. **`gpu_qy`（矩阵 flavor）**仍由主机打包——**有意保留**（非上机路径，无门禁覆盖）。

**下一处 = `sigma2`/统计上设备**（`channel_statistics_estimator_fixed` 只用 `input.sigma2`，
所以 `pilots_lse_view` 的两个消费者里有一个是空的——搬完 `sigma2` 就能删掉主机 pre-stage 与拷回）。
**动手前先画图**（§48.83(c)）。

---

## 6. 关键文件

| 文件 | 作用 |
|---|---|
| `.../metal/ocudu_metal_mmse_engine.h` | `corr_stage::nof_systems`（本轮唯一的引擎语义改动）+ 文档 |
| `.../metal/ocudu_metal_mmse_engine.mm` | `run_async`/`encode_weights_only` 用 `corr_stage::nof_systems` 覆盖批次数 |
| `.../metal/port_channel_estimator_metal_mmse_impl.cpp` | 判定上移 + `host_builds_std` + `dev_corr_std_merged` + `std_corr_prefix`（传给 merged 的 `engine_run`）+ `run_engine_blocks` 失败即 `return false` |
| `.../metal/capture_gates.sh` | **新 `k0dm` 模式**、`k0d` 覆盖率报告、**真串行复核**（`rechecked=`）、真空/无效开关断言 |
| `doc_chinese/full_gpu_chain/s2_full_chain_design.md` | 活文档：**§48.127（本步记录）+ §48.128（OTA 判据）** |

---

## 7. 一句话交接

**merged 批次的 A/R_hp 已由设备在引擎同一条 CB 内建好（`corr` 11.0→≈0 µs/hop，K0-d 覆盖率 25.7%→≈100%）；
离线六项门禁全绿（含新 `k0dm` 逐字节门禁与它的非真空断言），代码提交 `33eee2cdfd`、`gnb` 已同步；
请上机跑一腿，重点看 `device_corr_builds ≈ hops`、`corr ≈ 0`、`mean total ≈ 25 µs`、`cbs/lane` 仍 3.00；
确认链路没坏之后，下一处胶水是 `sigma2`/统计上设备。**
