#include <iostream>
#include <arm_neon.h>
#include <vector>
#include <thread>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include "master_thread.h" // 引入之前定义的 DecoderContext 结构

// --- 新增：声明外部实现的 Slave 线程函数 ---
extern void slave_worker_routine_update_s(DecoderContext& ctx, int slave_id, int num_slaves);
extern void slave_worker_routine_update_p(DecoderContext& ctx, int slave_id, int num_slaves);
extern void update_s_single_pass(DecoderContext& ctx);
extern void update_p_single_pass(DecoderContext& ctx);
extern void final_decision_with_bias(DecoderContext& ctx);

extern void update_s_ultimate(DecoderContext& ctx, uint32_t top_n, float power_val);
extern void update_p_ultimate(DecoderContext& ctx, uint32_t top_n, float power_val);
extern void final_decision_ultimate(DecoderContext& ctx);

float neon_mean_absolute_value(const float* data, size_t n) {
    if (n == 0) return 0.0f;

    float32x4_t sum_vec = vdupq_n_f32(0.0f);
    size_t i = 0;

    // 主循环：每次处理 4 个 float
    for (; i + 3 < n; i += 4) {
        float32x4_t v = vld1q_f32(data + i);
        float32x4_t v_abs = vabsq_f32(v);    // 修复：这里改为 C++ 注释 //
        sum_vec = vaddq_f32(sum_vec, v_abs); // 修复：这里改为 C++ 注释 //
    }

    // 水平求和
    float32x2_t low_high = vadd_f32(vget_low_f32(sum_vec), vget_high_f32(sum_vec));
    float total_sum = vget_lane_f32(low_high, 0) + vget_lane_f32(low_high, 1);

    // 处理余数
    for (; i < n; ++i) {
        total_sum += std::abs(data[i]);
    }

    return total_sum / static_cast<float>(n);
}

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

// 辅助函数：分配按页对齐的共享内存 (用于 UMA 零拷贝)
void* allocate_aligned_buffer(size_t size) {
    void* ptr = nullptr;
    // 4096 字节页对齐，这是 Metal newBufferWithBytesNoCopy 的基本要求
    posix_memalign(&ptr, 4096, size);
    if (ptr) std::memset(ptr, 0, size);
    return ptr;
}

/**
 * Master 译码调度主函数
 * @param Z 提升因子
 * @param input_s_llr 外部传入的初始 S 软信息
 * @param input_p_llr 外部传入的初始 P 软信息
 * @param max_wait_us 最大等待时间（微秒）
 */
