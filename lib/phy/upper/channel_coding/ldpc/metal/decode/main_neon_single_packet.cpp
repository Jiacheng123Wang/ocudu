// clang++ -O3 -std=c++17 -fobjc-arc     main_neon.cpp     LDPCDecodeMasterSequential.cpp     CollaborationDecoder.cpp     ../encode/MetalEngine.mm     -o ldpc_decoder     -lpthread -framework Foundation -framework Metal

#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include <chrono>
#include <cstring>
#include <fstream>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
// 核心头文件
#include "DecoderContext.h"        // 包含动态安全缓存定义
#include "CollaborationDecoder.h"  // 包含 update_s/p_ultimate 声明
#include "../encode/MetalEngine.h" // 包含 metal_compute_gf2 等

// 外部译码主函数声明
extern bool LDPC_Decode_Master_Sequential(
    void* metal_engine,
    DecoderContext& ctx,
    int max_iter,
    const float* in_s_llr,
    const float* in_p_llr,
    uint8_t* out_s_bits
);

// 辅助函数：使用 mmap 加载位包装矩阵
const uint32_t* mmap_matrix(const std::string& filename, size_t& out_size, bool advice_sequential) {
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0) {
        perror(("无法打开文件: " + filename).c_str());
        return nullptr;
    }

    struct stat sb;
    fstat(fd, &sb);
    out_size = sb.st_size;

    // 使用 MAP_SHARED 实现多线程共享，PROT_READ 保证只读安全
    void* addr = mmap(NULL, out_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) return nullptr;

    // 针对 CPU 扫描的 GT/FT 矩阵，通知内核开启激进预读
    if (advice_sequential) {
        madvise(addr, out_size, MADV_SEQUENTIAL);
    }

    return static_cast<const uint32_t*>(addr);
}

// --- 2. 辅助工具：信道模拟 ---
void generate_llr(const std::vector<uint8_t>& bits, std::vector<float>& llr, float snr_db) {
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float sigma = std::sqrt(1.0f / (2.0f * snr_linear));
    static std::mt19937 gen(42);
    std::normal_distribution<float> dist(0.0f, sigma);

    for (size_t i = 0; i < bits.size(); ++i) {
        float transmitted = 1.0f - 2.0f * (float)bits[i]; // BPSK
        float received = transmitted + dist(gen);
        llr[i] = 2.0f * received / (sigma * sigma);
    }
}

// --- 3. 辅助工具：计算节点度数 (Degree) ---
void calculate_all_degrees(DecoderContext& ctx) {
    // 计算 S 节点度数 (基于 G 矩阵列重)
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        uint32_t deg = 0;
        for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
            deg += __builtin_popcount(ctx.G_cols_packed[i * ctx.words_per_col_G + w]);
        }
        ctx.degree_s[i] = (deg == 0) ? 1 : deg;
    }
    // 计算 P 节点度数 (基于 F 矩阵列重)
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        uint32_t deg = 0;
        for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
            deg += __builtin_popcount(ctx.F_cols_packed[j * ctx.words_per_row_G + w]);
        }
        ctx.degree_p[j] = (deg == 0) ? 1 : deg;
    }
}

