# 效用飞行操作手册（三臂 / 四臂，投递形态配置）

> 目标里唯一还没达成的一条是**同信道条件下的闭环吞吐**。这份手册把"怎么飞"固定下来，
> 因为前几次飞行有两次是**白飞**的：一次被 `-b 20M` 限速（两条臂都打满，什么也没测到），
> 一次用的配置把 **UL MCS 钉死在 20**（公布 SINR 根本没有下游，标定的效用恒为零）。
> 这两条现在都有**守卫**，但仍然值得读一遍再开始。

---

## 0. 前置（缺一不可）

0. **sudo 权限**：射频需要 root。飞行脚本会**每条腿**用 `sudo -E` 启 `run_leg.sh`（它用 `SUDO_USER` 把预检构建降回普通用户，
   不会在 `build/` 里留 root 文件）。本机 `sudo` **需要密码** ⇒ 每条腿会要一次密码；先跑一次 `sudo -n true` 看是否已缓存。
   *（第一版脚本漏了这一步，会在第一条腿的射频初始化处直接失败 —— 这轮已修。）*

1. **手机附着**，`ping` 通。UE 的 IP 每次 gNB 重启都会变（历史上出现过 `10.45.0.27`…`.39`）。
2. **本机没有 gNB 在跑**（脚本会拒绝）。
3. 手机与 gNB **位置固定**，整批不动；环境（人/物）也不要变。
4. 用**投递形态**配置：脚本默认 `configs/gnb_rf_b200_tdd_n78_20mhz.yml`
   （n78 / 20 MHz / 30 kHz / 51 PRB，MCS 0–28 + OLLA 在环）。**不要**用
   `..._llr_collect_p0default.yml` —— 它钉死 MCS，脚本会直接拒绝。

## 1. 两次飞行，不是一次

| 批次 | 配置 | 臂 | 回答什么 | 预期 |
|---|---|---|---|---|
| **`r9r`（先飞这个）** | **采集** `..._llr_collect_p0default.yml`（MCS 钉死 20）| `ARMS="a c c a"` | **AI 接收机 vs 经典链**，同一码率 | 若以 256QAM 为主：c **明显高于** a（接收侧 100% vs 41.7%）；若以 QPSK/16QAM 为主：打平 |
| `r9c` | **投递** `gnb_rf_b200_tdd_n78_20mhz.yml`（MCS 自由）| `ARMS="a b c d d c b a"` | 公布值**标定**与 **R6 交付形态** | **B ≈ A**（OLLA 吸收恒定偏差）、**D ≈ B** |

**为什么不能合成一次**：采集配置钉死 MCS ⇒ `b`/`d`（测公布值）在那里恒等于 `a`；而投递配置的自由 MCS 会把接收机差异抹平。
脚本按**臂**判断：`ARMS` 含 `b`/`d` 且配置钉死 ⇒ 拒绝；只有 `a`/`c` ⇒ 允许。

- 先飞 `r9r`：它不需要校准产物、对比度大、而且用的是**已经飞熟**的采集配置。
- 再飞 `r9c`：它才需要投递配置与校准产物。

## 2. 开始一批

**一条命令跑完两批**（推荐 —— 它替每一批选好小区，并在两批之间停下来要你重新附着的 IP）：

```bash
cd ~/dev/ocudu
UE_IP=<手机IP> bash doc_chinese/llr_ai_detection/probe/fly_both.sh r9
```

两批也可以分开跑（同样正确，只是各选一次小区）：

```bash
cd ~/dev/ocudu
# ① 接收机对照（采集配置，四腿，约 10 分钟）
LEG_CONFIG=configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml ARMS="a c c a" \
  UE_IP=<手机IP> bash doc_chinese/llr_ai_detection/probe/fly_utility_abba.sh r9r
# ② 标定 / R6（投递配置，八腿，约 20 分钟）
UE_IP=<手机IP> ARMS="a b c d d c b a" bash doc_chinese/llr_ai_detection/probe/fly_utility_abba.sh r9c
```

★ **两批之间手机必须重新附着**（每批都重启 gNB ⇒ **IP 会变**）；`fly_both.sh` 会停下来问你新 IP。
★ 别把上一批的 IP 带到下一批 —— 那会让第二条腿测到一个**没有对端**的闭环。

臂的含义：

| 臂 | 含义 | 为什么它在 |
|---|---|---|
| `a` | AI 臂 + **现役**产物（未校准）| 当前默认；标定前的基线 |
| `b` | AI 臂 + **校准**产物 | 考验标定（§19.172）|
| `c` | **经典链** | 参照：AI 臂同信道下亏不亏 |
| `d`（可选）| AI 臂 + 校准产物 + `reporting=net` | **R6 的交付形态**；预期 ≈ `b` |

顺序是**镜像**的（`a b c c b a` / `a b c d d c b a`），每条臂在前后半各出现一次，漂移对三条臂均摊。