bool LDPC_Decode_Master(int Z, const float* input_s_llr, const float* input_p_llr, int max_wait_us, uint8_t* out_s_bits, void* metal_engine) {
    DecoderContext ctx;

    // 1. 根据 Z 计算维度
    ctx.n_info = 22 * Z;
    ctx.n_parity = 46 * Z;
    ctx.words_per_row_G = (ctx.n_info + 31) / 32;   // G 每行需要的 uint32
    ctx.words_per_col_G = (ctx.n_parity + 31) / 32; // GT 每行需要的 uint32 (即 G 的列)

    // 2. 加载矩阵 (mmap 零拷贝映射)
    size_t dummy_size;
    std::string prefix = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_";
    
    // G 和 F 对 GPU 友好，不强制要求 SEQUENTIAL 预读（由 Metal 调度）
    ctx.G_rows_packed = mmap_matrix(prefix + "G.bin", dummy_size, false);
    ctx.F_rows_packed = mmap_matrix(prefix + "F.bin", dummy_size, false);
    
    // GT 和 FT 由 CPU Slave 扫描，必须开启 MADV_SEQUENTIAL 优化缓存
    ctx.G_cols_packed = mmap_matrix(prefix + "GT.bin", dummy_size, true);
    ctx.F_cols_packed = mmap_matrix(prefix + "FT.bin", dummy_size, true);

    if (!ctx.G_rows_packed || !ctx.G_cols_packed || !ctx.F_rows_packed || !ctx.F_cols_packed) {
        std::cerr << "矩阵加载失败，请检查 bin 文件是否存在！" << std::endl;
        return false;
    }
    // 3. 分配动态 Buffer (UMA 模式)
    // 注意：这些内存直接暴露给 GPU，无需 memcpy
    ctx.s_llr = (float*)allocate_aligned_buffer(ctx.n_info * sizeof(float));
    ctx.p_llr = (float*)allocate_aligned_buffer(ctx.n_parity * sizeof(float));
    ctx.s_hard = (uint32_t*)allocate_aligned_buffer(ctx.words_per_row_G * 4); // 位包装存储
    ctx.p_hard = (uint32_t*)allocate_aligned_buffer(ctx.words_per_col_G * 4);
    
    ctx.p_pred = (uint8_t*)allocate_aligned_buffer(ctx.n_parity); // GPU 写入 uint8 结果
    ctx.s_pred = (uint8_t*)allocate_aligned_buffer(ctx.n_info);

    // 4. 初始化数据与参数
    std::memcpy(ctx.s_llr, input_s_llr, ctx.n_info * sizeof(float));
    std::memcpy(ctx.p_llr, input_p_llr, ctx.n_parity * sizeof(float));
    ctx.s_channel = input_s_llr; // 记录原始背景
    ctx.p_channel = input_p_llr;

    // 初始化硬判决 (基于初始 LLR：LLR < 0 判为 1)
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        if (ctx.s_llr[i] < 0.0f){
            ctx.s_hard[i / 32] |= (1U << (i % 32));
            //std::cout << "i= " << i << ", ctx.s_hard = " << ((ctx.s_hard[i / 32] >> (i % 32)) & 1) << std::endl;
        }
    }
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        if (ctx.p_llr[j] < 0.0f) ctx.p_hard[j / 32] |= (1U << (j % 32));
    }

    // 5. 设置算法超参数
    ctx.eta = 0.4f;
    ctx.threshold = 0.3f;
    ctx.s_bias = 1.3f;
    ctx.is_converged = false;
    ctx.stop_flag = false;

    // 5. 启动异构计算
    // --- 启动 GPU 预测线程 ---
    std::thread gpu_thread([&]() {
        while (!ctx.stop_flag.load() && !ctx.is_converged.load()) {
            // 计算 p_pred = G * s_hard
            metal_compute_gf2(metal_engine, ctx.G_rows_packed, (uint32_t*)ctx.s_hard, ctx.p_pred, ctx.n_parity, ctx.words_per_row_G);
            // 计算 s_pred = F * p_hard
            metal_compute_gf2(metal_engine, ctx.F_rows_packed, (uint32_t*)ctx.p_hard, ctx.s_pred, ctx.n_info, ctx.words_per_col_G);
            std::this_thread::yield(); // 让出 CPU，不空转
        }
    });

    // --- 启动 CPU 收敛检测线程 ---
    std::thread checker_thread([&]() {
        while (!ctx.stop_flag.load() && !ctx.is_converged.load()) {
            bool ok = true;
            uint32_t* p_h = (uint32_t*)ctx.p_hard;
            for(uint32_t j=0; j < ctx.n_parity; ++j) {
                if (ctx.p_pred[j] != ((p_h[j/32] >> (j%32)) & 1)) { 
                    //std::cout << "j = " << j << ", ctx.p_pred = " << (int)ctx.p_pred[j] << ", p_hard = " << ((p_h[j/32] >> (j%32)) & 1) << std::endl;
                    ok = false; 
                    break; 
                }
            }
            ok = true;
            uint32_t* s_h = (uint32_t*)ctx.s_hard;
            for(uint32_t i=0; i < ctx.n_info; ++i) {
                if (ctx.s_pred[i] != ((s_h[i/32] >> (i%32)) & 1)) { 
                    //std::cout << "i = " << i << ", ctx.s_pred = " << (int)ctx.s_pred[i] << ", s_hard = " << ((s_h[i/32] >> (i%32)) & 1) << std::endl;
                    ok = false; 
                    break; 
                }
            }
             if (ok) {
                ctx.is_converged.store(true);
                ctx.cv.notify_all();
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(500)); // 每 500 微秒检查一次，避免过度频繁访问共享内存
        }
    });

    // --- 启动 CPU Worker 线程池 ---
    int num_slaves = std::thread::hardware_concurrency() / 2; 
    if (num_slaves < 2) num_slaves = 2;
    int s_slaves = num_slaves / 2;
    int p_slaves = num_slaves - s_slaves;

    std::vector<std::thread> slaves;
    for (int i = 0; i < s_slaves; ++i) {
        slaves.emplace_back(slave_worker_routine_update_s, std::ref(ctx), i, s_slaves);
    }
    for (int i = 0; i < p_slaves; ++i) {
        slaves.emplace_back(slave_worker_routine_update_p, std::ref(ctx), i, p_slaves);
    }

