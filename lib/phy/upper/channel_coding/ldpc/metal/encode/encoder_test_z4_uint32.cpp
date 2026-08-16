// clang++ -O3 -std=c++17 -fobjc-arc -framework Metal -framework Foundation encoder_test_z4_uint32.cpp MetalEngine.mm -o test_predict

#include <iostream>
#include <vector>
#include <fstream>
#include <iomanip>
#include <cstring>
#include "MetalEngine.h"

/**
 * 核心修改 1：输入转换 (3GPP Hex -> Bit-packed uint32)
 * 映射逻辑：输入字节的最高位 (1<<7) 是流的第 0 位，
 * 必须映射到 uint32 的最低位 (1<<0)，以适配 Metal 的 popcount 逻辑。
 */
void hex_to_bitpacked_uint32(const uint8_t* hex, int byte_len, uint32_t* out_words) {
    for (int i = 0; i < byte_len; ++i) {
        uint8_t byte = hex[i];
        for (int bit = 0; bit < 8; ++bit) {
            // 检查字节的最高位 (Bit 7, 6, 5...)
            if (byte & (1 << (7 - bit))) { 
                int global_bit_idx = i * 8 + bit;
                int word_idx = global_bit_idx / 32;
                int bit_in_word = global_bit_idx % 32;
                // 存入 uint32 的最低位 (Bit 0, 1, 2...)
                out_words[word_idx] |= (1U << bit_in_word);
            }
        }
    }
}

/**
 * 核心修改 2：输出打印 (Metal uint8 Bits -> 3GPP Hex)
 * 映射逻辑：Metal 输出的第 0 个比特 (bit_array[0]) 应该放在第一个字节的最高位 (1<<7)。
 */
void print_bits_as_3gpp_hex(const uint8_t* bit_array, int total_bits) {
    // 按照 Z=4 分块打印，以匹配你的 Python 脚本输出格式
    const int Z = 4;
    for (int r = 0; r < 30; r++) { // 打印前 30 行
        uint8_t packed_byte = 0;
        int start_idx = r * Z;
        
        // 构造该块的 Hex 值：第一个遇到的比特放在最高位 (3GPP 习惯)
        for (int i = 0; i < Z; ++i) {
            if (bit_array[start_idx + i] == 1) {
                packed_byte |= (1 << (7 - i));
            }
        }
        
        printf("Row %2d | Hex: %02x | Binary: [", r, packed_byte);
        for (int i = 0; i < Z; ++i) {
            printf("%d", bit_array[start_idx + i]);
        }
        printf("]\n");
    }
}

int main_1() {
    // 1. 参数设置 (Z=4)
    const int Z = 4;
    const int num_bits = 46 * Z;      // 输出 184 比特
    const int input_bits = 22 * Z;    // 输入 88 比特
    const int num_chunks = (input_bits + 31) / 32; // 每行 3 个 uint32
    
    // 2. 准备输入数据
    uint8_t input_hex[] = {0xfa, 0x8d, 0xf1, 0x42, 0x9e, 0x01, 0x2a, 0xdc, 0x94, 0x16, 0xb8};
    uint32_t* s_packed = (uint32_t*)malloc(num_chunks * sizeof(uint32_t));
    memset(s_packed, 0, num_chunks * sizeof(uint32_t));
    hex_to_bitpacked_uint32(input_hex, 11, s_packed);

    // 3. 加载 G 矩阵
    // 文件格式：184 行，每行 3 个 uint32，无额外对齐
    size_t g_size = num_bits * num_chunks * sizeof(uint32_t);
    uint32_t* g_matrix = (uint32_t*)malloc(g_size);
    
    std::ifstream g_file("../g_f_matrix/BG1_LSindex0_Z4_G.bin", std::ios::binary);
    if (!g_file) {
        std::cerr << "错误: 无法打开矩阵文件 BG1_LSindex0_Z4_G.bin" << std::endl;
        return -1;
    }
    g_file.read((char*)g_matrix, g_size);
    g_file.close();

    // 4. 准备输出 Buffer (uint8_t 数组，184 字节)
    uint8_t* p_pred = (uint8_t*)malloc(num_bits);
    memset(p_pred, 0, num_bits);

    // 5. 初始化 Metal 引擎并计算
    // 注意：shader_path 指向包含 ldpc_predict_v4 的 .metal 文件
    void* engine = init_metal_engine("matrix_vector_gpu.metal"); 
    if (!engine) {
        std::cerr << "Metal 初始化失败" << std::endl;
        return -1;
    }

    std::cout << ">>> 启动 ldpc_predict_v4 内核计算 (Z=4)..." << std::endl;
    metal_compute_gf2(engine, KERNEL_TYPE_PREDICT_V4, g_matrix, s_packed, p_pred, num_bits, num_chunks);

    // 6. 结果展示
    std::cout << "校验位结果 (P, 3GPP Hex 格式):" << std::endl;
    print_bits_as_3gpp_hex(p_pred, num_bits);

    // 7. 清理
    deinit_metal_engine(engine);
    free(s_packed);
    free(g_matrix);
    free(p_pred);

    return 0;
}

