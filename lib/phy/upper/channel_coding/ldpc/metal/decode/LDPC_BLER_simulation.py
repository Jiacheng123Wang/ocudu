import numpy as np
import struct
import os
import matplotlib.pyplot as plt
import time

# --- 1. 基础工具 ---
def load_matrix_from_bin(filename, rows, cols):
    if not os.path.exists(filename):
        raise FileNotFoundError(f"找不到矩阵文件: {filename}")
    words_per_row = (cols + 31) // 32
    matrix = np.zeros((rows, cols), dtype=np.uint8)
    with open(filename, "rb") as f:
        for r in range(rows):
            for w in range(words_per_row):
                chunk = f.read(4)
                if not chunk: break
                word_val = struct.unpack("<I", chunk)[0]
                for b in range(32):
                    col_idx = w * 32 + b
                    if col_idx < cols:
                        matrix[r, col_idx] = (word_val >> b) & 1
    return matrix

def generate_llr(bits, snr_db):
    snr_linear = 10**(snr_db / 10.0)
    sigma = np.sqrt(1.0 / (2.0 * snr_linear))
    transmitted = 1.0 - 2.0 * bits.astype(float)
    noise = sigma * np.random.standard_normal(bits.shape)
    received = transmitted + noise
    return 2.0 * received / (sigma**2)

# --- 2. 核心译码：严格遵循原始博弈逻辑 ---
def collaboration_decoder_with_power(s_llr_A, p_llr_A, G, F, s_bias=1.0, max_iter=100, eta=0.8, power=2.0):
    curr_s_llr, curr_p_llr = s_llr_A.copy(), p_llr_A.copy()

    s_hard = (curr_s_llr < 0).astype(np.uint8)
    p_hard = (curr_p_llr < 0).astype(np.uint8)

    for it in range(max_iter):

        # 校验矛盾检测
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        if num_p_conflict == 0:
            return s_hard

        # --- 优化后的 S 空间更新 ---
        new_s_llr = curr_s_llr.copy()
        s_evidence = {} # 用于收集每个 S 比特的报错证据: {s_idx: [llr1, llr2, ...]}

        # 1. 收集所有报错方程的指控
        for j in np.where(p_conflict_mask)[0]:
            rel_s = np.where(G[j, :] == 1)[0]
            if len(rel_s) == 0: continue
            # 找到最弱嫌疑人
            i_sus = rel_s[np.argmin(np.abs(curr_s_llr[rel_s]))]
            
            if i_sus not in s_evidence:
                s_evidence[i_sus] = []
            s_evidence[i_sus].append(np.abs(curr_p_llr[j]))

        # 2. 融合证据并更新 S
        for i_sus, llr_list in s_evidence.items():
            all_c = np.where(G[:, i_sus] == 1)[0]
            # 陪审团票决逻辑保持不变
            conflict_ratio = np.sum(p_conflict_mask[all_c]) / len(all_c)

            # --- 核心改进：比例权重更新 ---
            # 更新强度由比例的幂函数决定，彻底消除“集体沉默”
            weight = np.power(conflict_ratio, power) 
            new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * (eta * weight) * np.mean(llr_list)
        
        # 更新 S 硬判决        
        s_hard = (new_s_llr < 0).astype(np.uint8)

        # --- P 空间更新同理 (可选) ---
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        num_s_conflict = np.sum(s_conflict_mask)

        if num_s_conflict == 0:
            return s_pred

        new_p_llr = curr_p_llr.copy()
        p_evidence = {}
        for i in np.where(s_conflict_mask)[0]:
            rel_p = np.where(F[i, :] == 1)[0]
            if len(rel_p) == 0: continue
            j_sus = rel_p[np.argmin(np.abs(curr_p_llr[rel_p]))]
            
            if j_sus not in p_evidence:
                p_evidence[j_sus] = []
            p_evidence[j_sus].append(np.abs(curr_s_llr[i]))

        for j_sus, llr_list in p_evidence.items():
            all_c = np.where(F[:, j_sus] == 1)[0]
            conflict_ratio = np.sum(s_conflict_mask[all_c]) / len(all_c)

            weight = np.power(conflict_ratio, power)
            new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * (eta * weight) * np.mean(llr_list)
     
        # 更新 P 硬判决        
        p_hard = (new_p_llr < 0).astype(np.uint8)
    
        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    # 最后一次死锁裁决
    p_satisfied_ratio = 1 - np.sum(p_conflict_mask) / len(p_conflict_mask)
    s_satisfied_ratio = 1 - np.sum(s_conflict_mask) / len(s_conflict_mask)
    if p_satisfied_ratio * s_bias > s_satisfied_ratio:
        return s_hard
    else:
        return p_hard

