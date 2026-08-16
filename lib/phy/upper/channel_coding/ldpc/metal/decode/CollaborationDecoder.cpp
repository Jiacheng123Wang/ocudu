#include "CollaborationDecoder.h"
#include "NeonHelper.h"
#include <cmath>
#include <vector>
#include <algorithm>

void update_s(DecoderContext& ctx, uint32_t top_n, float power_val) {
    // 证据收集计数器（用于计算平均强度）
    std::vector<uint32_t> evidence_count(ctx.n_info, 0);
    std::vector<float> evidence_sum(ctx.n_info, 0.0f);
    // 冲突计数器（用于计算 weight/ratio）
    std::vector<uint32_t> s_conflict_cnt(ctx.n_info, 0);

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
                    // --- 只要方程冲突，所有关联比特的冲突计数都增加 ---
                    s_conflict_cnt[i]++;
                    candidates.push_back({i, std::abs(ctx.s_llr[i])});
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
            float weight = std::pow((float)s_conflict_cnt[i] / (float)ctx.degree_s[i], power_val);
            float mean_llr = evidence_sum[i] / (float)evidence_count[i];
            float old_llr = ctx.s_llr[i];
            float sign_s = (old_llr >= 0.0f) ? 1.0f : -1.0f;

            ctx.s_llr[i] -= sign_s * ctx.eta * weight * mean_llr;

            // 同步硬判决
            if (old_llr * ctx.s_llr[i] < 0.0f){
                if (ctx.s_llr[i] < 0.0f) ctx.s_hard[i / 32] |= (1U << (i % 32));
                else ctx.s_hard[i / 32] &= ~(1U << (i % 32));
            }
        }
    }
}

void update_p(DecoderContext& ctx, uint32_t top_n, float power_val) {
    std::vector<uint32_t> evidence_count(ctx.n_parity, 0);
    std::vector<float> evidence_sum(ctx.n_parity, 0.0f);
    std::vector<uint32_t> p_conflict_cnt(ctx.n_parity, 0);

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
                    p_conflict_cnt[j]++; // 广义冲突计数
                    candidates.push_back({j, std::abs(ctx.p_llr[j])});
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
            float weight = std::pow((float)evidence_count[j] / (float)ctx.degree_p[j], power_val);
            float mean_llr = evidence_sum[j] / (float)evidence_count[j];
            float old_llr = ctx.p_llr[j];
            float sign_p = (old_llr >= 0.0f) ? 1.0f : -1.0f;

            ctx.p_llr[j] -= sign_p * ctx.eta * weight * mean_llr;

            if (old_llr * ctx.p_llr[j] < 0.0f){
                if (ctx.p_llr[j] < 0.0f) ctx.p_hard[j / 32] |= (1U << (j % 32));
                else ctx.p_hard[j / 32] &= ~(1U << (j % 32));
            }
        }
    }
}