/**
 * 输出转换：Bit-packed uint32 -> 3GPP Hex (MSB-first)
 * 逻辑：取出 uint32 的第 n 位，将其放入字节的第 (7 - (n%8)) 位
 */
void print_packed_uint32_as_3gpp_hex(const uint32_t* words, int total_bits) {
    int total_bytes = (total_bits + 7) / 8;
    
    for (int i = 0; i < total_bytes; ++i) {
        uint8_t byte_val = 0;
        for (int bit = 0; bit < 8; ++bit) {
            int global_bit_idx = i * 8 + bit;
            if (global_bit_idx >= total_bits) break;

            int word_idx = global_bit_idx / 32;
            int bit_in_word = global_bit_idx % 32;

            // 检查 Metal 算子存入的位
            if (words[word_idx] & (1U << bit_in_word)) {
                // 放入 3GPP 字节的高位
                byte_val |= (1 << (7 - bit));
            }
        }
        printf("%02x ", byte_val);
        if ((i + 1) % 16 == 0) printf("\n");
    }
    printf("\n");
}

int main() {
    const int Z = 4;
    const int num_info_bits = 22 * Z;   // 88
    const int num_parity_bits = 46 * Z; // 184
    const int num_chunks = (num_info_bits + 31) / 32; // 3
    
    // 矩阵行数补齐到 192 (32*6)，输出 6 个 uint32
    const int num_rows_padded = 192; 
    const int out_words_count = num_rows_padded / 32;

    // 1. 准备输入向量
    uint8_t input_hex[] = {0xfa, 0x8d, 0xf1, 0x42, 0x9e, 0x01, 0x2a, 0xdc, 0x94, 0x16, 0xb8};
    uint32_t s_packed[3] = {0, 0, 0};
    hex_to_bitpacked_uint32(input_hex, 11, s_packed);

    // 2. 加载对齐后的 G 矩阵 (G_matrix_Z4.bin)
    size_t g_size = num_rows_padded * num_chunks * sizeof(uint32_t);
    uint32_t* g_matrix = (uint32_t*)malloc(g_size);
    
    std::ifstream g_file("../g_f_matrix/G_matrix_Z4.bin", std::ios::binary);
    if (!g_file) {
        std::cerr << "错误: 无法打开 G_matrix_Z4.bin" << std::endl;
        return -1;
    }
    g_file.read((char*)g_matrix, g_size);
    g_file.close();

    // 3. 准备输出 Buffer (6 个 uint32)
    uint32_t* p_packed_out = (uint32_t*)malloc(out_words_count * sizeof(uint32_t));
    memset(p_packed_out, 0, out_words_count * sizeof(uint32_t));

    // 4. 初始化 Metal 引擎
    void* engine = init_metal_engine("matrix_vector_gpu.metal");
    if (!engine) return -1;

    // 5. 调用 gf2_matrix_vector_multiply
    // 注意：KERNEL_TYPE_BIT_PACKED 对应我们想要测试的 packed 算子
    std::cout << ">>> 启动 gf2_matrix_vector_multiply 内核计算 (Z=4)..." << std::endl;
    metal_compute_gf2(engine, KERNEL_TYPE_BIT_PACKED, g_matrix, s_packed, p_packed_out, num_rows_padded, num_chunks);

    // --- 验证打印：从 LSB (bit 0) 开始，每 4 位一组 ---
    if (out_words_count > 0) {
        uint32_t first_word = p_packed_out[0];
        std::cout << ">>> p_packed_out[0] 比特分析 (从 LSB 到 MSB) <<<" << std::endl;
        std::cout << "Hex 原值: 0x" << std::hex << std::setw(8) << std::setfill('0') << first_word << std::dec << std::endl;
        
        for (int group = 0; group < 8; ++group) {
            std::cout << "Group " << group << " (Bit " << std::setw(2) << group*4 << "-" << std::setw(2) << group*4+3 << "): ";
            for (int i = 0; i < 4; ++i) {
                int bit_idx = group * 4 + i;
                int bit_val = (first_word >> bit_idx) & 1;
                std::cout << bit_val;
            }
            std::cout << std::endl;
        }
    }

    // 6. 打印前 184 位的结果
    std::cout << "校验位结果 (P, 3GPP Hex 格式):" << std::endl;
    print_packed_uint32_as_3gpp_hex(p_packed_out, num_parity_bits);

    // 7. 清理
    deinit_metal_engine(engine);
    free(g_matrix);
    free(p_packed_out);

    return 0;
}