def collaboration_decoder_ultimate(s_llr_A, p_llr_A, G, F, s_bias=1.0, 
                                   max_iter=100, eta=0.8, 
                                   top_n=3, power=2.0):
    """
    top_n:  指控的嫌疑人数量 (建议设为 1, 3, 5...)
    power:  非线性权重系数 (power > 1 抑制随机噪声，增强强指控)
    """
    curr_s_llr, curr_p_llr = s_llr_A.copy(), p_llr_A.copy()

    s_hard = (curr_s_llr < 0).astype(np.uint8)
    p_hard = (curr_p_llr < 0).astype(np.uint8)

    actual_iters = 0
    for it in range(max_iter):
        actual_iters += 1

        # 1. 校验矛盾检测
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        if np.sum(p_conflict_mask) == 0:
            return s_hard, actual_iters

        # --- 2. S 空间：Top-N 证据收集 ---
        new_s_llr = curr_s_llr.copy()
        s_evidence = {} # {idx: [evidence_val1, ...]}

        for j in np.where(p_conflict_mask)[0]:
            rel_s = np.where(G[j, :] == 1)[0]
            if len(rel_s) == 0: continue
            
            abs_s_llrs = np.abs(curr_s_llr[rel_s])
            abs_p = np.abs(curr_p_llr[j])
            
            # 确定实际能抓出的嫌疑人数 (不能超过关联比特数)
            actual_n = min(top_n, len(rel_s))
            sorted_local_indices = np.argsort(abs_s_llrs)
            
            # 获取 Rank 1 和 Rank 2 的值，用于 Min-Sum 限制
            val1 = abs_s_llrs[sorted_local_indices[0]]
            val2 = abs_s_llrs[sorted_local_indices[1]] if len(rel_s) > 1 else abs_p

            for rank in range(actual_n):
                target_idx = rel_s[sorted_local_indices[rank]]
                
                # --- 核心：Min-Sum 证据强度限制 ---
                if rank == 0:
                    # Rank 1 的强度受 Rank 2 和 P 限制
                    strength = min(val2, abs_p)
                else:
                    # 其他 Rank 的强度受 Rank 1 和 P 限制
                    strength = min(val1, abs_p)
                
                if target_idx not in s_evidence: s_evidence[target_idx] = []
                s_evidence[target_idx].append(strength)

        # 汇总 S 更新 (应用 Power 权重)
        for i_sus, llr_list in s_evidence.items():
            all_c = np.where(G[:, i_sus] == 1)[0]
            # 计算指控比例
            # conflict_ratio = len(llr_list) / len(all_c)
            conflict_ratio = np.sum(p_conflict_mask[all_c]) / len(all_c)
            # 应用 Power 非线性映射
            weight = np.power(min(conflict_ratio, 1.0), power)
            
            new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * (eta * weight) * np.mean(llr_list)

        # 更新 S 硬判决        
        s_hard = (new_s_llr < 0).astype(np.uint8)

        # --- 3. P 空间：Top-N 证据收集 (对称) ---
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        if np.sum(s_conflict_mask) == 0:
            return s_pred, actual_iters
        new_p_llr = curr_p_llr.copy()
        p_evidence = {}

        for i in np.where(s_conflict_mask)[0]:
            rel_p = np.where(F[i, :] == 1)[0]
            if len(rel_p) == 0: continue
            
            abs_p_llrs = np.abs(curr_p_llr[rel_p])
            abs_s = np.abs(curr_s_llr[i])
            
            actual_n_p = min(top_n, len(rel_p))
            sorted_local_p = np.argsort(abs_p_llrs)
            
            pval1 = abs_p_llrs[sorted_local_p[0]]
            pval2 = abs_p_llrs[sorted_local_p[1]] if len(rel_p) > 1 else abs_s

            for rank in range(actual_n_p):
                target_p_idx = rel_p[sorted_local_p[rank]]
                strength = min(pval2, abs_s) if rank == 0 else min(pval1, abs_s)
                
                if target_p_idx not in p_evidence: p_evidence[target_p_idx] = []
                p_evidence[target_p_idx].append(strength)

        for j_sus, llr_list in p_evidence.items():
            all_c = np.where(F[:, j_sus] == 1)[0]
            # ratio = len(llr_list) / len(all_c)
            ratio = np.sum(s_conflict_mask[all_c]) / len(all_c)
            weight = np.power(min(ratio, 1.0), power)
            new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * (eta * weight) * np.mean(llr_list)

        # 更新 P 硬判决        
        p_hard = (new_p_llr < 0).astype(np.uint8)

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr
    
    # ========================================================
    # --- 终极改进：基于 LLR 加权置信度能量的死锁裁决 ---
    # ========================================================
    # 1. 评估 S 候选序列 (s_hard) 在 P 空间的表现
    # 使用当前 P 空间的 LLR 绝对值作为权重
    abs_p_llr = np.abs(curr_p_llr)
    total_p_energy = np.sum(abs_p_llr) + 1e-12
    # 只有不冲突 (satisfied) 的校验位贡献正向能量
    s_satisfied_energy = np.sum(abs_p_llr[~p_conflict_mask])
    s_weighted_score = s_satisfied_energy / total_p_energy

    # 2. 评估 P 候选序列 (p_hard) 在 S 空间的表现
    # 使用当前 S 空间的 LLR 绝对值作为权重
    abs_s_llr = np.abs(curr_s_llr)
    total_s_energy = np.sum(abs_s_llr) + 1e-12
    # 重新计算一次 P 的冲突情况 (确保使用最新的 s_hard)
    final_s_conflict = ((np.matmul(F, p_hard) % 2) != s_hard)
    p_satisfied_energy = np.sum(abs_s_llr[~final_s_conflict])
    p_weighted_score = p_satisfied_energy / total_s_energy

    # 3. 最终博弈裁决
    if s_weighted_score * s_bias >= p_weighted_score:
        return s_hard, actual_iters
    else:
        # 如果相信 P 空间，则输出 P 空间序列映射回 S 的结果
        return (np.matmul(F, p_hard) % 2).astype(np.uint8), actual_iters

