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

def pad_matrix_rows(matrix, alignment=32):
    """ 将矩阵的行数补齐为 alignment 的整数倍 """
    rows, cols = matrix.shape
    padded_rows = ((rows + alignment - 1) // alignment) * alignment
    if padded_rows == rows:
        return matrix
    padding = np.zeros((padded_rows - rows, cols), dtype=np.uint8)
    return np.vstack((matrix, padding))

def pack_matrix_to_uint32_bin(matrix, filename):
    """
    将矩阵打包为 uint32 二进制文件。
    行/列双重对齐，LSB-first 位序。
    """
    rows, cols = matrix.shape
    words_per_row = (cols + 31) // 32
    binary_data = bytearray()

    for r in range(rows):
        row_bits = matrix[r, :]
        for w in range(words_per_row):
            word_val = 0
            start = w * 32
            end = min(start + 32, cols)
            chunk = row_bits[start:end]
            for i, bit in enumerate(chunk):
                if bit:
                    word_val |= (1 << i)
            binary_data.extend(struct.pack('<I', word_val))
            
    with open(filename, "wb") as f:
        f.write(binary_data)
    
    print(f"✅ 已导出: {filename:20s} | 尺寸: {rows:3d}x{cols:3d} | 大小: {len(binary_data)} 字节")

def generate_ldpc_matrices_all(bg_file_path, Z):
    """
    生成 H, G 以及 HT 矩阵，并全部导出为对齐的二进制文件。
    """
    # 1. 解析 Base Graph (BG1: 46x68)
    BG_ROWS, BG_COLS = 46, 68
    with open(bg_file_path, 'r') as f:
        nums = re.findall(r'-?\d+', f.read().replace('NO_EDGE', '-1'))
    bg_data = [int(n) for n in nums[:BG_ROWS*BG_COLS]]
    bg_matrix = np.array(bg_data).reshape(BG_ROWS, BG_COLS)

    # 2. Lifting 生成原始 H 矩阵
    h_matrix = np.zeros((BG_ROWS * Z, BG_COLS * Z), dtype=np.uint8)
    for i in range(BG_ROWS):
        for j in range(BG_COLS):
            p = bg_matrix[i, j]
            if p != -1:
                shift = p % Z
                for r in range(Z):
                    h_matrix[i*Z + r, j*Z + (r + shift) % Z] = 1

    # 3. 计算生成矩阵 G
    split_col = 22 * Z
    h_s = h_matrix[:, :split_col]
    h_p = h_matrix[:, split_col:]
    print(f"--- 正在为 Z={Z} 计算矩阵 ---")
    h_p_inv = gf2_inverse(h_p)
    g_matrix = gf2_matmul(h_p_inv, h_s)
    
    # 4. 生成 HT 矩阵 (原始转置)
    ht_matrix = h_matrix.T

    # 5. 行补齐 (为了满足 Metal 端的无分支计算)
    h_padded = pad_matrix_rows(h_matrix, 32)
    g_padded = pad_matrix_rows(g_matrix, 32)
    ht_padded = pad_matrix_rows(ht_matrix, 32)

    # 6. 导出文件
    pack_matrix_to_uint32_bin(h_padded,  f"H_matrix_Z{Z}.bin")
    pack_matrix_to_uint32_bin(g_padded,  f"G_matrix_Z{Z}.bin")
    pack_matrix_to_uint32_bin(ht_padded, f"HT_matrix_Z{Z}.bin")

if __name__ == "__main__":
    generate_ldpc_matrices_all('bg1_LSindex1.txt', 384)
    pass
