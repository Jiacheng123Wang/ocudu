import numpy as np
import struct
import os


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

def collaboration_decoder_with_bias(s_llr_A, p_llr_A, G, F, s_bias, max_iter=100, eta=0.4, threshold=0.3):
    num_info_bits = G.shape[1]
    num_parity_bits = G.shape[0]
    curr_s_llr, curr_p_llr = s_llr_A.copy(), p_llr_A.copy()
    p_err_history = []

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 校验矛盾检测
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        if num_p_conflict == 0:
            return s_hard

        # 死锁裁决：20轮长窗口
        p_err_history.append(num_p_conflict)
        if len(p_err_history) >= 20:
            if len(set(p_err_history[-20:])) == 1:
                avg_s_conf = np.mean(np.abs(curr_s_llr))
                avg_p_conf = np.mean(np.abs(curr_p_llr))
                if avg_s_conf * s_bias >= avg_p_conf:
                    return s_hard
                else:
                    return (np.matmul(F, p_hard) % 2).astype(np.uint8)

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
            if conflict_ratio >= threshold:
                # --- 融合策略测试区 ---
                merged_llr = np.mean(llr_list)
                new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * eta * merged_llr
        
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
            if conflict_ratio >= threshold:
                merged_llr = np.mean(llr_list)
                new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * eta * merged_llr
        
        # 更新 P 硬判决        
        p_hard = (new_p_llr < 0).astype(np.uint8)
    
        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8)

def generate_fixed_test_vector():
    # 1. 基础维度设置 (根据你之前的 Z=2, n_info=44, n_parity=92)
    n_info = 44
    n_parity = 92
    eta = 0.4
    threshold = 0.3
    s_bias = 1.3
    
    # 2. 生成全 0 基准（transmitted = +1.0）
    # 为了模拟 SNR=4dB (sigma ≈ 0.447)，我们将背景噪声设为较小值
    # 但为了调试方便，我们手动设置两个错误比特
    np.random.seed(42) # 固定随机数种子
    
    # 初始化为高可靠性的正值 (对应比特 0)
    s_llr = np.ones(n_info, dtype=np.float32) * 5.0
    p_llr = np.ones(n_parity, dtype=np.float32) * 5.0
    
    # 3. 人为制造 2 个错误比特 (S: 2/44)
    # 模拟你日志中的 j=19 和 j=36 为错误比特 (s_hard=0, 但 s_pred=1)
    # 在 LLR 中，负值代表硬判决为 1
    s_llr[19] = -1.2  # 故意设一个较弱的错误
    s_llr[36] = -0.8  # 故意设一个更弱的错误
    
    # 4. 打印 C++ 格式的初始化代码
    print("// --- C++ 调试测试向量 ---")
    print(f"float s_llr_initial[{n_info}] = {{")
    print("    " + ", ".join([f"{x:.4f}f" for x in s_llr]))
    print("};")
    
    print(f"\nfloat p_llr_initial[{n_parity}] = {{")
    print("    " + ", ".join([f"{x:.4f}f" for x in p_llr]))
    print("};\n")

    # 5. 加载矩阵并调用 Python 译码器
    # 注意：请确保路径正确或将 bin 文件放在当前目录
    try:
        G = load_matrix_from_bin("../g_f_matrix/BG1_LSindex0_Z2_G.bin", 92, 44)
        F = load_matrix_from_bin("../g_f_matrix/BG1_LSindex0_Z2_F.bin", 44, 92)
    except:
        print("错误：未找到矩阵文件，请检查路径。")
        return

    print(">>> Python 译码器启动...")
    # 调用附件中的核心算法
    s_dec = collaboration_decoder_with_bias(
        s_llr, p_llr, G, F, 
        s_bias=s_bias, 
        max_iter=10, # 调试只需看前几轮
        eta=eta, 
        threshold=threshold
    )

    # 6. 验证结果
    # 因为我们 s_true 全是 0，所以 s_dec 应该全是 0
    errors = np.sum(s_dec != 0)
    print(f"\n>>> 译码结束。剩余错误比特数: {errors}")
    if errors == 0:
        print(">>> 验证通过：Python 成功修复了制造的 2 个错误比特。")
    else:
        print(f">>> 验证未通过：Python 仍有 {errors} 个错误。")
        print("错误索引:", np.where(s_dec != 0)[0])

