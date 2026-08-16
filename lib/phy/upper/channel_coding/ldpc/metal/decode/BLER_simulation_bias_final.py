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

def collaboration_decoder_ultimate(s_llr_A, p_llr_A, G, F, s_bias=1.0, max_iter=20, eta=0.8, top_n=2, power=2.0):
    """
    top_n:  指控的嫌疑人数量 (建议设为 1, 3, 5...)
    power:  非线性权重系数 (power > 1 抑制随机噪声，增强强指控)
    """
    curr_s_llr, curr_p_llr = s_llr_A.copy(), p_llr_A.copy()

    s_hard = (curr_s_llr < 0).astype(np.uint8)
    p_hard = (curr_p_llr < 0).astype(np.uint8)

    for it in range(max_iter):
        # 1. 校验矛盾检测
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        if np.sum(p_conflict_mask) == 0:
            return s_hard

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
            return s_pred
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
        return s_hard
    else:
        # 如果相信 P 空间，则输出 P 空间序列映射回 S 的结果
        return (np.matmul(F, p_hard) % 2).astype(np.uint8)

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


def collaboration_decoder_h_unified(llr_in, H, max_iter=20, eta=1.2, top_n=2, power=2.0):
    """
    基于统一校验矩阵 H 的协作译码算法 (v5.0)
    llr_in:  合并后的 LLR 向量 [s_llr, p_llr]
    H:       全量校验矩阵 (m_rows x n_bits)
    top_n:   每行不满足时指控的嫌疑人数量
    power:   冲突比例的非线性权重系数
    """
    curr_llr = llr_in.copy()
    n_bits = curr_llr.shape[0]
    m_rows = H.shape[0]

    # 预计算：每个比特参与了哪些校验方程 (用于计算 conflict_ratio)
    # 结果是一个列表，每个元素包含对应比特参与的行索引
    bit_to_checks = [np.where(H[:, i] == 1)[0] for i in range(n_bits)]

    for it in range(max_iter):
        # 1. 硬判决
        v_hard = (curr_llr < 0).astype(np.uint8)

        # 2. 校验矛盾检测 (Syndrome)
        # syndrome[j] == 1 表示第 j 行校验方程不满足
        syndrome = (np.matmul(H, v_hard) % 2).astype(np.uint8)
        conflict_mask = (syndrome == 1)
        num_conflicts = np.sum(conflict_mask)

        # 如果全通过，直接返回结果
        if num_conflicts == 0:
            return v_hard

        # 3. 证据收集 (Evidence Collection)
        new_llr = curr_llr.copy()
        evidence_pool = {} # {bit_idx: [strength1, strength2, ...]}

        # 遍历所有不满足的校验行
        for j in np.where(conflict_mask)[0]:
            # 找到该行涉及的所有比特索引 (包括 s 和 p)
            rel_indices = np.where(H[j, :] == 1)[0]
            if len(rel_indices) == 0: continue
            
            # 获取这些比特的 LLR 绝对值
            abs_vals = np.abs(curr_llr[rel_indices])
            
            # 找到最弱的几个嫌疑人 (LLR 绝对值最小的)
            actual_n = min(top_n, len(rel_indices))
            sorted_local_pos = np.argsort(abs_vals)
            
            # Min-Sum 逻辑：Rank 1 的强度受 Rank 2 限制
            v1 = abs_vals[sorted_local_pos[0]]
            # 如果这行只有一个比特(罕见)，则证据强度设为该比特本身
            v2 = abs_vals[sorted_local_pos[1]] if len(rel_indices) > 1 else v1

            for rank in range(actual_n):
                target_bit_idx = rel_indices[sorted_local_pos[rank]]
                
                # 计算指控强度 (依据 original ultimate 逻辑)
                # 如果当前比特是该行最弱的 (rank 0)，强度由次弱者 v2 决定
                # 否则，强度由最弱者 v1 决定
                strength = v2 if rank == 0 else v1
                
                if target_bit_idx not in evidence_pool:
                    evidence_pool[target_bit_idx] = []
                evidence_pool[target_bit_idx].append(strength)

        # 4. 融合证据并更新 LLR
        for bit_idx, strengths in evidence_pool.items():
            # 陪审团逻辑：计算该比特参与的校验方程中，有多少比例是报错的
            participated_checks = bit_to_checks[bit_idx]
            if len(participated_checks) == 0: continue
            
            # 计算冲突比例 (Conflict Ratio)
            conflict_ratio = np.sum(conflict_mask[participated_checks]) / len(participated_checks)
            
            # 应用非线性映射 (Power)
            weight = np.power(conflict_ratio, power)
            
            # 更新 LLR：方向与原符号相反，强度由比例权重和证据均值决定
            update_val = (eta * weight) * np.mean(strengths)
            new_llr[bit_idx] -= np.sign(curr_llr[bit_idx]) * update_val

        curr_llr = new_llr

    # 迭代结束，返回最终硬判决
    return (curr_llr < 0).astype(np.uint8)

