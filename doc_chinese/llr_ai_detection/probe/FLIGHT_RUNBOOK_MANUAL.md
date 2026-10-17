# 手动飞行手册（深度-3 AI 接收机 · 闭环效用验收）

> **一次只飞一条腿**，每条腿都**先起 gNB、等手机能 ping 通、再起流量**。
> 这份手册按"你可以照着一步步敲"的程度写：每条腿都给了**完整命令**、**该看到什么**、**看到什么算失败**。
> 判据与预期在 §8；故障排查在 §9（都是真实踩过的）。

---

## 0. 你要准备什么

| 项 | 说明 |
|---|---|
| 手机（UE） | 能附着、能跑 `iperf3 -s`（前几次都是这么飞的）|
| 本机（Mac） | 跑 gNB，射频是 B200；**需要 sudo 密码**（每条腿开始时输一次，`sudo -v` 会缓存 ~5 分钟）|
| CN 机 | `jwang@jwang-dn`，跑 `ping` / `iperf3`，它的 `ogstun`（10.45.0.1/16）通到手机 |
| 时间 | 第一批 4 腿 ≈ 15 分钟；第二批 8 腿 ≈ 30 分钟；**两批之间手机要重新附着** |
| 位置 | **整批不动**：手机位置、朝向、周围的人和物都别变 |

**三个批次参数（每次照抄，别自己改）**

- 第一批 `r9r`（接收机对照）：集合小区 = `configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml`，腿序 `a c c a`
- 第二批 `r9c`（标定 / R6）：投递小区 = `configs/gnb_rf_b200_tdd_n78_20mhz.yml`，腿序 `a b c d d c b a`

**臂的含义**

| 臂 | 含义 |
|---|---|
| `a` | AI 臂 + 现役产物（未校准）—— 当前默认 |
| `b` | AI 臂 + 校准产物 |
| `c` | 经典链（`backend=classic`，没有模型） |
| `d` | AI 臂 + 校准产物 + `--pusch_reporting_source=net`（R6 的交付形态） |

---

## 1. 开始之前（一次性，约 1 分钟）

```bash
cd ~/dev/ocudu

# 1) 手机先附着，然后拿到它的 IP（见 §2.1），代入下面的 <IP>
# 2) 自检：必须看到 GO（唯一允许的失败项是"手机回 ping"，那说明还没附着）
bash doc_chinese/llr_ai_detection/probe/preflight_check.sh both <IP>
```

自检会检查：工作树干净、`gnb` 二进制比源码新、两个 bundle 存在、校准产物确实声明了 `depth3_sinr_cal`、
两个小区都是 n78/20MHz、采集小区钉死 MCS 而投递小区不钉、没有 gNB 在跑、磁盘够、shell 里没有残留的采集变量。

**任何 `FAIL` 都先修掉再飞** —— 每一项都对应一次白飞。

---

## 2. 每条腿的固定六步

### 2.1 拿到手机 IP

**用你在 CN 机上惯用的那条命令**（历史飞行里你都是这么给的，以你的为准），**每批第一条腿之前查一次**，
同一批内不变；**两批之间会变**（每批都重启 gNB ⇒ 重新附着）。

若一时想不起来，下面两条在 CN 机上可用（都不需要 sudo）：

```bash
# ① 核心网为会话装的路由：出现 10.45.0.x 就是手机
ssh jwang@jwang-dn "ip route show dev ogstun | grep '10\.45\.0\.'"

# ② 手机产生过任何流量后，看与它相关的连接
ssh jwang@jwang-dn "ss -tun | grep -o '10\.45\.0\.[0-9]*' | sort -u"
```

补充：核心网的**控制台**（你开着的 `gcu5012` 容器终端）在会话建立时会打印会话与地址，可作对照；
手机自己的"关于本机 / 5G 数据连接"里也有。**三种里任一对上就够了**，对上之后整批都用它。

### 2.2 起 gNB（**必须最先**）

把 `<腿名>` / `<臂>` / `<小区>` 换成 §3、§5 里那一条的值。**臂 → 参数**：

| 臂 | 参数 |
|---|---|
| `a` | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc"` |
| `b` | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_cal.mlmodelc"` |
| `c` | `--expert_phy.pusch_receiver_backend=classic` |
| `d` | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_cal.mlmodelc" --expert_phy.pusch_reporting_source=net` |

```bash
cd ~/dev/ocudu
sudo -E LEG_CONFIG="$PWD/<小区>" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/<批名>" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <腿名> <该臂的参数>
```

**该看到（启动约 10–30 秒）**：

