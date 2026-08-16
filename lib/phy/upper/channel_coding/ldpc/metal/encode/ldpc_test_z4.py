import numpy as np
import re
import os

def get_ldpc_g_matrix(file_path, Z):
    """
    核心逻辑：读取文本，生成 H，切分并求逆得到 G
    """
    BG1_ROWS = 46
    BG1_COLS = 68
    
    # 1. 解析文件获取基图
    with open(file_path, 'r', encoding='utf-8') as f:
        content = f.read().replace('NO_EDGE', '-1')
    
    numbers = re.findall(r'-?\d+', content)
    data = [int(n) for n in numbers[:BG1_ROWS * BG1_COLS]]
    bg_matrix = np.array(data).reshape(BG1_ROWS, BG1_COLS)

    # 2. 构造二进制 H 矩阵
    h_matrix = np.zeros((BG1_ROWS * Z, BG1_COLS * Z), dtype=np.uint8)
    for i in range(BG1_ROWS):
        for j in range(BG1_COLS):
            p = bg_matrix[i, j]
            if p != -1:
                v = p % Z
                for r in range(Z):
                    h_matrix[i * Z + r, j * Z + (r + v) % Z] = 1

    # 3. 切分 H_s (信息位) 和 H_p (校验位)
    split_col = 22 * Z
    h_s = h_matrix[:, :split_col]
    h_p = h_matrix[:, split_col:]

    # 4. GF(2) 下求 H_p 的逆
    # 使用高斯-约旦消元
    n = h_p.shape[0]
    aug = np.hstack((h_p.copy(), np.eye(n, dtype=np.uint8)))
    for i in range(n):
        if aug[i, i] == 0:
            pivot = np.where(aug[i+1:, i] == 1)[0]
            if len(pivot) == 0: raise ValueError("H_p is singular!")
            idx = pivot[0] + i + 1
            aug[[i, idx]] = aug[[idx, i]]
        for j in range(n):
            if i != j and aug[j, i] == 1:
                aug[j] ^= aug[i]
    h_p_inv = aug[:, n:]

    # 5. 计算 G = (H_p_inv * H_s) % 2
    g_matrix = (np.dot(h_p_inv.astype(int), h_s.astype(int)) % 2).astype(np.uint8)
    return g_matrix

def run_all_ones_test(g_matrix, Z):
    """
    测试函数：输入全 1，输出 Hex 结果用于对比 srsRAN
    """
    num_info_bits = g_matrix.shape[1]
    # 构造全 1 输入
    s = np.ones(num_info_bits, dtype=np.uint8)
    
    # 编码：p = G * s
    p = (np.dot(g_matrix.astype(int), s.astype(int)) % 2).astype(np.uint8)

    print(f"\n🧪 Z={Z} 全 1 输入测试报告 (K={num_info_bits} bits)")
    print("-" * 50)
    
    # 打印前 10 个校验块 (每个块 Z 位)
    for r in range(30):
        block = p[r*Z : (r+1)*Z]
        
        # 为了方便对比 srsRAN 的十六进制打印
        # 我们假设 srsRAN 是把这 Z 位放在了一个字节的高位
        packed_byte = 0
        for i, bit in enumerate(block):
            if bit and i < 8: # Z 可能小于 8，也可能大于 8
                packed_byte |= (1 << (7 - i))
        
        binary_str = "".join(map(str, block))
        print(f"Row {r:2d} | Hex: {packed_byte:02x} | Binary: [{binary_str}]")

def test_random_sequence_response(g_matrix, Z):
    """
    随机比特序列测试：生成 22*Z 个随机位，输出输入 Hex 和预测的输出 Hex
    """
    num_info_bits = g_matrix.shape[1]
    
    # 1. 生成随机比特 (使用固定种子方便你多次试验，或者去掉种子追求纯随机)
    # np.random.seed(42) 
    s_rand = np.random.randint(0, 2, num_info_bits).astype(np.uint8)

    # 2. 将输入比特打包成 Hex，方便你写进 srsRAN 的 C++ 代码
    print(f"--- 随机测试输入 (Z={Z}, K={num_info_bits}) ---")
    input_hex = []
    for i in range(0, num_info_bits, 8):
        byte_bits = s_rand[i:i+8]
        byte_val = 0
        for j, bit in enumerate(byte_bits):
            if bit: byte_val |= (1 << (7 - j))
        input_hex.append(f"0x{byte_val:02x}")
    print(f"输入 Hex 序列: {', '.join(input_hex)}")

    # 3. 编码：p = G * s
    p_rand = (np.dot(g_matrix.astype(int), s_rand.astype(int)) % 2).astype(np.uint8)

    print(f"\n--- G-Matrix 预测输出 ---")
    for r in range(30): # 观察前 10 个块
        block = p_rand[r*Z : (r+1)*Z]
        packed_byte = 0
        for i, bit in enumerate(block):
            if bit and i < 8:
                packed_byte |= (1 << (7 - i))
        print(f"Row {r:2d} | Hex: {packed_byte:02x} | Binary: {list(block)}")
    
    return s_rand


# --- 主程序执行 ---
FILE_NAME = 'bg1_LSindex0.txt'
Z_FACTOR = 4  # 本次测试提升因子 Z=4

try:
    # 1. 生成 G
    G_RESULT = get_ldpc_g_matrix(FILE_NAME, Z_FACTOR)
    
    # 2. 运行全 1 测试
    # run_all_ones_test(G_RESULT, Z_FACTOR)
    random_s = test_random_sequence_response(G_RESULT, 4)    

except Exception as e:
    print(f"运行失败: {e}")
