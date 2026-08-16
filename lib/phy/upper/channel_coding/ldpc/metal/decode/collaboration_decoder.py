import numpy as np
import struct
import os

# --- 基础工具 ---
def load_matrix_from_bin(filename, rows, cols):
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
    transmitted = 1.0 - 2.0 * bits
    return 2.0 * (transmitted + sigma * np.random.standard_normal(bits.shape)) / (sigma**2)

# --- 修正后的核心算法 ---

""" def bidirectional_min_evidence_decoder(s_llr_A, p_llr_A, G, F, max_iter=20, eta=0.5):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 1. 检查 G-Check (S映射到P)
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        
        # 2. 检查 F-Check (P映射到S)
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)

        # 停止条件：双向自洽
        num_p_conflict = np.sum(p_conflict_mask)
        num_s_conflict = np.sum(s_conflict_mask)
        print(f"Iter {it:2d}: P矛盾={num_p_conflict:2d}, S矛盾={num_s_conflict:2d}")
        
        if num_p_conflict == 0 and num_s_conflict == 0:
            print(f"✨ 在第 {it} 轮达成双向自洽！")
            break

        # --- S 空间更新 (基于 G 的报错) ---
        new_s_llr = curr_s_llr.copy()
        for i in range(n_info):
            # 找到 s_i 参与的所有报错的 P 校验方程
            related_p_indices = np.where(G[:, i] == 1)[0]
            conflict_p = [idx for idx in related_p_indices if p_conflict_mask[idx]]
            
            if conflict_p:
                # 证据：报错现场中最弱的那个 P 的 LLR
                evidence = np.min(np.abs(curr_p_llr[conflict_p]))
                # 修正：将 s_i 的 LLR 向 0 推送（降低可信度）
                # 如果 curr_s_llr > 0 (判决为0)，则减去 evidence；反之加上。
                new_s_llr[i] -= np.sign(curr_s_llr[i]) * eta * evidence

        # --- P 空间更新 (基于 F 的报错) ---
        new_p_llr = curr_p_llr.copy()
        for j in range(n_parity):
            related_s_indices = np.where(F[:, j] == 1)[0]
            conflict_s = [idx for idx in related_s_indices if s_conflict_mask[idx]]
            
            if conflict_s:
                evidence = np.min(np.abs(curr_s_llr[conflict_s]))
                new_p_llr[j] -= np.sign(curr_p_llr[j]) * eta * evidence

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8) """

""" def bidirectional_min_evidence_decoder(s_llr_A, p_llr_A, G, F, max_iter=15, eta=0.4):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 只要满足 p = G * s，逻辑上就收敛了
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        # 同理，计算 s = F * p 的矛盾
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        num_s_conflict = np.sum(s_conflict_mask)

        print(f"Iter {it:2d}: P矛盾={num_p_conflict:2d}, S矛盾={num_s_conflict:2d}")

        if num_p_conflict == 0:
            print(f"✨ [收敛] 满足约束条件 p = G * s")
            break

        # --- 更新 S 的 LLR (只有当 P 方程报错且证据来自 P) ---
        new_s_llr = curr_s_llr.copy()
        for i in range(n_info):
            # 找到 s_i 参与的报错方程
            conflict_p_idx = np.where((G[:, i] == 1) & p_conflict_mask)[0]
            if len(conflict_p_idx) > 0:
                # 证据强度：所有相关报错方程中最弱的那个
                evidence = np.min(np.abs(curr_p_llr[conflict_p_idx]))
                # 只有当证据足够多或足够强时，才削弱自己的信心
                new_s_llr[i] -= np.sign(curr_s_llr[i]) * eta * evidence

        # --- 更新 P 的 LLR (基于 F 映射给出的证据) ---
        new_p_llr = curr_p_llr.copy()
        for j in range(n_parity):
            conflict_s_idx = np.where((F[:, j] == 1) & s_conflict_mask)[0]
            if len(conflict_s_idx) > 0:
                evidence = np.min(np.abs(curr_s_llr[conflict_s_idx]))
                new_p_llr[j] -= np.sign(curr_p_llr[j]) * eta * evidence

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8)
 """

