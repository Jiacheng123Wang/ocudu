# G-5 每站在线自适应：手动操作手册（逐步命令）

> **2026-09-01 实机结论（阅读前必看）**：全部 A/B（单 UE/双 UE/多模型/多
> 曲线）表明 classical CE ≥ helena 于所有实测区间——**默认 CE 已切回 `cpu`**。
> helena 为 opt-in（`--pusch_channel_estimator_algo helena`），本手册的
> 采集/A/B 流程保留 helena 的用法（如需复现其行为），但新采集默认建议用 cpu。
> 完整证据见 `AI_CE_G5_online_adaptation_memo.md` §6/§7。

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

#   手机 attach 后：iperf3 上行 30s ×3（服务端在核心网主机）：
#   服务端（127 上）：iperf3 -s -B 10.45.0.1 -p 5201
#   手机客户端：iperf3 -c 10.45.0.1 -p 5201 -t 30
# 完成后 Ctrl-C 停 gnb；校验产物：
ls ~/capture/site_<日期> | wc -l        # 应见 dump_*/rx_*/tb_* + 3 个 meta.csv
```

采集钩子现在对**每个 PUSCH 授权**落盘（含 prb<6/跳频/高 SNR 回退槽——
`meta.csv` 的 engine_nsc=0 标记 NN 未运行）；`rx_meta.csv`（16 列）与
`dd_meta.csv`（5 列）尾部带 slot 列，配对按 slot 精确进行，不受异步解码
时延抖动影响。

采集质量提示：CRC-OK 率 16~40% 属正常（3408.96 邻道干扰时段波动）；干扰样本
本身是稀缺训练料。数据格式全量规范见 `AI_CE_G5_online_adaptation_memo.md` §3。

## 2. 配对

```bash
python $AI_TRAIN/pair_capture.py ~/capture/site_<日期>
# 输出 pairs.npz：tb_idx/rx_idx/ce_idx/n_prb，每 TB 最多 K=4 个候选（按时间最近排序；
# 新格式采集含 slot 列时按 slot 精确配对）。打印候选数分布（0/1/>=2）。
```

> 快捷方式：§2–§5（配对→重建→划分→微调）与转换（§7）可一条命令跑完——
> `VENV_PY=~/helena_venv/bin/python $AI_TRAIN/run_g5_pipeline.sh ~/capture/site_<日期>
> --prb 52|106 [--epochs N] [--lr R] [--no-convert]`。

配对为什么需要多候选：gNB 的 TB 落盘发生在其 LDPC 解码**完成**时（比授权晚
0.2–3 个时隙）。相邻时隙都有授权时，"时间最近"会把下一个授权的网格误配给该 TB
（历史上观察到的 529 µs 错位）。因此配对只出候选，**内容验证**（第 3 步）定胜负。

## 3. 标签重建（决策导向 + 候选内容验证）

```bash
python $AI_TRAIN/build_labels.py ~/capture/site_<日期> \
    --out ~/capture/site_<日期>/labels_all.npz