def collaboration_decoder_v2_refined(s_llr_A, p_llr_A, G, F, s_bias=1.0, 
                                     max_iter=100, eta=0.8, 
                                     top_n=3, power=2.0, penalty_power=2.0):
    """
    精修版 V2：基于最小冲突能量 (MCE) 的硬裁决
    - penalty_power: 惩罚指数。2.0 代表平方惩罚，能显著压制强信号冲突。
    - s_bias: 默认为 1.0，保持两空间地位平等。
    """
    curr_s_llr, curr_p_llr = s_llr_A.copy(), p_llr_A.copy()
    s_hard = (curr_s_llr < 0).astype(np.uint8)
    p_hard = (curr_p_llr < 0).astype(np.uint8)

    # 预留冲突掩码变量
    p_conflict_mask = None
    
    for it in range(max_iter):
        # 1. 校验 S 空间
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        if np.sum(p_conflict_mask) == 0:
            return s_hard

        # --- S 空间更新 (保持你最有效的 top_n 逻辑) ---
        new_s_llr = curr_s_llr.copy()
        s_evidence = {}
        for j in np.where(p_conflict_mask)[0]:
            rel_s = np.where(G[j, :] == 1)[0]
            if len(rel_s) == 0: continue
            abs_s_llrs = np.abs(curr_s_llr[rel_s])
            abs_p = np.abs(curr_p_llr[j])
            # Min-Sum 修正
            sorted_idx = np.argsort(abs_s_llrs)
            v1 = abs_s_llrs[sorted_idx[0]]
            v2 = abs_s_llrs[sorted_idx[1]] if len(rel_s)>1 else abs_p
            for rank in range(min(top_n, len(rel_s))):
                idx = rel_s[sorted_idx[rank]]
                strength = min(v2, abs_p) if rank == 0 else min(v1, abs_p)
                if idx not in s_evidence: s_evidence[idx] = []
                s_evidence[idx].append(strength)

        for i_sus, llr_list in s_evidence.items():
            all_c = np.where(G[:, i_sus] == 1)[0]
            weight = np.power(np.sum(p_conflict_mask[all_c])/len(all_c), power)
            new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * (eta * weight) * np.mean(llr_list)
        
        s_hard = (new_s_llr < 0).astype(np.uint8)

        # 2. 校验 P 空间
        s_pred_from_p = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred_from_p != s_hard)
        if np.sum(s_conflict_mask) == 0:
            return s_pred_from_p

        # --- P 空间更新 ---
        new_p_llr = curr_p_llr.copy()
        p_evidence = {}
        for i in np.where(s_conflict_mask)[0]:
            rel_p = np.where(F[i, :] == 1)[0]
            if len(rel_p) == 0: continue
            abs_p_llrs = np.abs(curr_p_llr[rel_p])
            abs_s = np.abs(curr_s_llr[i])
            sorted_idx_p = np.argsort(abs_p_llrs)
            pv1 = abs_p_llrs[sorted_idx_p[0]]
            pv2 = abs_p_llrs[sorted_idx_p[1]] if len(rel_p)>1 else abs_s
            for rank in range(min(top_n, len(rel_p))):
                idx_p = rel_p[sorted_idx_p[rank]]
                strength = min(pv2, abs_s) if rank == 0 else min(pv1, abs_s)
                if idx_p not in p_evidence: p_evidence[idx_p] = []
                p_evidence[idx_p].append(strength)

        for j_sus, llr_list in p_evidence.items():
            all_c = np.where(F[:, j_sus] == 1)[0]
            weight = np.power(np.sum(s_conflict_mask[all_c])/len(all_c), power)
            new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * (eta * weight) * np.mean(llr_list)

        p_hard = (new_p_llr < 0).astype(np.uint8)
        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    # ========================================================
    # --- 最后一搏：最小冲突能量 (MCE) 裁决 ---
    # ========================================================

    # 计算 S 候选者的代价：复用最后一次迭代的 p_conflict_mask
    # 惩罚项 = sum( |涉及冲突的P位LLR| ^ penalty_power )
    s_penalty = np.sum(np.power(np.abs(curr_p_llr)[p_conflict_mask], penalty_power))

    # 计算 P 候选者的代价：
    # 重新同步一次 P 对当前最新 s_hard 的冲突情况
    final_s_conflict = ((np.matmul(F, p_hard) % 2) != s_hard)
    p_penalty = np.sum(np.power(np.abs(curr_s_llr)[final_s_conflict], penalty_power))

    # 谁的冲突代价小，谁胜出
    if s_penalty <= p_penalty * s_bias:
        return s_hard
    else:
        # 如果相信 P 空间，输出由 P 预测的结果
        return (np.matmul(F, p_hard) % 2).astype(np.uint8)

