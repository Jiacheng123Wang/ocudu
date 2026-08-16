import numpy as np
import re
import struct
import os

def gf2_inverse(matrix):
    """
    在 GF(2) 域下使用高斯-约旦消元法求逆矩阵
    """
    n = matrix.shape[0]
    # 构造增广矩阵 [A | I]
    aug = np.hstack((matrix.copy(), np.eye(n, dtype=np.uint8)))
    
    for i in range(n):
        # 1. 寻找主元：如果当前位置是 0，向下寻找第一个 1
        if aug[i, i] == 0:
            pivot_row = -1
            for k in range(i + 1, n):
                if aug[k, i] == 1:
                    pivot_row = k
                    break
            if pivot_row == -1:
                raise ValueError("矩阵在 GF(2) 下不可逆！")
            # 行交换
            aug[[i, pivot_row]] = aug[[pivot_row, i]]
        
        # 2. 消元：将其他行的第 i 列清零
        # 在 GF(2) 下，减法就是加法，也就是 XOR
        for j in range(n):
            if i != j and aug[j, i] == 1:
                aug[j] = aug[j] ^ aug[i]  # 使用位运算 XOR
                
    # 提取右侧的逆矩阵部分
    return aug[:, n:]

def gf2_matmul(A, B):
    """
    GF(2) 下的矩阵乘法 (A * B) % 2
    """
    # 使用矩阵点积后取模 2
    return (np.dot(A.astype(int), B.astype(int)) % 2).astype(np.uint8)

def process_ldpc_inversion(bg_file_path, Z):
    # --- 1. 生成 H 矩阵 (参考之前逻辑) ---
    BG1_ROWS, BG1_COLS = 46, 68
    with open(bg_file_path, 'r') as f:
        nums = re.findall(r'-?\d+', f.read().replace('NO_EDGE', '-1'))
    bg_data = [int(n) for n in nums[:BG1_ROWS*BG1_COLS]]
    bg_matrix = np.array(bg_data).reshape(BG1_ROWS, BG1_COLS)

    h_matrix = np.zeros((BG1_ROWS * Z, BG1_COLS * Z), dtype=np.uint8)
    for i in range(BG1_ROWS):
        for j in range(BG1_COLS):
            p = bg_matrix[i, j]
            if p != -1:
                v = p % Z
                for r in range(Z):
                    h_matrix[i*Z + r, j*Z + (r+v)%Z] = 1

    # --- 2. 切分 H_s 和 H_p ---
    # H_s: 前 22*Z 列, H_p: 后 46*Z 列
    split_col = 22 * Z
    h_s = h_matrix[:, :split_col]
    h_p = h_matrix[:, split_col:]
    
    print(f"--- 矩阵切分报告 ---")
    print(f"H_s 维度: {h_s.shape}")
    print(f"H_p 维度: {h_p.shape}")

    # --- 3. 在 GF(2) 下求 H_p 的逆 ---
    print(f"\n正在计算 H_p 的逆矩阵 (Z={Z})... 请稍候...")
    try:
        h_p_inv = gf2_inverse(h_p)
        print("✅ 逆矩阵计算完成。")
    except ValueError as e:
        print(f"❌ 失败: {e}")
        return

    # --- 4. 验证验证: H_p * H_p_inv == I ---
    print("\n正在验证结果 (H_p * H_p_inv)...")
    identity_check = gf2_matmul(h_p, h_p_inv)
    expected_i = np.eye(h_p.shape[0], dtype=np.uint8)
    
    if np.array_equal(identity_check, expected_i):
        print("🎉 验证通过！结果是一个完美的单位矩阵。")
    else:
        # 如果不相等，找出错误点
        diff = np.sum(np.abs(identity_check.astype(int) - expected_i.astype(int)))
        print(f"❌ 验证失败！与单位矩阵有 {diff} 个元素不同。")
        return

    # --- 5. 打印最终报告 ---
    print("\n" + "="*40)
    print("      5G NR LDPC 预计算报告")
    print("="*40)
    print(f"Lifting Size (Z): {Z}")
    print(f"H_p 维度        : {h_p.shape[0]}x{h_p.shape[1]}")
    print(f"H_p 是否满秩    : 是 (已求逆验证)")
    print(f"生成矩阵 G 准备 : G = (H_p^-1 * H_s) % 2")
    
    # 计算 G 并返回
    g_matrix = gf2_matmul(h_p_inv, h_s)
    print(f"G 矩阵最终维度  : {g_matrix.shape} (Row_Parity x Col_Info)")
    print("="*40)
    
    return g_matrix, h_p_inv, h_p, h_s