// 主线程监控
    {
        std::unique_lock<std::mutex> lock(ctx.mtx);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::microseconds(max_wait_us);
        bool reached = ctx.cv.wait_until(lock, deadline, [&] { return ctx.is_converged.load(); });

        if (!reached) {
            ctx.stop_flag = true; 
        }
    }

    // 回收所有线程
    if (gpu_thread.joinable()) gpu_thread.join();
    if (checker_thread.joinable()) checker_thread.join();
    for (auto& t : slaves) if (t.joinable()) t.join();
    
    // 导出结果
    if (out_s_bits) {
        uint32_t* final_s_hard = (uint32_t*)ctx.s_hard;
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            out_s_bits[i] = (final_s_hard[i / 32] >> (i % 32)) & 1;
        }
    }

    free(ctx.s_llr); free(ctx.p_llr);
    free(ctx.s_hard); free(ctx.p_hard);
    free(ctx.p_pred); free(ctx.s_pred);
    
    return ctx.is_converged.load();
}

// 辅助函数：统计预测位与硬判决位的差异
uint32_t count_bit_errors(const uint8_t* pred, const uint32_t* hard, uint32_t n) {
    uint32_t errors = 0;
    for (uint32_t i = 0; i < n; ++i) {
        uint8_t h_bit = (hard[i / 32] >> (i % 32)) & 1U;
        if (pred[i] != h_bit) errors++;
    }
    return errors;
}

// 在迭代循环开始前强制注入 Python 的测试向量
void inject_test_vectors(DecoderContext& ctx) {
    float s_llr_initial[44] = {
        -13.4072f, -11.7073f, -6.5004f, -11.0049f, -16.4603f, 7.3466f, 8.3492f, 11.2242f, 14.4897f, 9.7813f, -1.9781f, -10.3451f, 12.4402f, -9.1873f, 10.0930f, 12.0510f, 7.9991f, -6.2694f, 6.0088f, 13.3359f, -5.9404f, -21.4995f, 8.8025f, -11.9194f, -4.1086f, 14.1715f, -2.7288f, 9.5796f, -7.3007f, -6.6794f, -13.8369f, -8.0255f, 4.1229f, -6.4458f, -7.9439f, 8.0816f, 5.7725f, 11.5094f, 6.8310f, -0.8857f, -12.7472f, 4.4126f, 14.2734f, 5.4604f
    };

    float p_llr_initial[92] = {
        -0.7843f, 13.4437f, -5.7952f, -13.6448f, -11.2954f, 13.0583f, 17.3996f, -19.4137f, 9.0269f, 11.9332f, 8.2822f, -17.8029f, 8.8140f, -11.4124f, 7.1792f, -7.7011f, -12.6937f, -8.3668f, -15.3947f, -10.2558f, -10.5720f, -6.5285f, 8.4668f, -14.0883f, 6.5911f, 11.6491f, 12.4090f, -5.0635f, 0.7304f, -11.5730f, 12.8838f, 16.0452f, -18.9350f, 19.7850f, -13.6448f, -14.9121f, 7.0469f, 3.6835f, 15.8952f, -17.3486f, -11.3740f, -6.5333f, -8.2318f, -8.4879f, 19.9498f, -7.9762f, 4.9943f, -11.1553f, 13.6137f, 0.3297f, 6.7162f, -10.6039f, 8.8701f, -14.2609f, -9.1383f, -15.1063f, -15.6362f, -8.4091f, 8.5253f, -12.4193f, -9.3500f, -15.4082f, -1.9741f, -11.9843f, -7.8208f, -14.1382f, 12.4753f, 13.0258f, 12.4384f, -10.4191f, 10.2808f, -3.1846f, -10.5804f, -12.4734f, 1.8391f, 9.0637f, 12.7018f, -10.1981f, -11.3816f, -12.8556f, 12.4770f, 1.2074f, -7.3485f, -17.9510f, -14.3238f, 9.3234f, -13.9039f, -12.7849f, -11.3117f, 5.0391f, -12.8991f, 13.6136f
    };

    for(int i=0; i<44; ++i) {
        ctx.s_llr[i] = s_llr_initial[i];
        // 记得同步更新 s_hard，符号为负则为 1
        if(ctx.s_llr[i] < 0) ctx.s_hard[i/32] |= (1U << (i%32));
        else ctx.s_hard[i/32] &= ~(1U << (i%32));
    }

    for(int j=0; j<92; ++j) {
        ctx.p_llr[j] = p_llr_initial[j];
        // 记得同步更新 p_hard，符号为负则为 1
        if(ctx.p_llr[j] < 0) ctx.p_hard[j/32] |= (1U << (j%32));
        else ctx.p_hard[j/32] &= ~(1U << (j%32));
    }
}