def generate_random_test_vector():
    n_info = 44
    n_parity = 92
    snr_db = 4.0
    
    # 1. 加载矩阵 (用于计算 p = G * s)
    G = load_matrix_from_bin("../g_f_matrix/BG1_LSindex0_Z2_G.bin", 92, 44)
    F = load_matrix_from_bin("../g_f_matrix/BG1_LSindex0_Z2_F.bin", 44, 92)

    # 2. 生成随机信息位 s_true (0 或 1)
    # np.random.seed(202) # 固定种子方便重复实验
    s_true = np.random.randint(0, 2, size=n_info, dtype=np.uint8)
    
    # 3. 计算对应的校验位 p_true (GF2 域下的矩阵乘法)
    # p = G * s (mod 2)
    p_true = (G @ s_true) % 2

    # 4. 生成带噪声的 LLR
    # 模拟信道：0 -> +LLR, 1 -> -LLR
    snr_linear = 10**(snr_db / 10.0)
    sigma = np.sqrt(1.0 / (2.0 * snr_linear))
    
    def get_llr(bits):
        transmitted = 1.0 - 2.0 * bits.astype(float) # 0->1, 1->-1
        noise = sigma * np.random.standard_normal(bits.shape)
        received = transmitted + noise
        return 2.0 * received / (sigma**2)

    s_llr = get_llr(s_true)
    p_llr = get_llr(p_true)

    # # 5. 打印 C++ 格式代码
    # print(f"// --- 随机测试向量 (SNR: {snr_db}dB) ---")
    # print(f"// s_true (参考): {''.join(map(str, s_true))}")
    # print(f"float s_llr_initial[{n_info}] = {{")
    # print("    " + ", ".join([f"{x:.4f}f" for x in s_llr]))
    # print("};\n")

    # print(f"float p_llr_initial[{n_parity}] = {{")
    # print("    " + ", ".join([f"{x:.4f}f" for x in p_llr]))
    # print("};\n")

    s_initial_hard = (s_llr < 0).astype(np.uint8)
    p_initial_hard = (p_llr < 0).astype(np.uint8)
    
    s_errors = np.where(s_initial_hard != s_true)[0]
    p_errors = np.where(p_initial_hard != p_true)[0]

    # --- 打印审计信息 ---
    print("="*50)
    print(f"【初始状态审计 - SNR: {snr_db}dB】")
    print(f"S 空间初始错误数: {len(s_errors)} / {n_info}")
    if len(s_errors) > 0:
        print(f"S 错误索引位置: {s_errors.tolist()}")
        
    print(f"P 空间初始错误数: {len(p_errors)} / {n_parity}")
    if len(p_errors) > 0:
        print(f"P 错误索引位置: {p_errors.tolist()}")
    print("="*50 + "\n")

    # 6. 运行 Python 译码器验证
    print(">>> Python 译码器验证中...")
    s_dec = collaboration_decoder_with_bias(
        s_llr.copy(), p_llr.copy(), G, F, 
        s_bias=1.3, max_iter=20, eta=0.4, threshold=0.3
    )

    errors = np.sum(s_dec != s_true)
    print(f">>> 译码结束。剩余错误比特 (与 s_true 对比): {errors}")
    if errors == 0:
        print(">>> 成功：Python 能够完全还原随机序列！")
    else:
        # 如果报错，可以看下是不是 SNR 太低了
        print(">>> 未完全纠正，请检查是否需要调高 SNR 或增加迭代次数。")

if __name__ == "__main__":
    # 确保 load_matrix_from_bin 和 collaboration_decoder_with_bias 已在脚本中
    generate_random_test_vector()