def collaboration_decoder_g_symmetric(s_llr_in, p_llr_in, G, max_iter=20, eta=0.8, top_n=2, power=2.0):
    """
    基于 G 矩阵校验方程 (p = G * s) 的对称协作译码器
    s_llr_in: 信息位 LLR 向量
    p_llr_in: 校验位 LLR 向量
    G: 生成矩阵 (n_parity x n_info)
    top_n: 每行冲突时，指控的嫌疑人数量 (在 s 和 p 中统一选取)
    """
    curr_s_llr = s_llr_in.copy()
    curr_p_llr = p_llr_in.copy()
    n_parity, n_info = G.shape

    # 预计算：每个 s 比特参与了哪些校验方程 (G 的每一列中 1 的位置)
    s_to_checks = [np.where(G[:, i] == 1)[0] for i in range(n_info)]

    for it in range(max_iter):
        # 1. 硬判决
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 2. 校验冲突检测: p_pred = G * s, 检查 p_pred 是否等于 p_hard
        # 在 GF2 下，这等价于检查 G*s + p = 0
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        conflict_mask = (p_pred != p_hard)
        
        if np.sum(conflict_mask) == 0:
            return s_hard

        # 3. 证据收集 (Evidence Collection)
        # 为 s 和 p 分别准备证据池
        s_evidence = {} # {s_idx: [strength1, ...]}
        p_evidence = {} # {p_idx: [strength1, ...]}

        # 遍历所有报错的校验方程
        for j in np.where(conflict_mask)[0]:
            # 该方程涉及的比特索引：s 部分 (G_j == 1) 和对应的 p_j
            rel_s_indices = np.where(G[j, :] == 1)[0]
            
            # 收集这一组对比特（s 和 p）的 LLR 绝对值
            # 注意：我们将 p_j 也加入竞争
            abs_vals = [np.abs(curr_s_llr[i]) for i in rel_s_indices]
            abs_vals.append(np.abs(curr_p_llr[j]))
            
            # 将所有相关比特的标识符统一：(类型 's'/'p', 索引)
            participants = [( 's', i) for i in rel_s_indices]
            participants.append(('p', j))
            
            # 找到最弱的 Top-N 个嫌疑人
            actual_n = min(top_n, len(participants))
            sorted_indices = np.argsort(abs_vals)
            
            # Min-Sum 强度逻辑：
            # v1: 组内最小 LLR，v2: 组内次小 LLR
            v1 = abs_vals[sorted_indices[0]]
            v2 = abs_vals[sorted_indices[1]] if len(participants) > 1 else v1

            for rank in range(actual_n):
                target_type, target_idx = participants[sorted_indices[rank]]
                # 强度计算：如果是最弱的，受次弱者限制；否则受最弱者限制
                strength = v2 if rank == 0 else v1
                
                if target_type == 's':
                    if target_idx not in s_evidence: s_evidence[target_idx] = []
                    s_evidence[target_idx].append(strength)
                else:
                    if target_idx not in p_evidence: p_evidence[target_idx] = []
                    p_evidence[target_idx].append(strength)

        # 4. 同时更新 s 和 p 的 LLR
        # 更新 S 空间
        for i, strengths in s_evidence.items():
            # 冲突比例：参与的 G 方程中报错的比例
            relevant_checks = s_to_checks[i]
            ratio = np.sum(conflict_mask[relevant_checks]) / len(relevant_checks)
            weight = np.power(ratio, power)
            curr_s_llr[i] -= np.sign(curr_s_llr[i]) * (eta * weight) * np.mean(strengths)

        # 更新 P 空间 (每个 p_j 只参与一个 G 方程，所以 ratio 要么是 0 要么是 1)
        for j, strengths in p_evidence.items():
            # 对于 p_j，如果方程 j 报错，ratio 就是 1.0
            ratio = 1.0 if conflict_mask[j] else 0.0
            weight = np.power(ratio, power)
            curr_p_llr[j] -= np.sign(curr_p_llr[j]) * (eta * weight) * np.mean(strengths)

    return (curr_s_llr < 0).astype(np.uint8)

# --- 3. 仿真运行：修复变量冲突 ---
def run_sim_with_random_data(G, F, H, snrs, b_val, min_err_count=200):
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
            # s_dec = collaboration_decoder_ultimate(s_llr, p_llr, G, F)

            # # 将 s 和 p 的 LLR 拼接
            llr_combined = np.concatenate([s_llr, p_llr])

            # # 使用统一解码器
            v_dec = collaboration_decoder_h_unified(llr_combined, H)

            # # 提取信息位部分 (假设前 22*Z 位是信息位)
            s_dec = v_dec[:n_info_dim]            
            
            # 判错
            if not np.array_equal(s_dec, s_true):
                err_count += 1
                
        bler = err_count / pkt_count
        bler_results.append(bler)
        duration = time.time() - start_time
        print(f"   SNR: {snr:4.1f} | BLER: {bler:.2e} | 样本: {pkt_count:4d} | 耗时: {duration:4.1f}s")
        
        if bler < 1e-5: break 
    return bler_results

# --- 4. 执行 ---
if __name__ == "__main__":
    # G_BIN = "../g_f_matrix/BG1_LSindex0_Z2_G.bin"
    G_BIN = "../g_f_matrix/G_matrix_Z2.bin"
    F_BIN = "../g_f_matrix/BG1_LSindex0_Z2_F.bin"
    H_BIN = "../g_f_matrix/H_matrix_Z2.bin"

    
    G = load_matrix_from_bin(G_BIN, 92, 44)
    F = load_matrix_from_bin(F_BIN, 44, 92)
    H = load_matrix_from_bin(H_BIN, 92, 136)
    
    snr_axis = np.arange(0.0, 6.0, 0.5)
    
    plt.figure(figsize=(10, 6))

    # np.random.seed(202) # 固定种子方便重复实验

    # 依次跑三组对比
    for b_value, style, label_text in [(1.0, 'r*--', 'Bias 1.0')]:
        bler_curve = run_sim_with_random_data(G, F, H, snr_axis, b_value)
        plt.semilogy(snr_axis[:len(bler_curve)], bler_curve, style, label=label_text)

    plt.grid(True, which="both", ls="--", alpha=0.5)
    plt.xlabel('SNR (dB)')
    plt.ylabel('BLER')
    plt.title('Final Refined Performance (Random Data & Proper Mapping)')
    plt.legend()
    plt.savefig("Final_Bias_Comparison_Smooth.png")
    plt.show()