- `AI ARM ACTIVE over 4 model(s)`（臂 a/b/d）或 `receiver=classic`（臂 c）；
- 臂 b/d 还会多一行 `the published SINR is CALIBRATED by the bundle (… Qm=8 slope 1.137 offset +0.070 …)`；
- 之后是 `[metal_stats] …` 与槽计数在滚。

> `run_leg.sh` 会先做一次增量构建（约几十秒到 2 分钟，取决于改了没有）。**不要在这个阶段 Ctrl+C。**

### 2.3 等手机就绪（**别抢跑**）

在 CN 机上（或从 Mac 走 ssh）打一次 ping，**通了才起流量**：

```bash
ssh jwang@jwang-dn "ping -c 1 -W 2 <IP>"
```

看到 `1 received` ⇒ 就绪。没通就等 5–10 秒再来一次；**两分钟还不通** ⇒ 见 §9.2。

### 2.4 起流量（**逐字照抄**）

在 Mac 上一条命令跑完（它会自己 ssh 到 CN 机）：

```bash
ssh jwang@jwang-dn "ping -i 0.2 -c 200 <IP>; echo '--- iperf3 ---'; iperf3 -c <IP> -R -t 120"
```

等 **约 130 秒**。结束时打印里有 `iperf Done.`。

**三条不许犯**：
- **不许加 `-b`** —— 加了测的是你给的上限，不是链路容量（第 2 次飞行就是这么作废的）；
- **必须 `-R`** —— 那是上行（手机 → gNB），也就是我们接收的方向；
- **`ping` 与 `iperf3` 都用同一个 IP**，整批别换。

### 2.5 停 gNB

**在那台跑 gNB 的终端里按一次 Ctrl+C**（SIGINT，不是 `kill`），等它把收尾报告打完
（会打印 `[phy_pipeline] contract …`、`[depth3-timing] …`、`[depth3-sinr] …`）。
**等一下**：报告没打完就读日志会读到半条腿。

### 2.6 记录 + 自查这一腿

把这三个数记到 §6 的表里：

1. `iperf3` 的 **receiver** 行（Mbit/s），以及同一行的 **Retr**；
2. `ping` 的 `avg/mdev` 与丢包率（`0% packet loss`）；
3. 手机 IP。

再跑三行自查（腿名对应上一步）：

```bash
W=doc_chinese/llr_ai_detection/work_tmp
ls -t $W/legs/<批名>/*<腿名>*.log.stderr | head -1                     # 应该有日志
grep -ac 'DEGRADED to the classical chain' <上面那一行打印的路径>        # 期望 0；几百条说明大量回落
grep -a 'depth3-sinr' <路径> | tail -1                                  # 臂 a/b/d 应有一行；臂 c 没有
```

> **臂 a/b/d 没有 `[depth3-sinr]` 行** ⇒ 那条腿的公布值没被记录 ⇒ 见 §9.5。

---

## 3. 第一批：`r9r`（接收机对照，4 条腿）

- 小区：`configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml`（**MCS 钉死在 20**，两条臂面对同一码率）
- 回答的问题：**同一码率下，AI 接收机能不能扛起更多流量**
- 腿序（镜像）：`r9ra1`(a) → `r9rc1`(c) → `r9rc2`(c) → `r9ra2`(a)

逐条命令（把 `<IP>` 换成 §2.1 的地址；**每条腿都从 2.2 重新开始**）：

```bash
# 腿 1/4：r9ra1，臂 a（AI 臂 + 现役产物）
sudo -E LEG_CONFIG="$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/r9r" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu r9ra1 \
    --expert_phy.pusch_receiver_backend=ai \
    --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc"

# 腿 2/4：r9rc1，臂 c（经典链）
sudo -E LEG_CONFIG="$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/r9r" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu r9rc1 \
    --expert_phy.pusch_receiver_backend=classic

# 腿 3/4：r9rc2，臂 c（再次经典链）
sudo -E LEG_CONFIG="$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/r9r" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu r9rc2 \
    --expert_phy.pusch_receiver_backend=classic

# 腿 4/4：r9ra2，臂 a（再次 AI 臂）
sudo -E LEG_CONFIG="$PWD/configs/gnb_rf_b200_tdd_n78_20mhz_llr_collect_p0default.yml" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/r9r" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu r9ra2 \
    --expert_phy.pusch_receiver_backend=ai \
    --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc"
```

> 每条腿之间的间隔尽量短（手机位置不变、信道不要漂）。四条腿的 `ping` RTT 相差应 **≤10%**。

---

## 4. 两批之间：手机重新附着

第二批会**重启 gNB** ⇒ 手机会**重新附着**、**拿到新 IP**。

1. 等第一批第 4 条腿完全停下；
2. 手机若不自动重连，开关一次飞行模式；
3. **重新取 IP**（§2.1），**新 IP 用于第二批全部 8 条腿**；
4. 再跑一次 `ping -c 1 <新IP>` 确认可达。

