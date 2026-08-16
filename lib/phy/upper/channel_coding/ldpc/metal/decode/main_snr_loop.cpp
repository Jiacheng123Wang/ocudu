// clang++ -O3 -std=c++17 -fobjc-arc main_snr_loop.cpp master_thread.cpp slave_thread.cpp ../encode/MetalEngine.mm -o ldpc_decoder -lpthread -framework Foundation -framework Metal


#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include <chrono>
#include <iomanip>
#include <cstring>
#include "master_thread.h"
#include "../encode/MetalEngine.h"

// 外部译码函数声明 (对应主程序逻辑)
extern bool LDPC_Decode_Master_Sequential(int Z, const float* s_llr, const float* p_llr, uint8_t* out_s, void* metal_engine);

int main() {
    // 1. 基础维度设置
    const int Z = 2;
    const int n_info = 22 * Z;   // 44
    const int n_parity = 46 * Z; // 92
    const int words_per_row_G = (n_info + 31) / 32;

    // 2. 初始化 Metal Engine
    void* metal_engine = init_metal_engine("../encode/matrix_vector_gpu.metal");
    if (!metal_engine) {
        std::cerr << "Metal Engine 初始化失败！" << std::endl;
        return -1;
    }

    // 3. 加载 G 矩阵用于编码
    std::string g_file = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_G.bin";
    FILE* fp = fopen(g_file.c_str(), "rb");
    if (!fp) {
        std::cerr << "无法读取编码矩阵: " << g_file << std::endl;
        return -1;
    }
    std::vector<uint32_t> G_matrix(n_parity * words_per_row_G);
    fread(G_matrix.data(), 4, G_matrix.size(), fp);
    fclose(fp);

    // 4. 仿真参数 (对齐 Python 脚本)
    std::vector<float> snr_axis = {0.0, 0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 4.5, 5.0, 5.5};
    const int max_packets = 200000;
    const int target_errors = 100;

    std::cout << ">>> 启动仿真 [Z=" << Z << "] 维度: " << n_info << " x " << n_parity << std::endl;

    // 5. 随机数生成器 (使用固定种子 42 以便对比)
    std::mt19937 gen(42);
    std::uniform_int_distribution<uint8_t> bit_dis(0, 1);
    std::normal_distribution<float> noise_dist(0.0f, 1.0f);

    // 6. SNR 遍历循环
    for (float snr_db : snr_axis) {
        // 每个 SNR 点重置种子，确保测试数据序列可复现
        gen.seed(42); 

        auto start_time = std::chrono::high_resolution_clock::now();
        float snr_linear = std::pow(10.0f, snr_db / 10.0f);
        float sigma = std::sqrt(1.0f / (2.0f * snr_linear));
        float sigma_sq = sigma * sigma;

        long err_count = 0;
        long pkt_count = 0;

        for (int p = 0; p < max_packets; ++p) {
            pkt_count++;

            // A. 随机产生信息位 s
            std::vector<uint8_t> s_bits(n_info);
            std::vector<uint32_t> s_packed(words_per_row_G, 0);
            for (int i = 0; i < n_info; ++i) {
                s_bits[i] = bit_dis(gen);
                if (s_bits[i]) s_packed[i / 32] |= (1U << (i % 32));
            }

            // B. GPU 编码 (p = G * s)
            std::vector<uint8_t> p_bits(n_parity, 0);
            metal_compute_gf2(metal_engine, G_matrix.data(), s_packed.data(), p_bits.data(), n_parity, words_per_row_G);

            // C. AWGN 信道与 LLR 生成
            std::vector<float> s_llr(n_info);
            std::vector<float> p_llr(n_parity);

            auto apply_awgn = [&](const std::vector<uint8_t>& bits, std::vector<float>& llrs) {
                for (size_t i = 0; i < bits.size(); ++i) {
                    float x = 1.0f - 2.0f * (float)bits[i]; // BPSK: 0->1, 1->-1
                    float received = x + noise_dist(gen) * sigma;
                    llrs[i] = 2.0f * received / sigma_sq;
                }
            };
            apply_awgn(s_bits, s_llr);
            apply_awgn(p_bits, p_llr);

            // D. 运行译码器
            std::vector<uint8_t> decoded_s(n_info, 0);
            LDPC_Decode_Master_Sequential(Z, s_llr.data(), p_llr.data(), decoded_s.data(), metal_engine);

            // E. BLER 统计 (逐比特对比)
            bool has_error = false;
            for (int i = 0; i < n_info; ++i) {
                if (decoded_s[i] != s_bits[i]) {
                    has_error = true;
                    break;
                }
            }
            if (has_error) err_count++;

            // 达到错误包目标提前跳出
            if (err_count >= target_errors && pkt_count >= 100) break;
        }

        auto end_time = std::chrono::high_resolution_clock::now();
        double duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count() / 1000.0;
        double bler = (double)err_count / pkt_count;

        // 7. 按照 Python 格式打印结果
        std::cout << "   SNR: " << std::fixed << std::setprecision(1) << std::setw(4) << snr_db 
                  << " | BLER: " << std::scientific << std::setprecision(2) << bler 
                  << " | 样本: " << std::defaultfloat << std::setw(6) << pkt_count 
                  << " | 耗时: " << std::fixed << std::setprecision(1) << duration << "s" << std::endl;

        // 如果 BLER 极低，停止仿真以节省时间
        if (bler < 5e-6 && pkt_count >= 5000) break;
    }

    // 清理资源
    deinit_metal_engine(metal_engine);
    return 0;
}