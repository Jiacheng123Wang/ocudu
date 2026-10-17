# 深度-3 的测量仪器（tracked）

这些脚本是**测量仪器**，不是产品代码：它们决定"一个结论能不能被相信"。放在跟踪目录里，是因为
`work_tmp/` 被 gitignore（那里的语料/数据集太大），而仪器必须跨会话存活 —— 上一轮就有一批脚本只在
`work_tmp` 里，靠人工才知道怎么跑。

**数据与产物仍在 `doc_chinese/llr_ai_detection/work_tmp/`**（语料、数据集、模型 bundle、日志），所以脚本里
的 `W` 都指那里。运行前 `cd doc_chinese/llr_ai_detection/work_tmp`（或按脚本里的绝对路径调用）。

| 脚本 | 回答什么问题 |
|---|---|
| `check_matrix.sh` | 五旋钮 × 流水线的**配置矩阵**（21 行，合法/非法都跑真实二进制）。动过臂或 CLI 之后**必须**跑；2026-10-17 修过两处仪器缺陷：`mktemp` 模板（macOS 要求 X 在结尾）与过宽的覆盖行模式（把条件化契约的汇总行也算成一条覆盖） |
| `soak_timing.sh` | 一个进程跑 N 次接收，填满臂的 `[depth3-timing]` 环形缓冲（第 1 次与每 500 次打印）。**单条回放只有冷样本**，不能用来说稳态 |
| `ab_timing.py` | 两个二进制/两种设置**交错 ABBA** 计时；带 `--gap-ms` 调用间隔与模态标记（这台机器主机侧有 8× 的慢模态，只会抬高读数） |
| `scan_arch.sh` | 架构 → ANE 时延曲线：`train_depth3.py --probe-graph`（随机权重、只测时间）→ 编译 → 用真实臂计时，min-of-3 |
| `truncate_depth.py` | 把**已训练**模型按深度截断（权重不变，四个头接到第 k 块）并导出 |
| `refit_heads.py` | 冻结截断后的主干、**只重拟合四个头** —— 没有它，深度曲线量到的是"头的不适应"而不是能力 |
| `ber_by_snr.py` | 按 SNR 分箱的 uncoded BER（同一批样本）：看的是"地板"而不是被低 SNR 样本支配的总 BER |
| `capability_decode.sh` | 每个深度的**判决级**能力（真实捕获、真实链条） |
| `verify_compact.sh` | 压缩候选的五条预登记判据（判决等价 / head 3 / head 4 / criterion 1 / 时延与放置） |
| `verify_sinr_cal.sh` | 公布 SINR 标定的**留出**验证（拟合偶数位、验证奇数位） |
| `verify_xfer.sh` | 标定的**跨语料迁移**检验（另一采集会话） |

`work_tmp/probe/` 里仍有同样的工作副本；**以本目录为准**，改动请改这里再同步过去。
