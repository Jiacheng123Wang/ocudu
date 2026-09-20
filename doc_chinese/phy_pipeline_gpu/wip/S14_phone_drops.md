# 手机掉线：诊断（2026-09-20 下午的两条腿）

> ## ✅ 已定案（2026-09-20，用户实测）：**手机开了 WiFi**
>
> **手机开 WiFi 时，蜂窝的数据连接（PDU 会话）就会被手机自己拆掉；关掉 WiFi 一切正常。**
> 这解释了 §2 那条"会话建好 0.16–0.29 秒后就被拆"以及它前面 4.4 ms 的**上行 NAS**——
> 那条 NAS 就是手机发出的会话释放请求，AMF 只是照办。**gNB 与核心网都没有问题。**
> ⇒ **上腿之前先把手机的 WiFi 关掉**（这条已进 §5 与腿流程）。
>
> 下面保留原始诊断与判读方法（"怎么把'谁的锅'从日志里分出来"仍然通用）。

> **结论（原始）：这两条腿的"连上就 release"来自 5G 核心网，不是 gNB。**
> gNB 侧另有一个**独立的**实时性问题（见 §4），它让这两条腿按本线的判据**无效**，
> 但它不解释会话被拆。
>
> 证据来源：`logs/gnb_gpu_s13p4-phases_0920_1602.log`（8.2 分钟，人工停止）、
> `logs/gnb_gpu_s13p4-phases_0920_1619.log`（**写这份时仍在运行**）。对照腿：
> `logs/gnb_gpu_s13p4-phases_0920_1013.log` / `_1015.log`（上午，同一 gNB 配置、同一手机）。

---

## 1. 一句话证据

**每个建起来的 PDU 会话，都在 0.16–0.29 秒后被核心网拆掉**；上午的两条腿里，
会话建起来之后一直活到人工停机。

| 腿 | PDU 会话建立 | 被释放 | 建后存活 |
|---|---|---|---|
| `1013`（上午，好） | 1 | **0** | 一直到 Ctrl+C（`NGReset`），期间 iperf 跑满 |
| `1015`（上午，好） | 1 | **0** | 同上 |
| `1602`（下午，坏） | 5 | **5** | 0.24 / 0.29 / 1.21 / 1.31 / 33.5 秒 |
| `1619`（下午，坏） | 3 | **3** | 0.164 / 0.209 / 0.208 秒 |

---

## 2. 原始序列（`1619`，ue=2，毫秒级）

```
08:20:03.264565 [NGAP] Tx UplinkNASTransport                (UE -> AMF)
08:20:03.276680 [NGAP] Rx PDUSessionResourceSetupRequest     (+12 ms)
08:20:03.323420 [NGAP] Tx PDUSessionResourceSetupResponse    (gNB 已把 DRB 配好，+47 ms)
08:20:03.483407 [NGAP] Tx UplinkNASTransport                 (UE -> AMF，+160 ms)
08:20:03.487806 [NGAP] Rx PDUSessionResourceReleaseCommand   (+4.4 ms)
```

同一跳里 gNB 侧一切正常，逐条可查：

* `RRC Reconfiguration Procedure finished successfully`（UE 回了 `rrcReconfigurationComplete`）；
* `PDU Session Resource Setup Procedure finished successfully`；
* 就在释放前几毫秒，`PDSCH rnti=0x460b mod=256QAM`、`PUCCH ack=1`、`PUCCH format=2 csi1=1111`。

⇒ **拆会话的那条消息是 AMF 发给 gNB 的**（`Rx PDU … PDUSessionResourceReleaseCommand`），
而它前面 4.4 ms 有**一条 UE 发往 AMF 的 NAS 消息**。gNB 读不到 NAS 内容 ⇒
"是手机自己请求拆"还是"核心网自己决定拆"，**只能靠 NAS/NGAP 抓包判定**。

## 3. "连上后立即 release"的另一种形态：AMF 直接拒绝

`1602` 与 `1619` 里各有若干次，AMF 在收到 `InitialUEMessage` 后**十几到几十毫秒**就发
`UEContextReleaseCommand`，中间**没有** `InitialContextSetupRequest`：

```
1619: 08:19:58 Tx ue=1 InitialUEMessage
      08:19:58 Rx ue=1 UEContextReleaseCommand        <-- 没有任何上下文建立
1602: 08:02:36.912525 Tx ue=0 InitialUEMessage
      08:02:36.926987 Rx ue=0 UEContextReleaseCommand  (+14 ms)
```

gNB 在这两次之间**只发过一条消息**（`InitialUEMessage`）⇒ **不是 gNB 造成的**。

## 4. gNB 侧确实有的问题（独立，且让腿无效）

| 腿 | CRC OK/KO | `Real-time failure in RF` | 其它 |
|---|---|---|---|
| `1013`（上午） | 14566 / 512（**96.6%**） | **0** | — |
| `1015`（上午） | 15216 / 614（96.1%） | **0** | — |
| `1602`（下午） | 251 / 784（**24.2%**） | **576 underflow + 210 late** | +13 `Downlink data late for sector`、+13 `Discarding error indication` |
| `1619`（下午） | 140 / 298（32.0%） | 48 underflow + 11 late | — |