> 用旧 IP 的腿**跑起来完全正常**，只是测到一个**没有对端**的闭环 —— 什么也没测到，而且看不出来。这是最容易犯的错。

---

## 5. 第二批：`r9c`（标定 / R6，8 条腿）

- 小区：`configs/gnb_rf_b200_tdd_n78_20mhz.yml`（**MCS 自由**，OLLA 在环）
- 回答的问题：**校准过的公布值会不会损失吞吐**、**R6 的交付形态（`net`）是否无损**
- 腿序（镜像）：`r9ca1`(a) `r9cb1`(b) `r9cc1`(c) `r9cd1`(d) `r9cd2`(d) `r9cc2`(c) `r9cb2`(b) `r9ca2`(a)

命令模板（只列不同处，其余同 §2.2）：

```bash
# 通用：先 cd ~/dev/ocudu，然后
sudo -E LEG_CONFIG="$PWD/configs/gnb_rf_b200_tdd_n78_20mhz.yml" \
        LEG_LOGDIR="$PWD/doc_chinese/llr_ai_detection/work_tmp/legs/r9c" \
  bash doc_chinese/phy_pipeline_gpu/wip/run_leg.sh gpu <腿名> <臂参数>
```

| 腿 | 臂 | 臂参数（照抄） |
|---|---|---|
| `r9ca1` | a | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_ft.mlmodelc"` |
| `r9cb1` | b | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_cal.mlmodelc"` |
| `r9cc1` | c | `--expert_phy.pusch_receiver_backend=classic` |
| `r9cd1` | d | `--expert_phy.pusch_receiver_backend=ai --expert_phy.pusch_receiver_model="$PWD/doc_chinese/llr_ai_detection/work_tmp/data/models/d3_modcond_cal.mlmodelc" --expert_phy.pusch_reporting_source=net` |
| `r9cd2` | d | 同 `r9cd1` |
| `r9cc2` | c | 同 `r9cc1` |
| `r9cb2` | b | 同 `r9cb1` |
| `r9ca2` | a | 同 `r9ca1` |

> 臂 b/d 启动时**必须有** `the published SINR is CALIBRATED by the bundle` 那一行；没有就说明用错了 bundle。

---

## 6. 记录表（填这个给我）

**第一批 `r9r`（采集小区，MCS 20）**

| 腿 | 臂 | 手机 IP | ping avg/mdev (ms) | 丢包 | **iperf3 receiver (Mbit/s)** | Retr | 备注 |
|---|---|---|---|---|---|---|---|
| r9ra1 | a | | | | | | |
| r9rc1 | c | | | | | | |
| r9rc2 | c | | | | | | |
| r9ra2 | a | | | | | | |

**第二批 `r9c`（投递小区，MCS 自由）**

| 腿 | 臂 | 手机 IP | ping avg/mdev (ms) | 丢包 | **iperf3 receiver (Mbit/s)** | Retr | 备注 |
|---|---|---|---|---|---|---|---|
| r9ca1 | a | | | | | | |
| r9cb1 | b | | | | | | |
| r9cc1 | c | | | | | | |
| r9cd1 | d | | | | | | |
| r9cd2 | d | | | | | | |
| r9cc2 | c | | | | | | |
| r9cb2 | b | | | | | | |
| r9ca2 | a | | | | | | |

日志已经在 `doc_chinese/llr_ai_detection/work_tmp/legs/r9r` 与 `…/r9c` 里，**不用拷**。

---

## 7. 飞完之后

把 §6 的两张表（或六/八条腿的 `iperf3` receiver 数字）发给我即可，我这边一条命令出裁决：

```bash
# 第一批：接收机对照
doc_chinese/llr_ai_detection/probe/flight_verdict.py \
  --legs doc_chinese/llr_ai_detection/work_tmp/legs/r9r \
  --iperf "r9ra1=?,r9rc1=?,r9rc2=?,r9ra2=?" \
  --ping  "r9ra1=?,r9rc1=?,r9rc2=?,r9ra2=?" --batch r9r

# 第二批：标定 / R6
doc_chinese/llr_ai_detection/probe/flight_verdict.py \
  --legs doc_chinese/llr_ai_detection/work_tmp/legs/r9c \
  --iperf "r9ca1=?,r9cb1=?,r9cc1=?,r9cd1=?,r9cd2=?,r9cc2=?,r9cb2=?,r9ca2=?" --batch r9c
```

它会给每腿：可比性依据（经典 SNR 或 ping RTT，并注明是哪一种）、公布 SINR（原始 → 校准）、
时延、退化数，以及裁决。