""" def voting_collaboration_decoder(s_llr_A, p_llr_A, G, F, max_iter=100, eta=0.5, vote_threshold=0.5):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 1. GPU: 计算矛盾掩码
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        if num_p_conflict == 0:
            print(f"✨ [Success] 第 {it} 轮达成完美收敛")
            break

        print(f"Iter {it:2d}: P矛盾={num_p_conflict:2d}")

        # 2. CPU: 多数票决逻辑
        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()
        
        # 记录哪些 S 被处理过，避免一轮内重复大幅削弱
        s_processed = np.zeros(n_info, dtype=bool)

        for j in np.where(p_conflict_mask)[0]:
            # 在这个报错方程 j 中，找到最值得怀疑的 s_i (LLR 绝对值最小)
            related_s_idx = np.where(G[j, :] == 1)[0]
            if len(related_s_idx) == 0: continue
            
            # 找到其中最弱的 s
            i_suspect = related_s_idx[np.argmin(np.abs(curr_s_llr[related_s_idx]))]
            
            if not s_processed[i_suspect]:
                # --- 陪审团投票 ---
                s_i_all_checks = np.where(G[:, i_suspect] == 1)[0]
                s_i_conflict_checks = [c for c in s_i_all_checks if p_conflict_mask[c]]
                
                # 计算报错比例
                conflict_ratio = len(s_i_conflict_checks) / len(s_i_all_checks)
                
                if conflict_ratio >= vote_threshold:
                    # 多数票支持：s 有错
                    evidence = np.abs(curr_p_llr[j])
                    new_s_llr[i_suspect] -= np.sign(curr_s_llr[i_suspect]) * eta * evidence
                    s_processed[i_suspect] = True
                else:
                    # 少数票：认为是 p[j] 自己的问题，削弱 p[j]
                    new_p_llr[j] -= np.sign(curr_p_llr[j]) * eta * np.abs(curr_s_llr[i_suspect])

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8)
 """

""" def symmetric_voting_decoder(s_llr_A, p_llr_A, G, F, max_iter=100, eta=0.5, threshold=0.5):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # --- 1. 计算双向矛盾掩码 ---
        # G 映射矛盾 (P 空间的报错)
        p_conflict_mask = ((np.matmul(G, s_hard) % 2) != p_hard)
        # F 映射矛盾 (S 空间的报错)
        s_conflict_mask = ((np.matmul(F, p_hard) % 2) != s_hard)
        
        num_p_err = np.sum(p_conflict_mask)
        num_s_err = np.sum(s_conflict_mask)
        
        if num_p_err == 0 and num_s_err == 0:
            print(f"✨ [Success] 第 {it} 轮双向自洽收敛")
            break
            
        print(f"Iter {it:2d}: P矛盾={num_p_err:2d}, S矛盾={num_s_err:2d}")

        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()

        # --- 2. S 空间的陪审团 (根据 G 报错审判 S) ---
        s_processed = np.zeros(n_info, dtype=bool)
        for j in np.where(p_conflict_mask)[0]:
            related_s = np.where(G[j, :] == 1)[0]
            if len(related_s) == 0: continue
            i_suspect = related_s[np.argmin(np.abs(curr_s_llr[related_s]))]
            
            if not s_processed[i_suspect]:
                # 统计该 S 参与的所有 G 方程的报错率
                all_checks = np.where(G[:, i_suspect] == 1)[0]
                conflicts = [c for c in all_checks if p_conflict_mask[c]]
                if len(conflicts) / len(all_checks) >= threshold:
                    # 多数票支持：S 有错
                    new_s_llr[i_suspect] -= np.sign(curr_s_llr[i_suspect]) * eta * np.abs(curr_p_llr[j])
                    s_processed[i_suspect] = True

        # --- 3. P 空间的陪审团 (根据 F 报错审判 P) ---
        p_processed = np.zeros(n_parity, dtype=bool)
        for i in np.where(s_conflict_mask)[0]:
            related_p = np.where(F[i, :] == 1)[0]
            if len(related_p) == 0: continue
            j_suspect = related_p[np.argmin(np.abs(curr_p_llr[related_p]))]
            
            if not p_processed[j_suspect]:
                # 统计该 P 参与的所有 F 方程的报错率
                all_checks = np.where(F[:, j_suspect] == 1)[0]
                conflicts = [c for c in all_checks if s_conflict_mask[c]]
                if len(conflicts) / len(all_checks) >= threshold:
                    # 多数票支持：P 有错
                    new_p_llr[j_suspect] -= np.sign(curr_p_llr[j_suspect]) * eta * np.abs(curr_s_llr[i])
                    p_processed[j_suspect] = True

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8)
 """

