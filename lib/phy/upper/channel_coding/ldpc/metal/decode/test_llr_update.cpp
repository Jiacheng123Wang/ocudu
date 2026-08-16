//  clang++ -O3 -std=c++17 -fobjc-arc     -framework Metal -framework Foundation -framework DeviceCheck     test_llr_update.cpp MetalEngineUpdateLLS.mm     -o test_llr_update

#include <iostream>
#include <vector>
#include <fstream>
#include <cmath>
#include <iomanip>
#include <stdlib.h> // 必须包含，用于 posix_memalign
#include "MetalEngineUpdateLLS.h"

// 使用 __fp16 匹配 Metal 的 half
// typedef __fp16 float16;

// --- 辅助工具：分配页面对齐的内存 ---
void* malloc_aligned(size_t size) {
    void* ptr = nullptr;
    // 向上取整到 4096 的倍数
    size_t aligned_size = (size + 4095) & ~4095;
    if (posix_memalign(&ptr, 4096, aligned_size) != 0) {
        std::cout << "\nmalloc_aligned failed..." << std::endl;
        return nullptr;
    }
    memset(ptr, 0, aligned_size);
    return ptr;
}

// 辅助函数：加载二进制矩阵到对齐内存
uint32_t* load_bin_matrix_aligned(const char* path, size_t size_bytes) {
    uint32_t* buffer = (uint32_t*)malloc_aligned(size_bytes);
    std::ifstream file(path, std::ios::binary);
    if (!file || !buffer) {
        std::cerr << "错误：无法加载文件或分配内存 " << path << std::endl;
        exit(-1);
    }
    file.read((char*)buffer, size_bytes);
    return buffer;
}

int main() {
    // --- 1. 参数初始化 (Z=2) ---
    const uint32_t Z = 256;
    const uint32_t M = 46 * Z; // 92
    const uint32_t N = 68 * Z; // 136

    LLRUpdateParams params;
    params.n_h_chunks = (N + 31) / 32;       // 5
    params.n_h_full_loops = params.n_h_chunks / 32;
    params.n_h_remainder = params.n_h_chunks % 32;
    params.n_ht_chunks = (M + 31) / 32;      // 3
    params.n_ht_full_loops = params.n_ht_chunks / 32;
    params.n_ht_remainder = params.n_ht_chunks % 32;
    params.eta = 0.5f;
    params.power = 0.8f;
    params.num_v_nodes = N; // 变量节点数 (如 136)
    params.num_h_nodes = M; // 校验方程数 (如 92)
    // --- 2. 为所有变量分配页面对齐的内存 ---
    
    // 辅助宏：对齐长度到 4096 字节
    auto align_page = [](size_t size) { return (size + 4095) & ~4095; };

    // --- 重新计算所有对齐长度 ---
    size_t llr_len = align_page(N * sizeof(float16));
    size_t h_len = align_page(M * params.n_h_chunks * sizeof(uint32_t));
    size_t ht_len = align_page(N * params.n_ht_chunks * sizeof(uint32_t));
    size_t h_pred_len = align_page(params.n_ht_chunks * sizeof(uint32_t));
    size_t deg_len = align_page(N * sizeof(uint16_t));

    // 使用 malloc_aligned 分配确保首地址对齐
    uint32_t* h_matrix = load_bin_matrix_aligned("../g_f_matrix/H_matrix_Z256.bin", h_len);
    uint32_t* ht_matrix = load_bin_matrix_aligned("../g_f_matrix/HT_matrix_Z256.bin", ht_len);

    // LLR 数组 (这是最需要零拷贝的，因为要写回)
    float16* llr_array = (float16*)malloc_aligned(llr_len);
    for(int i=0; i<N; ++i) llr_array[i] = 2.0f; 

    // H_pred 和 v_degrees
    uint32_t* h_pred = (uint32_t*)malloc_aligned(h_pred_len);
    for(int i = 0; i < params.n_ht_chunks; ++i) h_pred[i] = 0xFFFFFFFF;
    
    uint16_t* v_degrees = (uint16_t*)malloc_aligned(deg_len);
    
    // 计算度数 (直接在对齐内存上操作)
    uint32_t max_deg = 0;
    for (uint32_t i = 0; i < N; ++i) {
        uint32_t count = 0;
        for (uint32_t j = 0; j < params.n_ht_chunks; ++j) {
            count += __builtin_popcount(ht_matrix[i * params.n_ht_chunks + j]);
        }
        v_degrees[i] = (uint16_t)count;
        if (count > max_deg) max_deg = count;
    }

    // --- 3. 构造测试用例 ---
    h_pred[0] |= (1U << 0);  // 假设第 0 个方程报错
    llr_array[5] = 0.5f;     // 设置嫌疑比特

    // --- 4. 运行 Metal 引擎 ---
    void* engine = init_llr_update_engine("llr_update_vnode_centric.metal");
    
    std::cout << "执行零拷贝 Metal 更新 (已确认页面对齐)..." << std::endl;
    
    metal_update_llr(engine, 
                     (float16 *)llr_array, 
                     h_matrix, 
                     ht_matrix, 
                     h_pred, 
                     v_degrees, 
                     N, 
                     max_deg, 
                     &params);

    metal_update_llr(engine, 
                     (float16 *)llr_array, 
                     h_matrix, 
                     ht_matrix, 
                     h_pred, 
                     v_degrees, 
                     N, 
                     max_deg, 
                     &params);
    metal_update_llr(engine, 
                     (float16 *)llr_array, 
                     h_matrix, 
                     ht_matrix, 
                     h_pred, 
                     v_degrees, 
                     N, 
                     max_deg, 
                     &params);


    // --- 5. 结果打印 ---
    std::cout << "\nIndex | Old LLR | New LLR" << std::endl;
    for (int i = 0; i < 10; ++i) {
        std::cout << i << " | " << 2.0f << " | " << (float)llr_array[i] << std::endl;
    }

    // --- 6. 清理内存 (重要) ---
    free(h_matrix);
    free(ht_matrix);
    free(llr_array);
    free(h_pred);
    free(v_degrees);
    deinit_llr_update_engine(engine);

    return 0;
}