## 2. 每条腿要做的流量（**逐字相同**）

腿一开起来（脚本打印 `=== leg r9a1`），在 **CN 机**（`jwang@jwang-dn`）上跑：

```bash
ping -i 0.2 -c 200 <手机IP>
iperf3 -c <手机IP> -R -t 120          # ★ 绝对不要加 -b
```

- **不要 `-b`**：加了测的是你给的上限，不是链路容量 —— 第 2 次飞行就是这么作废的。
- `-R` 是**上行**（接收端），也就是我们的 PUSCH 方向。
- 每条腿跑完，**Ctrl+C 停 gNB**，脚本会摘出本腿的指标并提示开下一条腿。

## 3. 要贴回来的东西

每条腿三样，缺一样就无法裁决：

1. `iperf3 -R` 的**接收端**结果（`Receiver` 行，Mbit/s）与 `Retr`；
2. `ping` 的 `avg/mdev` 与丢包率；
3. `UE_IP`（只记录一次也行，但要知道哪条腿对应哪个 IP）。

贴回 console 即可，我这边落盘入库。

## 4. 回来后怎么出裁决

```bash
# r9r（接收机对照）：把 ping RTT 一起给 —— 经典腿没有 [depth3-measure] 行，RTT 是唯一与接收机无关的可比性依据
doc_chinese/llr_ai_detection/probe/flight_verdict.py --legs doc_chinese/llr_ai_detection/work_tmp/legs/r9r \
  --iperf "r9ra1=?,r9rc1=?,r9rc2=?,r9ra2=?" --ping "r9ra1=?,r9rc1=?,r9rc2=?,r9ra2=?" --batch r9r

# r9c（标定/R6）
doc_chinese/llr_ai_detection/probe/flight_verdict.py --legs doc_chinese/llr_ai_detection/work_tmp/legs/r9c \
  --iperf "r9ca1=?,r9cb1=?,r9cc1=?,r9cc2=?,r9cb2=?,r9ca2=?" --batch r9c
```

★ **c 腿的读法**：它不是"候选方案"，而是"AI 接收机值不值得"的参照 ⇒ 裁决会写成
**"AI arm is BETTER by X%"** 或 **"AI arm is NOT better"**，不写 PASS/FAIL（那会读反）。
★ 若两腿的 SNR 依据不同、或某腿根本没有 SNR（经典腿就是这种），**可比性会自动改用 ping RTT**
（容差 10%），并在输出里写明用的是什么口径。

它给出每腿：经典 SNR（可比性依据）、公布 SINR（原始 vs 校准）、时延、退化，以及预登记裁决
（**B ≥ A**、**B ≥ C**）。语料不在时自检脚本会跳过；改过脚本的话先跑
`probe/selftest_flight_verdict.sh`。

## 5. 判据与预期（**飞行前写下，不许事后改**）

- **接收机对照（`r9r`）**：**c ≥ a** 才算"接收机不亏"；若 **a ≫ c** 则接收机**明确有益**，可写进结论。
- 下面三条只适用于 `r9c`。

- **B ≥ A** ⇒ 标定在闭环里不减吞吐。
- **B ≥ C** ⇒ AI 臂在同信道下不亏于经典链。
- **预期 B ≈ A**：OLLA 会吸收**恒定**偏差（+0.001 dB/ACK、cap ±5 dB、≈2 900 ACK ≈ 11 s）。
  所以 **B ≈ A 是预期之内**，不是"标定无效"；**大的** B−A 才是新信息（说明瞬态权重更大）。
- 若干条腿的经典 SNR 相邻差 **> 1.5 dB** ⇒ **那一轮不算数**（脚本会标 `NOT COMPARABLE`）。
- **等效带 ±5%**：差值小于 5% 时脚本打印 **"NO MEASURABLE DIFFERENCE"**，既不判通过也不判失败 ——
  依据是同一臂两条腿自己就差了 4.4%–5.5%（r8 批量），更小的差在这套设置里不可分辨。

## 6. 状态记录表（每飞一次填一行，贴进 `work_tmp/tmp/evidence/`）

| 批次 | 配置 | 臂 | 每条腿 Mbit/s | 经典 SNR | 公布 SINR（raw→cal）| 裁决 |
|---|---|---|---|---|---|---|
| r9 | delivery | a b c (d) | | | | |

## 7. 仍然开着的两个问题（与本次飞行无关，但别混进来）

- `net` 在**旧配置**下慢 12.6% 的原因**仍未知** —— 已证明与公布 SINR 无关（两种模式相同 + MCS 被钉死），
  候选是接收/授权量与飞行间条件。
- QPSK 的 RSRP/EPRE 有约 **1.4 dB** 的尾巴（11/60 条），已刻画为 **head 3 的公共尺度误差、只在高 SNR 侧**，
  SINR 不受影响（§19.181）。它只在 `net` 之下才会到达调度器，所以不影响 A/B/C 三条腿的吞吐判读。
