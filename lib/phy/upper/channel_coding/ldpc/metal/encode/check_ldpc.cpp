// “全量链接”法
// g++ -o check_ldpc check_ldpc.cpp -I./include -I./build/include -I./external/fmt/include -Wl,--start-group $(find ./build/lib -name "*.a") ./build/external/fmt/libfmt.a -Wl,--end-group -lpthread -lm

#include <iostream>
#include <vector>
#include <iomanip>
#include "srsran/phy/upper/channel_coding/ldpc/ldpc_encoder.h"
#include "srsran/phy/upper/channel_coding/channel_coding_factories.h"
#include "srsran/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"

using namespace srsran;

int main() {
    // 1. 类型对齐：设置为极小提升因子 2
    const uint32_t Z_val = 4;
    const srsran::ldpc::lifting_size_t Z = static_cast<srsran::ldpc::lifting_size_t>(Z_val);
    const uint32_t msg_len_bits = 22 * Z_val; // 44 bits

    // 2. 构造输入：单比特脉冲 (Bit 0 = 1)
    srsran::dynamic_bit_buffer message_packed(msg_len_bits);
    message_packed.zero();
    
    // 0x80 (1000 0000) 插入到起始位置，意味着第一个块的第一个比特为 1
    //message_packed.insert<uint8_t>(0x80, 0, 8);
    
    // 44 bits = 5 bytes (40 bits) + 4 bits
   // for (int i = 0; i < 5; ++i) {
   //     message_packed.insert<uint8_t>(0xFF, i * 8, 8);
   // }
    // 补齐最后 4 位
   // message_packed.insert<uint8_t>(0xF0, 40, 4); 

    // Z=4, K=88 bits = 11 bytes
    //for (int i = 0; i < 11; ++i) {
    //    message_packed.insert<uint8_t>(0xFF, i * 8, 8);
    //}
    // 此时 message_packed 包含 88 个连续的 1

    // 示例：根据 Python 输出的 Hex 序列填入
    uint8_t random_input[] = {0xa1, 0x61, 0x53, 0x89, 0x71, 0x1a, 0xaa, 0x42, 0x47, 0x1d, 0xc}; // 共 11 个字节
    for (int i = 0; i < 11; ++i) {
        message_packed.insert<uint8_t>(random_input[i], i * 8, 8);
    }

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

    // 5. 打印校验位（仅打印前 10 行 Row 0 - Row 9）
    printf("📊 srsRAN Z=2 Pulse Test (Bit0=1):\n");
    printf("Note: Offset 20 is the start of Parity Bits\n\n");

    for (uint32_t r : {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29}) {
        // row_bits 只需要存储 Z_val (2) 个比特
        std::vector<uint8_t> row_bits(Z_val); 
        
        // 关键修正：srsRAN 输出打孔了前 2 个 Z 块。
        // 原矩阵第 22 列（第一列校验位）在输出 buffer 中的偏移是 20。
        rm_buffer.write_codeblock(row_bits, (20 + r) * Z_val);
        
        printf("Row %2d | ", r);
        
        // 只有 2 个比特，我们直接按位压入一个字节显示，并同时打印出二进制
        uint8_t packed_byte = 0;
        if (row_bits[0]) packed_byte |= 0x80; // Bit 0 对应高位
        if (row_bits[1]) packed_byte |= 0x40; // Bit 1 对应次高位
        if (row_bits[2]) packed_byte |= 0x20; // Bit 1 对应次高位
        if (row_bits[3]) packed_byte |= 0x10; // Bit 1 对应次高位
        
        // 打印十六进制和清晰的比特值
        printf("Hex: %02x | Binary: [%d, %d, %d, %d]\n", packed_byte, (int)row_bits[0], (int)row_bits[1], (int)row_bits[2], (int)row_bits[3]);
    }

    return 0;
}
