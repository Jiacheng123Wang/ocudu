#include <iostream>
#include <cstring>
#include "./DecoderContextMetal.h"
#include "../encode/MetalEngine.h"

bool LDPCDecodeMetal(
    DecoderContext &ctx, // 引擎现在包含在 ctx 内
    int max_iter,
    uint32_t n_info,
    const float16 *in_sp_llr,
    uint8_t *out_s_bits)
{
    std::memcpy(ctx.sp_llr_buf->get(), in_sp_llr, ctx.N_logical * sizeof(float16));

    uint32_t *sp_hard_ptr = ctx.sp_hard_buf->get();
    std::memset(sp_hard_ptr, 0, ctx.n_h_chunks * sizeof(uint32_t));
    const float16 *llr_ptr = ctx.sp_llr_buf->get();
    for (uint32_t i = 0; i < ctx.N_logical; ++i)
    {
        if (llr_ptr[i] < 0)
        {
            sp_hard_ptr[i / 32] |= (1U << (i % 32));
        }
    }
    /*
        std::cout << "[DEBUG] ctx.n_h_chunks = " << ctx.n_h_chunks << std::endl;

        // 【新增调试 1】：检查 H 矩阵的前几个字，确保 mmap 加载正确
        uint32_t* h_ptr = ctx.h_matrix_buf->get();
        std::cout << "\n[CPU CHECK] H-Matrix 内存前 4 个字: ";
        for(int i=0; i<4; ++i) printf("0x%08x ", h_ptr[i]);
        std::cout << std::endl;
    */
    for (int iter = 0; iter < max_iter; ++iter)
    {
        // 关键修复：此处专一使用 gf2_engine
        metal_compute_gf2(
            ctx.gf2_engine,
            KERNEL_TYPE_BIT_PACKED,
            ctx.h_matrix_buf->get(),
            ctx.sp_hard_buf->get(),
            ctx.h_pred_buf->get(),
            ctx.M_aligned,
            ctx.n_h_chunks);
        /*
        // 【新增调试 2】：打印完整的 h_pred 状态
                uint32_t* pred_ptr1 = ctx.h_pred_buf->get();
                std::cout << "[CPU CHECK] Iter " << iter << " 校验子 (h_pred) 详情: ";
                for (uint32_t i = 0; i < ctx.M_aligned / 32; ++i) {
                    printf("[%d]:0x%08x ", i, pred_ptr1[i]);
                }
                std::cout << std::endl;
        */

        bool all_pass = true;
        uint32_t *pred_ptr = ctx.h_pred_buf->get();
        for (uint32_t i = 0; i < ctx.M_aligned / 32; ++i)
        {
            if (pred_ptr[i] != 0)
            {
                all_pass = false;
                break;
            }
        }

        if (all_pass)
        {
            if (out_s_bits)
            {
                for (uint32_t i = 0; i < n_info; ++i)
                {
                    out_s_bits[i] = (sp_hard_ptr[i / 32] >> (i % 32)) & 1U;
                }
            }
            return true;
        }

        // 调用 LLS engine
        ctx.run_iteration();
        /*
                // 【新增调试 3】：扫描 debug_stats_buf，寻找非零的 VN
                if (iter == 0)
                {
                    VNStats *stats = ctx.debug_stats_buf->get();
                    int active_vns = 0;
                    std::cout << "[---CPU CHECK] 搜索第一个迭代中受影响的 VN:" << std::endl;
                    for (uint32_t i = 0; i < ctx.N_logical; ++i)
                    {
                        if (stats[i].ErrEqCnt > 0)
                        {
                            printf("  VN[%u]: ErrEqCnt=%u, Suspect=%u, EvidSum=%.4f\n",
                                   i, stats[i].ErrEqCnt, stats[i].SuspectCnt, stats[i].EvidSum);
                            active_vns++;
                            //                    if (active_vns > 10) break; // 只看前10个
                        }
                    }
                    if (active_vns == 0)
                    {
                        std::cout << "  警告：没有任何 VN 的 ErrEqCnt > 0！Kernel 1 逻辑未触发。" << std::endl;
                    }
                }

                // --- 调试打印 ---
                if (iter == 0)
                {
                    VNStats *stats = ctx.debug_stats_buf->get();
                    float16 *current_llr = ctx.sp_llr_buf->get();

                    std::cout << "\n[GPU DEBUG - Iter 0] 重点观察 VN[8] (已知嫌疑人):" << std::endl;
                    printf("VN[8]: ErrEqCnt=%u, Suspect=%u, EvidSum=%.4f, LLR_After_Iter=%.4f\n",
                           stats[8].ErrEqCnt, stats[8].SuspectCnt, stats[8].EvidSum, (float)current_llr[8]);
                }
                // --- 调试打印结束 ---
            }
        */
        if (out_s_bits)
        {
            for (uint32_t i = 0; i < n_info; ++i)
            {
                out_s_bits[i] = (sp_hard_ptr[i / 32] >> (i % 32)) & 1U;
            }
        }
    }
    return false;
}
