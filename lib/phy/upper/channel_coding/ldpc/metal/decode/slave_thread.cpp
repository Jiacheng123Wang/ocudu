#include <iostream>
#include <arm_neon.h>
#include <cmath>
#include <algorithm>
#include <vector>
#include "master_thread.h"


void slave_worker_routine_update_s(DecoderContext& ctx, int slave_id, int num_slaves) {
    uint32_t j_per_slave = (ctx.n_parity + num_slaves - 1) / num_slaves;
    uint32_t start_j = slave_id * j_per_slave;
    uint32_t end_j = std::min(start_j + j_per_slave, ctx.n_parity);

    if (start_j >= ctx.n_parity) return;

    while (!ctx.stop_flag.load(std::memory_order_relaxed) && 
           !ctx.is_converged.load(std::memory_order_relaxed)) {
        
        for (uint32_t j = start_j; j < end_j; ++j) {
            // 1. 判定第 j 个校验方程是否矛盾
            // 提取 p_hard 中的第 i 位
            uint32_t p_bit = (ctx.p_hard[j / 32] >> (j % 32)) & 1U;
            if (ctx.p_pred[j] != (uint8_t)p_bit) { 
                
                // 2. 定位该方程中最弱嫌疑人 i_sus
                uint32_t i_sus = 0xFFFFFFFF;
                float min_abs_llr = 1e10f;
                
                const uint32_t* row_ptr = ctx.G_rows_packed + (j * ctx.words_per_row_G);
                for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
                    uint32_t val = row_ptr[w];
                    while (val != 0) {
                        int local_bit = __builtin_ctz(val);
                        uint32_t i = w * 32 + local_bit;
                        if (i < ctx.n_info) {
                            float abs_val = std::abs(ctx.s_llr[i]);
                            if (abs_val < min_abs_llr) {
                                min_abs_llr = abs_val;
                                i_sus = i;
                            }
                        }
                        val &= (val - 1);
                    }
                }

                if (i_sus == 0xFFFFFFFF) continue;

                // 3. 证据融合：检查 i_sus 参与的所有方程
                const uint32_t* col_ptr = ctx.G_cols_packed + (i_sus * ctx.words_per_col_G);
                uint32_t total_c = 0;
                uint32_t conflict_c = 0;
                float sum_abs_p_llr = 0.0f;

                for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
                    uint32_t val = col_ptr[w];
                    while (val != 0) {
                        int local_bit = __builtin_ctz(val);
                        uint32_t j_linked = w * 32 + local_bit;
                        if (j_linked < ctx.n_parity) {
                            total_c++;
                            if (ctx.p_pred[j_linked] == 1) {
                                conflict_c++;
                                sum_abs_p_llr += std::abs(ctx.p_llr[j_linked]);
                            }
                        }
                        val &= (val - 1);
                    }
                }

                // 4. 判定指控比例并执行均值更新
                if (total_c > 0 && ((float)conflict_c / total_c) >= ctx.threshold) {
                    float merged_llr = sum_abs_p_llr / (float)conflict_c;
                    
                    // --- 优化点：对比更新前后的符号 ---
                    float old_llr = ctx.s_llr[i_sus];
                    float sign_s = (old_llr >= 0.0f) ? 1.0f : -1.0f;
                    float update_step = sign_s * ctx.eta * merged_llr;
                    
                    float new_llr = old_llr - update_step;
                    ctx.s_llr[i_sus] = new_llr;

                    // 5. 符号翻转检查：只有符号变了才更新 s_hard
                    // 逻辑：(old_llr > 0) != (new_llr > 0)
                    if ((old_llr > 0.0f) != (new_llr > 0.0f)) {
                        uint32_t word_idx = i_sus / 32;
                        uint32_t bit_pos = i_sus % 32;

                        if (new_llr > 0.0f) {
                            __sync_fetch_and_or(&ctx.s_hard[word_idx], (1U << bit_pos));
                        } else {
                            __sync_fetch_and_and(&ctx.s_hard[word_idx], ~(1U << bit_pos));
                        }
                    }
                }
            }
        }
    }
}

/**
 * P 空间更新线程函数
 * 逻辑与 S 空间更新对称：
 * 监控 s_pred (由 GPU 计算 F * p_hard 得到)，若 s_pred[i] != s_hard[i]，则更新关联的 p_llr
 */