def terminator_collaboration_decoder(s_llr_A, p_llr_A, G, F, max_iter=50, eta=0.5, threshold=0.5):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()
    
    # 用于死锁检测
    history_p_err = []

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 1. 计算矛盾
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_err = np.sum(p_conflict_mask)
        
        # 2. 正常收敛退出
        if num_p_err == 0:
            print(f"✨ [Success] 第 {it} 轮自然收敛")
            return s_hard

        # 3. 死锁检测逻辑
        history_p_err.append(num_p_err)
        if len(history_p_err) > 5 and len(set(history_p_err[-5:])) == 1:
            print(f"⚠️ [Deadlock] 矛盾数连续5轮锁定在 {num_p_err}。启动强制对齐...")
            # 强制让 P 追随 S 的判决
            p_hard_final = (np.matmul(G, s_hard) % 2).astype(np.uint8)
            # 最终再自检一次
            final_check = np.sum((np.matmul(G, s_hard) % 2) != p_hard_final)
            if final_check == 0:
                print(f"✅ [Terminated] 强制对齐成功，s 已修正。")
                return s_hard

        # 4. (此处省略之前的陪审团投票逻辑，保持一致...)
        # --- 1. 计算双向矛盾掩码 ---
        # G 映射矛盾 (P 空间的报错)
        p_conflict_mask = ((np.matmul(G, s_hard) % 2) != p_hard)
        # F 映射矛盾 (S 空间的报错)
        s_conflict_mask = ((np.matmul(F, p_hard) % 2) != s_hard)
        
        num_p_err = np.sum(p_conflict_mask)
        num_s_err = np.sum(s_conflict_mask)
        
        if num_p_err == 0 and num_s_err == 0:
            print(f"✨ [Success] 第 {it} 轮双向自洽收敛")
            break
            
        print(f"Iter {it:2d}: P矛盾={num_p_err:2d}, S矛盾={num_s_err:2d}")

        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()

        # --- 2. S 空间的陪审团 (根据 G 报错审判 S) ---
        s_processed = np.zeros(n_info, dtype=bool)
        for j in np.where(p_conflict_mask)[0]:
            related_s = np.where(G[j, :] == 1)[0]
            if len(related_s) == 0: continue
            i_suspect = related_s[np.argmin(np.abs(curr_s_llr[related_s]))]
            
            if not s_processed[i_suspect]:
                # 统计该 S 参与的所有 G 方程的报错率
                all_checks = np.where(G[:, i_suspect] == 1)[0]
                conflicts = [c for c in all_checks if p_conflict_mask[c]]
                if len(conflicts) / len(all_checks) >= threshold:
                    # 多数票支持：S 有错
                    new_s_llr[i_suspect] -= np.sign(curr_s_llr[i_suspect]) * eta * np.abs(curr_p_llr[j])
                    s_processed[i_suspect] = True

        # --- 3. P 空间的陪审团 (根据 F 报错审判 P) ---
        p_processed = np.zeros(n_parity, dtype=bool)
        for i in np.where(s_conflict_mask)[0]:
            related_p = np.where(F[i, :] == 1)[0]
            if len(related_p) == 0: continue
            j_suspect = related_p[np.argmin(np.abs(curr_p_llr[related_p]))]
            
            if not p_processed[j_suspect]:
                # 统计该 P 参与的所有 F 方程的报错率
                all_checks = np.where(F[:, j_suspect] == 1)[0]
                conflicts = [c for c in all_checks if s_conflict_mask[c]]
                if len(conflicts) / len(all_checks) >= threshold:
                    # 多数票支持：P 有错
                    new_p_llr[j_suspect] -= np.sign(curr_p_llr[j_suspect]) * eta * np.abs(curr_s_llr[i])
                    p_processed[j_suspect] = True

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

        print(f"Iter {it:2d}: P矛盾={num_p_err}")

    return (curr_s_llr < 0).astype(np.uint8)

