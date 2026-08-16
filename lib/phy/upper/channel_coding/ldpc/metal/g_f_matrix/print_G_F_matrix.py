import os

def print_hex_dump(file_path, bytes_per_line=16):
    if not os.path.exists(file_path):
        print(f"错误: 找不到文件 '{file_path}'")
        return

    print(f"--- 文件内容: {file_path} ---")
    try:
        with open(file_path, "rb") as f:
            offset = 0
            while True:
                # 每次读取一行数据量 (默认 16 字节)
                chunk = f.read(bytes_per_line)
                if not chunk:
                    break

                # 将字节转为 16 进制字符串
                hex_string = ' '.join(f"{b:02X}" for b in chunk)
                
                # 打印偏移量和 16 进制内容
                print(f"{offset:08X}: {hex_string.ljust(bytes_per_line * 3)} ")
                
                offset += len(chunk)
    except Exception as e:
        print(f"读取文件时出错: {e}")
    print("-" * (len(file_path) + 20) + "\n")

def main():
    # 在这里替换你的文件名
    file1 = "BG1_LSindex0_Z2_G.bin"
    file2 = "G_matrix_Z2_uint4.bin"
    # file1 = "H_matrix_Z4_uint4.bin"
    # file2 = "HT_matrix_Z4_uint4.bin"

    print_hex_dump(file1)
    print_hex_dump(file2)

if __name__ == "__main__":
    main()
