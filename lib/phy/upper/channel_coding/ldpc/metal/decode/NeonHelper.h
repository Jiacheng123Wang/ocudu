#ifndef NEON_HELPER_H
#define NEON_HELPER_H

#include <arm_neon.h>
#include <utility> // for std::swap
#include <cstdint>

namespace NeonHelper {
    // 快速计算 4 个 float 的绝对值
    inline float32x4_t abs_v(float* ptr) {
        return vabsq_f32(vld1q_f32(ptr));
    }

    // 针对 Top-3 优化的极简排序
    inline void sort_top3(uint32_t* ids, float* vals, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) {
            for (uint32_t j = i + 1; j < count; ++j) {
                if (vals[j] < vals[i]) {
                    std::swap(vals[i], vals[j]);
                    std::swap(ids[i], ids[j]);
                }
            }
        }
    }

    // NEON 加速绝对值累加 (用于能量比例)
    inline float sum_abs_neon(const float* data, uint32_t len) {
        float32x4_t v_sum = vdupq_n_f32(0.0f);
        for (uint32_t i = 0; i < len; i += 4) {
            v_sum = vaddq_f32(v_sum, vabsq_f32(vld1q_f32(data + i)));
        }
        return vaddvq_f32(v_sum); // ARMv8 原生横向求和
    }
}

#endif // NEON_HELPER_H