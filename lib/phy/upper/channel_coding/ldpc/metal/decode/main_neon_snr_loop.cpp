// clang++ -O3 -std=c++17 -fobjc-arc     main_neon_snr_loop.cpp     LDPCDecodeMasterSequential.cpp     CollaborationDecoder.cpp     ../encode/MetalEngine.mm     -o ldpc_decoder     -lpthread -framework Foundation -framework Metal

#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "DecoderContext.h"
#include "CollaborationDecoder.h"
#include "../encode/MetalEngine.h"

// #define IF_PRINT_DEBUG 0

// 外部函数声明
extern bool LDPC_Decode_Master_Sequential(
    void *metal_engine,
    DecoderContext &ctx,
    int max_iter,
    const float *in_s_llr,
    const float *in_p_llr,
    uint8_t *out_s_bits);

// 辅助函数：使用 mmap 加载位包装矩阵
const uint32_t *mmap_matrix(const std::string &filename, size_t &out_size, bool advice_sequential)
{
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0)
    {
        perror(("无法打开文件: " + filename).c_str());
        return nullptr;
    }

    struct stat sb;
    fstat(fd, &sb);
    out_size = sb.st_size;

    // 使用 MAP_SHARED 实现多线程共享，PROT_READ 保证只读安全
    void *addr = mmap(NULL, out_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (addr == MAP_FAILED)
        return nullptr;

    // 针对 CPU 扫描的 GT/FT 矩阵，通知内核开启激进预读
    if (advice_sequential)
    {
        madvise(addr, out_size, MADV_SEQUENTIAL);
    }

    return static_cast<const uint32_t *>(addr);
}

// --- 2. 辅助工具：信道模拟 ---
void generate_llr(const std::vector<uint8_t> &bits, std::vector<float> &llr, float snr_db)
{
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float sigma = std::sqrt(1.0f / (2.0f * snr_linear));
    static std::mt19937 gen(42);
    std::normal_distribution<float> dist(0.0f, sigma);

    for (size_t i = 0; i < bits.size(); ++i)
    {
        float transmitted = 1.0f - 2.0f * (float)bits[i]; // BPSK
        float received = transmitted + dist(gen);
        llr[i] = 2.0f * received / (sigma * sigma);
    }
}

// --- 3. 辅助工具：计算节点度数 (Degree) ---
void calculate_all_degrees(DecoderContext &ctx)
{
    // 计算 S 节点度数 (基于 G 矩阵列重)
    for (uint32_t i = 0; i < ctx.n_info; ++i)
    {
        uint32_t deg = 0;
        for (uint32_t w = 0; w < ctx.words_per_col_G; ++w)
        {
            deg += __builtin_popcount(ctx.G_cols_packed[i * ctx.words_per_col_G + w]);
        }
        ctx.degree_s[i] = (deg == 0) ? 1 : deg;
    }
    // 计算 P 节点度数 (基于 F 矩阵列重)
    for (uint32_t j = 0; j < ctx.n_parity; ++j)
    {
        uint32_t deg = 0;
        for (uint32_t w = 0; w < ctx.words_per_row_G; ++w)
        {
            deg += __builtin_popcount(ctx.F_cols_packed[j * ctx.words_per_row_G + w]);
        }
        ctx.degree_p[j] = (deg == 0) ? 1 : deg;
    }
}

