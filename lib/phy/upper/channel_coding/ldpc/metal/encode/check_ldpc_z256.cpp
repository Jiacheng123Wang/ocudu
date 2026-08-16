// “全量链接”法
// g++ -o check_ldpc check_ldpc_z256.cpp -I./include -I./build/include -I./external/fmt/include -Wl,--start-group $(find ./build/lib -name "*.a") ./build/external/fmt/libfmt.a -Wl,--end-group -lpthread -lm


#include <iostream>
#include <vector>
#include <iomanip>
#include <fstream>
#include <string>
#include <sstream>
#include "srsran/phy/upper/channel_coding/ldpc/ldpc_encoder.h"
#include "srsran/phy/upper/channel_coding/channel_coding_factories.h"
#include "srsran/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"

using namespace srsran;

// 辅助函数：解析十六进制字符串（如 "0x2f"）
uint8_t parse_hex(std::string s) {
    if (s.find("0x") != std::string::npos) s = s.substr(2);
    return (uint8_t)std::stoul(s, nullptr, 16);
}

int main() {
    // 1. 类型对齐：设置为提升因子 256
    const uint32_t Z_val = 256; // 原为 4 
    const srsran::ldpc::lifting_size_t Z = static_cast<srsran::ldpc::lifting_size_t>(Z_val);
    const uint32_t msg_len_bits = 22 * Z_val; // 5632 bits

    // 2. 构造输入：从 input_z256.txt 加载数据
    srsran::dynamic_bit_buffer message_packed(msg_len_bits);
    message_packed.zero();

    /* 原代码注释掉 
    uint8_t random_input[] = {0xa1, 0x61, 0x53, 0x89, 0x71, 0x1a, 0xaa, 0x42, 0x47, 0x1d, 0xc};
    for (int i = 0; i < 11; ++i) {
        message_packed.insert<uint8_t>(random_input[i], i * 8, 8);
    }
    */

    // 新增：读取 input_z256.txt
    std::ifstream infile("input_z256.txt");
    if (!infile.is_open()) {
        std::cerr << "Could not open input_z256.txt" << std::endl;
        return -1;
    }
    std::string line, token;
    uint32_t byte_idx = 0;
    while (std::getline(infile, line)) {
        std::stringstream ss(line);
        while (std::getline(ss, token, ',')) {
            // 移除空格
            token.erase(0, token.find_first_not_of(" \t\r\n"));
            token.erase(token.find_last_not_of(" \t\r\n") + 1);
            if (!token.empty()) {
                message_packed.insert<uint8_t>(parse_hex(token), byte_idx * 8, 8);
                byte_idx++;
            }
        }
    }
    printf("📥 srsRAN Loaded %d bytes from input_z256.txt\n", byte_idx);

    // 3. 初始化编码器
    auto enc_factory = create_ldpc_encoder_factory_sw("generic");
    if (!enc_factory) {
        std::cerr << "Failed to create factory" << std::endl;
        return -1;
    }
    auto encoder = enc_factory->create();
    
    srsran::ldpc_encoder::configuration cfg;
    cfg.base_graph = srsran::ldpc_base_graph_type::BG1;
    cfg.lifting_size = Z;

    // 4. 执行编码
    const auto& rm_buffer = encoder->encode(message_packed, cfg);

    // 5. 打印校验位（Row 0 - Row 45）
    printf("\n📊 srsRAN Z=%d Test Result:\n", Z_val);
    printf("----------------------------------------------------------------------\n");

    for (uint32_t r = 0; r < 46; ++r) {
        // 每行 Z_val (256) 个比特
        std::vector<uint8_t> row_bits(Z_val); 
        
        // srsRAN 输出打孔了前 2 个 Z 块（系统位 22 个 Z 块中的前 2 个）
        // 系统位从偏移 0 开始，校验位从偏移 20*Z 开始 
        rm_buffer.write_codeblock(row_bits, (20 + r) * Z_val);
        
        printf("Row %2d | Hex: ", r);
        
        // 每行打印前 10 个字节 (80 比特) 的十六进制
        for (int b = 0; b < 10; ++b) {
            uint8_t packed_byte = 0;
            for (int i = 0; i < 8; ++i) {
                if (row_bits[b * 8 + i]) {
                    packed_byte |= (1 << (7 - i)); // MSB-first 对齐打印
                }
            }
            printf("0x%02x ", packed_byte);
        }
        printf("...\n");
    }
    printf("----------------------------------------------------------------------\n");

    return 0;
}