# 假设之前的 gf2_matmul, h_p_inv, h_s 已经在内存中
def generate_and_analyze_g(h_p_inv, h_s):
    """
    计算 G = H_p_inv * H_s 并统计每一行的 1 的个数
    """
    print("\n正在计算生成矩阵 G = (H_p^-1 * H_s) % 2 ...")
    
    # 1. 计算 G
    # 使用 int 矩阵乘法后取模 2
    g_matrix = (np.dot(h_p_inv.astype(int), h_s.astype(int)) % 2).astype(np.uint8)
    
    # 2. 统计每一行的非零个数 (Weight of each row)
    row_weights = np.sum(g_matrix, axis=1)
    
    # 3. 统计总体的稀疏度
    total_elements = g_matrix.size
    total_ones = np.sum(row_weights)
    density = (total_ones / total_elements) * 100
    
    print("-" * 40)
    print(f"📊 G 矩阵分析报告 (Density Analysis)")
    print("-" * 40)
    print(f"G 矩阵维度: {g_matrix.shape[0]} 行 x {g_matrix.shape[1]} 列")
    print(f"总元素个数: {total_elements}")
    print(f"总共 '1' 的个数: {total_ones}")
    print(f"平均密度 (Density): {density:.2f}%")
    print(f"最稀疏行 (Min ones in a row): {np.min(row_weights)}")
    print(f"最密集行 (Max ones in a row): {np.max(row_weights)}")
    print(f"平均每行 '1' 的个数: {np.mean(row_weights):.2f}")
    print("-" * 40)

    # 4. 打印每行的具体统计数据（前 20 行作为参考）
    print("前 20 行每行 '1' 的个数统计:")
    for idx, weight in enumerate(row_weights[:20]):
        print(f"Row {idx:2d}: {weight:2d} 个 '1'", end=" | " if (idx+1)%4 != 0 else "\n")
    
    return g_matrix, row_weights

def export_g_matrix_for_metal(g_matrix, Z, filename="g_matrix.bin"):
    """
    将 G 矩阵打包为 uint32 数组并保存为二进制文件
    """
    rows, cols = g_matrix.shape
    # 每一行包含的 uint32 个数 (向上取整)
    words_per_row = (cols + 31) // 32
    
    # 构造打包后的二进制流
    binary_data = bytearray()
    
    print(f"导出参数: Z={Z}, 行={rows}, 原列数={cols}, 每行words={words_per_row}")
    
    for r in range(rows):
        row_bits = g_matrix[r, :]
        for w in range(words_per_row):
            # 提取 32 个比特
            start = w * 32
            end = min(start + 32, cols)
            bits = row_bits[start:end]
            
            # 打包为 uint32 (采用小端字节序，或者根据你的 Metal 习惯调整)
            # 这里采用：第一个比特在 uint32 的最低位 (LSB)
            word_val = 0
            for i, bit in enumerate(bits):
                if bit:
                    word_val |= (1 << i)
            
            # 使用 'I' (unsigned int, 32 bit) 打包
            binary_data.extend(struct.pack('<I', word_val))

    with open(filename, "wb") as f:
        f.write(binary_data)
    
    print(f"✅ 成功导出！文件大小: {len(binary_data) / 1024:.2f} KB")
    return words_per_row
#####################################################################
# def calculate_f_matrix(h_s, h_p, Z):
#     """
#     计算 F 矩阵: F = inv(Hs^T * Hs) * Hs^T * Hp
#     依据: Hs * S + Hp * P = 0 => Hs * S = Hp * P (GF2)
#     映射结果: S = F * P
#     """
#     print(f"\n🚀 开始计算 F 矩阵 (反向映射, Z={Z})...")
    
#     # 1. 计算 Hs 的转置 (22Z x 46Z)
#     hs_t = h_s.T
    
#     # 2. 计算 (Hs^T * Hs) (22Z x 22Z)
#     print("正在计算 Hs^T * Hs ...")
#     hs_t_hs = gf2_matmul(hs_t, h_s)
    
#     # 3. 计算其在 GF2 下的逆 (22Z x 22Z)
#     print("正在对自相关矩阵求逆 (GF2高斯消元)...")
#     try:
#         inv_hs_t_hs = gf2_inverse(hs_t_hs)
#         print("✅ 自相关矩阵求逆成功。")
#     except ValueError as e:
#         print(f"❌ 错误: Hs 可能是列不相关的，无法计算广义逆: {e}")
#         return None

#     # 4. 计算 F = inv_hs_t_hs * hs_t * hp
#     print("正在生成最终映射矩阵 F...")
#     # 先算中间项 (22Z x 46Z)
#     mid_term = gf2_matmul(inv_hs_t_hs, hs_t)
#     # 得到 F (22Z x 46Z)
#     f_matrix = gf2_matmul(mid_term, h_p)
    
#     print(f"✅ F 矩阵计算完成, 维度: {f_matrix.shape[0]} 行 x {f_matrix.shape[1]} 列")
#     return f_matrix