# --- 3. 仿真运行：修复变量冲突 ---
def run_sim_with_random_data(G, F, snrs, b_val, min_err_count=200):
    bler_results = []
    # 使用清晰的命名避免覆盖
    n_parity_dim, n_info_dim = G.shape
    
    print(f"\n>>> 启动仿真 [Bias={b_val}] 维度: {n_info_dim} x {n_parity_dim}")
    
    for snr in snrs:
        err_count = 0
        pkt_count = 0
        start_time = time.time()
        
        # 显式使用 pkt_count 和 err_count，避免与 p, e 混淆
        while err_count < min_err_count and pkt_count < 200000:
            pkt_count += 1
            
            # 生成随机 S 并通过 G 矩阵编码得到 P
            s_true = np.random.randint(0, 2, n_info_dim).astype(np.uint8)
            p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
            
            # 信道
            s_llr = generate_llr(s_true, snr)
            p_llr = generate_llr(p_true, snr)
            
            # 译码
            s_dec = collaboration_decoder_ultimate(s_llr, p_llr, G, F)
            
            # 判错
            if not np.array_equal(s_dec, s_true):
                err_count += 1
                
        bler = err_count / pkt_count
        bler_results.append(bler)
        duration = time.time() - start_time
        print(f"   SNR: {snr:4.1f} | BLER: {bler:.2e} | 样本: {pkt_count:4d} | 耗时: {duration:4.1f}s")
        
        if bler < 1e-5: break 
    return bler_results

