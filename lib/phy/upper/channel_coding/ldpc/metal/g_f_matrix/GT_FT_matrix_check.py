import numpy as np
import struct
import os

def verify_matrices(Z):
    # 维度定义
    dim_46z = 46 * Z  # 92
    dim_22z = 22 * Z  # 44

    def load_packed_bin(filename, rows, cols):
        words_per_row = (cols + 31) // 32
        matrix = np.zeros((rows, cols), dtype=np.uint8)
        
        if not os.path.exists(filename):
            raise FileNotFoundError(f"找不到文件: {filename}")
            
        with open(filename, "rb") as f:
            for r in range(rows):
                row_data = f.read(words_per_row * 4)
                if not row_data: break
                words = struct.unpack(f"<{words_per_row}I", row_data)
                
                for w_idx, word_val in enumerate(words):
                    for b in range(32):
                        bit_idx = w_idx * 32 + b
                        if bit_idx < cols:
                            # 提取 LSB-first 位
                            matrix[r, bit_idx] = (word_val >> b) & 1
        return matrix

    print(f"--- 开始验证 Z={Z} 的转置矩阵 ---")

    try:
        # 1. 加载 GT (44 x 92)
        gt_file = f"BG1_LSindex1_Z{Z}_GT.bin"
        GT = load_packed_bin(gt_file, dim_22z, dim_46z)
        print(f"Successfully loaded GT: {GT.shape}")

        # 2. 加载 FT (92 x 44)
        ft_file = f"BG1_LSindex1_Z{Z}_FT.bin"
        FT = load_packed_bin(ft_file, dim_46z, dim_22z)
        print(f"Successfully loaded FT: {FT.shape}")

        # 3. 计算 GT * FT (mod 2)
        # 结果应该是 44 x 44
        print("计算矩阵乘法 GT * FT (mod 2)...")
        result = np.matmul(GT.astype(np.int32), FT.astype(np.int32)) % 2
        
        # 4. 检查是否为单位矩阵
        identity = np.eye(dim_22z, dtype=np.int32)
        
        if np.array_equal(result, identity):
            print("\n" + "="*30)
            print("🌟 验证成功！GT * FT 是一个单位矩阵。")
            print("="*30)
        else:
            print("\n" + "!"*30)
            print("❌ 验证失败！结果不是单位矩阵。")
            # 打印前几行看看哪里出错了
            print("结果矩阵的前 5x5 区域:")
            print(result[:5, :5])
            
            # 统计差异点
            diff_count = np.sum(result != identity)
            print(f"总计有 {diff_count} 个元素与单位矩阵不符。")
            print("!"*30)

    except Exception as e:
        print(f"发生错误: {e}")

if __name__ == "__main__":
    verify_matrices(Z=12)