int main() {
    // --- 参数设置 ---
    const int Z = 2;
    const uint32_t n_info = 22 * Z;   // 44
    const uint32_t n_parity = 46 * Z; // 92
    const float snr_db = 1.0f;
    const int max_iter = 100;

    std::cout << "=== LDPC 协作译码全流程集成测试 (Metal + NEON) ===" << std::endl;

    // --- 1. 初始化 Metal 引擎 ---
    void* metal_engine = init_metal_engine("../encode/matrix_vector_gpu.metal");
    if (!metal_engine) return -1;

    // --- 2. 初始化 DecoderContext (一次性分配对齐内存) ---
    DecoderContext ctx;
    ctx.init(n_info, n_parity, 0.2f, 1.1f, 1.5f, 0.01f);

    // 3. 重用 mmap 矩阵加载逻辑
    size_t dummy_size;
    std::string prefix = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_";

    // G 和 F 对 GPU 友好
    ctx.G_rows_packed = mmap_matrix(prefix + "G.bin", dummy_size, false);
    ctx.F_rows_packed = mmap_matrix(prefix + "F.bin", dummy_size, false);

    // GT 和 FT (此处通常文件名为 GT/FT 或 G_cols/F_cols) 开启预读优化
    ctx.G_cols_packed = mmap_matrix(prefix + "GT.bin", dummy_size, true);
    ctx.F_cols_packed = mmap_matrix(prefix + "FT.bin", dummy_size, true);

    if (!ctx.G_rows_packed || !ctx.G_cols_packed || !ctx.F_rows_packed || !ctx.F_cols_packed) {
        std::cerr << "矩阵加载失败，请检查 bin 文件是否存在！路径前缀: " << prefix << std::endl;
        return -1;
    }
    // 注意：G_cols 和 F_cols 建议同样从预先生成的转置二进制文件中加载以保证性能
    // 此处假设您已经加载了这些数据...
    calculate_all_degrees(ctx);

    // --- 4. 数据生成与 GPU 加速编码 ---
    // A. 随机生成信息位 s 并进行位包装 (Packed)
    std::vector<uint8_t> s_bits(n_info);
    std::vector<uint32_t> s_packed(ctx.words_per_row_G, 0);
    std::mt19937 gen(time(0));
    std::uniform_int_distribution<> dist(0, 1);

    for (uint32_t i = 0; i < n_info; ++i) {
        s_bits[i] = dist(gen);
        if (s_bits[i]) s_packed[i / 32] |= (1U << (i % 32));
    }

    // B. GPU 极速编码 (p = G * s)
    std::vector<uint8_t> p_bits(n_parity, 0);
    metal_compute_gf2(metal_engine, ctx.G_rows_packed, s_packed.data(), p_bits.data(), n_parity, ctx.words_per_row_G);

    // --- 5. 信道 LLR 模拟 ---
    std::vector<float> s_llr_in(n_info);
    std::vector<float> p_llr_in(n_parity);
    generate_llr(s_bits, s_llr_in, snr_db);
    generate_llr(p_bits, p_llr_in, snr_db);

    int s_errors_initial = 0;
    for (int i = 0; i < n_info; ++i) {
        if ((s_llr_in[i] < 0) != s_bits[i]) s_errors_initial++; // 统计硬判决初始错误
    }

    int p_errors_initial = 0;
    for (int i = 0; i < n_parity; ++i) {
        if ((p_llr_in[i] < 0) != p_bits[i]) p_errors_initial++;
    }

    std::cout << "[信道状态] SNR: " << snr_db << " dB" << std::endl;
    std::cout << "[信道状态] 译码前比特错误 -> S: " << s_errors_initial << "/" << n_info 
              << ", P: " << p_errors_initial << "/" << n_parity << std::endl;


    // --- 6. 执行译码器 ---
    std::vector<uint8_t> s_decoded(n_info, 0);
    
    std::cout << "[INFO] 开始译码..." << std::endl;
    auto t1 = std::chrono::high_resolution_clock::now();

    bool converged = LDPC_Decode_Master_Sequential(
        metal_engine,
        ctx,
        max_iter,
        s_llr_in.data(),
        p_llr_in.data(),
        s_decoded.data()
    );

    auto t2 = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1).count();

    // --- 7. 性能评估 ---
    uint32_t bit_errors = 0;
    for (uint32_t i = 0; i < n_info; ++i) {
        if (s_decoded[i] != s_bits[i]) bit_errors++;
    }

    std::cout << "\n----------------------------------------" << std::endl;
    std::cout << "译码结果: " << (converged ? "成功收敛" : "达到最大迭代") << std::endl;
    std::cout << "误比特数 (BER): " << bit_errors << " / " << n_info << std::endl;
    std::cout << "处理耗时: " << duration << " us" << std::endl;
    std::cout << "----------------------------------------\n" << std::endl;

    // --- 8. 清理 ---
    // ctx 析构时会自动执行内存释放
    return 0;
}