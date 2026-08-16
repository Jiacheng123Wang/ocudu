// clang++ -O3 -std=c++17 -fobjc-arc main_single_packet.cpp master_thread.cpp slave_thread.cpp ../encode/MetalEngine.mm -o ldpc_decoder -lpthread -framework Foundation -framework Metal

#include <iostream>
#include <vector>
#include <random>
#include <cmath>
#include "master_thread.h"
#include "../encode/MetalEngine.h"

int main() {
    int Z = 2;
    int n_info = 22 * Z;   // 44
    int n_parity = 46 * Z; // 92
    int words_per_row_G = (n_info + 31) / 32;

    std::cout << "=== 协作译码器端到端测试 (Z=" << Z << ") ===" << std::endl;

    // 1. 初始化 Metal Engine
    void* metal_engine = init_metal_engine("../encode/matrix_vector_gpu.metal");
    if (!metal_engine) {
        std::cerr << "Metal Engine 初始化失败！" << std::endl;
        return -1;
    }

    // 2. 加载 G 矩阵用于编码
    std::string g_file = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_G.bin";
    FILE* fp = fopen(g_file.c_str(), "rb");
    if (!fp) {
        std::cerr << "无法读取编码矩阵: " << g_file << std::endl;
        return -1;
    }
    std::vector<uint32_t> G_matrix(n_parity * words_per_row_G);
    fread(G_matrix.data(), 4, G_matrix.size(), fp);
    fclose(fp);

    // 3. 产生随机信源 s
    //std::mt19937 gen(1337); // 固定种子复现
    std::random_device rd; 
    std::mt19937 gen(rd());

    std::uniform_int_distribution<> dis(0, 1);
    
    std::vector<uint8_t> s_bits(n_info);
    std::vector<uint32_t> s_packed(words_per_row_G, 0);
    
    for (int i = 0; i < n_info; ++i) {
        s_bits[i] = dis(gen);
        if (s_bits[i]) s_packed[i / 32] |= (1U << (i % 32));
    }

    // 4. GPU 极速编码 (p = G * s)
    std::vector<uint8_t> p_bits(n_parity, 0);
    metal_compute_gf2(metal_engine, G_matrix.data(), s_packed.data(), p_bits.data(), n_parity, words_per_row_G);

    // 5. BPSK 调制与 AWGN 信道
    float snr_db = 1.0f; // 可以调节 SNR 观察译码器性能
    float snr_linear = std::pow(10.0f, snr_db / 10.0f);
    float sigma = std::sqrt(1.0f / (2.0f * snr_linear));
    std::normal_distribution<float> noise(0.0f, sigma);

    std::vector<float> s_llr(n_info);
    std::vector<float> p_llr(n_parity);

    int s_errors_initial = 0;
    for (int i = 0; i < n_info; ++i) {
        float x = 1.0f - 2.0f * s_bits[i]; // BPSK: 0->1, 1->-1
        float y = x + noise(gen);
        s_llr[i] = 2.0f * y / (sigma * sigma);
        if ((s_llr[i] < 0) != s_bits[i]) s_errors_initial++; // 统计硬判决初始错误
    }

    int p_errors_initial = 0;
    for (int i = 0; i < n_parity; ++i) {
        float x = 1.0f - 2.0f * p_bits[i];
        float y = x + noise(gen);
        //float y = x;
        p_llr[i] = 2.0f * y / (sigma * sigma);
        if ((p_llr[i] < 0) != p_bits[i]) p_errors_initial++;
    }

    std::cout << "[信道状态] SNR: " << snr_db << " dB" << std::endl;
    std::cout << "[信道状态] 译码前比特错误 -> S: " << s_errors_initial << "/" << n_info 
              << ", P: " << p_errors_initial << "/" << n_parity << std::endl;

    // 6. 运行译码器
    std::vector<uint8_t> decoded_s(n_info, 0);
    auto start_time = std::chrono::high_resolution_clock::now();
    
    // 最大等待时间设为 100ms
    //bool converged = LDPC_Decode_Master(Z, s_llr.data(), p_llr.data(), 100000, decoded_s.data(), metal_engine);
    bool converged = LDPC_Decode_Master_Sequential(Z, s_llr.data(), p_llr.data(), decoded_s.data(), metal_engine);

    auto end_time = std::chrono::high_resolution_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

    // 7. 评估性能
    int final_errors = 0;
    for (int i = 0; i < n_info; ++i) {
        if (decoded_s[i] != s_bits[i]) final_errors++;
        //std::cout << "i= " << i << ", s_bits: " << (int)s_bits[i] << ", decoded_s: " << (int)decoded_s[i] << std::endl;
    }

    std::cout << "\n=== 译码结果 ===" << std::endl;
    if (converged) {
        std::cout << "状态: 成功收敛！" << std::endl;
    } else {
        std::cout << "状态: 未收敛（超时或陷入死循环）。" << std::endl;
    }
    std::cout << "耗时: " << duration.count() / 1000.0 << " ms" << std::endl;
    std::cout << "最终信息位错误 (BER): " << final_errors << " / " << n_info << std::endl;

    // 清理资源
    deinit_metal_engine(metal_engine);
    return 0;
}