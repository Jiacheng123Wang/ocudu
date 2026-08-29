# OAI UE 与 OCUDU gNB 互通 E2E 记录（ZMQ 虚拟链路 + UHD 真无线 OTA）

> 日期：2026-08-29
> 目标：OAI UE（Duranta 版）与 OCUDU gNB 通过 ZMQ 虚拟射频建立通信；随后用 USRP B210 实现真无线（OTA）接入。
> 参考：https://docs.ocudu.org/tutorials/oaiue/（官方教程，配置文件在 docs 仓库 assets/ 下）

## 0. 环境拓扑

| 角色 | 机器 | IP | 硬件/软件 |
|---|---|---|---|
| OCUDU gNB | macOS（本机） | 192.168.100.125 / 192.168.64.1 | OCUDU `dev_usrsctp` 分支（commit 52af9442b0），ZMQ 或 UHD(B210, UHD 4.10) |
| OAI UE | Ubuntu 24.04 (aarch64) | 192.168.64.2 | OAI `work` 分支（1bc13d7f0f），ZMQ |
| OAI UE | Ubuntu 24.04 (x86_64, jwang-dn) | 192.168.100.153 | OAI `work` 分支，UHD 4.6 + B210 (serial 2604127) |
| Open5GS 核心网 | 独立 VM | 192.168.100.127 | AMF N2 SCTP 38412，PLMN 00101，TAC 7 |

## 1. Ubuntu 上的 OAI UE 构建

### 1.1 网络问题（153 无法访问 GitHub）

- 症状：`git clone git@github.com:...` 超时；实际上**所有外网都不通**。
- 根因：残留的 **tun0/tun1 接口 + `0.0.0.0/1` + `128.0.0.0/1` 黑洞路由**（之前 Clash TUN 客户端停止后残留，无进程接收流量），优先级高于默认路由，把全部流量送入黑洞。
- 修复（需 sudo）：
  ```bash
  sudo ip route del 0.0.0.0/1 dev tun0
  sudo ip route del 128.0.0.0/1 dev tun0
  sudo ip link del tun0
  sudo ip link del tun1
  ```
- 恢复后流量走路由器（192.168.100.1，OpenClash 透明代理），GitHub 22/443 均可达。

### 1.2 依赖准备（离线环境的关键坑）

- **asn1c**：Ubuntu apt 的 `asn1c 0.9.28` **不支持 `-gen-APER`**（OAI cmake 的 `check_option` 会失败）。必须用 OAI 指定的 fork：
  `https://github.com/mouse07410/asn1c` @ `940dd5fa9f3917913fd487b13dfddfacd0ded06e`
  编译安装（`autoreconf -fi && ./configure --prefix=$HOME/asn1c-local && make && make install`），并把 `$HOME/asn1c-local/bin` 加入 PATH（或 `-DASN1C_EXEC=`）。
- **CPM 模块下载**：OAI 的 CMakeLists.txt 设置 `CPM_SOURCE_CACHE=~/.cache/cpm/`，首次配置会从 GitHub 下载 `CPM_0.40.1.cmake`。离线机器可从可联网机器拷贝整个 `~/.cache/cpm/` 目录。
- **simde**：`libsimde-dev`（`openair1/PHY/sse_intrin.h` 依赖），缺失时编译报 `simde/simde-common.h: No such file or directory`。
- 其他 dev 包：`libconfig-dev libconfig++-dev libfftw3-dev libmbedtls-dev libsctp-dev libzmq3-dev libyaml-cpp-dev libuhd-dev libboost-dev libcurl4-gnutls-dev libgcrypt20-dev libpcsclite-dev libssl-dev libtasn1-6-dev libusb-1.0-0-dev libx11-dev libxml2-dev`。
- 注意：153 的 apt 经 OpenClash 透明代理时，对 `cn.archive.ubuntu.com` 部分 deb 返回 **400 Bad Request**（代理 bug）。换阿里云镜像可绕过。