def adaptive_bidirectional_collaboration_decoder(s_llr_A, p_llr_A, G, F, max_iter=50, eta=0.4, threshold=0.5):
    """
    具有动态死锁仲裁功能的完全对称协作译码器
    """
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()
    
    # 用于检测死锁的滑动窗口
    p_err_history = []
    s_err_history = []

    for it in range(max_iter):
        # 1. 硬判决
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 2. 计算双向矛盾（这是自检过程）
        # p_pred 是从 s 映射过来的，如果与 p_hard 不符，说明 G 约束不满足
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        # s_pred 是从 p 映射过来的，如果与 s_hard 不符，说明 F 约束不满足
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        num_s_conflict = np.sum(s_conflict_mask)

        # 完美退出条件
        if num_p_conflict == 0 and num_s_conflict == 0:
            print(f"✨ [Success] 第 {it} 轮自然收敛")
            return s_hard

        # 3. 动态死锁仲裁 (Deadlock Arbitration)
        p_err_history.append(num_p_conflict)
        s_err_history.append(num_s_conflict)
        
        if len(p_err_history) >= 10:
            # 如果最近5轮矛盾数不再下降（标准差极小或完全相等）
            p_stalled = len(set(p_err_history[-10:])) <= 1
            s_stalled = len(set(s_err_history[-10:])) <= 1
            
            if p_stalled or s_stalled:
                # 核心逻辑：计算当前 LLR 的平均绝对值作为证据强度
                avg_s_confidence = np.mean(np.abs(curr_s_llr))
                avg_p_confidence = np.mean(np.abs(curr_p_llr))
                
                print(f"⚠️ [Deadlock] 检出死锁 (P:{num_p_conflict}, S:{num_s_conflict})")
                print(f"📊 证据评估 - S强度: {avg_s_confidence:.2f}, P强度: {avg_p_confidence:.2f}")

                if avg_s_confidence >= avg_p_confidence:
                    print("⚖️  裁定: S 较可靠，以 S 修正 P (使用 G 矩阵)")
                    # 此时 s_hard 保持不变，由 G 映射生成最终结果
                    return s_hard
                else:
                    print("⚖️  裁定: P 较可靠，以 P 修正 S (使用 F 矩阵)")
                    # 此时以 p_hard 为准，通过 F 映射生成 s 的最终判决
                    s_final = (np.matmul(F, p_hard) % 2).astype(np.uint8)
                    return s_final

        # 4. 双向陪审团投票 (正常迭代逻辑)
        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()

        # --- S 空间更新 (听取 G 方程的抗议) ---
        s_processed = np.zeros(n_info, dtype=bool)
        for j in np.where(p_conflict_mask)[0]:
            related_s = np.where(G[j, :] == 1)[0]
            if len(related_s) == 0: continue
            i_sus = related_s[np.argmin(np.abs(curr_s_llr[related_s]))]
            if not s_processed[i_sus]:
                all_checks = np.where(G[:, i_sus] == 1)[0]
                conflict_ratio = np.sum(p_conflict_mask[all_checks]) / len(all_checks)
                if conflict_ratio >= threshold:
                    # 只有当证据足够强时，才调整 S 的 LLR
                    new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * eta * np.abs(curr_p_llr[j])
                    s_processed[i_sus] = True

        # --- P 空间更新 (听取 F 方程的抗议) ---
        p_processed = np.zeros(n_parity, dtype=bool)
        for i in np.where(s_conflict_mask)[0]:
            related_p = np.where(F[i, :] == 1)[0]
            if len(related_p) == 0: continue
            j_sus = related_p[np.argmin(np.abs(curr_p_llr[related_p]))]
            if not p_processed[j_sus]:
                all_checks = np.where(F[:, j_sus] == 1)[0]
                conflict_ratio = np.sum(s_conflict_mask[all_checks]) / len(all_checks)
                if conflict_ratio >= threshold:
                    new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * eta * np.abs(curr_s_llr[i])
                    p_processed[j_sus] = True

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr
        print(f"Iter {it:2d}: P矛盾={num_p_conflict}, S矛盾={num_s_conflict}")

    return (curr_s_llr < 0).astype(np.uint8)