int main()
{
    // 1. 基本参数设置
    const int Z = 2;
    const uint32_t n_info = 22 * Z;   // 44
    const uint32_t n_parity = 46 * Z; // 92
    const int max_iter = 20;

    // 仿真范围
    std::vector<float> snr_axis = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 50, 55, 6.0};
    const int max_packets = 200000; // 对应 Python 的最大样本量
    const int target_errors = 200;  // 错误包收集目标，达到即进入下一 SNR 点以节省时间

    std::cout << ">>> 启动仿真 [eta=0.8, Bias=1.0, Power=2.0] 维度: " << n_info << " x " << n_parity << std::endl;

    // 2. 环境初始化
    void *metal_engine = init_metal_engine("../encode/matrix_vector_gpu.metal");
    DecoderContext ctx;
    ctx.init(metal_engine, n_info, n_parity, 0.8f, 1.0f, 2.0f, 0.01f); // 包含安全分配修复

    size_t d_size;
    std::string prefix = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_";
    *(const uint32_t **)&ctx.G_rows_packed = mmap_matrix(prefix + "G.bin", d_size, false);
    *(const uint32_t **)&ctx.F_rows_packed = mmap_matrix(prefix + "F.bin", d_size, false);
    *(const uint32_t **)&ctx.G_cols_packed = mmap_matrix(prefix + "GT.bin", d_size, true);
    *(const uint32_t **)&ctx.F_cols_packed = mmap_matrix(prefix + "FT.bin", d_size, true);

    calculate_all_degrees(ctx);

    // 3. 随机发生器
    // std::mt19937 gen(time(0));
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint8_t> bit_dist(0, 1);
    std::normal_distribution<float> norm_dist(0.0f, 1.0f);

    // 4. SNR 遍历
    for (float snr_db : snr_axis)
    {
        auto start_time = std::chrono::high_resolution_clock::now();

        float snr_linear = std::pow(10.0f, snr_db / 10.0f);
        float sigma = std::sqrt(1.0f / (2.0f * snr_linear));

        long err_count = 0;
        long pkt_count = 0;

        for (int p = 0; p < max_packets; ++p)
        {
            pkt_count++;

            // 数据准备
            std::vector<uint8_t> s_true(n_info);
            std::vector<uint32_t> s_packed(ctx.words_per_row_G, 0);
            for (uint32_t i = 0; i < n_info; ++i)
            {
                s_true[i] = bit_dist(gen);
                if (s_true[i])
                    s_packed[i / 32] |= (1U << (i % 32));
            }

            // 编码
            std::vector<uint8_t> p_true(n_parity);
#ifdef IF_PRINT_DEBUG
            auto t10 = std::chrono::high_resolution_clock::now();
#endif

            metal_compute_gf2(metal_engine, KERNEL_TYPE_PREDICT_V4, ctx.G_rows_packed, s_packed.data(), p_true.data(), n_parity, ctx.words_per_row_G);
#ifdef IF_PRINT_DEBUG
            auto t20 = std::chrono::high_resolution_clock::now();
            auto metal_time = std::chrono::duration_cast<std::chrono::microseconds>(t20 - t10);
            std::cout << " ============== metal 耗时: " << std::fixed << std::setprecision(1) << metal_time.count() << "us" << std::endl;
#endif
            // for (uint32_t i = 0; i < n_info; i++)
            // {
            //     std::cout << "i = " << i << ", s_true[i] = " << (int)s_true[i] << std::endl;
            // }
            // for (uint32_t i = 0; i < n_parity; i++)
            // {
            //     std::cout << "i = " << i << ", p_true[i] = " << (int)p_true[i] << std::endl;
            // }
            // return 0;

            // 加噪生成 LLR
            std::vector<float> s_llr(n_info), p_llr(n_parity);
            auto generate_llr = [&](const std::vector<uint8_t> &bits, std::vector<float> &llrs)
            {
                for (size_t i = 0; i < bits.size(); ++i)
                {
                    float bpsk = 1.0f - 2.0f * bits[i];
                    float received = bpsk + sigma * norm_dist(gen);
                    llrs[i] = 2.0f * received / (sigma * sigma);
                }
            };
            generate_llr(s_true, s_llr);
            generate_llr(p_true, p_llr);
            for (uint32_t i = 0; i < n_info; i++)
            {
                std::cout << "i = " << i << ", s_llr[i] = " << s_llr[i] << std::endl;
            }

            for (uint32_t i = 0; i < n_parity; i++)
            {
                std::cout << "i = " << i << ", p_llr[i] = " << p_llr[i] << std::endl;
            }
            return 0;

#ifdef IF_PRINT_DEBUG
            auto t1 = std::chrono::high_resolution_clock::now();
#endif
            // 译码
            std::vector<uint8_t> s_decoded(n_info, 0);
            LDPC_Decode_Master_Sequential(metal_engine, ctx, max_iter, s_llr.data(), p_llr.data(), s_decoded.data());
#ifdef IF_PRINT_DEBUG
            auto t2 = std::chrono::high_resolution_clock::now();
            auto decoder_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
            std::cout << " ============== decoder 耗时: " << std::fixed << std::setprecision(1) << decoder_time.count() << "us" << std::endl;
#endif

            // 5. 核心逻辑修改：统一对比比特结果 (模拟 CRC check)
            bool has_error = false;
            for (uint32_t i = 0; i < n_info; ++i)
            {
                if (s_decoded[i] != s_true[i])
                {
                    has_error = true;
                    break;
                }
            }
            if (has_error)
                err_count++;

            // 提前终止该 SNR 点：收集够错误包且样本过百
            if (err_count >= target_errors && pkt_count >= 100)
                break;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;
        double bler = (double)err_count / pkt_count;

        // 格式化打印 (匹配 Python 输出)
        std::cout << "   SNR: " << std::fixed << std::setprecision(1) << std::setw(4) << snr_db
                  << " | BLER: " << std::scientific << std::setprecision(2) << bler
                  << " | 样本: " << std::defaultfloat << std::setw(6) << pkt_count
                  << " | 耗时: " << std::fixed << std::setprecision(1) << duration << "s" << std::endl;

        // 如果误码率已经降到极低，停止仿真
        if (bler < 5e-6 && pkt_count >= 10000)
            break;
    }

    return 0;
}