def simulate_bler(G, F, snr_axis):
    bler_results = []
    uncoded_bler_results = [] # 新增：用于存储未编码 BLER
    
    n_info = G.shape[1]
    n_parity = G.shape[0]
    
    for snr in snr_axis:
        start_time = time.time()
        err_count = 0
        uncoded_err_count = 0 # 新增计数器
        pkt_count = 0
        
        # 统计逻辑：为了让曲线平滑，低 SNR 样本少，高 SNR 样本多
        min_err_count = 100
        max_packets = 2000
        
        while err_count < min_err_count and pkt_count < max_packets:
            pkt_count += 1
            
            # 生成数据
            s_true = np.random.randint(0, 2, n_info).astype(np.uint8)
            p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
            
            # 信道加噪
            s_llr = generate_llr(s_true, snr)
            p_llr = generate_llr(p_true, snr)
            
            # --- 1. 统计未编码 BLER (Uncoded) ---
            # 只要 s_llr 的硬判决与 s_true 不一致，该包即为错误包
            s_raw_dec = (s_llr < 0).astype(np.uint8)
            if not np.array_equal(s_raw_dec, s_true):
                uncoded_err_count += 1

            # --- 2. 统计译码后的 BLER ---
            s_dec = collaboration_decoder_ultimate(s_llr, p_llr, G, F)
            if not np.array_equal(s_dec, s_true):
                err_count += 1
                
        bler = err_count / pkt_count
        uncoded_bler = uncoded_err_count / pkt_count
        
        bler_results.append(bler)
        uncoded_bler_results.append(uncoded_bler)
        
        duration = time.time() - start_time
        print(f"   SNR: {snr:4.1f} | 译码 BLER: {bler:.2e} | 未编码 BLER: {uncoded_bler:.2e} | 样本: {pkt_count:4d} | 耗时: {duration:4.1f}s")
        
        # 如果译码 BLER 已经非常低，可以提前停止
        if bler < 1e-6 and pkt_count >= 5000: break 
        
    return bler_results, uncoded_bler_results

# --- 绘图部分修改 ---
def plot_results(snr_axis, bler_decoded, bler_uncoded):
    plt.figure(figsize=(8, 6))
    plt.semilogy(snr_axis[:len(bler_decoded)], bler_decoded, 'bo-', label='LDPC Collaboration Decoder', markersize=6)
    plt.semilogy(snr_axis[:len(bler_uncoded)], bler_uncoded, 'rx--', label='Uncoded (Raw Channel)', markersize=6)
    
    plt.grid(True, which='both', linestyle='--')
    plt.xlabel('SNR (dB)')
    plt.ylabel('Block Error Rate (BLER)')
    plt.title('BLER Performance Comparison')
    plt.legend()
    plt.show()