void final_decision(DecoderContext& ctx) {
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

void update_s_neon(DecoderContext& ctx, uint32_t top_n, float power_val) {
    // 零初始化本轮证据缓存 (使用 memset 或 NEON 填充)
    std::memset(ctx.s_evidence_sum, 0, ctx.padded_n_info * sizeof(float));
    std::memset(ctx.s_evidence_cnt, 0, ctx.padded_n_info * sizeof(uint16_t));
    // 2. 冲突计数（全局）
    std::vector<uint32_t> s_conflict_cnt(ctx.padded_n_info, 0);

    for (uint32_t j = 0; j < ctx.n_parity; ++j) {
        uint8_t h_p = (ctx.p_hard[j / 32] >> (j % 32)) & 1U;
        if (ctx.p_pred[j] == h_p) continue; 

        float abs_p = std::abs(ctx.p_llr[j]);
        uint32_t found = 0;

        // --- 安全：使用动态缓存 row_suspect_ids ---
        const uint32_t* row_ptr = &ctx.G_rows_packed[j * ctx.words_per_row_G];
        for (uint32_t w = 0; w < ctx.words_per_row_G; ++w) {
            uint32_t val = row_ptr[w];
            while (val) {
                uint32_t i = w * 32 + __builtin_ctz(val);
                if (i < ctx.n_info) {
                    ctx.row_suspect_ids[found] = i;
                    ctx.row_suspect_abs[found] = std::abs(ctx.s_llr[i]);
                    // --- 全局统计逻辑：只要方程冲突，所有关联比特计数都加 1 ---
                    s_conflict_cnt[i]++;
                    found++;
                }
                val &= (val - 1);
            }
        }

        uint32_t actual_n = std::min(found, top_n);
        // 对 found 长度的数组进行安全排序
        NeonHelper::sort_top3(ctx.row_suspect_ids, ctx.row_suspect_abs, found);

        float val1 = ctx.row_suspect_abs[0];
        float val2 = (found > 1) ? ctx.row_suspect_abs[1] : abs_p;

        for (uint32_t r = 0; r < actual_n; ++r) {
            uint32_t id = ctx.row_suspect_ids[r];
            float strength = (r == 0) ? std::min(val2, abs_p) : std::min(val1, abs_p);
            ctx.s_evidence_sum[id] += strength;
            ctx.s_evidence_cnt[id]++;
        }
    }

    // --- NEON 批量更新 LLR (利用对齐和 Padding) ---
    // 由于 padded_n_info 是 4 的倍数，我们可以每 4 个 float 一组进行向量化
    for (uint32_t i = 0; i < ctx.padded_n_info; i += 4) {
        float32x4_t v_llr = vld1q_f32(&ctx.s_llr[i]);
        // 此处逻辑较复杂（包含 pow 和分支），通常在末尾 4-lane 展开即可提升缓存命中率
        // 对于 pow 计算，M4 编译器会自动进行自动向量化 (Auto-vectorization)
        for(int k=0; k<4; ++k) {
            uint32_t idx = i + k;
            if (idx >= ctx.n_info || ctx.s_evidence_cnt[idx] == 0) continue;
            float weight = std::pow((float)s_conflict_cnt[idx] / (float)ctx.degree_s[idx], power_val);
            float delta = ctx.eta * weight * (ctx.s_evidence_sum[idx] / ctx.s_evidence_cnt[idx]);
            ctx.s_llr[idx] += (ctx.s_llr[idx] >= 0) ? -delta : delta;
            
            // 同步更新位
            if (ctx.s_llr[idx] < 0.0f) ctx.s_hard[idx/32] |= (1U << (idx%32));
            else ctx.s_hard[idx/32] &= ~(1U << (idx%32));
        }
    }
}

void update_p_neon(DecoderContext& ctx, uint32_t top_n, float power_val) {
    std::memset(ctx.p_evidence_sum, 0, ctx.padded_n_parity * sizeof(float));
    std::memset(ctx.p_evidence_cnt, 0, ctx.padded_n_parity * sizeof(uint16_t));
    std::vector<uint32_t> p_conflict_cnt(ctx.padded_n_parity, 0); // 新增全局统计

    for (uint32_t i = 0; i < ctx.n_info; ++i) {
        uint8_t h_s = (ctx.s_hard[i / 32] >> (i % 32)) & 1U;
        if (ctx.s_pred[i] == h_s) continue; 

        float abs_s = std::abs(ctx.s_llr[i]);
        uint32_t found = 0;

        const uint32_t* row_ptr = &ctx.F_rows_packed[i * ctx.words_per_col_G];
        for (uint32_t w = 0; w < ctx.words_per_col_G; ++w) {
            uint32_t val = row_ptr[w];
            while (val) {
                uint32_t j = w * 32 + __builtin_ctz(val);
                if (j < ctx.n_parity) {
                    ctx.row_suspect_ids[found] = j;
                    ctx.row_suspect_abs[found] = std::abs(ctx.p_llr[j]);
                    // --- 全局统计：只要方程不满足就计数 ---
                    p_conflict_cnt[j]++;
                    found++;
                }
                val &= (val - 1);
            }
        }

        if (found == 0) continue;
        uint32_t actual_n = std::min(found, top_n);
        NeonHelper::sort_top3(ctx.row_suspect_ids, ctx.row_suspect_abs, found);

        float val1 = ctx.row_suspect_abs[0];
        float val2 = (found > 1) ? ctx.row_suspect_abs[1] : abs_s;

        for (uint32_t r = 0; r < actual_n; ++r) {
            uint32_t id = ctx.row_suspect_ids[r];
            float strength = (r == 0) ? std::min(val2, abs_s) : std::min(val1, abs_s);
            ctx.p_evidence_sum[id] += strength;
            ctx.p_evidence_cnt[id]++;
        }
    }

    // 批量修正 P 空间 LLR
    for (uint32_t j = 0; j < ctx.padded_n_parity; j += 4) {
        for(int k=0; k<4; ++k) {
            uint32_t idx = j + k;
            if (idx >= ctx.n_parity || ctx.p_evidence_cnt[idx] == 0) continue;
            float ratio = (float)p_conflict_cnt[idx] / (float)ctx.degree_p[idx];
            float weight = std::pow(ratio, power_val);
            float delta = ctx.eta * weight * (ctx.p_evidence_sum[idx] / ctx.p_evidence_cnt[idx]);
            
            if (ctx.p_llr[idx] >= 0.0f) ctx.p_llr[idx] -= delta;
            else ctx.p_llr[idx] += delta;

            if (ctx.p_llr[idx] < 0.0f) ctx.p_hard[idx / 32] |= (1U << (idx % 32));
            else ctx.p_hard[idx / 32] &= ~(1U << (idx % 32));
        }
    }
}

void final_decision_neon(DecoderContext& ctx) {
    auto compute_score = [&](float* llr_ptr, uint8_t* pred, uint32_t* hard, uint32_t n, uint32_t padded_n) {
        float32x4_t v_total_e = vdupq_n_f32(0.0f);
        float satisfied_e = 0.0f;

        for (uint32_t i = 0; i < padded_n; i += 4) {
            float32x4_t v_abs = vabsq_f32(vld1q_f32(llr_ptr + i));
            v_total_e = vaddq_f32(v_total_e, v_abs); 

            for (int k = 0; k < 4 && (i + k) < n; ++k) {
                uint8_t h = (hard[(i+k)/32] >> ((i+k)%32)) & 1U;
                if (pred[i+k] == h) {
                    float tmp[4]; vst1q_f32(tmp, v_abs);
                    satisfied_e += tmp[k];
                }
            }
        }
        return satisfied_e / (vaddvq_f32(v_total_e) + 1e-12f);
    };

    float s_score = compute_score(ctx.p_llr, ctx.p_pred, ctx.p_hard, ctx.n_parity, ctx.padded_n_parity);
    float p_score = compute_score(ctx.s_llr, ctx.s_pred, ctx.s_hard, ctx.n_info, ctx.padded_n_info);

    if (s_score * ctx.s_bias < p_score) {
        std::memset(ctx.s_hard, 0, ((ctx.n_info+31)/32)*4);
        for (uint32_t i = 0; i < ctx.n_info; ++i) {
            if (ctx.s_pred[i]) ctx.s_hard[i/32] |= (1U << (i%32));
        }
    }
}