### 1.3 构建命令与陷阱

```bash
cd openairinterface5g/cmake_targets
PATH=$HOME/asn1c-local/bin:$PATH ./build_oai --nrUE -w USRP    # OTA 用 USRP；ZMQ 用 -w ZMQ
```

- `-w` 参数决定构建的**设备库 target**：`-w USRP` 只构建 `liboai_usrpdevif.so`，`-w ZMQ` 只构建 `liboai_zmqdevif.so`。
- **CMakeCache 陷阱**：`OAI_ZMQ`/`OAI_USRP` 的缓存值优先于 CMakeLists 默认值。旧缓存 `OAI_USRP=ON` 会导致找不到 UHD 失败；`OAI_ZMQ=OFF` 会导致没有 `liboai_zmqdevif.so`（运行时 `Library oai_zmqdevif couldn't be loaded`）。修改默认值后需同步缓存或删除 `CMakeCache.txt`。
- 设备库查找：loader 用 `dlopen("liboai_<name>.so")`，靠二进制 RUNPATH（构建目录）定位；不同构建目录的二进制与 .so 必须配套（有 buildversion 检查）。
- UE 运行需要 root（TUN 设备）：`sudo ./nr-uesoftmodem -O oaiue_xxx.conf`。

## 2. ZMQ 虚拟链路配置

### 2.1 架构（REP bind / REQ connect）

- OCUDU gNB：TX = `ZMQ_REP`（bind），RX = `ZMQ_REQ`（connect）。
- OAI UE：TX = `ZMQ_REP`（bind），RX = `ZMQ_REQ`（connect）。
- 配对（跨主机，官方教程端口 4556/4557）：

```
gNB TX (REP) bind  0.0.0.0:4556  <->  UE RX (REQ) connect <gNB-IP>:4556
gNB RX (REQ) connect <UE-IP>:4557 <->  UE TX (REP) bind  0.0.0.0:4557
```

### 2.2 gNB 配置（configs/gnb_zmq_oaiue.yaml）

```yaml
ru_sdr:
  device_driver: zmq
  device_args: tx_port=tcp://0.0.0.0:4556,rx_port=tcp://192.168.64.2:4557
  srate: 23.04
  tx_gain: -12      # macOS ZMQ 要求 gain <= 0；-12dB 回退为 OAI 定点 FFT 所需
  rx_gain: 0
```

- **macOS 移植版 ZMQ 强制 `gain <= 0.0 dB`**（`radio_session_zmq_impl.cpp` 的 set_tx_gain 校验），正增益直接报 `Channel gain must be <= 0.0 dB for ZMQ-device`。
- cell_cfg 关键项（须与 UE 匹配）：`dl_arfcn: 632628`（n78）、`channel_bandwidth_MHz: 20`、`common_scs: 30`、TDD DDDSU（`dl_ul_tx_period: 5, nof_dl_slots: 3, nof_dl_symbols: 10, nof_ul_slots: 1, nof_ul_symbols: 2`）、`csi_rs_enabled: true` + `nof_cell_csi_res: 1`（官方教程值）。
- 注意：OCUDU 文档 config 里 `addr`/`bind_addr`（单数）与 `addrs`/`bind_addrs`（复数）均被支持；本机配置用单数即可。

### 2.3 UE 配置（configs/oaiue_zmq.conf）

```c
r               = 51;
numerology      = 1;
band            = 78;
C               = 3489420000L;   // 必须加 L 后缀！
```

- **`C = 3489420000` 必须加 `L` 后缀**：值超过 INT32_MAX，libconfig 无后缀时按 32 位截断为负频率（`-805547296 Hz`），`get_freq_range_from_freq` 断言崩溃。官方教程配置无后缀是文档问题（或与特定 libconfig 版本相关）。
- `ue-scan-carrier = 1`（自动扫描 GSCN 找 SSB offset）、`E = 1`（3/4 FFT 采样率 23.04 Msps 匹配 gNB）、`uecap_file` 用绝对路径。
- `thread-pool = "-1,...,-1"` 保留官方配置默认。
- uicc0 凭据必须与核心网数据库一致（本项目沿用 srsUE 时代的 IMSI 001010123456789 + opc 全零）。

