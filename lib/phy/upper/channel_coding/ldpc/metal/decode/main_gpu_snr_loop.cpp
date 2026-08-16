// # 1. 编译源代码为中间表示 (.air)
// xcrun -sdk macosx metal -c ldpc_gpu_decoder.metal -o ldpc_gpu_decoder.air
// # 2. 将中间表示转换为库文件 (.metallib)
// xcrun -sdk macosx metallib ldpc_gpu_decoder.air -o ldpc_decoder.metallib

// 编译命令 (Apple Silicon):
// 请注意，现在需要同时编译旧版的 MetalEngine.mm 和 新版的 ldpc_gpu_decoder.mm
// clang++ -O3 -std=c++17 -fobjc-arc main_gpu_snr_loop.cpp ldpc_gpu_decoder.mm ../encode/MetalEngine.mm -o ldpc_sim -lpthread -framework Foundation -framework Metal

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

// 1. 引入已验证的 Tx 编码引擎
#include "../encode/MetalEngine.h"
// 2. 引入全 Kernel Rx 译码引擎
#include "DecoderContextGPU.h"

#define IF_PRINT_DEBUG 0

#ifdef __arm64__
typedef __fp16 float16;
#else
typedef uint16_t float16;
#endif

// 辅助工具：使用 mmap 加载位包装矩阵
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

    // 严格遵循原版的 MAP_SHARED 与 PROT_READ
    void *mapped = mmap(NULL, out_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);

    if (mapped == MAP_FAILED)
        return nullptr;
    if (advice_sequential)
        madvise(mapped, out_size, MADV_SEQUENTIAL);

    return static_cast<const uint32_t *>(mapped);
}