/**
 * P 空间更新线程函数 (已修正矩阵引用)
 */
void slave_worker_routine_update_p(DecoderContext& ctx, int slave_id, int num_slaves) {
    // 1. 任务分发：每个线程负责一部分信息方程 i 的检查 (0 到 n_info-1)
    uint32_t i_per_slave = (ctx.n_info + num_slaves - 1) / num_slaves;
    uint32_t start_i = slave_id * i_per_slave;
    uint32_t end_i = std::min(start_i + i_per_slave, ctx.n_info);

    if (start_i >= ctx.n_info) return;

    while (!ctx.stop_flag.load(std::memory_order_relaxed) && 
           !ctx.is_converged.load(std::memory_order_relaxed)) {
        
        for (uint32_t i = start_i; i < end_i; ++i) {
            // 2. 检查第 i 个信息方程是否矛盾 (s_pred[i] == 1 表示矛盾)
            // 提取 s_hard 中的第 i 位
            uint32_t s_bit = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
            if (ctx.s_pred[i] != (uint8_t)s_bit) { 
                
                // 3. 定位该方程中最弱嫌疑人 j_sus (P 比特)
                // 使用 F_rows_packed (i 映射到 P)
                uint32_t j_sus = 0xFFFFFFFF;
                float min_abs_llr = 1e10f;
                
                // F 矩阵每行有 n_parity 个比特，所以每行宽度是 words_per_col_G
                const uint32_t* row_ptr = ctx.F_rows_packed + (i * ctx.words_per_col_G);
                for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
                    uint32_t val = row_ptr[w];
                    while (val != 0) {
                        int local_bit = __builtin_ctz(val);
                        uint32_t j = w * 32 + local_bit;
                        if (j < ctx.n_parity) {
                            float abs_val = std::abs(ctx.p_llr[j]);
                            if (abs_val < min_abs_llr) {
                                min_abs_llr = abs_val;
                                j_sus = j;
                            }
                        }
                        val &= (val - 1);
                    }
                }

                if (j_sus == 0xFFFFFFFF) continue;

                // 4. 证据融合：检查 j_sus 参与的所有信息方程 (列扫描)
                // 使用 FT 矩阵 (F_cols_packed)，FT 每行对应一个 P 比特，连接若干个信息方程 i
                // FT 的每行宽度是 words_per_row_G
                const uint32_t* col_ptr = ctx.F_cols_packed + (j_sus * ctx.words_per_row_G); 
                uint32_t total_c = 0;
                uint32_t conflict_c = 0;
                float sum_abs_s_llr = 0.0f;

                for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
                    uint32_t val = col_ptr[w];
                    while (val != 0) {
                        int local_bit = __builtin_ctz(val);
                        uint32_t i_linked = w * 32 + local_bit;
                        if (i_linked < ctx.n_info) {
                            total_c++;
                            // 如果关联的信息方程也报错
                            if (ctx.s_pred[i_linked] == 1) { 
                                conflict_c++;
                                sum_abs_s_llr += std::abs(ctx.s_llr[i_linked]); 
                            }
                        }
                        val &= (val - 1);
                    }
                }

                // 5. 判定指控比例并执行更新
                if (total_c > 0 && ((float)conflict_c / (float)total_c) >= ctx.threshold) {
                    float merged_llr = sum_abs_s_llr / (float)conflict_c;
                    float old_llr = ctx.p_llr[j_sus];
                    float sign_p = (old_llr >= 0.0f) ? 1.0f : -1.0f;
                    
                    float new_llr = old_llr - (sign_p * ctx.eta * merged_llr);
                    ctx.p_llr[j_sus] = new_llr;

                    // 6. 符号翻转静默更新 p_hard
                    if ((old_llr > 0.0f) != (new_llr > 0.0f)) {
                        uint32_t word_idx = j_sus / 32;
                        uint32_t bit_pos = j_sus % 32;
                        if (new_llr > 0.0f) __sync_fetch_and_or(&ctx.p_hard[word_idx], (1U << bit_pos));
                        else __sync_fetch_and_and(&ctx.p_hard[word_idx], ~(1U << bit_pos));
                    }
                }
            }
        }
    }
}

