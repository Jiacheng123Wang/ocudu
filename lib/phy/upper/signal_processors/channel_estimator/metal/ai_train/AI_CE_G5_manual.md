# G-5 每站在线自适应：手动操作手册（逐步命令）

> 本手册覆盖完整闭环：**采集 → 配对 → 标签重建 → 微调 → 转换 → A/B 对比 →
> 上线运行**，每一步给出可直接复制执行的命令。52-PRB 模型为主流程；
> 106-PRB 模型的差异点单独标注（§10）。设计原理/数据格式/档案见
> `AI_CE_G5_online_adaptation_memo.md`。

## 0. 前提

- **硬件/拓扑**：B210 + 手机（或 OAI UE）+ open5gs 核心网 + Apple Silicon Mac
  （gNB 运行时）。本手册按"iPhone17 配置"（`configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17`，
  gpsdo + rx_gain 60）给出命令——这是唯一经过验证的实机配置。
- **目录约定**：`AI_TRAIN` = 本目录（`lib/phy/upper/signal_processors/channel_estimator/metal/ai_train`）。
  所有 Python 命令在任意工作目录执行均可（脚本按自身所在目录找依赖）。
- **Python 环境**（一次性）：
  ```bash
  python3 -m venv ~/helena_venv && source ~/helena_venv/bin/activate
  pip install numpy tensorflow tf-keras h5py coremltools   # coremltools 仅 macOS
  ```
  后续命令默认 `source ~/helena_venv/bin/activate` 已执行。

## 1. 采集（白天，gnb 侧）

```bash
cd <ocudu 仓库根目录>
git pull && cmake --build build --target gnb -j8

mkdir -p ~/capture/site_<日期>
# 注意：环境变量必须在 sudo 之后（macOS sudo 的 env_reset 会过滤 sudo 前的变量）
sudo OCUDU_CE_TIME=1 OCUDU_HELENA_DUMP_DIR=$HOME/capture/site_<日期> \
  ./build/apps/gnb/gnb -c configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena

# 手机 attach 后：iperf3 上行 30s ×3（服务端在核心网主机，如 127）：
#   服务端（127 上）：iperf3 -s -B 10.45.0.1 -p 5201
#   手机客户端：iperf3 -c 10.45.0.1 -p 5201 -t 30
# 完成后 Ctrl-C 停 gnb；校验产物：
ls ~/capture/site_<日期> | wc -l        # 应见 dump_*/rx_*/tb_* + 3 个 meta.csv
```

采集质量提示：CRC-OK 率 16~40% 属正常（3408.96 邻道干扰时段波动）；干扰样本
本身是稀缺训练料。数据格式全量规范见 `AI_CE_G5_online_adaptation_memo.md` §3。

## 2. 配对

```bash
python $AI_TRAIN/pair_capture.py ~/capture/site_<日期>
# 输出 pairs.npz（tb_idx/rx_idx/ce_idx/n_prb）；打印配对质量（紧配对比例、dt 分布）
```

## 3. 标签重建（决策导向）

```bash
python $AI_TRAIN/build_labels.py ~/capture/site_<日期> --out ~/capture/site_<日期>/labels_all.npz
# 输出 labels_all.npz {X, Y, width}；末尾打印 |X-Y|²/|Y|² 的聚合值（供参考，
# 重传对存在时间错位，聚合值会偏乐观/悲观，用第 4 步的质量门为准）。
```

## 4. 训练集生成（质量门过滤）

```bash
python - <<'EOF'
import numpy as np, os
d = np.load(os.path.expanduser('~/capture/site_<日期>/labels_all.npz'))
X, Y, W = d['X'], d['Y'], d['width']
keep = [i for i in range(len(W)) if
        10*np.log10((np.abs(X[i,:W[i]*12]-Y[i,:W[i]*12])**2).sum()/
        max((np.abs(Y[i,:W[i]*12])**2).sum(),1e-30)) < -10]
keep = np.array(keep)
rng = np.random.default_rng(42); perm = rng.permutation(len(keep))
nva = max(len(keep)//10, 50); te, tr = perm[:nva], perm[nva:]
np.savez(os.path.expanduser('~/capture/site_<日期>/realtrain.npz'),
         X_train=X[keep[tr]], Y_train=Y[keep[tr]], width_train=W[keep[tr]],
         X_test=X[keep[te]], Y_test=Y[keep[te]], width_test=W[keep[te]])
print(f'train={len(tr)} test={len(te)}')
EOF
```