void munmap_matrix(const uint32_t *mapped, size_t size)
{
    if (mapped)
        munmap(const_cast<uint32_t *>(mapped), size);
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

int main()
{
    // 1. 系统参数配置 (严格匹配方正程序)
    const int Z = 384;
    const uint32_t n_info = 22 * Z;               // 44
    const uint32_t n_parity = 46 * Z;             // 92
    const uint32_t N_logical = n_info + n_parity; // 136
    const uint32_t M_logical = n_parity;          // 92

    const float alpha = 1.2f;
    const int max_iter = 20;
    const double code_rate = (double)n_info / N_logical;

    // 仿真测试范围
    std::vector<double> snr_axis = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5, 6.0};
    const int target_errors = 100;
    const int max_packets = 200000;

    // --- 2. 初始化发送端 (Tx) 编码引擎 ---
    void *metal_engine_tx = init_metal_engine("../encode/matrix_vector_gpu.metal");
    if (!metal_engine_tx)
    {
        std::cerr << "Tx 编码引擎初始化失败。" << std::endl;
        return -1;
    }

    // 加载编码矩阵 (G 矩阵)
    size_t g_size = 0;
    std::string prefix_base = "../g_f_matrix/G_matrix_Z" + std::to_string(Z);
    const uint32_t *matrix_g_packed = mmap_matrix(prefix_base + ".bin", g_size, false);
    if (!matrix_g_packed)
    {
        std::cerr << "编码矩阵 G.bin 加载失败。" << std::endl;
        return -1;
    }
    // G 矩阵的列向打包长度
    uint32_t words_per_row_G = (n_info + 31) / 32;

    // --- 3. 初始化接收端 (Rx) 全 Kernel 译码引擎 ---
    DecoderContext ctx;
    if (!ctx.init("ldpc_decoder.metallib", N_logical, M_logical, alpha))
    {
        std::cerr << "Rx 译码器初始化失败。" << std::endl;
        return -1;
    }

    std::string h_path = "../g_f_matrix/H_matrix_Z" + std::to_string(Z) + ".bin";
    if (!ctx.load_files_manually(h_path))
    {
        std::cerr << "H 矩阵加载失败: " << h_path << std::endl;
        return -1;
    }

    std::cout << ">>> GPU 混合仿真 (Tx: Predict_V4, Rx: Full-Kernel) [Rate=" << code_rate << "] 维度: " << n_info << " x " << n_parity << std::endl;
    /*
        // ================== [新增：内存探针] 打印 H 矩阵与度数 ==================
        uint32_t *h_ptr = ctx.h_matrix_buf->get();
        uint32_t *cn_ptr = ctx.vn_total_cn_buf->get();

        std::cout << "\n=========== [内存探针] H 矩阵与列重 ===========" << std::endl;

        // 1. 打印前 10 个变量节点 (VN) 的连接度数
        std::cout << "--- 前 10 个 VN 的列重 (TotalCN) ---" << std::endl;
        for (int i = 0; i < 10; ++i)
        {
            std::cout << "VN " << std::setw(2) << i << " 列重 = " << cn_ptr[i] << std::endl;
        }

        // 2. 打印 H 矩阵的前 5 行 (十六进制)，验证是否为空
        // ctx.n_h_chunks 是每行的 uint32_t 数量 (维度 92 下应该是 3)
        std::cout << "\n--- H 矩阵前 5 行 (位包装 Hex) ---" << std::endl;
        for (uint32_t r = 0; r < 5; ++r)
        {
            std::cout << "Row " << std::setw(2) << r << ": ";
            for (uint32_t c = 0; c < ctx.n_h_chunks; ++c)
            {
                std::cout << "0x" << std::hex << std::setw(8) << std::setfill('0')
                          << h_ptr[r * ctx.n_h_chunks + c] << "  ";
            }
            std::cout << std::dec << std::endl; // 恢复十进制打印
        }
        std::cout << "===============================================\n"
                  << std::endl;
        // =========================================================================
    */
    // 随机数生成器 (采用种子 42 匹配 Python 基准测试)
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint8_t> bit_dist(0, 1);
    std::normal_distribution<double> norm_dist(0.0, 1.0);

    std::vector<uint8_t> s_decoded(n_info, 0);

    // 4. SNR 循环遍历
    for (double snr_db : snr_axis)
    {
        double snr_linear = std::pow(10.0, snr_db / 10.0);
        double sigma = std::sqrt(1.0 / (2.0 * snr_linear));

        long err_count = 0;
        long pkt_count = 0;

        auto start_time = std::chrono::high_resolution_clock::now();

        for (pkt_count = 0; pkt_count < max_packets; ++pkt_count)
        {
            // -- a. 生成随机信息比特并打包 --
            std::vector<uint8_t> s_true(n_info);
            std::vector<uint32_t> s_packed(words_per_row_G, 0);
            for (uint32_t i = 0; i < n_info; ++i)
            {
                s_true[i] = bit_dist(gen);
                if (s_true[i])
                {
                    s_packed[i / 32] |= (1U << (i % 32));
                }
            }

            // -- b. 严格调用已验证的 Metal 算子进行编码 --
            std::vector<uint8_t> p_true(n_parity, 0);
            metal_compute_gf2(metal_engine_tx, KERNEL_TYPE_PREDICT_V4, matrix_g_packed, s_packed.data(), p_true.data(), n_parity, words_per_row_G);

            // for (uint32_t i = 0; i < n_info; i++)
            // {
            //     std::cout << "i = " << i << ", s_true[i] = " << (int)s_true[i] << std::endl;
            // }
            // for (uint32_t i = 0; i < n_parity; i++)
            // {
            //     std::cout << "i = " << i << ", p_true[i] = " << (int)p_true[i] << std::endl;
            // }

            // return 0;
            /*
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
            */
            // for (uint32_t i = 0; i < n_info; i++)
            // {
            //     std::cout << "i = " << i << ", s_llr[i] = " << s_llr[i] << std::endl;
            // }

            // for (uint32_t i = 0; i < n_parity; i++)
            // {
            //     std::cout << "i = " << i << ", p_llr[i] = " << p_llr[i] << std::endl;
            // }
            // return 0;

            // // -- c. 加噪并生成 LLR (严格复用原始公式 1.0f - 2.0f*x) --
            float16 *llr_gpu_ptr = ctx.sp_llr_buf->get();
            double scale = 2.0 / (sigma * sigma);

            auto inject_noise_and_store = [&](const std::vector<uint8_t> &bits, uint32_t offset)
            {
                for (size_t i = 0; i < bits.size(); ++i)
                {
                    double bpsk = 1.0 - 2.0 * (double)bits[i]; // BPSK: 0 -> +1.0, 1 -> -1.0
                    double received = bpsk + sigma * norm_dist(gen);
                    float llr_val = (float)(2.0 * received / (sigma * sigma));
                    llr_gpu_ptr[offset + i] = static_cast<float16>(llr_val);
                }
            };

            // 将 s_true 和 p_true 映射为 LLR 填入 GPU 缓冲区
            inject_noise_and_store(s_true, 0);
            inject_noise_and_store(p_true, n_info);

            // for (uint32_t i = 0; i < n_info; ++i)
            // {
            //     llr_gpu_ptr[i] = s_llr[i];
            // }
            // for (uint32_t i = 0; i < n_parity; ++i)
            // {
            //     llr_gpu_ptr[i + n_info] = p_llr[i];
            // }

            // 【非常重要】清空补齐位，防止脏数据干扰 Syndrome 计算
            for (uint32_t i = N_logical; i < ctx.n_h_chunks * 32; ++i)
            {
                llr_gpu_ptr[i] = static_cast<float16>(0.0f);
            }

#ifdef IF_PRINT_DEBUG
            auto t1 = std::chrono::high_resolution_clock::now();
#endif
            // -- d. 核心译码 (GPU 全 Kernel 模式) --
            int actual_iters = ctx.decode(llr_gpu_ptr, s_decoded.data(), max_iter);
#ifdef IF_PRINT_DEBUG
            auto t2 = std::chrono::high_resolution_clock::now();
            auto decoder_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
            std::cout << " ============== decoder 耗时: " << std::fixed << std::setprecision(1) << decoder_time.count() << "us" << std::endl;
            // if (decoder_result)
            //     std::cout << " ============== decoder seuccsefully =================== " << std::endl;
            // else
            //     std::cout << " ============== decoder failed =================== " << std::endl;
#endif
            /*
                        // ================== [新插入：调试对齐代码] ==================
                        std::ifstream debug_file("debug_llr_in.txt");
                        if (debug_file.is_open())
                        {
                            float val;
                            uint32_t idx = 0;
                            while (debug_file >> val && idx < N_logical)
                            {
                                llr_gpu_ptr[idx++] = (float16)val;
                            }
                            debug_file.close();
                            if (pkt_count == 0)
                                std::cout << "[DEBUG] 已从文件加载 Python LLR 数据" << std::endl;
                        }
                        else
                        {
                            std::cerr << "Fatal: 找不到 debug_llr_in.txt" << std::endl;
                            return -1;
                        }
                        // =========================================================
                        // for (uint32_t i = 0; i < N_logical; ++i)
                        // {
                        //     std::cout << "i = " << i << ", 输入 LLR = " << (float)llr_gpu_ptr[i] << std::endl;
                        // }

                        int actual_iters = ctx.decode(llr_gpu_ptr, s_decoded.data(), 2);

            #ifdef IF_PRINT_DEBUG
                        // 4. 通过成员函数获取指针
                        VNStats *debug_ptr = ctx.engine.get_debug_ptr();

                        if (debug_ptr)
                        {
                            for (int i = 0; i < 136; ++i)
                            {
                                if (debug_ptr[i].SuspectCnt != 0)
                                {
                                    std::cout << "VN " << i << ": LLR=" << debug_ptr[i].current_llr
                                              << ", Delta=" << debug_ptr[i].last_delta
                                              << ", SuspectCnt=" << debug_ptr[i].SuspectCnt
                                              << ", EvidSum=" << debug_ptr[i].EvidSum
                                              << ", ErrEqCnt=" << debug_ptr[i].ErrEqCnt
                                              << ", TotalCN=" << debug_ptr[i].VNTotalCN
                                              << std::endl;
                                }
                            }
                        }

                        // return 0; // 先只跑一个包，验证数据对齐和更新逻辑
            #endif
                        // ================== [新插入：打印更新后的 LLR] ==================
                        if (pkt_count == 0)
                        { // 只看第一个包
                            float16 *gpu_updated_llr = ctx.sp_llr_buf->get();
                            std::cout << "--- 1次迭代后 C++ 端 LLR (前10位) ---" << std::endl;
                            for (int i = 0; i < 136; ++i)
                            {
                                std::cout << " i = " << i << ":  " << (float)gpu_updated_llr[i] << " " << std::endl;
                            }
                            std::cout << "\n-----------------1------------------" << std::endl;
                            for (int i = 0; i < 136; ++i)
                            {
                                std::cout << " i = " << i << ":  " << ((float)gpu_updated_llr[i] < 0) << " " << std::endl;
                            }
                            std::cout << "\n-----------------2------------------" << std::endl;

                            // // 打印前几个校验方程的状态 (从 h_pred_buf 读取)
                            // uint32_t* pred_ptr = ctx.h_pred_buf->get();
                            // std::cout << "初始校验子 (Syndrome) 状态 (Hex): 0x"
                            //             << std::hex << pred_ptr[0] << std::dec << std::endl;
                        }
                        // =========================================================
                        return 0; // 先只跑一个包，验证数据对齐和更新逻辑
            */
            // -- e. 错误统计 --
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

            if (err_count >= target_errors && pkt_count >= 100)
                break;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;
        double bler = (double)err_count / pkt_count;

        std::cout << "   SNR: " << std::fixed << std::setprecision(1) << std::setw(4) << snr_db
                  << " | BLER: " << std::scientific << std::setprecision(2) << bler
                  << " | Errs: " << std::setw(3) << err_count
                  << " / " << std::setw(6) << pkt_count
                  << " | Time: " << std::fixed << std::setprecision(2) << duration << "s"
                  << std::endl;

        if (bler < 5e-6 && pkt_count >= 10000)
            break;
    }

    // 清理资源
    munmap_matrix(matrix_g_packed, g_size);
    ctx.release();
    // 假设你有 deinit_metal_engine 函数，需在此调用清理 tx 引擎

    std::cout << "仿真结束。" << std::endl;
    return 0;
}
