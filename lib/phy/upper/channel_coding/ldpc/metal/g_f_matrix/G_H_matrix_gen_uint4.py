import numpy as np
import re
import struct

def gf2_inverse(matrix):
    """ 在 GF(2) 域下求逆矩阵 """
    n = matrix.shape[0]
    aug = np.hstack((matrix.copy(), np.eye(n, dtype=np.uint8)))
    for i in range(n):
        if aug[i, i] == 0:
            pivot_row = -1
            for k in range(i + 1, n):
                if aug[k, i] == 1: pivot_row = k; break
            if pivot_row == -1: raise ValueError("矩阵在 GF(2) 下不可逆")
            aug[[i, pivot_row]] = aug[[pivot_row, i]]
        for j in range(n):
            if i != j and aug[j, i] == 1: aug[j] ^= aug[i]
    return aug[:, n:]

def gf2_matmul(A, B):
    """ GF(2) 域矩阵乘法 """
    return (np.dot(A.astype(int), B.astype(int)) % 2).astype(np.uint8)

def pad_matrix_for_uint4(matrix):
    """ 
    执行 uint4 (128-bit) 架构的硬性补齐:
    1. 行数补齐为 128 的倍数 (满足 Metal threadgroup 调度)
    2. 列数补齐为 128 的倍数 (确保每行是 4 个 uint32，即 1 个 uint4)
    """
    rows, cols = matrix.shape
    padded_rows = ((rows + 127) // 128) * 128
    padded_cols = ((cols + 127) // 128) * 128
    
    padded_matrix = np.zeros((padded_rows, padded_cols), dtype=np.uint8)
    padded_matrix[:rows, :cols] = matrix
    return padded_matrix

def pack_matrix_to_uint4_bin(matrix, filename):
    """
    按照流式小端 (Streaming Little-Endian) 打包：
    矩阵 Row[r, c] 的位 -> 对应 uint32 的第 (c % 32) 位。
    """
    rows, cols = matrix.shape
    # 每一行包含多少个 uint32 (已经过 pad_matrix_for_uint4 处理，必然是 4 的倍数)
    words_per_row = cols // 32 
    binary_data = bytearray()

    for r in range(rows):
        row_bits = matrix[r, :]
        for w in range(words_per_row):
            word_val = 0
            start = w * 32
            chunk = row_bits[start:start+32]
            # 核心修改：比特流中的第一个比特 (index 0) 映射到寄存器的 LSB (1 << 0)
            for i, bit in enumerate(chunk):
                if bit:
                    word_val |= (1 << i)
            
            # 使用 '<I' (小端 uint32) 确保在 Apple Silicon 内存中，
            # word_val 的 Bit 0 处于字节流的起始位置。
            binary_data.extend(struct.pack('<I', word_val))
            
    with open(filename, "wb") as f:
        f.write(binary_data)
    
    print(f"✅ 已导出: {filename:25s} | 维度: {rows:4d}x{cols:4d} | 每行 Word 数: {words_per_row}")

def generate_ldpc_matrices_uint4(bg_file_path, Z):
    """ 生成并导出符合 uint4 对齐要求的 H, G, HT 矩阵 """
    BG_ROWS, BG_COLS = 46, 68
    try:
        with open(bg_file_path, 'r') as f:
            nums = re.findall(r'-?\d+', f.read().replace('NO_EDGE', '-1'))
        bg_data = [int(n) for n in nums[:BG_ROWS*BG_COLS]]
        bg_matrix = np.array(bg_data).reshape(BG_ROWS, BG_COLS)
    except FileNotFoundError:
        print(f"❌ 错误: 找不到文件 {bg_file_path}")
        return

    # 1. Lifting 生成原始 H 矩阵
    h_matrix = np.zeros((BG_ROWS * Z, BG_COLS * Z), dtype=np.uint8)
    for i in range(BG_ROWS):
        for j in range(BG_COLS):
            p = bg_matrix[i, j]
            if p != -1:
                shift = p % Z
                for r in range(Z):
                    # 5G NR 标准 Lifting
                    h_matrix[i*Z + r, j*Z + (r + shift) % Z] = 1

    # 2. 计算生成矩阵 G (G = Hp^-1 * Hs)
    split_col = 22 * Z
    h_s = h_matrix[:, :split_col]
    h_p = h_matrix[:, split_col:]
    print(f"--- 正在为 Z={Z} 计算矩阵 ---")
    h_p_inv = gf2_inverse(h_p)
    g_matrix = gf2_matmul(h_p_inv, h_s)
    
    # 3. 生成 HT 矩阵 (用于校验计算)
    ht_matrix = h_matrix.T

    # 4. 执行 uint4 补齐并打包导出
    pack_matrix_to_uint4_bin(pad_matrix_for_uint4(h_matrix),  f"H_matrix_Z{Z}_uint4.bin")
    pack_matrix_to_uint4_bin(pad_matrix_for_uint4(g_matrix),  f"G_matrix_Z{Z}_uint4.bin")
    pack_matrix_to_uint4_bin(pad_matrix_for_uint4(ht_matrix), f"HT_matrix_Z{Z}_uint4.bin")

if __name__ == "__main__":
    # 生成 Z=2 (用于对比你的 srsRAN 输出) 和 Z=4 (用于你的 C++ 测试)
    for z_val in [128, 64, 32]:
        generate_ldpc_matrices_uint4('bg1_LSindex0.txt', z_val)
