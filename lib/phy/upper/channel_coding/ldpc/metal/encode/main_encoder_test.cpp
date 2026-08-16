// clang++ -O3 -std=c++17 -fobjc-arc \
    main_encoder_test.cpp \
    MetalEngine.mm \
    -o ldpc_encoder_test \
    -framework Foundation \
    -framework Metal


#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/mman.h>
#include <unistd.h>
#include "MetalEngine.h"

// --- 辅助工具：位反转 (与 srsRAN/main_v4.swift 对齐) ---
uint8_t reverseBitsInByte(uint8_t n) {
    n = (n & 0xF0) >> 4 | (n & 0x0F) << 4;
    n = (n & 0xCC) >> 2 | (n & 0x33) << 2;
    n = (n & 0xAA) >> 1 | (n & 0x55) << 1;
    return n;
}

// --- 辅助工具：解析十六进制文本文件 ---
std::vector<uint8_t> parseHexInput(const std::string& path) {
    std::vector<uint8_t> bytes;
    std::ifstream file(path);
    if (!file.is_open()) return bytes;

    std::string word;
    while (file >> word) {
        // 处理逗号、0x 前缀等
        size_t pos = word.find("0x");
        if (pos != std::string::npos) word = word.substr(pos + 2);
        if (word.back() == ',') word.pop_back();
        
        if (!word.empty()) {
            uint8_t val = static_cast<uint8_t>(std::stoul(word, nullptr, 16));
            bytes.push_back(val);
        }
    }
    return bytes;
}

// --- 辅助工具：对齐内存分配 (用于 UMA 零拷贝) ---
void* allocate_aligned(size_t size) {
    void* ptr = nullptr;
    size_t pageSize = getpagesize();
    size_t alignedSize = (size + pageSize - 1) & ~(pageSize - 1);
    posix_memalign(&ptr, pageSize, alignedSize);
    if (ptr) std::memset(ptr, 0, alignedSize);
    return ptr;
}

int main() {
    // 1. 设置参数 (Z=256)
    const int z = 256;
    const int numRows = 46;                 
    const int numBits = numRows * z;         
    const uint32_t numChunks = (22 * z + 31) / 32; // Z=256 时为 176

    std::cout << ">>> 编码器 C++ 验证版启动 (Z=" << z << ")" << std::endl;

    // 2. 加载 G 矩阵文件
    std::string gFile = "../g_f_matrix/BG1_LSindex0_Z256_G.bin";
    std::ifstream gIfs(gFile, std::ios::binary | std::ios::ate);
    if (!gIfs.is_open()) {
        std::cerr << "无法打开 G 矩阵文件" << std::endl;
        return -1;
    }
    std::streamsize gSize = gIfs.tellg();
    gIfs.seekg(0, std::ios::beg);
    
    uint32_t* g_matrix = (uint32_t*)allocate_aligned(gSize);
    gIfs.read((char*)g_matrix, gSize);
    gIfs.close();

    // 3. 处理输入数据 (位反转)
    std::vector<uint8_t> inputRaw = parseHexInput("input_z256.txt");
    uint32_t* bit_in = (uint32_t*)allocate_aligned(numChunks * 4);
    uint8_t* bit_in_bytes = reinterpret_cast<uint8_t*>(bit_in);
    
    for (size_t i = 0; i < inputRaw.size(); ++i) {
        bit_in_bytes[i] = reverseBitsInByte(inputRaw[i]);
    }

    // 4. 准备输出 Buffer (每个比特 1 字节)
    uint8_t* bit_out = (uint8_t*)allocate_aligned(numBits);

    // 5. 初始化 Metal 引擎并计算
    void* engine = init_metal_engine("matrix_vector_gpu.metal");
    if (!engine) {
        std::cerr << "Metal 引擎初始化失败" << std::endl;
        return -1;
    }

    std::cout << ">>> GPU 正在编码..." << std::endl;
    // 调用通用 GF2 矩阵乘法接口
    metal_compute_gf2(engine, g_matrix, bit_in, bit_out, numBits, numChunks);
    std::cout << ">>> 编码完成。" << std::endl;

    // 6. 格式化输出验证 (前 46 行，每行前 10 字节)
    std::cout << "\n--- 编码输出结果 (验证前 10 字节/行) ---" << std::endl;
    for (int r = 0; r < numRows; ++r) {
        int rowStartBit = r * z;
        std::cout << "Row " << std::setw(2) << r << " | Hex: ";
        for (int b = 0; b < 10; ++b) {
            uint8_t byteVal = 0;
            for (int i = 0; i < 8; ++i) {
                if (bit_out[rowStartBit + b * 8 + i] == 1) {
                    byteVal |= (1 << (7 - i));
                }
            }
            std::cout << "0x" << std::hex << std::setw(2) << std::setfill('0') << (int)byteVal << " ";
        }
        std::cout << std::dec << std::endl;
    }

    // 7. 清理
    deinit_metal_engine(engine);
    free(g_matrix);
    free(bit_in);
    free(bit_out);

    return 0;
}