# L1 harness（S-7f-5x 快照，从 `/tmp/l1` 抢救出来）

`/tmp` 不抗重启（本文档已经为丢语料付过一次代价），而 L1 是**唯一**在 GPU 上验证 CE 内核的
工具（CE 单元测试的 grid 没有 device view，`device_sigma2=0`，是空转绿）。所以把这套放到仓库树里。

```bash
bash s7f5x_l1_build.sh      # 用 ul_chain_replay 的 include/define/link 行重建 /tmp/l1/l1
bash s7f5x_l1_run_all.sh    # 10 个几何 + L1_REFUSE 反向探针，每个都有 90s wall-clock 上限
```

- 必须在 metal 源目录下运行（`ocudu_mmse.metallib` 是回退路径；`s7f5x_l1_run_all.sh` 自己 cd）。
- 每次改 engine/内核后都要重建 harness，否则你验证的是旧目标文件。
- 每个用例 `kill -9` 兜底：GPU 挂死时 `kill -9` 释放不了 GPU，只能强制断电（§48.131/§48.133）。
- `L1_REFUSE=1` 是反向探针：几何超出内核契约时必须**拒绝**（`build_ok=1 sigma2_done=0 sentinel_intact=1`），
  而不是静默截断。