## 5. 微调（夜间）

```bash
# 从当前线上模型（52 桶）初始化微调；lr 1e-5、6 epoch 是验证过的起点
python $AI_TRAIN/train_pad.py ~/capture/site_<日期>/realtrain.npz \
    ~/capture/site_<日期>/helena_pusch52_sm_real \
    --prb 52 --init <当前线上 52 SavedModel，例如 ai_assets 对应模型的 sm 导出> \
    --epochs 6 --batch 32 --lr 1e-5
```

> 初始化的 SavedModel 从哪来：若无历史 sm 文件，先用合成数据训练一个基线
> （`gen_pusch_dataset.py 15000 3000 pusch52pad.npz --nfft 624 --pad-aware
> --min-prb 6 --max-prb 52 --snr-min -5 --snr-max 55` +
> `train_pad.py ... --prb 52 --init <52 基模型> --epochs 8`，基模型可由
> `helena_arch.py` 的 `transfer_from_51` 从仓库 ai_assets 的任一 52 模型生成）。

## 6. 验证（双保险）

```bash
# 6a. 合成集回归（不得明显劣化；历史基线 ~-16.2 dB @52）
python $AI_TRAIN/eval_pad.py ~/capture/site_<日期>/helena_pusch52_sm_real \
    <合成 52 pad-aware 测试 npz> --prb 52 --bands "6-12,13-25,26-52"

# 6b. 45 dB 恒等探针（模型不得破坏干净信道；期望 >= -25 dB @6 PRB）
python - <<'EOF'
import sys, numpy as np
sys.path.insert(0, '$AI_TRAIN')
from tf_keras.models import load_model
from gen_pusch_dataset import gen_true_channels, ls_and_interp
rng = np.random.default_rng(0); nsc = 624
model = load_model('~/capture/site_<日期>/helena_pusch52_sm_real')
for prb in (6, 18, 52):
    w = prb*12
    H = gen_true_channels(32, w, rng)
    ns = np.full(32, np.sqrt(10.0**(-45.0/10.0)))
    X = ls_and_interp(H, ns, rng, w, prb)
    Xin = np.zeros((32, nsc, 14, 2), np.float32); Xin[:, :w] = X
    out = model.predict(Xin, verbose=0)[:, :w]
    sig = np.abs(H)**2
    print(prb, 10*np.log10((np.abs(X-H)**2).sum()/sig.sum()),
          10*np.log10((np.abs(out-H)**2).sum()/sig.sum()))
EOF
```

## 7. 转换与入库（macOS）

```bash
python $AI_TRAIN/convert_coreml.py ~/capture/site_<日期>/helena_pusch52_sm_real \
    ~/capture/site_<日期>/helena_pusch52_real.mlpackage --shape 624
mkdir -p /tmp/mlc && xcrun coremlcompiler compile \
    ~/capture/site_<日期>/helena_pusch52_real.mlpackage /tmp/mlc
# 入库（保留 incumbent 供回退；新模型单独命名）：
cp -R /tmp/mlc/helena_pusch52_real.mlmodelc \
    $AI_TRAIN/../ai_assets/helena_pusch52_real.mlmodelc
cd <ocudu 仓库根目录> && git add -A && git commit -m "G-5: new site-tuned 52 model" && git push
```

## 8. A/B 性能对比（实机）