void update_s_single_pass(DecoderContext& ctx) {
    // 1. 本地证据累加器（初始化为0）
    // evidence_count: 该比特被判定为“头号嫌疑人”的次数
    // evidence_sum:   累加这些指控方程的 LLR 绝对值（用于计算更新强度）
    std::vector<uint32_t> evidence_count(ctx.n_info, 0);
    std::vector<float> evidence_sum(ctx.n_info, 0.0f);

    // --- 第一阶段：全量搜证 (指名道姓) ---
    // 遍历所有校验方程，每个报错方程只能投出一张“嫌疑票”
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        // 获取当前 P 的硬判决值
        uint8_t target_p = (ctx.p_hard[j / 32] >> (j % 32)) & 1U;
        
        // 只有方程报错时才寻找嫌疑人
        if (ctx.p_pred[j] != target_p) {
            uint32_t i_sus = 0xFFFFFFFF;
            float min_abs_s = 1e10f;

            // 寻找该方程连接的所有 S 中，LLR 绝对值最小的那个
            const uint32_t* row_ptr = &ctx.G_rows_packed[j * ctx.words_per_row_G];
            for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
                uint32_t val = row_ptr[w];
                while (val) {
                    uint32_t i = w * 32 + __builtin_ctz(val);
                    if (i < ctx.n_info) {
                        float abs_val = std::abs(ctx.s_llr[i]);
                        if (abs_val < min_abs_s) {
                            min_abs_s = abs_val;
                            i_sus = i;
                        }
                    }
                    val &= (val - 1);
                }
            }

            // 该方程投出一张票给 i_sus，强度为该校验位的 LLR
            if (i_sus != 0xFFFFFFFF) {
                evidence_count[i_sus]++;
                evidence_sum[i_sus] += std::abs(ctx.p_llr[j]);
                //std::cout << "i_sus = " << i_sus << ", evidence_count = " << evidence_count[i_sus] << ", evidence_sum = " << evidence_sum[i_sus] << std::endl;
            }
            //std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>> conflict found in parity equation " << j << ", accusing parity bit " << i_sus << std::endl;
        }
    }

    // --- 第二阶段：陪审团票决 (统一更新) ---
    // 遍历所有被指控过的比特
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        if (evidence_count[i] > 0) {
            // 计算该比特参与的总方程数 (Degree)
            uint32_t total_connected = 0;
            const uint32_t* col_ptr = &ctx.G_cols_packed[i * ctx.words_per_col_G];
            for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
                total_connected += __builtin_popcount(col_ptr[w]);
            }

            // 严格比例计算：只有被指证次数 / 总方程数 >= threshold 才能定罪
            float conflict_ratio = (float)evidence_count[i] / (float)total_connected;
            //std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>> info bit " << i << " has conflict ratio: " << conflict_ratio << std::endl;

            if (conflict_ratio >= 0.5) {
                ctx.s_llr[i] *= -1.0f; // 直接翻转符号

                // 同步更新硬判决 s_hard (用于下一轮迭代的 p_pred 计算)
                if (ctx.s_llr[i] < 0.0f) {
                    ctx.s_hard[i / 32] |= (1U << (i % 32));
                } else {
                    ctx.s_hard[i / 32] &= ~(1U << (i % 32));
                }
            } else if (conflict_ratio >= ctx.threshold) {
                float merged_llr = evidence_sum[i] / (float)evidence_count[i];
                float old_llr = ctx.s_llr[i];
                float sign_s = (old_llr >= 0.0f) ? 1.0f : -1.0f;

                // 只有通过票决，才执行 LLR 更新
                ctx.s_llr[i] -= sign_s * ctx.eta * merged_llr;

                // 同步更新硬判决 s_hard (用于下一轮迭代的 p_pred 计算)
                if (ctx.s_llr[i] < 0.0f) {
                    ctx.s_hard[i / 32] |= (1U << (i % 32));
                } else {
                    ctx.s_hard[i / 32] &= ~(1U << (i % 32));
                }
            }
        }
    }
}