bool LDPC_Decode_Master_Sequential(int Z, const float* input_s_llr, const float* input_p_llr, uint8_t* out_s_bits, void* metal_engine) {
    DecoderContext ctx;

    // 1. 根据 Z 计算维度
    ctx.n_info = 22 * Z;
    ctx.n_parity = 46 * Z;
    ctx.words_per_row_G = (ctx.n_info + 31) / 32;   // G 每行需要的 uint32
    ctx.words_per_col_G = (ctx.n_parity + 31) / 32; // GT 每行需要的 uint32 (即 G 的列)

    // 2. 加载矩阵 (mmap 零拷贝映射)
    size_t dummy_size;
    std::string prefix = "../g_f_matrix/BG1_LSindex0_Z" + std::to_string(Z) + "_";
    
    // G 和 F 对 GPU 友好，不强制要求 SEQUENTIAL 预读（由 Metal 调度）
    ctx.G_rows_packed = mmap_matrix(prefix + "G.bin", dummy_size, false);
    ctx.F_rows_packed = mmap_matrix(prefix + "F.bin", dummy_size, false);
    
    // GT 和 FT 由 CPU Slave 扫描，必须开启 MADV_SEQUENTIAL 优化缓存
    ctx.G_cols_packed = mmap_matrix(prefix + "GT.bin", dummy_size, true);
    ctx.F_cols_packed = mmap_matrix(prefix + "FT.bin", dummy_size, true);

    if (!ctx.G_rows_packed || !ctx.G_cols_packed || !ctx.F_rows_packed || !ctx.F_cols_packed) {
        std::cerr << "矩阵加载失败，请检查 bin 文件是否存在！" << std::endl;
        return false;
    }
    // 3. 分配动态 Buffer (UMA 模式)
    // 注意：这些内存直接暴露给 GPU，无需 memcpy
    ctx.s_llr = (float*)allocate_aligned_buffer(ctx.n_info * sizeof(float));
    ctx.p_llr = (float*)allocate_aligned_buffer(ctx.n_parity * sizeof(float));
    ctx.s_hard = (uint32_t*)allocate_aligned_buffer(ctx.words_per_row_G * 4); // 位包装存储
    ctx.p_hard = (uint32_t*)allocate_aligned_buffer(ctx.words_per_col_G * 4);
    
    ctx.p_pred = (uint8_t*)allocate_aligned_buffer(ctx.n_parity); // GPU 写入 uint8 结果
    ctx.s_pred = (uint8_t*)allocate_aligned_buffer(ctx.n_info);

    // 4. 初始化数据与参数
    std::memcpy(ctx.s_llr, input_s_llr, ctx.n_info * sizeof(float));
    std::memcpy(ctx.p_llr, input_p_llr, ctx.n_parity * sizeof(float));
    ctx.s_channel = input_s_llr; // 记录原始背景
    ctx.p_channel = input_p_llr;

    // 初始化硬判决 (基于初始 LLR：LLR < 0 判为 1)
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        if (ctx.s_llr[i] < 0.0f){
            ctx.s_hard[i / 32] |= (1U << (i % 32));
            //std::cout << "i= " << i << ", ctx.s_hard = " << ((ctx.s_hard[i / 32] >> (i % 32)) & 1) << std::endl;
        }
    }
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        if (ctx.p_llr[j] < 0.0f) ctx.p_hard[j / 32] |= (1U << (j % 32));
    }    
    // 5. 设置算法超参数
    ctx.eta = 0.8f;
    ctx.threshold = 0.3f;
    ctx.s_bias = 1.0f;
    ctx.is_converged = false;
    ctx.stop_flag = false;


    // inject_test_vectors(ctx); // 注入 Python 的测试向量

    // std::cout << ">>> 开始顺序迭代译码 (Z=" << Z << ")" << std::endl;

    for (int iter = 0; iter < 100; ++iter) {
        // --- 步骤 1: GPU 计算 P 空间预测 (p_pred = G * s_hard) ---
        metal_compute_gf2(metal_engine, ctx.G_rows_packed, ctx.s_hard, 
                           ctx.p_pred, ctx.n_parity, ctx.words_per_row_G);
        
        uint32_t p_errors = count_bit_errors(ctx.p_pred, ctx.p_hard, ctx.n_parity);
        // 检查收敛
        if (p_errors == 0) {
            // std::cout << ">>> 译码于第 " << iter << " 次迭代成功收敛！p_errors == 0" << std::endl;
            // 导出结果
            if (out_s_bits) {
                for (uint32_t i = 0; i < ctx.n_info; ++i) {
                    out_s_bits[i] = ctx.s_hard[i / 32] >> (i % 32) & 1U; // 直接使用硬判决结果，因为此时 p_pred 已经完全正确了  
                }
            }
            return true;
        }
        
        //for (uint32_t j = 0; j < ctx.n_info; ++j) {
        //    std::cout << "before s update: j = " << j << ", ctx.s_hard = " << ((ctx.s_hard[j / 32] >> (j % 32)) & 1) << std::endl;
        //}

        // --- 步骤 2: CPU 更新 S 空间 (单线程顺序执行一次) ---
        // 我们需要把 slave_worker_routine 里的 while 循环去掉，只跑一遍内部的 for 循环
        // update_s_single_pass(ctx); 
        update_s_ultimate(ctx, 3, 2.0);

        //std::cout << "==============================" << std::endl;
        //for (uint32_t j = 0; j < ctx.n_info; ++j) {
        //    std::cout << "after s update: j = " << j << ", ctx.s_hard = " << ((ctx.s_hard[j / 32] >> (j % 32)) & 1) << std::endl;
        //}

        // --- 步骤 3: GPU 计算 S 空间预测 (s_pred = F * p_hard) ---
        metal_compute_gf2(metal_engine, ctx.F_rows_packed, ctx.p_hard, 
                           ctx.s_pred, ctx.n_info, ctx.words_per_col_G);
        
        uint32_t s_errors = count_bit_errors(ctx.s_pred, ctx.s_hard, ctx.n_info);
        // 检查收敛
        if (s_errors == 0) {
            // std::cout << ">>> 译码于第 " << iter << " 次迭代成功收敛！s_errors == 0" << std::endl;
            // 导出结果
            if (out_s_bits) {
                for (uint32_t i = 0; i < ctx.n_info; ++i) {
                    out_s_bits[i] = ctx.s_pred[i]; // 直接使用 GPU 预测结果，因为此时 s_pred 已经完全正确了
                }
            }
            return true;
        }

        //for (uint32_t j = 0; j < ctx.n_info; ++j) {
        //    std::cout << "j = " << j << ", ctx.s_pred = " << (int)ctx.s_pred[j] << ", s_hard = " << ((ctx.s_hard[j / 32] >> (j % 32)) & 1) << std::endl;
        //}

        // --- 步骤 4: CPU 更新 P 空间 ---
        // update_p_single_pass(ctx);
        update_p_ultimate(ctx, 3, 2.0);


         // 打印调试信息
//        if (iter % 5 == 0 || p_errors == 0) {
//            std::cout << "Iter " << iter << " | P-Conflicts: " << p_errors 
//                      << " | S-Conflicts: " << s_errors << std::endl;
//        }
    }

    //final_decision_with_bias(ctx);
    final_decision_ultimate(ctx);

    if (out_s_bits) {
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            out_s_bits[i] = ctx.s_hard[i / 32] >> (i % 32) & 1U; // 直接使用硬判决结果，因为此时 p_pred 已经完全正确了  
        }
    }


    return false;
}