> **两条会被拒绝的情形**（都是防止把错的数当结论）：
> - **某条腿没给数字** ⇒ 打印 `NO VERDICT: … have no throughput number`，**不出裁决**（补齐再跑）；
> - **同一个腿名出现两次**（两次会话混在一个批次目录）⇒ `REFUSING to read this batch`。重飞请换批次名（`r9` → `r10`）。

---

## 8. 判据与预期（**飞行前已写死，不许事后改**）

**第一批 `r9r`（接收机对照）**

- **定量预期**：采集小区下每条 PUSCH 都是 256QAM；接收侧 AI **300/300** vs 经典 **125/300**；
  而固定 MCS + 独立重传时，**闭环比恰好等于每尝试成功率**（与 HARQ 上限无关）⇒
  **经典腿应约为 AI 腿的 42%**，裁决应读 **"AI arm is BETTER by ≈58%"**。
- 偏差**明显小** ⇒ HARQ/RLC 把失败补回来了，或腿里不是 256QAM 为主；偏差**明显大** ⇒ 经典链还付了 RLC 重传/超时。
- 判据：**c ≥ a** 才算"接收机不亏"；**a ≫ c** ⇒ 接收机**明确有益**，写进结论。

**第二批 `r9c`（标定 / R6）**

- 预期 **B ≈ A**（只差公布 SINR 的标定，OLLA 会吸收恒定偏差）；**D ≈ B**（`net` 与 `classic` 公布值差 ~0.1 dB）。
- 判据：**B ≥ A**、**D ≥ A**、以及 C 作参照（C 低于 A 是 AI 接收机赢）。
- **等效带 ±5%**：差值小于 5% 时裁决写 **"NO MEASURABLE DIFFERENCE"**（依据：同一臂两条腿自己就差 4.4–5.5%）。
- **可比性**：相邻腿经典 SNR 差 ≤1.5 dB，或 ping RTT 差 ≤10%；依据不同会标注。

---

## 9. 故障排查（都是真实发生过的）

### 9.1 起 gNB 时立刻报 `REFUSING`
| 报什么 | 原因 | 处置 |
|---|---|---|
| `working tree is dirty` | 有未提交改动 | `git status` 看；要么提交，要么 `LEG_ALLOW_DIRTY=1`（会打警告）|
| `not n78 / not 20 MHz` | `LEG_CONFIG` 指错文件 | 照 §3/§5 抄；**注意 `run_leg.sh` 的默认是 N1/5MHz/FDD** |
| `a gNB is already running` | 上一条腿没停干净 | `pkill -INT -f 'build/apps/gnb/gnb'`，等 3 秒 |
| `Invalid configuration` | 参数组合被 validator 拒 | `--pusch_reporting_source=net` 只能在 `backend=ai` 下用 |

### 9.2 手机两分钟还不通
1. 手机上开关一次飞行模式；
2. 看 gNB 日志里的 RACH/attach 行（`$W/legs/<批名>/*<腿名>*.log`）；
3. 确认用的是**当前**这批的 IP（§4）；
4. 都不行就停腿重来 —— **别在没对端的情况下起流量**，那条腿等于没测。

### 9.3 `iperf3` 立刻报错
- `Connection refused` ⇒ 手机上的 `iperf3 -s` 没在跑；
- `No route to host` ⇒ IP 不对，或手机没附着（§9.2）。

### 9.4 吞吐异常高、所有腿都一样（≈19 Mbit/s 且几乎不动）
**你加了 `-b`**。去掉重跑那一条腿。历史上整批就是因此作废的。

### 9.5 臂 a/b/d 的日志里没有 `[depth3-sinr]`
说明这条腿的日志里没有 1000 次接收（腿太短/流量没到），或读错了文件。
**读 `.log.stderr`，不是 `.log`**（`.log` 是 ocudulog，170 MB 级，里面没有臂的行）。

### 9.6 `LEG HEALTH` 报 "only partly the AI receiver"
该腿有 >1% 的接收**回落到经典链**（通常是 SNR 掉出 12–48 dB 的定义域）。方向仍对、幅度偏小；
先查那一腿的 SNR 与手机位置，再决定是否重飞。

### 9.7 停不下来
`Ctrl+C` 一次，等 10–20 秒。仍不退：`pkill -INT -f 'build/apps/gnb/gnb'`。
**不要用 `kill -9`**，否则收尾报告与 `[depth3-sinr]` 会缺。

---

## 10. 一句话版本

```bash
# 每条腿：
#   1) 起 gNB（先！）  2) ping 通手机  3) 跑 ping+iperf3（不许 -b、必须 -R）  4) Ctrl+C 停  5) 记数
# 第一批 4 腿用采集小区（a c c a），第二批 8 腿用投递小区（a b c d d c b a），两批之间手机重附着、换 IP。
```