void update_p_single_pass(DecoderContext& ctx) {
    std::vector<uint32_t> evidence_count(ctx.n_parity, 0);
    std::vector<float> evidence_sum(ctx.n_parity, 0.0f);

    // --- 第一阶段：搜证 ---
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        uint8_t target_s = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
        
        if (ctx.s_pred[i] != target_s) {
            uint32_t j_sus = 0xFFFFFFFF;
            float min_abs_p = 1e10f;

            const uint32_t* row_ptr = &ctx.F_rows_packed[i * ctx.words_per_col_G];
            for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
                uint32_t val = row_ptr[w];
                while (val) {
                    uint32_t j = w * 32 + __builtin_ctz(val);
                    if (j < ctx.n_parity) {
                        float abs_val = std::abs(ctx.p_llr[j]);
                        if (abs_val < min_abs_p) {
                            min_abs_p = abs_val;
                            j_sus = j;
                        }
                    }
                    val &= (val - 1);
                }
            }

            if (j_sus != 0xFFFFFFFF) {
                evidence_count[j_sus]++;
                evidence_sum[j_sus] += std::abs(ctx.s_llr[i]);
            }
            //std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>> conflict found in info equation " << i << ", accusing parity bit " << j_sus << std::endl;
        } 
    }

    // --- 第二阶段：票决 ---
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        if (evidence_count[j] > 0) {
            uint32_t total_connected = 0;
            const uint32_t* col_ptr = &ctx.F_cols_packed[j * ctx.words_per_row_G];
            for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
                total_connected += __builtin_popcount(col_ptr[w]);
            }

            float conflict_ratio = (float)evidence_count[j] / (float)total_connected;
            //std::cout << ">>>>>>>>>>>>>>>>>>>>>>>>>> parity bit " << j << " has conflict ratio: " << conflict_ratio << std::endl;
            
            if (conflict_ratio >= 0.5) {
                ctx.p_llr[j] *= -1.0f; // 直接翻转符号

                if (ctx.p_llr[j] < 0.0f) {
                    ctx.p_hard[j / 32] |= (1U << (j % 32));
                } else {
                    ctx.p_hard[j / 32] &= ~(1U << (j % 32));
                }
            } else if (conflict_ratio >= (ctx.threshold)) {
                float merged_llr = evidence_sum[j] / (float)evidence_count[j];
                float old_llr = ctx.p_llr[j];
                float sign_p = (old_llr >= 0.0f) ? 1.0f : -1.0f;

                ctx.p_llr[j] -= sign_p * ctx.eta * merged_llr; // 加大更新力度

                if (ctx.p_llr[j] < 0.0f) {
                    ctx.p_hard[j / 32] |= (1U << (j % 32));
                } else {
                    ctx.p_hard[j / 32] &= ~(1U << (j % 32));
                }
            }
        }
    }
}

// 在 LDPC_Decode_Master 的迭代循环结束后调用
void final_decision_with_bias(DecoderContext& ctx) {
    uint32_t s_satisfied = 0;
    uint32_t p_satisfied = 0;

    // 统计 G 方程满足数 (S 空间置信度)
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        uint8_t target_p = (ctx.p_hard[j / 32] >> (j % 32)) & 1U;
        if (ctx.p_pred[j] == target_p) s_satisfied++;
    }

    // 统计 F 方程满足数 (P 空间置信度)
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        uint8_t target_s = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
        if (ctx.s_pred[i] == target_s) p_satisfied++;
    }

    float avg_s_conf = (float)s_satisfied / ctx.n_parity;
    float avg_p_conf = (float)p_satisfied / ctx.n_info;

    std::cout << ">>> 迭代结束。S-Conf: " << avg_s_conf << ", P-Conf: " << avg_p_conf << std::endl;

    // 关键逻辑：如果 avg_s_conf * s_bias >= avg_p_conf，维持当前 s_hard
    // 否则，说明 P 空间更可信，将 s_hard 强制重置为由 p_hard 预测出的结果 (即 ctx.s_pred)
    if (avg_s_conf * ctx.s_bias < avg_p_conf) {
        std::cout << ">>> 检测到 S 空间弱势，根据 s_bias 切换至 P 空间预测结果。" << std::endl;
        uint32_t* s_h = (uint32_t*)ctx.s_hard;
        std::memset(s_h, 0, ctx.words_per_row_G * 4);
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            if (ctx.s_pred[i] == 1) {
                s_h[i / 32] |= (1U << (i % 32));
            }
        }
    }
}