# def export_matrix_to_bitmap(matrix, filename):
#     """
#     将矩阵导出为 Bitmap (.bmp) 图像
#     每行自动按照 32 位 (4字节) 向上取整补齐
#     """
#     rows, cols = matrix.shape
#     # 计算 32 位对齐后的宽度
#     padded_cols = ((cols + 31) // 32) * 32
    
#     # 创建对齐后的数组 (0->黑色, 1->255白色)
#     # 使用 uint8 兼容 PIL L 模式
#     padded_data = np.zeros((rows, padded_cols), dtype=np.uint8)
#     padded_data[:, :cols] = matrix * 255
    
#     # 保存图像
#     img = Image.fromarray(padded_data, mode='L')
#     img.save(filename)
#     print(f"🖼️  Bitmap 已导出: {filename} (对齐尺寸: {rows}x{padded_cols})")

def analyze_f_matrix(f_matrix):
    """ 对 F 矩阵的稀疏度进行简单分析 """
    if f_matrix is None: return
    row_weights = np.sum(f_matrix, axis=1)
    print("-" * 40)
    print(f"📊 F 矩阵分析报告")
    print(f"总 '1' 个数: {np.sum(row_weights)}")
    print(f"平均每行权重: {np.mean(row_weights):.2f}")
    print(f"单行最大权重: {np.max(row_weights)}")
    print("-" * 40)
# def gf2_left_inverse(A):
#     """
#     在 GF(2) 域下求长方形矩阵 A (m x n, m > n) 的左逆矩阵 L (n x m)
#     使得 L * A = I_n
#     """
#     m, n = A.shape
#     # 构造增广矩阵 [A | I_m] -> 注意这里是拼一个 m 维单位阵
#     aug = np.hstack((A.astype(np.uint8), np.eye(m, dtype=np.uint8)))
    
#     row = 0
#     for col in range(n):
#         if row >= m: break
        
#         # 寻找主元
#         pivot = row + np.argmax(aug[row:, col])
#         if aug[pivot, col] == 0:
#             # 如果这一列全为0，说明 A 不是列满秩的
#             continue 
            
#         # 交换行
#         aug[[row, pivot]] = aug[[pivot, row]]
        
#         # 消元
#         for i in range(m):
#             if i != row and aug[i, col] == 1:
#                 aug[i] ^= aug[row]
#         row += 1
    
#     # 检查前 n 行前 n 列是否构成了单位阵
#     if not np.array_equal(aug[:n, :n], np.eye(n, dtype=np.uint8)):
#         raise ValueError("矩阵在 GF(2) 下不是列满秩的，无法计算左逆！")
        
#     # 左逆矩阵 L 就是变换后的前 n 行的后 m 列部分
#     return aug[:n, n:]

def calculate_f_matrix_fixed(h_s, h_p, z):
    """
    修正后的 F 矩阵计算：直接利用左逆性质
    """
    print(f"\n🚀 开始修正版 F 矩阵计算 (Z={z})...")
    
    # 方案：F = LeftInverse(Hs) * Hp
    try:
        print("正在计算 Hs 的左逆矩阵...")
        L_hs = gf2_left_inverse(h_s)
        print("✅ 左逆矩阵 L_hs 计算完成。")
        
        print("正在合成 F = L_hs * Hp ...")
        f_matrix = gf2_matmul(L_hs, h_p)
        
        return f_matrix
    except ValueError as e:
        print(f"❌ 失败: {e}")
        return None

# 如果你已经有了 G 矩阵，用这个最快：
def calculate_f_from_g(g_matrix):
    """
    直接求 G 的左逆。因为 P = G*S，所以 S = LeftInv(G)*P
    """
    print("正在通过 G 矩阵直接推导 F ...")
    return gf2_left_inverse(g_matrix)
########################################################################
def gf2_left_inverse(A):
    """
    在 GF(2) 域下求长方形矩阵 A (m x n, m > n) 的左逆矩阵 L (n x m)
    使得 L * A = I_n
    """
    m, n = A.shape
    # 构造增广矩阵 [A | I_m]
    aug = np.hstack((A.astype(np.uint8), np.eye(m, dtype=np.uint8)))
    
    row = 0
    for col in range(n):
        if row >= m: break
        
        # 寻找主元
        pivot = row + np.argmax(aug[row:, col])
        if aug[pivot, col] == 0:
            continue 
            
        # 交换行
        aug[[row, pivot]] = aug[[pivot, row]]
        
        # 消元
        for i in range(m):
            if i != row and aug[i, col] == 1:
                aug[i] ^= aug[row]
        row += 1
    
    # 检查前 n 行前 n 列是否构成了单位阵
    if not np.array_equal(aug[:n, :n], np.eye(n, dtype=np.uint8)):
        raise ValueError("矩阵在 GF(2) 下不是列满秩的，无法计算左逆！")
        
    return aug[:n, n:]