# --- 3. 仿真逻辑 ---
def simulate_bler_suite(G, F, snr_axis, iter_list):
    results = {}
    n_info = G.shape[1]
    
    # 先计算未编码 (Uncoded) BLER 作为基准
    uncoded_results = []
    print(">>> 正在统计 Uncoded BLER...")
    for snr in snr_axis:
        err = 0
        pkts = 2000 # 基准测试样本数可稍少
        for _ in range(pkts):
            s_true = np.random.randint(0, 2, n_info).astype(np.uint8)
            s_llr = generate_llr(s_true, snr)
            s_raw_dec = (s_llr < 0).astype(np.uint8)
            if not np.array_equal(s_raw_dec, s_true): err += 1
        uncoded_results.append(err / pkts)
    results['Uncoded'] = uncoded_results

    # 遍历不同的迭代次数
    for m_iter in iter_list:
        print(f"\n>>> 正在仿真 max_iter = {m_iter} ...")
        bler_list = []
        for snr in snr_axis:
            err_count = 0
            pkt_count = 0
            while err_count < 50 and pkt_count < 5000: # 快速对比参数
                pkt_count += 1
                s_true = np.random.randint(0, 2, n_info).astype(np.uint8)
                p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
                
                s_llr = generate_llr(s_true, snr)
                p_llr = generate_llr(p_true, snr)
                
                # 这里调用带迭代次数限制的译码器
                s_dec = collaboration_decoder_ultimate(s_llr, p_llr, G, F, s_bias=1.0, max_iter=m_iter)
                
                if not np.array_equal(s_dec, s_true):
                    err_count += 1
            
            bler = err_count / pkt_count
            bler_list.append(bler)
            print(f"   SNR: {snr:4.1f} | BLER: {bler:.2e} | 样本: {pkt_count}")
            if bler < 1e-4: break
        results[f'Iter {m_iter}'] = bler_list
        
    return results

# --- 4. 绘图 ---
def plot_comparison(snr_axis, results):
    plt.figure(figsize=(10, 7))
    styles = ['ko-', 'g^-', 'bs-', 'rd-', 'mv-']
    
    # 绘制 Uncoded
    plt.semilogy(snr_axis, results['Uncoded'], 'k--', label='Uncoded', alpha=0.6)
    
    # 绘制各迭代次数
    for i, (label, data) in enumerate(results.items()):
        if label == 'Uncoded': continue
        plt.semilogy(snr_axis[:len(data)], data, styles[i % len(styles)], label=label)
    
    plt.grid(True, which='both', linestyle='--', alpha=0.5)
    plt.xlabel('SNR (dB)')
    plt.ylabel('Block Error Rate (BLER)')
    plt.title('Collaboration Decoder Performance: Iteration Comparison')
    plt.legend()
    plt.show()

# --- 3. 仿真主循环 ---
def run_full_simulation(G, F, snr_axis, max_iter=100):
    bler_decoded = []
    bler_uncoded = []
    avg_iterations = []
    
    n_info = G.shape[1]
    
    for snr in snr_axis:
        start_time = time.time()
        err_decoded = 0
        err_uncoded = 0
        total_iters = 0
        pkt_count = 0
        
        # 为了保证统计意义，每个 SNR 至少跑 1000 个包
        while (err_decoded < 50 and pkt_count < 2000) or pkt_count < 200:
            pkt_count += 1
            s_true = np.random.randint(0, 2, n_info).astype(np.uint8)
            p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
            
            s_llr = generate_llr(s_true, snr)
            p_llr = generate_llr(p_true, snr)
            
            # 统计 Uncoded (未编码)
            s_raw = (s_llr < 0).astype(np.uint8)
            if not np.array_equal(s_raw, s_true):
                err_uncoded += 1
                
            # 统计 Decoded (译码后)
            s_dec, iters = collaboration_decoder_ultimate(s_llr, p_llr, G, F, s_bias=1.0, max_iter=max_iter)
            total_iters += iters
            if not np.array_equal(s_dec, s_true):
                err_decoded += 1
        
        bler_d = err_decoded / pkt_count
        bler_u = err_uncoded / pkt_count
        avg_it = total_iters / pkt_count
        
        bler_decoded.append(bler_d)
        bler_uncoded.append(bler_u)
        avg_iterations.append(avg_it)
        
        print(f"SNR: {snr:4.1f} | 译码 BLER: {bler_d:.2e} | 未编码 BLER: {bler_u:.2e} | 平均迭代: {avg_it:5.2f}")
        
    return bler_decoded, bler_uncoded, avg_iterations