void update_s_ultimate(DecoderContext& ctx, uint32_t top_n, float power_val) {
    std::vector<uint32_t> evidence_count(ctx.n_info, 0);
    std::vector<float> evidence_sum(ctx.n_info, 0.0f);

    // --- 第一阶段：Top-N 证据收集 ---
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        uint8_t target_p = (ctx.p_hard[j / 32] >> (j % 32)) & 1U;
        if (ctx.p_pred[j] != target_p) {
            float abs_p = std::abs(ctx.p_llr[j]);
            
            // 找出该行所有关联 S 的索引和绝对值
            struct Suspect { uint32_t idx; float abs_llr; };
            std::vector<Suspect> candidates;
            
            const uint32_t* row_ptr = &ctx.G_rows_packed[j * ctx.words_per_row_G];
            for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
                uint32_t val = row_ptr[w];
                while (val) {
                    uint32_t i = w * 32 + __builtin_ctz(val);
                    if (i < ctx.n_info) candidates.push_back({i, std::abs(ctx.s_llr[i])});
                    val &= (val - 1);
                }
            }

            // 排序选出弱比特 (Top-N)
            std::sort(candidates.begin(), candidates.end(), [](const Suspect& a, const Suspect& b) {
                return a.abs_llr < b.abs_llr;
            });

            uint32_t actual_n = std::min((uint32_t)candidates.size(), top_n);
            if (actual_n == 0) continue;

            float val1 = candidates[0].abs_llr;
            float val2 = (candidates.size() > 1) ? candidates[1].abs_llr : abs_p;

            // 应用 Min-Sum 强度限制
            for (uint32_t rank = 0; rank < actual_n; ++rank) {
                uint32_t target_i = candidates[rank].idx;
                float strength = (rank == 0) ? std::min(val2, abs_p) : std::min(val1, abs_p);
                evidence_count[target_i]++;
                evidence_sum[target_i] += strength;
            }
        }
    }

    // --- 第二阶段：Power 权重更新 ---
    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        if (evidence_count[i] > 0) {
            uint32_t total_c = 0; // 计算 Degree
            const uint32_t* col_ptr = &ctx.G_cols_packed[i * ctx.words_per_col_G];
            for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) total_c += __builtin_popcount(col_ptr[w]);

            float ratio = (float)evidence_count[i] / (float)total_c;
            float weight = std::pow(std::min(ratio, 1.0f), power_val);
            float mean_llr = evidence_sum[i] / (float)evidence_count[i];

            float old_llr = ctx.s_llr[i];
            float sign_s = (old_llr >= 0.0f) ? 1.0f : -1.0f;
            ctx.s_llr[i] -= sign_s * ctx.eta * weight * mean_llr;

            // 同步硬判决
            if (ctx.s_llr[i] < 0.0f) ctx.s_hard[i / 32] |= (1U << (i % 32));
            else ctx.s_hard[i / 32] &= ~(1U << (i % 32));
        }
    }
}

