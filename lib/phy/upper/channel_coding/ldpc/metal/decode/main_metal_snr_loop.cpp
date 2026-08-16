// clang++ -O3 -std=c++17 -fobjc-arc     main_metal_snr_loop.cpp     LDPCDecodeMetal.cpp     MetalLLSUpdater.mm     ../encode/MetalEngine.mm     -o ldpc_metal_decoder     -framework Metal -framework Foundation

#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include <iomanip>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>

#include "DecoderContextMetal.h"
#include "MetalLLSUpdater.h"
#include "../encode/MetalEngine.h"

// #define IF_PRINT_DEBUG 0

extern bool LDPCDecodeMetal(DecoderContext &ctx, int max_iter,
                            uint32_t n_info, const float16 *in_sp_llr, uint8_t *out_s_bits);

const uint32_t *mmap_matrix(const std::string &filename, size_t &out_size)
{
    int fd = open(filename.c_str(), O_RDONLY);
    if (fd < 0)
        return nullptr;
    struct stat sb;
    fstat(fd, &sb);
    out_size = sb.st_size;
    void *addr = mmap(NULL, out_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return (addr == MAP_FAILED) ? nullptr : (const uint32_t *)addr;
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

int main(int argc, char *argv[])
{
    const uint32_t Z = 2;
    const uint32_t n_info = 22 * Z;
    const uint32_t n_parity = 46 * Z;
    const uint32_t n_total = n_info + n_parity;
    const float alpha = 0.45f;

    void *lls_engine = init_llr_updater_engine("./cn_scan_vn_lls_update.metal");
    if (!lls_engine)
    {
        std::cerr << "Failed to init LLS engine!" << std::endl;
        return -1;
    }
    void *gf2_engine = init_metal_engine("../encode/matrix_vector_gpu.metal");
    if (!gf2_engine)
    {
        std::cerr << "Failed to init GF2 engine!" << std::endl;
        return -1;
    }

    DecoderContext ctx;
    ctx.init(lls_engine, gf2_engine, n_total, n_parity, alpha);

    std::string h_file = "../g_f_matrix/H_matrix_Z" + std::to_string(Z) + ".bin";
    size_t h_size = 0;
    const uint32_t *h_raw = mmap_matrix(h_file, h_size);
    if (h_raw)
    {
        std::memcpy(ctx.h_matrix_buf->get(), h_raw, h_size);
        engine_prepare_internal_buffers(lls_engine, ctx.h_matrix_buf->get(), ctx.M_aligned, ctx.n_h_chunks);
        munmap((void *)h_raw, h_size);
    }
    else
    {
        std::cerr << "Cannot open H_matrix file." << std::endl;
        return -1;
    }

    // std::default_random_engine gen(42);
    std::mt19937 gen(time(0));

    // 原始发送信息比特（全零码字假设）
    std::vector<uint8_t> s_true(n_info, 0);

    for (float snr = 0.0f; snr <= 4.5f; snr += 0.5f)
    {
        float sigma = std::sqrt(1.0f / (2.0f * std::pow(10.0f, snr / 10.0f)));
        int errs = 0, pkts = 0;

        while (pkts < 10000 && errs < 100)
        {
            std::vector<float16> sp_llr_merged(n_total);
            std::vector<uint8_t> s_res(n_info);
            std::normal_distribution<float> dist(0.0, sigma);

            float snr_factor = 2.0f / (sigma * sigma);
            for (uint32_t i = 0; i < n_total; ++i)
            {
                // BPSK 调制假设：0 映射为 +1.0
                float raw_llr = snr_factor * (1.0f + dist(gen));
                sp_llr_merged[i] = (float16)raw_llr;
            }

#ifdef IF_PRINT_DEBUG
            auto t1 = std::chrono::high_resolution_clock::now();
#endif
            // 执行译码，无论是否提前收敛，都进行后续的比特比对
            bool decoder_result = LDPCDecodeMetal(ctx, 20, n_info, sp_llr_merged.data(), s_res.data());
#ifdef IF_PRINT_DEBUG
            auto t2 = std::chrono::high_resolution_clock::now();
            auto decoder_time = std::chrono::duration_cast<std::chrono::microseconds>(t2 - t1);
            std::cout << " ============== decoder 耗时: " << std::fixed << std::setprecision(1) << decoder_time.count() << "us" << std::endl;
            if (decoder_result)
                std::cout << " ============== decoder seuccsefully =================== " << std::endl;
            else
                std::cout << " ============== decoder failed =================== " << std::endl;
#endif
            // return 0;
            /*
                        // ================== [新插入：调试对齐代码] ==================
                        std::ifstream debug_file("debug_llr_in.txt");
                        if (debug_file.is_open()) {
                            float val;
                            uint32_t idx = 0;
                            while (debug_file >> val && idx < n_total) {
                                sp_llr_merged[idx++] = (float16)val;
                            }
                            debug_file.close();
                            if (pkts == 0) std::cout << "[DEBUG] 已从文件加载 Python LLR 数据" << std::endl;
                        } else {
                            std::cerr << "Fatal: 找不到 debug_llr_in.txt" << std::endl;
                            return -1;
                        }
                        // =========================================================

                        // 执行译码：将迭代次数固定为 1
                        LDPCDecodeMetal(ctx, 1, n_info, sp_llr_merged.data(), s_res.data());

                        // ================== [新插入：打印更新后的 LLR] ==================
                        if (pkts == 0) { // 只看第一个包
                            float16* gpu_updated_llr = ctx.sp_llr_buf->get();
                            std::cout << "--- 1次迭代后 C++ 端 LLR (前10位) ---" << std::endl;
                            for (int i = 0; i < 136; ++i) {
                                std::cout << " i = " << i << ":  " << (float)gpu_updated_llr[i] << " " << std::endl;
                            }
                            std::cout << "\n-----------------1------------------" << std::endl;
                           for (int i = 0; i < 136; ++i) {
                                std::cout << " i = " << i << ":  "  << ((float)gpu_updated_llr[i] < 0) << " "  << std::endl;
                            }
                            std::cout << "\n-----------------2------------------" << std::endl;

                            // 打印前几个校验方程的状态 (从 h_pred_buf 读取)
                            uint32_t* pred_ptr = ctx.h_pred_buf->get();
                            std::cout << "初始校验子 (Syndrome) 状态 (Hex): 0x"
                                      << std::hex << pred_ptr[0] << std::dec << std::endl;
                        }
                        // =========================================================
                        return 0; // 先只跑一个包，验证数据对齐和更新逻辑
            */
            // 严格比对译码输出与原始信息比特
            bool has_error = false;
            for (uint32_t i = 0; i < n_info; ++i)
            {
                if (s_res[i] != s_true[i])
                {
                    has_error = true;
                    break;
                }
            }

            if (has_error)
            {
                errs++;
            }

            pkts++;
        }
        std::cout << "SNR: " << std::fixed << std::setprecision(5) << snr
                  << " | BLER: " << (double)errs / pkts << std::endl;
    }

    ctx.release();
    deinit_llr_updater_engine(lls_engine);
    deinit_metal_engine(gf2_engine);
    return 0;
}