# 对每个 TB 把候选网格逐一重编码重建标签，取"标签 vs 输入"功率比最负（<-10 dB）
# 的候选——错配网格的比值 ~0 dB，真网格 ~-20 dB，天然分离。
# 输出 labels_all.npz {X, Y, width, snr_train, rank}，已按质量门过滤。
# rank 直方图显示经内容验证从非最近候选救回的标签数（rank>=1）。
```

## 4. 训练集划分（质量门已在第 3 步内置，这里只做划分）

```bash
python - <<'EOF'
import numpy as np, os
d = np.load(os.path.expanduser('~/capture/site_<日期>/labels_all.npz'))
X, Y, W = d['X'], d['Y'], d['width']
rng = np.random.default_rng(42); perm = rng.permutation(len(W))
nva = max(len(W)//10, 50); te, tr = perm[:nva], perm[nva:]
np.savez(os.path.expanduser('~/capture/site_<日期>/realtrain.npz'),
         X_train=X[tr], Y_train=Y[tr], width_train=W[tr],
         X_test=X[te], Y_test=Y[te], width_test=W[te])
print(f'train={len(tr)} test={len(te)}')
EOF
```

## 5. 微调（夜间）

```bash
# 从仓库自带的 init SavedModel 初始化（init_models/ 目录，独立于 ~/ai_ce_work）：
#   52 桶：init_models/helena_pusch52_sm_hi
#   106 桶：init_models/helena_pusch106_sm_hi
python $AI_TRAIN/train_pad.py ~/capture/site_<日期>/realtrain.npz \
    ~/capture/site_<日期>/helena_pusch52_sm_real \
    --prb 52 --init $AI_TRAIN/init_models/helena_pusch52_sm_hi \
    --epochs 6 --batch 32 --lr 1e-5
# 可选：--filter-snr-min N 只保留高 SNR 样本做聚焦 pass；--snr-weight 按
# 10^((snr-25)/10) 加权（需要 labels 含 snr_train，build_labels 已存档）。
```

> init SavedModel 的由来（复现链）：`gen_pusch_dataset.py` 合成数据 +
> `train_pad.py`（52 桶从 `helena_arch.transfer_from_51` 宽度迁移初始化；
> 106 桶同样由 52 权重零训练迁移），详见 `AI_CE_training_memo.md`。

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

> **先读这里（2026-08-31 勘误）**：RF 实机（n78 20 MHz @ 30 kHz）= **51 PRB**
> 天花板（3GPP 带宽表；srsRAN 不支持 n78@15 kHz），53–106 桶在 RF 链上永远
> 不会触发。**当前硬件上唯一能拿到真实 106-PRB PUSCH 授权的链路是 40 MHz
> ZMQ harness**（虚拟采样率 46.08 Msps，OAI UE r=106）。手机空口 106 数据
> 需等 40 MHz 载波的无线电（B210 不支持）或 15 kHz 重配。

**ZMQ 106 桶流程（当前硬件可行）**：

```bash
# gnb 侧（本机，40 MHz / 106 PRB 小区）：
sudo OCUDU_CE_TIME=1 OCUDU_HELENA_DUMP_DIR=<新目录> \
  ./build/apps/gnb/gnb -c configs/gnb_zmq_oaiue_40mhz.yaml \
  expert_phy --pusch_ldpc_decoder_type auto --pusch_channel_estimator_algo helena

# UE 侧（153，ZMQ 模式，r=106）：
sudo ./nr-uesoftmodem -O configs/oaiue_zmq_40m.conf   # 把仓库 configs/ 的 conf 拷过去
# （gnb 配置已预验证：bw=40 MHz, dl_ssb_arfcn=631680 自动落同步栅格）
# UE 接入后经 oaitun_ue1 跑 iperf3 上行（服务端在核心网主机），BSR 满缓冲即拿 106 PRB。
# 采集后：capture_qa.py 体检 → 确认 prb 53–106 有量 → §2–§7 流程加 --bucket 106。
```

**RF 20 MHz 流程（52 桶全覆盖，106 桶不触发）**：

1. **合成基线数据**（若从零开始）：
   `gen_pusch_dataset.py 20000 3000 pusch106pad.npz --nfft 1272 --pad-aware --min-prb 53 --max-prb 106 --snr-min -5 --snr-max 55`
2. **采集**：与 §1 完全相同。手机侧 iperf3 多流长跑——`iperf3 -c 10.45.0.1 -p 5201 -t 120 -P 4`
   （**不要加 `-w 1M`**——Android 的 socket 缓冲上限会钳制 setsockopt 导致
   "socket buffer size not set correctly" 报错退出；窗口大小与授权宽度无关，
   授权由 BSR 满缓冲驱动）。采集后先验授权分布：
   `python $AI_TRAIN/capture_qa.py ~/capture/site_<日期>`；
3. **配对/重建/训练集**：与 §2–§4 完全相同，`build_labels.py` 加
   `--bucket 106`（填充宽度 1272）；
4. **微调**：`--prb 106`；初始化用 `$AI_TRAIN/init_models/helena_pusch106_sm_hi`
   （106 合成基线，52 权重零训练迁移 + pad-aware 训练得来），小 lr 起步；
5. **转换**：`convert_coreml.py ... --shape 1272`；入库
   `ai_assets/helena_pusch106_real.mlmodelc`；
6. **运行时**：`--pusch_channel_estimator_helena_model_path_106 <路径>`
   （分桶分发自动把 53-106 PRB 授权路由到该模型；≤52 PRB 仍走 52 模型）。
7. **A/B/上线**：同 §8/§9（B 腿加 106 路径参数）。106 桶 A/B 的指标只看
   53–106 PRB 授权的首传 CRC 与大授权吞吐（≤52 PRB 走 52 模型，不混合比较）。

## 附录：常见坑速查

- `sudo` 前的环境变量被 env_reset 过滤 → 变量放 sudo 后；
- `modulation_scheme` 枚举 = 每符号比特数（2/4/6/8），不是 1/2/3/4；
- NR LDPC 速率匹配 = 自然序循环缓冲（无 32 列交织器）；位交织逐码块；
- 重传/相邻时隙授权的 tb/rx 时间错位 → 配对只出候选，`build_labels.py` 内容
  验证定胜负（rank 直方图可查救回数）；旧格式采集（无 slot 列）也走此路径；
- `build_labels.py` 默认读 `<采集目录>/pairs.npz`；用 `--pairs` 指定其他文件名；
- prb<6/跳频/高 SNR 回退槽在旧钩子里没有 CE 输入 dump（NN 未运行）→ 新钩子
  已改为全授权落盘（engine_nsc=0）；
- 采集必须 iPhone17 配置（gpsdo），stock 配置会时钟漂移导致接入不稳；
- **每次采集必须用全新目录**——多次 gnb 运行追加写入同一目录会把配对
  （slot/时间戳）搞乱（site7 教训：56 个 TB 只剩 5 个可配对）；
- 采集后先跑 `capture_qa.py <采集目录>` 体检：确认 prb/mod/engine_nsc 分布
  与 **CE 输入尺度**（|X| rms 应在 ~0.1–0.25 附近；差 10× 以上说明 rx_gain
  或 UE 功率异常，模型会 OOD——OAI UE 接入失败的根因）；
- 训练别和实机测试抢 CPU（nice + 限制线程）。