void update_p_ultimate(DecoderContext& ctx, uint32_t top_n, float power_val) {
    std::vector<uint32_t> evidence_count(ctx.n_parity, 0);
    std::vector<float> evidence_sum(ctx.n_parity, 0.0f);

    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        uint8_t target_s = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
        if (ctx.s_pred[i] != target_s) {
            float abs_s = std::abs(ctx.s_llr[i]);
            struct Suspect { uint32_t idx; float abs_llr; };
            std::vector<Suspect> candidates;
            
            const uint32_t* row_ptr = &ctx.F_rows_packed[i * ctx.words_per_col_G];
            for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
                uint32_t val = row_ptr[w];
                while (val) {
                    uint32_t j = w * 32 + __builtin_ctz(val);
                    if (j < ctx.n_parity) candidates.push_back({j, std::abs(ctx.p_llr[j])});
                    val &= (val - 1);
                }
            }
            std::sort(candidates.begin(), candidates.end(), [](const Suspect& a, const Suspect& b) { return a.abs_llr < b.abs_llr; });

            uint32_t actual_n = std::min((uint32_t)candidates.size(), top_n);
            if (actual_n == 0) continue;
            float val1 = candidates[0].abs_llr;
            float val2 = (candidates.size() > 1) ? candidates[1].abs_llr : abs_s;

            for (uint32_t rank = 0; rank < actual_n; ++rank) {
                uint32_t target_j = candidates[rank].idx;
                float strength = (rank == 0) ? std::min(val2, abs_s) : std::min(val1, abs_s);
                evidence_count[target_j]++;
                evidence_sum[target_j] += strength;
            }
        }
    }

    // 更新 P 空间 LLR (同 S 空间逻辑)
    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        if (evidence_count[j] > 0) {
            uint32_t total_c = 0;
            const uint32_t* col_ptr = &ctx.F_cols_packed[j * ctx.words_per_row_G];
            for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) total_c += __builtin_popcount(col_ptr[w]);
            float weight = std::pow((float)evidence_count[j] / total_c, power_val);
            float sign_p = (ctx.p_llr[j] >= 0.0f) ? 1.0f : -1.0f;
            ctx.p_llr[j] -= sign_p * ctx.eta * weight * (evidence_sum[j] / evidence_count[j]);
            if (ctx.p_llr[j] < 0.0f) ctx.p_hard[j / 32] |= (1U << (j % 32));
            else ctx.p_hard[j / 32] &= ~(1U << (j % 32));
        }
    }
}

void final_decision_ultimate(DecoderContext& ctx) {
    auto compute_energy = [](const float* llr_ptr, uint32_t n, const uint8_t* pred, const uint32_t* hard, bool is_satisfied_only) {
        float total_sum = 0.0f;
        uint32_t i = 0;
        float32x4_t v_sum = vdupq_n_f32(0.0f);

        // 使用 NEON 加速绝对值求和 (一次处理 4 个 float)
        for (; i + 3 < n; i += 4) {
            float32x4_t v_llr = vld1q_f32(llr_ptr + i);
            float32x4_t v_abs = vabsq_f32(v_llr);
            
            if (is_satisfied_only) {
                // 如果只计算 Satisfied 能量，需检查 pred 与 hard 是否一致
                for (int k = 0; k < 4; ++k) {
                    uint32_t idx = i + k;
                    uint8_t h = (hard[idx / 32] >> (idx % 32)) & 1U;
                    if (pred[idx] == h) total_sum += std::abs(llr_ptr[idx]);
                }
            } else {
                v_sum = vaddq_f32(v_sum, v_abs);
            }
        }
        if (!is_satisfied_only) {
            float tmp[4];
            vst1q_f32(tmp, v_sum);
            total_sum = tmp[0] + tmp[1] + tmp[2] + tmp[3];
        }
        // 处理剩余部分
        for (; i < n; ++i) {
            if (is_satisfied_only) {
                uint8_t h = (hard[i / 32] >> (i % 32)) & 1U;
                if (pred[i] == h) total_sum += std::abs(llr_ptr[i]);
            } else total_sum += std::abs(llr_ptr[i]);
        }
        return total_sum + 1e-12f;
    };

    // 1. S 空间在 P 方程的表现评分
    float s_satisfied_energy = compute_energy(ctx.p_llr, ctx.n_parity, ctx.p_pred, ctx.p_hard, true);
    float total_p_energy = compute_energy(ctx.p_llr, ctx.n_parity, nullptr, nullptr, false);
    float s_weighted_score = s_satisfied_energy / total_p_energy;

    // 2. P 空间在 S 方程的表现评分
    float p_satisfied_energy = compute_energy(ctx.s_llr, ctx.n_info, ctx.s_pred, ctx.s_hard, true);
    float total_s_energy = compute_energy(ctx.s_llr, ctx.n_info, nullptr, nullptr, false);
    float p_weighted_score = p_satisfied_energy / total_s_energy;

    // 3. 最终博弈裁决
    if (s_weighted_score * ctx.s_bias < p_weighted_score) {
        // 切换至 P 空间预测结果
        std::memset(ctx.s_hard, 0, ctx.words_per_row_G * 4);
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            if (ctx.s_pred[i] == 1) ctx.s_hard[i / 32] |= (1U << (i % 32));
        }
    }
}