def generate_ldpc_filename(bg_type, ls_index, z, matrix_type):
    """
    统一命名规则: BGx_LSindexX_ZXXX_G.bin 或 BGx_LSindexX_ZXXX_F.bin
    """
    return f"{bg_type}_LSindex{ls_index}_Z{z}_{matrix_type}.bin"

def calculate_f_matrix_and_export_bin_v2(h_s, h_p, bg_type, ls_index, z):
    """
    计算 F 矩阵并按照标准命名规则导出二进制文件
    """
    print(f"\n🚀 开始计算 {bg_type} (LSindex={ls_index}, Z={z}) 的 F 矩阵...")
    
    # 1. 计算左逆
    try:
        # 确保这里调用的是定义的函数名 gf2_left_inverse
        l_hs = gf2_left_inverse(h_s)
        # 使用你代码里已有的 gf2_matmul 
        f_matrix = gf2_matmul(l_hs, h_p)
    except ValueError as e:
        print(f"❌ 计算终止: {e}")
        return None

    # 2. 构造符合规则的文件名
    filename = generate_ldpc_filename(bg_type, ls_index, z, "F")
    
    # 3. 执行二进制打包导出
    rows, cols = f_matrix.shape
    words_per_row = (cols + 31) // 32
    
    binary_data = bytearray()
    for r in range(rows):
        row_bits = f_matrix[r, :]
        for w in range(words_per_row):
            start = w * 32
            end = min(start + 32, cols)
            bits = row_bits[start:end]
            
            # 采用 LSB-first 模式，与你的 G 矩阵导出逻辑保持一致
            word_val = 0
            for i, bit in enumerate(bits):
                if bit:
                    word_val |= (1 << i)
            binary_data.extend(struct.pack("<I", word_val))

    with open(filename, "wb") as f:
        f.write(binary_data)
    
    print(f"✅ 成功导出: {filename}")
    print(f"   规格: {rows}行 x {cols}列 | 每行 {words_per_row} uint32 | 大小: {len(binary_data)/1024:.2f} KB")
    return f_matrix

def check_matrix_G_F(F_RESULT, G_RESULT):
    # ---  验证: F * G == I ---
    print("\n正在验证反向映射的一致性 (F * G)...")
    
    # 确保 F_RESULT 和 G_RESULT 都存在
    if F_RESULT is not None and G_RESULT is not None:
        # 计算乘积
        identity_check = gf2_matmul(F_RESULT, G_RESULT)
        
        # 构造预期的单位矩阵 (维度应该是 信息位数量 x 信息位数量)
        # 即 22Z x 22Z
        expected_size = F_RESULT.shape[0] 
        expected_i = np.eye(expected_size, dtype=np.uint8)
        
        if np.array_equal(identity_check, expected_i):
            print(f"🎉 验证通过！F * G 是一个完美的 {expected_size}x{expected_size} 单位矩阵。")
        else:
            # 使用异或找出不同点（在 GF2 中，x != y 等价于 x ^ y == 1）
            diff_matrix = identity_check ^ expected_i
            diff_count = np.sum(diff_matrix)
            print(f"❌ 验证失败！")
            print(f"   F 维度: {F_RESULT.shape}, G 维度: {G_RESULT.shape}")
            print(f"   乘积维度: {identity_check.shape}, 预期维度: {expected_i.shape}")
            print(f"   共有 {diff_count} 个位置不符合单位矩阵定义。")


# --- 在原来的 __main__ 部分追加调用 ---
if __name__ == "__main__":
    # 你原来的逻辑
    Z_VAL = 6 # 建议先用小 Z 验证逻辑
    bg_type = "BG1"
    ls_index = 1
    G_RESULT, hp_inv, hp, hs = process_ldpc_inversion('bg1_LSindex1.txt', Z_VAL)
    filename = generate_ldpc_filename(bg_type, ls_index, Z_VAL, "G") 
    export_g_matrix_for_metal(G_RESULT, Z_VAL, filename=filename)

    # F_RESULT = calculate_f_from_g(G_RESULT) # 示例占位，具体传入变量
    F_RESULT = calculate_f_matrix_and_export_bin_v2(hs, hp, bg_type, ls_index, Z_VAL)

    analyze_f_matrix(F_RESULT)
    check_matrix_G_F(F_RESULT, G_RESULT)
    
 # --- 运行验证 ---
# 建议先用 Z=2 运行，速度极快且易于观察
#Z = 2
#G_RESULT, h_p_inv, h_s = process_ldpc_inversion('bg1_LSindex0.txt', Z)
#g_matrix, row_weights = generate_and_analyze_g(h_p_inv, h_s)
#words_per_line = export_g_matrix_for_metal(g_matrix, Z)
