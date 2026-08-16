import numpy as np

def generate_ldpc_test_input(bit_length, output_file="test_input.txt"):
    """
    生成随机比特序列并按 16 进制打包保存
    逻辑：每 8 个比特打包为一个字节，字节内第一个比特在最高位 (MSB)
    """
    if bit_length % 8 != 0:
        print(f"警告: 长度 {bit_length} 不是 8 的倍数，生成的 16 进制将包含补零。")
    
    # 1. 生成随机比特 0 或 1
    bits = np.random.randint(0, 2, bit_length).astype(np.uint8)
    
    # 2. 打包为字节数组
    hex_list = []
    for i in range(0, len(bits), 8):
        byte_bits = bits[i:i+8]
        byte_val = 0
        for j, bit in enumerate(byte_bits):
            if bit:
                # 按照 MSB-first 原则：序列中的第 1 个比特在 0x80 位 
                byte_val |= (1 << (7 - j))
        hex_list.append(f"0x{byte_val:02x}")
    
    # 3. 写入文件
    content = ", ".join(hex_list)
    with open(output_file, "w") as f:
        f.write(content)
    
    print(f"✅ 已生成 {bit_length} 比特 (\(len(hex_list)) 字节) 并写入 {output_file}")
    return content

# 执行生成：对于 Z=256，信息位长度为 22 * 256 = 5632
generate_ldpc_test_input(8448, "input_z384.txt")
