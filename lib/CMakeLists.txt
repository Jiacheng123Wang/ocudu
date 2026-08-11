# SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
# SPDX-License-Identifier: BSD-3-Clause-Open-MPI

# Do not build by default.
set_directory_properties(PROPERTIES EXCLUDE_FROM_ALL TRUE)

########################################################################
# 1. 基础支撑库 (公共基础组件，协议栈运行必须)
########################################################################
add_subdirectory(asn1)        # 3GPP ASN.1 协议编解码 (RRC/NGAP/F1AP 必须)
add_subdirectory(ocudulog)    # 基础日志系统
add_subdirectory(ocuduvec)    # 向量与数学算法组件
add_subdirectory(support)     # 通用数据结构、定时器管理与系统支撑

########################################################################
# 2. 核心协议实体 (MAC, RLC, PDCP, SDAP, RRC)
########################################################################
add_subdirectory(mac)         # MAC 层协议实体
add_subdirectory(scheduler)   # MAC 调度器逻辑
add_subdirectory(rlc)         # RLC 层协议实体
# add_subdirectory(pdcp)        # PDCP 层协议实体
add_subdirectory(sdap)        # SDAP 层协议实体
add_subdirectory(rrc)         # RRC 层协议实体

########################################################################
# 3. 控制面与用户面逻辑及网络接口 (CU/DU 逻辑架构)
########################################################################
add_subdirectory(cu_cp)       # CU 控制面逻辑
add_subdirectory(cu_up)       # CU 用户面逻辑
add_subdirectory(du)          # DU 协议栈高层逻辑
add_subdirectory(ngap)        # 核心网 5GC NG-C 信令接口
add_subdirectory(f1ap)        # CU-DU 间 F1-C 信令接口 (支持网络切分与跨节点部署)
add_subdirectory(e1ap)        # CU-CP / CU-UP 间 E1 信令接口
add_subdirectory(gtpu)        # GTP-U 数据传输协议
add_subdirectory(f1u)         # CU-DU 间 F1-U 用户面接口

########################################################################
# 4. 可选组件与加密/压缩算法
########################################################################
add_subdirectory(rohc)        # PDCP 鲁棒性包头压缩 (ROHC)
add_subdirectory(pcap)        # PCAP 协议抓包分析组件
if (NOT DISABLE_MBEDTLS)
    add_subdirectory(security)# RRC/PDCP 鉴权与加解密算法
endif ()

########################################################################
# 5. 可选协议扩展组件 (若编解码无报错可开启，若缺少依赖可取消注释)
########################################################################
# add_subdirectory(e2)        # O-RAN E2 接口
# add_subdirectory(lpp)       # LPP 定位协议
# add_subdirectory(ntn)       # 非地面网络 (NTN 卫星) 扩展
# add_subdirectory(xnap)      # Xn 接口 (基站间切换)
# add_subdirectory(nrppa)     # NRPPa 定位接口
# add_subdirectory(nru)       # 5G NR-Unlicensed 扩展
# add_subdirectory(psup)      # 业务流辅助调度

########################################################################
# 6. 物理层、硬件驱动与底层 I/O 绑定模块 (macOS 第一阶段：彻底屏蔽)
########################################################################
# add_subdirectory(fapi_adaptor) # MAC-PHY FAPI 适配层 (依赖硬件/PHY 状态)
# add_subdirectory(phy)          # 物理层 DSP/信号处理 (极度依赖 Linux/AVX/DPDK/MKL)
# add_subdirectory(radio)        # SDR 射频硬件驱动 (UHD/Sidekiq)
# add_subdirectory(ru)           # Radio Unit 硬件抽象
# add_subdirectory(ofh)          # Open FrontHaul 前传 (DPDK/硬件绑核)
# if (DPDK_FOUND)
#     add_subdirectory(hal)      # 硬件抽象层
# endif (DPDK_FOUND)
# if (NOT DISABLE_SCTP)
#     add_subdirectory(gateways) # 网关/网络 Socket 处理 (依赖 Linux 系统 API)
# endif ()
# add_subdirectory(ran)          # RAN 底层物理组件
# add_subdirectory(instrumentation)