### 2.4 ZMQ E2E 结果（64.2 + macOS）

- UE 获取 IP `10.45.0.7`，AMF 注册完成。
- UL ping：网关 10.45.0.1 与互联网 8.8.8.8 均 0% 丢包。
- gNB 侧调度正常（c-rnti 0x4601，DRB1 F1-U/PDCP 活跃）。

## 3. UHD 真无线（OTA）配置

### 3.1 gNB 配置（configs/gnb_uhd_oaiue.yaml）

```yaml
ru_sdr:
  device_driver: uhd
  device_args: type=b200,num_recv_frames=64,num_send_frames=64   # B210 即 type=b200
  srate: 23.04
  otw_format: sc12
  tx_gain: 85
  rx_gain: 40
  amplitude_control:
    tx_gain_backoff: 0
```

- **TX 功率的隐藏衰减**：OCUDU 幅度控制器把 TX 信号归一化到
  `input_gain_dB = -10·log10(子载波数) - gain_backoff_dB`（20MHz/30kHz = 612 子载波 → -27.9dB，再加**默认 12dB backoff** → -39.9dBFS）。B210 输出能力本就有限（3.5GHz ~0dBm），信号到达 0.5m 外的接收机后完全淹没在噪声里。修复：`tx_gain_backoff: 0` + `tx_gain: 85`。
- 症状特征：手机（近场、高灵敏度）能接入，但另一台 B210（0.5m）完全收不到宽带信号（频谱分析只见噪声底）；CW 测试（uhd_siggen）却正常 —— 链路硬件没问题，是 TX 电平太低。
- B210 在 mac 上 UHD 4.10 正常（注意：uhd_find_devices 在设备被 gnb 占用时枚举不到，属正常）。

### 3.2 UE 配置（configs/oaiue_b210.conf）

```c
device = { name = "oai_usrpdevif"; };   // USRP 设备库
usrp-args = "type=b200";
ue-txgain = 0;      // OAI 映射: UHD增益 = 89.75 - ue_txgain，0 才是最大发射增益！
ue-rxgain = 110;    // OAI 减去 3.5GHz 校准偏移 45.25dB 后仍有 ~65dB 实际增益
initial-fo = 10000; // 预补偿 UE B210 时钟偏差（实测 ~2.8ppm -> ~9.8kHz @ 3.489GHz）
```

- **TX 增益映射是反的**（`radio/USRP/usrp_lib.cpp`）：`set_tx_gain(gain_range.stop() - tx_gain)`。`ue-txgain=80` 实际只有 9.75dB（PRACH 太弱，gNB 检测不到，RAR 失败）；`ue-txgain=0` 才是最大增益 89.75dB。
- **RX 增益有校准偏移**（`calib_table_b210_38` 在 3.5GHz 偏移 44dB，`rx_gain_offset≈45.25`）：`ue-rxgain=50` 实际只剩 4.75dB，信号低于 ADC 量化底 → 完全同步失败；`ue-rxgain=110`（默认值）实际 ~65dB。
- **CFO 预补偿**：gNB 有 GPS 时钟时，UE B210 free-run 时钟偏差（srsUE 时代测 2.82ppm）在 3.489GHz 约 +9.8kHz，`initial-fo = 10000` 预补偿后实测残留 CFO ≈ -20Hz。
- 同步失败排查顺序：先 `rsrp`/`PSS Corr` 确认信号到达（收不到 → 查 TX 功率/天线），再查 CFO（`Measured Carrier Frequency offset`），再查实时性（大量 `L` 字符 = deadline missed）。
- **天线**：两台 B210 的 SMA 天线需覆盖 3.5GHz；1.8GHz（FDD 时代）天线在 n78 严重失谐，会显著压缩链路余量（0.5m 近距离仍可工作）。