# --- 4. 绘图 ---
def plot_all(snr_axis, b_dec, b_unc, avg_it):
    plt.figure(figsize=(12, 5))
    
    # 子图 1: BLER 曲线
    plt.subplot(1, 2, 1)
    plt.semilogy(snr_axis, b_unc, 'k--', label='Uncoded (Raw Channel)')
    plt.semilogy(snr_axis, b_dec, 'ro-', label='Collaboration Decoder')
    plt.grid(True, which='both', linestyle='--', alpha=0.5)
    plt.xlabel('SNR (dB)')
    plt.ylabel('BLER')
    plt.title('BLER Performance')
    plt.legend()
    
    # 子图 2: 平均迭代次数 (效率分析)
    plt.subplot(1, 2, 2)
    plt.plot(snr_axis, avg_it, 'bs-', label='Avg Iterations')
    plt.grid(True, linestyle='--', alpha=0.5)
    plt.xlabel('SNR (dB)')
    plt.ylabel('Iterations')
    plt.title('Decoding Complexity (Average Iterations)')
    plt.legend()
    
    plt.tight_layout()
    plt.show()


# --- 3. 新思路：三段式仲裁半次迭代译码器 ---
def triple_logic_half_iter_decoder(s_llr, p_llr, F, s_true, alpha=2.0, n_weak=2):
    """
    增加了误判统计的功能
    """
    n_info = len(s_llr)
    s_hard = (s_llr < 0).astype(np.uint8)
    p_hard = (p_llr < 0).astype(np.uint8)
    
    # s_pred = F * p (GF2)
    s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
    
    s_new = s_hard.copy()
    
    # 统计项
    stats = {
        'caseA_total': 0, 'caseA_wrong': 0, # 捞了多少，捞错（翻转后反而不对）多少
        'caseB_total': 0, 'caseB_wrong': 0, # 确认了多少，确认错（维持原判但原判本就是错）多少
        'caseC_total': 0, 'caseC_wrong': 0  # 模糊地带多少，其中错的多少
    }
    
    for i in range(n_info):
        # 仅针对 s_hard 和 s_pred 冲突的比特进行仲裁
        if s_hard[i] != s_pred[i]:
            involved_p_idx = np.where(F[i, :] == 1)[0]
            p_abs = np.abs(p_llr[involved_p_idx])
            s_abs = np.abs(s_llr[i])
            
            # --- Case A: 捞回 (尝试纠错) ---
            if np.min(p_abs) > alpha * s_abs:
                s_new[i] = s_pred[i]
                stats['caseA_total'] += 1
                if s_new[i] != s_true[i]: # 捞错了：预测值其实是错的
                    stats['caseA_wrong'] += 1
                
            # --- Case B: 确认原判 (拒绝纠错) ---
            elif np.sum(p_abs < s_abs) >= n_weak:
                s_new[i] = s_hard[i]
                stats['caseB_total'] += 1
                if s_new[i] != s_true[i]: # 确认错了：维持了错误的硬判决
                    stats['caseB_wrong'] += 1
                
            # --- Case C: 模糊地带 (默认不改) ---
            else:
                s_new[i] = s_hard[i]
                stats['caseC_total'] += 1
                if s_new[i] != s_true[i]:
                    stats['caseC_wrong'] += 1
                    
    return s_new, stats

