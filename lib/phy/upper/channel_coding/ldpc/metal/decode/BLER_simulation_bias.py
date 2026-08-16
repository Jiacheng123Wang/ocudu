import numpy as np
import struct
import os
import matplotlib.pyplot as plt
import time

# --- 1. 严格保留原始工具函数 ---
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

# --- 2. 核心译码：恢复最初的“慢磨”逻辑 ---
def original_style_decoder(s_llr_A, p_llr_A, G, F, s_bias, max_iter=1000, eta=0.4, threshold=0.5):
    n_info, n_parity = G.shape[1], G.shape[0]
    curr_s_llr = s_llr_A.copy()
    curr_p_llr = p_llr_A.copy()
    
    p_err_history = []

    for it in range(max_iter):
        s_hard = (curr_s_llr < 0).astype(np.uint8)
        p_hard = (curr_p_llr < 0).astype(np.uint8)

        # 矛盾检测
        p_pred = (np.matmul(G, s_hard) % 2).astype(np.uint8)
        p_conflict_mask = (p_pred != p_hard)
        num_p_conflict = np.sum(p_conflict_mask)
        
        s_pred = (np.matmul(F, p_hard) % 2).astype(np.uint8)
        s_conflict_mask = (s_pred != s_hard)
        num_s_conflict = np.sum(s_conflict_mask)

        # 完美收敛退出
        if num_p_conflict == 0 and num_s_conflict == 0:
            return s_hard

        # --- 死锁仲裁：恢复 20 轮的长窗口，仅引入 bias ---
        p_err_history.append(num_p_conflict)
        if len(p_err_history) >= 20: 
            if len(set(p_err_history[-20:])) == 1:
                avg_s_conf = np.mean(np.abs(curr_s_llr))
                avg_p_conf = np.mean(np.abs(curr_p_llr))
                # 仅在此处根据 bias 决定相信谁
                if avg_s_conf * s_bias >= avg_p_conf:
                    return s_hard
                else:
                    return (np.matmul(F, p_hard) % 2).astype(np.uint8)

        # --- 原始陪审团投票逻辑 ---
        new_s_llr = curr_s_llr.copy()
        new_p_llr = curr_p_llr.copy()

        # S 更新
        s_processed = np.zeros(n_info, dtype=bool)
        for j in np.where(p_conflict_mask)[0]:
            rel_s = np.where(G[j, :] == 1)[0]
            i_sus = rel_s[np.argmin(np.abs(curr_s_llr[rel_s]))]
            if not s_processed[i_sus]:
                all_c = np.where(G[:, i_sus] == 1)[0]
                if np.sum(p_conflict_mask[all_c]) / len(all_c) >= threshold:
                    new_s_llr[i_sus] -= np.sign(curr_s_llr[i_sus]) * eta * np.abs(curr_p_llr[j])
                    s_processed[i_sus] = True

        # P 更新
        p_processed = np.zeros(n_parity, dtype=bool)
        for i in np.where(s_conflict_mask)[0]:
            rel_p = np.where(F[i, :] == 1)[0]
            j_sus = rel_p[np.argmin(np.abs(curr_p_llr[rel_p]))]
            if not p_processed[j_sus]:
                all_c = np.where(F[:, j_sus] == 1)[0]
                if np.sum(s_conflict_mask[all_c]) / len(all_c) >= threshold:
                    new_p_llr[j_sus] -= np.sign(curr_p_llr[j_sus]) * eta * np.abs(curr_s_llr[i])
                    p_processed[j_sus] = True

        curr_s_llr, curr_p_llr = new_s_llr, new_p_llr

    return (curr_s_llr < 0).astype(np.uint8)

# --- 3. 仿真系统 ---
def run_compare(G, F, snrs, bias_val, min_err=300):
    bler_results = []
    for snr in snrs:
        errs, pkts = 0, 0
        total_pkts = 10000
        while errs < min_err and pkts < total_pkts:
            pkts += 1
            s_true = np.zeros(G.shape[1], dtype=np.uint8)
            p_true = np.zeros(G.shape[0], dtype=np.uint8)
            s_llr = generate_llr(s_true, snr)
            p_llr = generate_llr(p_true, snr)
            
            s_dec = original_style_decoder(s_llr, p_llr, G, F, s_bias=bias_val, max_iter=total_pkts)
            if np.any(s_dec != s_true):
                errs += 1
        bler = errs / min(total_pkts, pkts)
        bler_results.append(bler)
        print(f"Bias {bias_val} | SNR {snr:.1f} | BLER {bler:.2e}")
        if bler < 1e-4: break
    return bler_results

if __name__ == "__main__":
    G = load_matrix_from_bin("BG1_LSindex0_Z2_G.bin", 92, 44)
    F = load_matrix_from_bin("BG1_LSindex0_Z2_F.bin", 44, 92)
    snr_axis = np.arange(0.0, 6.0, 0.5)

    # 运行对比
    res_10 = run_compare(G, F, snr_axis, 1.0)
    res_13 = run_compare(G, F, snr_axis, 1.3)
    res_16 = run_compare(G, F, snr_axis, 1.6)

    # 绘图（纯英文避免报错）
    plt.figure(figsize=(10, 6))
    plt.semilogy(snr_axis[:len(res_10)], res_10, 'r-*', label='s_bias=1.0')
    plt.semilogy(snr_axis[:len(res_13)], res_13, 'g-x',  label='s_bias=1.3')
    plt.semilogy(snr_axis[:len(res_16)], res_16, 'b-o', label='s_bias=1.6')
    plt.grid(True, which="both", ls="--")
    plt.xlabel('SNR (dB)')
    plt.ylabel('BLER')
    plt.title('Restored Performance Verification')
    plt.legend()
    plt.savefig("Restored_Comparison.png")
    plt.show()