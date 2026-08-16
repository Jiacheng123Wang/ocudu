import numpy as np
import struct
import os

def process_ldpc_transpose(Z):
    """
    针对给定的 Z 值，读取 G 和 F 矩阵并生成对应的转置矩阵 GT 和 FT。
    """
    # 定义基础维度
    dim_46z = 46 * Z  # 92
    dim_22z = 22 * Z  # 44
    
    # 定义矩阵的任务配置: { 矩阵名: (原始行, 原始列) }
    tasks = {
        "G": (dim_46z, dim_22z),  # G: 92 x 44
        "F": (dim_22z, dim_46z)   # F: 44 x 92
    }
    
    for name, (rows, cols) in tasks.items():
        input_file = f"BG1_LSindex1_Z{Z}_{name}.bin"
        output_file = f"BG1_LSindex1_Z{Z}_{name}T.bin"
        
        if not os.path.exists(input_file):
            print(f"❌ 未找到输入文件: {input_file}")
            continue

        # --- A. 读取并解包原始矩阵 ---
        # 原始矩阵每行的 uint32 数量
        words_per_row = (cols + 31) // 32
        matrix = np.zeros((rows, cols), dtype=np.uint8)
        
        with open(input_file, "rb") as f:
            for r in range(rows):
                # 准确读取当前行所需的字节数
                row_data = f.read(words_per_row * 4)
                if not row_data: break
                
                # 将字节流转换为 uint32 数组
                words = struct.unpack(f"<{words_per_row}I", row_data)
                
                for w_idx, word_val in enumerate(words):
                    for b in range(32):
                        bit_idx = w_idx * 32 + b
                        if bit_idx < cols:
                            # 提取位信息 (LSB-first)
                            matrix[r, bit_idx] = (word_val >> b) & 1
        
        print(f"✅ 已读取 {name}: {rows}x{cols} (原始文件大小: {os.path.getsize(input_file)} bytes)")

        # --- B. 矩阵转置 ---
        # 对于 G: 92x44 -> 44x92
        # 对于 F: 44x92 -> 92x44
        matrix_t = matrix.T
        new_rows, new_cols = matrix_t.shape
        
        # --- C. 重新包装并写入转置矩阵 ---
        new_words_per_row = (new_cols + 31) // 32
        
        with open(output_file, "wb") as f:
            for r in range(new_rows):
                packed_words = []
                for w in range(new_words_per_row):
                    word_val = np.uint32(0)
                    for b in range(32):
                        bit_idx = w * 32 + b
                        if bit_idx < new_cols:
                            if matrix_t[r, bit_idx] == 1:
                                word_val |= (np.uint32(1) << b)
                    packed_words.append(word_val)
                
                # 按 uint32 序列写入
                f.write(struct.pack(f"<{new_words_per_row}I", *packed_words))
        
        actual_size = os.path.getsize(output_file)
        print(f"🚀 已生成 {name}T: {new_rows}x{new_cols}, 最终文件大小: {actual_size} 字节")

if __name__ == "__main__":
    process_ldpc_transpose(Z=384)