# --- 4. 仿真主循环 ---
def run_simulation(G, F, snr_axis, alpha=1.5, n_weak=1):
    n_info = G.shape[1]
    results = {'Uncoded': [], 'Half_Iter_New': []}
    # 创建一个列表，用来存储每个 SNR 点下的详细统计情况，供绘图使用
    stats_history = {'A_w_rate': [], 'B_w_rate': [], 'A_total': [], 'B_total': []}

    print(f"{'SNR':>5} | {'Uncoded':>10} | {'New_BLER':>10} | {'Case A(捞)错/总':^15} | {'Case B(确)错/总':^15}")
    print("-" * 75)

    for snr in snr_axis:
        err_u, err_new = 0, 0
        sum_stats = {k: 0 for k in ['A_t', 'A_w', 'B_t', 'B_w']}
        pkt_count = 2000
        
        for _ in range(pkt_count):
            s_true = np.random.randint(0, 2, n_info).astype(np.uint8)
            p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
            s_llr = generate_llr(s_true, snr)
            p_llr = generate_llr(p_true, snr)
            
            # 1. Uncoded
            s_raw = (s_llr < 0).astype(np.uint8)
            if not np.array_equal(s_raw, s_true): err_u += 1
            
            # 2. New Logic
            s_new, s_pkt = triple_logic_half_iter_decoder(s_llr, p_llr, F, s_true, alpha, n_weak)
            if not np.array_equal(s_new, s_true): err_new += 1
            
            # 累加统计
            sum_stats['A_t'] += s_pkt['caseA_total']
            sum_stats['A_w'] += s_pkt['caseA_wrong']
            sum_stats['B_t'] += s_pkt['caseB_total']
            sum_stats['B_w'] += s_pkt['caseB_wrong']
            
        # 格式化打印
        str_A = f"{sum_stats['A_w']}/{sum_stats['A_t']}"
        str_B = f"{sum_stats['B_w']}/{sum_stats['B_t']}"
        
        print(f"{snr:5.1f} | {err_u/pkt_count:10.2e} | {err_new/pkt_count:10.2e} | {str_A:^15} | {str_B:^15}")
        
        results['Uncoded'].append(err_u / pkt_count)
        results['Half_Iter_New'].append(err_new / pkt_count)

        # 将统计结果存入历史，以便绘图
        stats_history['A_w_rate'].append(sum_stats['A_w'] / (sum_stats['A_t'] + 1e-9))
        stats_history['B_w_rate'].append(sum_stats['B_w'] / (sum_stats['B_t'] + 1e-9))
        stats_history['A_total'].append(sum_stats['A_t'] / pkt_count)
        stats_history['B_total'].append(sum_stats['B_t'] / pkt_count)
        
        results['Uncoded'].append(err_u / pkt_count)
        results['Half_Iter_New'].append(err_new / pkt_count)

    return results, stats_history

# --- 5. 绘图 ---
def plot_results(snr_axis, res, stats):
    plt.figure(figsize=(14, 6))
    
    # BLER 曲线图
    plt.subplot(1, 2, 1)
    plt.semilogy(snr_axis, res['Uncoded'], 'k--', label='Uncoded (Baseline)')
    plt.semilogy(snr_axis, res['20_Iter'], 'gD-', label='Ultimate 20-Iter')
    plt.semilogy(snr_axis, res['Half_Iter_New'], 'ro-', label='Half-Iter (Triple-Logic)', linewidth=2)
    plt.grid(True, which='both', linestyle='--', alpha=0.5)
    plt.xlabel('SNR (dB)'); plt.ylabel('BLER'); plt.title('BLER Comparison')
    plt.legend()
    
    # 冲突统计图
    plt.subplot(1, 2, 2)
    plt.plot(snr_axis, stats['rec'], 'g^-', label='Case A: Recovered (Flipped)')
    plt.plot(snr_axis, stats['miss'], 'x-', color='orange', label='Case C: Missed (Ambiguous)')
    plt.grid(True, linestyle='--', alpha=0.5)
    plt.xlabel('SNR (dB)'); plt.ylabel('Avg Bits/Packet'); plt.title('Conflict Resolution Stats')
    plt.legend()
    
    plt.tight_layout(); plt.show()


# --- 4. 执行 ---
if __name__ == "__main__":
    G_BIN = "../g_f_matrix/BG1_LSindex0_Z2_G.bin"
    F_BIN = "../g_f_matrix/BG1_LSindex0_Z2_F.bin"
    
    G = load_matrix_from_bin(G_BIN, 92, 44)
    F = load_matrix_from_bin(F_BIN, 44, 92)
    
    # snr_range = np.arange(0.0, 6.0, 0.5)
    
    # plt.figure(figsize=(10, 6))
    # iters_to_test = [1, 10, 20, 100]
    
    # # 运行综合仿真
    # all_res = simulate_bler_suite(G, F, snr_range, iters_to_test)
    
    # # 绘图对比
    # plot_comparison(snr_range, all_res)

    # b_dec, b_unc, avg_it = run_full_simulation(G, F, snr_range, max_iter=100)
    # plot_all(snr_range, b_dec, b_unc, avg_it)

    snr_range = np.arange(0.0, 7.5, 0.5)
    
    # 测试常规参数
    res, stats = run_simulation(G, F, snr_range, alpha=3.0, n_weak=3)
    plot_results(snr_range, res, stats)
 