### 3.3 OTA E2E 结果（153 + macOS，真无线）

- UE 完成同步 → SIB1 解码 → PRACH → RAR → RRC 建立 → **IP 10.45.0.11**。
- 信号：SINR 16.5~20.6 dB，RSRP -96~-98 dBm；UL HARQ 0 错误。
- UL 数据面：网关 10.45.0.1 与互联网 8.8.8.8 均 0% 丢包。

## 4. 连接建立后的测试方法

```bash
# UE 侧（Ubuntu，TUN 接口由 OAI UE 自动创建）
ip addr show oaitun_ue1
ping -I oaitun_ue1 -c 4 10.45.0.1      # UL: 到核心网网关（UE_IP_BASE 的 .1）
ping -I oaitun_ue1 -c 4 8.8.8.8        # UL: 互联网（核心网需配置 NAT/转发）
iperf3 -c <核心网IP> -B <UE-IP> -t 10 -i 1          # UL 吞吐
iperf3 -c <核心网IP> -B <UE-IP> -t 10 -i 1 -R        # DL 吞吐

# 核心网侧（DL）
ping <UE-IP>                            # 例如 10.45.0.11
iperf3 -s -i 1                          # 先起服务端
```

- gNB 侧观察：`/tmp/gnb.log` 的调度统计（c-rnti、PUCCH/PUSCH、F1-U DRB 流量）、`[NR_MAC] UE x RNTI ... stats`。
- 核心网侧观察：AMF 日志 `gNB-N2 accepted`、`[Added] Number of gNB-UEs`、注册完成、PDU session 建立。
- 排查 UE 未入网的常见模式（教程 Troubleshooting）：
  - RACH 成功但随即 RRC Release → 核心网 subscriber 数据库（IMSI/key/opc）不匹配；
  - 注册完成但 PDU session 失败 → UE 的 DNN 与核心网 APN 不一致（默认 `internet`）；
  - 5QI 需 gNB 与核心网一致（默认 9）。

## 5. 注意事项与经验

1. **实时性（UE 刷 `L`/`Deadline missed`，gNB `underflow/late`）**：
   - 153 上曾有多进程吃满 CPU（`gcsctrl` 100%×14 天、`openvpn` 83%、`journald`/`rsyslogd` ~92%），UE 的 PHY 实时线程被抢占。清理后明显改善。
   - UE 日志风暴会加重 journald 负载，形成恶性循环；调低 `log_config.phy_log_level` 可缓解。
   - mac 上 gNB 的 UHD TX 偶发 underflow/late（macOS USB 调度），性能脚本（ocudu_performance）与提高进程优先级（nice）可缓解。
2. **OCUDU 的 ZMQ 执行模式**：不要启用 `execution_profile: dual/triple`（DL 生产会超过慢速 pull，累积 TX backlog，破坏 1-slot RAR 窗口）。
3. **手机是验证 gNB 的好工具**：商用手机（测试 SIM）能接入说明 gNB 小区/核心网链路正常，可快速区分 gNB 侧与 UE 侧问题。
4. **时钟**：两台 B210 均 free-run 时依赖 UE 的 `initial-fo` 预补偿；若 gNB B210 有 GPSDO，可配 `ru_sdr.clock: gpsdo` + `sync: gpsdo`，UE 侧实测残留 CFO 可到 -20Hz 量级。
5. **版本配套**：OAI UE 二进制与 `liboai_*devif.so` 必须同一次构建（loader 有 buildversion 校验）；不同构建目录（`./build` vs `cmake_targets/ran_build/build`）不可混用。
6. **git 子模块**：OAI 仓库 `openair2/E2AP/flexric` 子模块未初始化时，`git commit .` 会报 `does not have a commit checked out`；用显式路径提交（`git commit <file>...`）。