历史规律（`leg_report.sh` 扫全部腿）：**RTF = 0 的腿 CRC 都 ≥ 85%；RTF ≥ 200 的腿 CRC 都在 33% 上下。**
⇒ 这两条腿落在"坏"的那一档，**按本线的判据（RF failure 0）它们本来就不算腿**。

**但它不是会话被拆的原因**：`1602` 的第一次 RF 失败在 `08:05:08`，而第一次会话拆除在 `08:02:43`
——**拆会话早了 2.5 分钟**，那时 RTF 还是 0。

### 4.1 这两条腿里 gNB 侧"没坏"的部分（免得误伤）

* 契约 **8/8 MET**、`host device data crossings` **0.00 + 0.00**、`ce device estimates 11385 device / 0 host`；
* DFT residency 中位 **422.0 µs**（上午 417.8 µs）——前端没变慢；
* DL 反馈：`PUCCH ack=1` 264 次、**`ack=0` 0 次**、`csi1` 有正常值 ⇒ **DL 是通的**。

### 4.2 一个顺带的口径修正

`busy split ch_wt`：上午 **383.1 µs**，下午 **564.0 µs**（`eq_demap` 72.4 → 116.0）。
两边**跑的 kernel 完全相同**（契约逐项相同、每 lane 的 dispatch 数相同：equalizer 12 次/lane）。
⇒ **空中 `ch_wt` 的"跨腿 ±2%"不成立**——它随腿变，上午四条腿接近只是那四条腿的负载接近。
（这不影响 §5.8.15 的结论：**离线**协议仍然是 `--repeat 1` + 中位数。）

## 5. 下一步（判定顺序）

> **★ 已由用户解决**：原因是**手机开 WiFi** —— 关掉 WiFi 后手机就不再拆蜂窝会话。
> 下面 1–3 条保留为**方法**（下次再遇到"谁的锅"时按这个顺序分），不需要再跑；
> 第 4 条（跑腿前确认 RF failure 回到 0）**仍然成立**。

0. **上腿之前：关掉手机的 WiFi**（并在跑腿记录里写一行）。
1. ~~拿到 NAS/NGAP 原因码~~（方法留存）：把配置里 `pcap.ngap_enable` 改成 `true`
   （`ngap_filename: /tmp/gnb_ngap.pcap`），跑一次，用 Wireshark 看
   `PDUSessionResourceReleaseCommand` 的 cause，以及它前面那条 **UE → AMF 的 NAS** 是什么消息。
   （`all_level: debug` **没有用**：NGAP 的 PDU 日志只打消息名，不打 cause——这次已经确认过。）
2. **查核心网**（192.168.64.3）：`systemctl status open5gs-amfd open5gs-smfd open5gs-upfd`；
   `journalctl -u open5gs-smfd -u open5gs-upfd --since "16:00"`；`ip addr show ogstun`；
   UPF 的会话表/地址池。**"建起来立刻拆"最典型的原因是 SMF↔UPF（N4）或 UPF 本身不工作。**
3. **查手机**：`1602` 里手机做了 **107 次 PRACH**（上午只有 8 次）⇒ 它在反复重连；
   看它是否显示"已连接但无互联网"，以及 APN/移动数据设置。
4. 核心网修好之后再跑腿；**跑之前先确认 `RF failure` 那一档回到 0**（见 §4），
   否则腿跑出来的数还是不能用。

## 6. 复跑命令

```bash
# 每腿的头条数字（CRC / RTF / ch_wt / 契约）
bash doc_chinese/phy_pipeline_gpu/wip/leg_report.sh doc_chinese/phy_pipeline_gpu/wip/logs/<leg>.log

# 会话建立 -> 释放的延迟（本文 §1 那张表就是它算的）
python3 - doc_chinese/phy_pipeline_gpu/wip/logs/<leg>.log <<'PY'
import re, sys, datetime
setup, rel = [], []
for line in open(sys.argv[1], errors='replace'):
    m = re.match(r'(\S+)\s', line)
    if not m: continue
    try: t = datetime.datetime.fromisoformat(m.group(1))
    except ValueError: continue
    if 'Tx PDU' in line and 'PDUSessionResourceSetupResponse' in line: setup.append(t)
    if 'Rx PDU' in line and 'PDUSessionResourceReleaseCommand' in line: rel.append(t)
for s in setup:
    nxt = [r for r in rel if r >= s]
    print(f"setup {s.time()} -> release {nxt[0].time()} (+{(nxt[0]-s).total_seconds():.3f}s)" if nxt else f"setup {s.time()} -> (none)")
PY

# NGAP 消息序列
grep -E "\[NGAP *\] \[I\] (Tx|Rx) PDU" <leg>.log | \
  sed -E 's/^([0-9-]+T[0-9:]+)\.[0-9]+.*\[NGAP *\] \[I\] (Tx|Rx) PDU(.*)/\1 \2\3/' | \
  sed -E 's/ue=([0-9]+) ran_ue=[0-9]+( amf_ue=[0-9]+)?: /ue=\1 /'
```
