// clang++ -O3 -std=c++17 -fobjc-arc -framework Metal -framework Foundation encoder_test_z4_uint4.cpp ../encode/MetalEngineUint4.mm -o test_ldpc_uint4


#include <iostream>
#include <vector>
#include <fstream>
#include <iomanip>
#include "MetalEngineUint4.h"

// 修改后的 C++ 转换函数，模仿 Swift 的逻辑
void hex_to_little_endian_uint32(const uint8_t* hex, int byte_len, uint32_t* out_words) {
    uint8_t* out_bytes = reinterpret_cast<uint8_t*>(out_words);
    for (int i = 0; i < byte_len; ++i) {
        uint8_t b = hex[i];
        // 字节内位反转 (MSB-first -> LSB-first)
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        out_bytes[i] = b; 
    }
}

void print_as_3gpp_hex(const uint32_t* words, int total_bits) {
    const uint8_t* res_bytes = reinterpret_cast<const uint8_t*>(words);
    int total_bytes = (total_bits + 7) / 8;
    for (int i = 0; i < total_bytes; ++i) {
        uint8_t b = res_bytes[i];
        // 再次反转回大端格式用于打印
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b << " ";
    }
    std::cout << std::dec << std::endl;
}
int main() {
    // 1. 定义输入数据 (Z=4, s = 22 * 4 = 88 bits)
    uint8_t input_hex[] = {0xfa, 0x8d, 0xf1, 0x42, 0x9e, 0x01, 0x2a, 0xdc, 0x94, 0x16, 0xb8};
    
    // 按照 128-bit (uint4) 对齐准备输入内存
    uint32_t* s_packed;
    posix_memalign((void**)&s_packed, 16, 4 * sizeof(uint32_t)); 
    memset(s_packed, 0, 4 * sizeof(uint32_t));
    hex_to_little_endian_uint32(input_hex, 11, s_packed);

    // 2. 加载矩阵 G (Z=4)
    // 根据 Python 生成逻辑，G 维度为 (46*Z) x (22*Z) = 184 x 88
    // 补齐后维度为 256 x 128
    uint32_t num_rows_padded = 256;
    uint32_t words_per_row = 4; // 128 bit / 32 = 4 words
    
    size_t g_size = num_rows_padded * words_per_row * sizeof(uint32_t);
    uint32_t* g_matrix;
    posix_memalign((void**)&g_matrix, 16, g_size);

    std::ifstream g_file("../g_f_matrix/G_matrix_Z4_uint4.bin", std::ios::binary);
    if (!g_file) {
        std::cerr << "无法打开矩阵文件！" << std::endl;
        return -1;
    }
    g_file.read((char*)g_matrix, g_size);
    g_file.close();

    // 3. 准备输出 Buffer (p = 184 bits, 补齐至 256 bits)
    uint32_t* p_out;
    posix_memalign((void**)&p_out, 16, 8 * sizeof(uint32_t)); // 256 bits
    memset(p_out, 0, 8 * sizeof(uint32_t));

    // 4. 初始化并运行 Metal 引擎
    void* engine = init_metal_engine_uint4("./gf2_multiply_uint4_extreme.metal");
    if (!engine) return -1;

    std::cout << ">>> 开始 uint4 架构计算..." << std::endl;
    metal_compute_gf2_uint4(engine, g_matrix, s_packed, p_out, num_rows_padded, words_per_row);

    // 5. 打印结果 (我们感兴趣的是前 184 位)
    std::cout << "校验位结果 (P, 16进制大端格式): " << std::endl;
    print_as_3gpp_hex(p_out, 184);

    // 6. 清理
    destroy_metal_engine_uint4(engine);
    free(s_packed);
    free(g_matrix);
    free(p_out);

    return 0;
}