```bash
cd <ocudu 仓库根目录>

# A 腿：incumbent（默认路径）
sudo OCUDU_CE_TIME=1 ./build/apps/gnb/gnb -c configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena
# → ping 100 + iperf3 UL ×3 → Ctrl-C → cp /tmp/gnb.log /tmp/gnb_ab_old.log

# B 腿：站点微调模型（用 CLI 指定路径，不改默认）
sudo OCUDU_CE_TIME=1 ./build/apps/gnb/gnb -c configs/gnb_rf_b200_tdd_n78_20mhz.yml_iPhone17 \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena \
  --pusch_channel_estimator_helena_model_path_52 \
  <绝对路径>/ai_assets/helena_pusch52_real.mlmodelc
# → 同流程 → cp /tmp/gnb.log /tmp/gnb_ab_new.log

# 指标提取：
for f in /tmp/gnb_ab_old.log /tmp/gnb_ab_new.log; do
  echo == $f; grep "CRC.indication" $f | grep -oE "tb_status=(OK|KO)" | sort | uniq -c
done
```

判定：B 腿首传 CRC OK 率与 iperf 吞吐中位显著高于 A 腿（历史参考：+30 pp /
+81%）。两腿必须同配置、同位置、背靠背。

## 9. 上线运行

晋升通过的模型替换默认资产（或保留双模型按站点配置）：

```bash
# 方式 A：替换默认（影响所有用默认路径的站点）
cd <ocudu 仓库根目录>
cp -R ai_assets/helena_pusch52_real.mlmodelc ai_assets/helena_pusch52_ml.mlmodelc.new
mv ai_assets/helena_pusch52_ml.mlmodelc ai_assets/helena_pusch52_ml.mlmodelc.prev
mv ai_assets/helena_pusch52_ml.mlmodelc.new ai_assets/helena_pusch52_ml.mlmodelc
cmake --build build --target gnb -j8        # 路径是编译期烘焙的，重建生效
git add -A && git commit -m "promote site-tuned 52 model to default" && git push

# 方式 B：按站点配置（不改默认，站点配置里指定）——在 gnb 的 yaml 配置加：
#   expert_phy:
#     pusch_channel_estimator_helena_model_path_52: "<绝对路径>/helena_pusch52_real.mlmodelc"
```

热加载：`port_channel_estimator_helena_impl::reload()` API 已就绪（训练 sidecar
换文件 + 触发 reload，下一槽生效）；按站点配置（方式 B）更简单可靠。

## 10. 106-PRB 模型全套（差异点）

与 52 流程相同，仅宽度参数与运行时配置不同：

1. **合成基线数据**（若从零开始）：
   `gen_pusch_dataset.py 20000 3000 pusch106pad.npz --nfft 1272 --pad-aware --min-prb 53 --max-prb 106 --snr-min -5 --snr-max 55`
2. **采集**：与 §1 完全相同（dump 钩子自动按授权宽度落盘；要触发 106 桶，
   UE 需支持大授权——OAI UE 的 conf 里 `r = 106`，或手机大上传）；
3. **配对/重建/训练集**：与 §2-§4 完全相同（宽度从 meta 读出，无需改脚本）；
4. **微调**：`--prb 106`；初始化用 106 模型（`helena_pusch106_sm_hi` 或
   `transfer_from_51(52 模型, 106)` 迁移生成）；
5. **转换**：`--shape 1272`；入库 `helena_pusch106_real.mlmodelc`；
6. **运行时**：`--pusch_channel_estimator_helena_model_path_106 <路径>`
   （分桶分发自动把 53-106 PRB 授权路由到该模型；≤52 PRB 仍走 52 模型）。
7. **A/B/上线**：同 §8/§9（B 腿加 106 路径参数）。

## 附录：常见坑速查

- `sudo` 前的环境变量被 env_reset 过滤 → 变量放 sudo 后；
- `modulation_scheme` 枚举 = 每符号比特数（2/4/6/8），不是 1/2/3/4；
- NR LDPC 速率匹配 = 自然序循环缓冲（无 32 列交织器）；位交织逐码块；
- 重传（new_data=0）对的 tb/rx 可能时间错位 → 用质量门过滤；
- 采集必须 iPhone17 配置（gpsdo），stock 配置会时钟漂移导致接入不稳；
- 训练别和实机测试抢 CPU（nice + 限制线程）。