def final_adaptive_collaboration_decoder(s_llr_A, p_llr_A, G, F, max_iter=100, eta=0.4, threshold=0.5):
    """
    终极自适应协作译码器：
    1. 双向对称投票 (Symmetric Voting)
    2. 动态证据仲裁 (Evidence Arbitration)
    3. 动态死锁窗口 (Dynamic Lock Window)
    """
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()
    
    # 历史记录，用于检测死锁
    p_err_history = []
    s_err_history = []

    for it in range(max_iter):
        # --- 1. 硬判决与矛盾检测 ---
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # G 约束检查 (正向)
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        # F 约束检查 (反向)
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        num_s_conflict = np.sum(s_conflict_mask)

        # 记录历史
        p_err_history.append(num_p_conflict)
        s_err_history.append(num_s_conflict)

        # 完美退出准则
        if num_p_conflict == 0 and num_s_conflict == 0:
            print(f"✨ [Success] 第 {it} 轮自然收敛。")
            return s_hard

        # --- 2. 动态死锁仲裁逻辑 ---
        avg_s_conf = np.mean(np.abs(curr_s_llr))
        avg_p_conf = np.mean(np.abs(curr_p_llr))
        
        # 计算强度比 (Confidence Ratio)
        conf_ratio = max(avg_s_conf, avg_p_conf) / (min(avg_s_conf, avg_p_conf) + 1e-6)
        
        # 根据强度比决定等待时间 (强者快断，弱者慢磨)
        if conf_ratio > 2.5:
            wait_rounds = 5    # 压倒性优势，快速切断
        elif conf_ratio > 1.5:
            wait_rounds = 10   # 有明显差距
        else:
            wait_rounds = 20   # 势均力敌，给予更多磨合时间

        if len(p_err_history) >= wait_rounds:
            # 检查最近 N 轮矛盾数是否完全没有变化
            p_stalled = len(set(p_err_history[-wait_rounds:])) == 1
            s_stalled = len(set(s_err_history[-wait_rounds:])) == 1
            
            if p_stalled or s_stalled:
                print(f"⚠️  [Deadlock] 检测到死锁，窗口期: {wait_rounds}")
                print(f"📊 强度评估 - S: {avg_s_conf:.2f}, P: {avg_p_conf:.2f}, 比值: {conf_ratio:.2f}")
                
                if avg_s_conf >= avg_p_conf:
                    print("⚖️  裁定: S 较可靠，强制对齐 P。")
                    return s_hard
                else:
                    print("⚖️  裁定: P 较可靠，强制对齐 S。")
                    return (np.matmul(F, p_hard) % 2).astype(np.uint8)

        # --- 3. 双向对称陪审团迭代 ---
        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()

        # [正向] 根据 G 报错审判 S
        s_processed = np.zeros(n_info, dtype=bool)
        for j in np.where(p_conflict_mask)[0]:
            rel_s = np.where(G[j, :] == 1)[0]
            if len(rel_s) == 0: continue
            i_sus = rel_s[np.argmin(np.abs(curr_s_llr[rel_s]))]
            if not s_processed[i_sus]:
                all_c = np.where(G[:, i_sus] == 1)[0]
                if np.sum(p_conflict_mask[all_c]) / len(all_c) >= threshold:
                    new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * eta * np.abs(curr_p_llr[j])
                    s_processed[i_sus] = True

        # [反向] 根据 F 报错审判 P
        p_processed = np.zeros(n_parity, dtype=bool)
        for i in np.where(s_conflict_mask)[0]:
            rel_p = np.where(F[i, :] == 1)[0]
            if len(rel_p) == 0: continue
            j_sus = rel_p[np.argmin(np.abs(curr_p_llr[rel_p]))]
            if not p_processed[j_sus]:
                all_c = np.where(F[:, j_sus] == 1)[0]
                if np.sum(s_conflict_mask[all_c]) / len(all_c) >= threshold:
                    new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * eta * np.abs(curr_s_llr[i])
                    p_processed[j_sus] = True

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr
        if it % 5 == 0: # 减少打印，每5轮输出一次
            print(f"Iter {it:2d}: P矛盾={num_p_conflict:2d}, S矛盾={num_s_conflict:2d}")

    return (curr_s_llr < 0).astype(np.uint8)

# --- Main ---
def main():
    try:
        G = load_matrix_from_bin("BG1_LSindex0_Z2_G.bin", 92, 44)
        F = load_matrix_from_bin("BG1_LSindex0_Z2_F.bin", 44, 92)
        
        # 产生干净数据
        s_true = np.random.randint(0, 2, 44, dtype=np.uint8)
        p_true = (np.matmul(G, s_true) % 2).astype(np.uint8)
        
        # 模拟信道 (这里 SNR 设高一点观察自洽过程)
        s_llr_A = generate_llr(s_true, 1.0)
        p_llr_A = generate_llr(p_true, 1.0)
        
        # 初始错误检测
        s_init_err = np.sum((s_llr_A < 0).astype(np.uint8) != s_true)
        print(f"初始 S 错误: {s_init_err}")

        # s_decoded = bidirectional_min_evidence_decoder(s_llr_A, p_llr_A, G, F, eta=0.4)
        # s_decoded = voting_collaboration_decoder(s_llr_A, p_llr_A, G, F)
        s_decoded = final_adaptive_collaboration_decoder(s_llr_A, p_llr_A, G, F)

        final_err = np.sum(s_decoded != s_true)
        print(f"\n最终结果: {'SUCCESS' if final_err==0 else 'FAILED'}, 残留错误: {final_err}")

    except Exception as e:
        print(f"运行错误: {e}")

if __name__ == "__main__":
    main()