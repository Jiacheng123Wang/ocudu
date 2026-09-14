// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Soft demapper (approximate LLR): one thread per modulation symbol, replicating the CPU
/// generic demodulation_mapper_impl bit-exactly for QPSK, 16/64/256 QAM. The scalar math,
/// the interval tables (pre-computed with the same float32 expressions as the CPU), the
/// per-component near-zero masking, the safe noise reciprocal and the mid-tread int8 LLR
/// quantization (round half to even, like the CPU NEON path) all mirror the CPU NEON
/// implementation; BPSK and pi/2-BPSK keep the CPU implementation.
///
/// Layout: llrs[symbol * B + bit] with B = 2/4/6/8, matching the CPU output order
/// (bit 0 = real-axis first interval, bit 1 = imag-axis first interval, and so on).
///
/// One dispatch covers one or more OFDM symbols: the grid is (modulation symbols) x (OFDM symbols)
/// and the parameters carry the stride of each array between two OFDM symbols, so the deferred
/// chain's page-aligned per-symbol slots are read without staging them first.

#include <metal_stdlib>
using namespace metal;

// Modulation ids (must match the host adapter).
constant uint MOD_QPSK   = 0;
constant uint MOD_QAM16  = 1;
constant uint MOD_QAM64  = 2;
constant uint MOD_QAM256 = 3;

struct demod_params {
    uint nof_symbols; // OFDM symbols covered by this dispatch (the grid's y dimension)
    uint nof_re;      // modulation symbols of one OFDM symbol (the grid's x dimension)
    uint mod;         // MOD_QPSK .. MOD_QAM256
    uint sym_stride;  // float2 elements between two consecutive OFDM symbols of symbols[]
    uint nv_stride;   // floats between two consecutive OFDM symbols of noise_var[]
    uint llr_stride;  // bytes between two consecutive OFDM symbols of llrs[]
};

// ---- Quantization: value -> int8 LLR (LLR_MAX = 120, ties to even, NaN -> 0) ----
constant float LLR_MAX_F = 120.0f;
constant float NEAR_ZERO = 1e-9f;

static inline char quantize_llr(float value, float range_limit)
{
    const float scaled = value * (LLR_MAX_F / range_limit);
    float       v      = clamp(scaled, -LLR_MAX_F, LLR_MAX_F);
    if (isnan(v)) {
        return 0;
    }
    return (char)rint(v);
}

// Safe noise reciprocal: 1 / noise_var when noise_var > 0 (NaN fails the compare), else 0.
static inline float rcp_noise_safe(float noise_var)
{
    return (noise_var > 0.0f) ? precise::divide(1.0f, noise_var) : 0.0f;
}

// ---- QPSK (range limit 24) ----
constant float GAIN_QPSK = 2.82842708f; // 2 * M_SQRT2f32

// ---- QAM16 (range limit 20) ----
constant float GAIN_FIRST_16 = 1.26491106f;  // 4 * M_SQRT1_10
constant float THR_16        = 0.632455528f; // 2 * M_SQRT1_10
constant float CONST_0_8     = 0.8f;

// ---- QAM64 tables (range limit 20) ----
constant float SLOPE_01_64[8] = {2.46885371f, 1.85164022f, 1.23442686f, 0.617213428f,
                                 0.617213428f, 1.23442686f, 1.85164022f, 2.46885371f};
constant float INTERCEPT_01_64[8] = {1.14285719f, 0.571428597f, 0.190476194f, 0.0f,
                                     0.0f, -0.190476194f, -0.571428597f, -1.14285719f};
constant float SLOPE_23_64[8] = {1.23442686f, 0.617213428f, 0.617213428f, 1.23442686f,
                                 -1.23442686f, -0.617213428f, -0.617213428f, -1.23442686f};
constant float INTERCEPT_23_64[8] = {0.952380955f, 0.380952388f, 0.380952388f, 0.571428597f,
                                     0.571428597f, 0.380952388f, 0.380952388f, 0.952380955f};
constant float SLOPE_45_64[8]      = {0.617213428f, -0.617213428f, 0.617213428f, -0.617213428f, 0.0f, 0.0f, 0.0f, 0.0f};
constant float INTERCEPT_45_64[8]  = {0.571428597f, -0.190476194f, -0.190476194f,
                                      0.571428597f, 0.0f, 0.0f, 0.0f, 0.0f};
constant float INV_W_2_64 = 3.24037027f; // 1 / (2 * M_SQRT1_42)
constant float INV_W_4_64 = 1.62018514f; // 1 / (4 * M_SQRT1_42)

// ---- QAM256 tables (range limit 20) ----
constant float SLOPE_01_256[16] = {2.45428801f, 2.14750195f, 1.840716f,   1.53393006f, 1.227144f,   0.920358002f,
                                   0.613572001f, 0.306786001f, 0.306786001f, 0.613572001f, 0.920358002f, 1.227144f,
                                   1.53393006f, 1.840716f,   2.14750195f, 2.45428801f};
constant float INTERCEPT_01_256[16] = {1.3176471f,   0.988235295f, 0.70588237f,  0.470588237f, 0.282352954f,
                                       0.141176477f, 0.0470588244f, 0.0f,         0.0f,         -0.0470588244f,
                                       -0.141176477f, -0.282352954f, -0.470588237f, -0.70588237f,  -0.988235295f,
                                       -1.3176471f};
constant float SLOPE_23_256[16] = {1.227144f,   0.920358002f, 0.613572001f, 0.306786001f, 0.306786001f, 0.613572001f,
                                   0.920358002f, 1.227144f,    -1.227144f,   -0.920358002f, -0.613572001f, -0.306786001f,
                                   -0.306786001f, -0.613572001f, -0.920358002f, -1.227144f};
constant float INTERCEPT_23_256[16] = {1.03529418f,  0.70588237f,  0.423529416f, 0.188235298f, 0.188235298f,
                                       0.329411775f, 0.423529416f, 0.470588237f, 0.470588237f, 0.423529416f,
                                       0.329411775f, 0.188235298f, 0.188235298f, 0.423529416f, 0.70588237f,
                                       1.03529418f};
constant float SLOPE_45_256[16] = {0.613572001f, 0.306786001f, 0.306786001f, 0.613572001f, -0.613572001f, -0.306786001f,
                                   -0.306786001f, -0.613572001f, 0.613572001f, 0.306786001f, 0.306786001f, 0.613572001f,
                                   -0.613572001f, -0.306786001f, -0.306786001f, -0.613572001f};
constant float INTERCEPT_45_256[16] = {0.611764729f,  0.282352954f,  0.282352954f,  0.517647088f,  -0.235294119f,
                                       -0.0941176489f, -0.0941176489f, -0.141176477f, -0.141176477f, -0.0941176489f,
                                       -0.0941176489f, -0.235294119f, 0.517647088f,  0.282352954f,  0.282352954f,
                                       0.611764729f};
constant float SLOPE_67_256[8] = {0.306786001f, -0.306786001f, 0.306786001f, -0.306786001f,
                                  0.306786001f, -0.306786001f, 0.306786001f, -0.306786001f};
constant float INTERCEPT_67_256[8] = {0.329411775f, -0.235294119f, 0.141176477f, -0.0470588244f,
                                      -0.0470588244f, 0.141176477f, -0.235294119f, 0.329411775f};
constant float INV_W_2_256 = 6.51920223f; // 1 / (2 * M_SQRT1_170)
constant float INV_W_4_256 = 3.25960112f; // 1 / (4 * M_SQRT1_170)

// Piecewise-linear interval function (CPU interval_function, NEON flavour: index computed
// as floor(value * precomputed_inv_width) + n/2, clamped; result zeroed near zero).
static inline float interval_l(float value, float rcp_noise, float inv_width, uint n, constant float* slope,
                               constant float* intercept)
{
    float idx_f = floor(value * inv_width) + (float)(n / 2u);
    int   idx   = isnan(idx_f) ? 0 : (int)idx_f;
    idx         = clamp(idx, 0, (int)n - 1);
    float l     = slope[idx] * value + intercept[idx];
    l *= rcp_noise;
    return (fabs(value) >= NEAR_ZERO) ? l : 0.0f;
}

// 16QAM bits 0/1 (thresholded linear piece) and 2/3 (absolute-value piece).
static inline float qam16_01(float x, float rcp_noise)
{
    const float first = GAIN_FIRST_16 * x;
    float       l     = (fabs(x) > THR_16) ? (2.0f * first - copysign(CONST_0_8, x)) : first;
    l *= rcp_noise;
    return (fabs(x) >= NEAR_ZERO) ? l : 0.0f;
}

static inline float qam16_23(float x, float rcp_noise)
{
    float l = CONST_0_8 - fabs(GAIN_FIRST_16 * x);
    l *= rcp_noise;
    return (fabs(x) >= NEAR_ZERO) ? l : 0.0f;
}

kernel void demod_soft(device const float2* symbols    [[buffer(0)]], // [OFDM symbol][modulation symbol]
                       device const float*  noise_var  [[buffer(1)]], // [OFDM symbol][modulation symbol]
                       device char*         llrs_base  [[buffer(2)]], // [OFDM symbol][modulation symbol][bit]
                       constant demod_params& p [[buffer(3)]],
                       uint2 pos [[thread_position_in_grid]])
{
    if ((pos.x >= p.nof_re) || (pos.y >= p.nof_symbols)) {
        return;
    }
    // One grid axis per dimension: pos.x walks the modulation symbols of one OFDM symbol and pos.y
    // the OFDM symbols covered by this dispatch. The strides make a whole run of symbols ONE
    // dispatch: the deferred chain's equalized symbols live in page-aligned per-symbol slots, so
    // the symbols of a group are a constant stride apart rather than adjacent. A single-symbol
    // dispatch passes stride 1 / 1 and its LLR count, which is exactly the packed layout this
    // kernel used before batching - the arithmetic per element is untouched.
    const uint   sym  = pos.x;
    const float2 z    = symbols[pos.y * p.sym_stride + sym];
    const float  rcp  = rcp_noise_safe(noise_var[pos.y * p.nv_stride + sym]);
    device char* llrs = llrs_base + pos.y * p.llr_stride;

    if (p.mod == MOD_QPSK) {
        const float l0 = (GAIN_QPSK * z.x) * rcp;
        const float l1 = (GAIN_QPSK * z.y) * rcp;
        llrs[2 * sym + 0] = quantize_llr(l0, 24.0f);
        llrs[2 * sym + 1] = quantize_llr(l1, 24.0f);
        return;
    }
    if (p.mod == MOD_QAM16) {
        llrs[4 * sym + 0] = quantize_llr(qam16_01(z.x, rcp), 20.0f);
        llrs[4 * sym + 1] = quantize_llr(qam16_01(z.y, rcp), 20.0f);
        llrs[4 * sym + 2] = quantize_llr(qam16_23(z.x, rcp), 20.0f);
        llrs[4 * sym + 3] = quantize_llr(qam16_23(z.y, rcp), 20.0f);
        return;
    }
    if (p.mod == MOD_QAM64) {
        llrs[6 * sym + 0] =
            quantize_llr(interval_l(z.x, rcp, INV_W_2_64, 8, SLOPE_01_64, INTERCEPT_01_64), 20.0f);
        llrs[6 * sym + 1] =
            quantize_llr(interval_l(z.y, rcp, INV_W_2_64, 8, SLOPE_01_64, INTERCEPT_01_64), 20.0f);
        llrs[6 * sym + 2] =
            quantize_llr(interval_l(z.x, rcp, INV_W_2_64, 8, SLOPE_23_64, INTERCEPT_23_64), 20.0f);
        llrs[6 * sym + 3] =
            quantize_llr(interval_l(z.y, rcp, INV_W_2_64, 8, SLOPE_23_64, INTERCEPT_23_64), 20.0f);
        llrs[6 * sym + 4] =
            quantize_llr(interval_l(z.x, rcp, INV_W_4_64, 4, SLOPE_45_64, INTERCEPT_45_64), 20.0f);
        llrs[6 * sym + 5] =
            quantize_llr(interval_l(z.y, rcp, INV_W_4_64, 4, SLOPE_45_64, INTERCEPT_45_64), 20.0f);
        return;
    }
    if (p.mod == MOD_QAM256) {
        llrs[8 * sym + 0] =
            quantize_llr(interval_l(z.x, rcp, INV_W_2_256, 16, SLOPE_01_256, INTERCEPT_01_256), 20.0f);
        llrs[8 * sym + 1] =
            quantize_llr(interval_l(z.y, rcp, INV_W_2_256, 16, SLOPE_01_256, INTERCEPT_01_256), 20.0f);
        llrs[8 * sym + 2] =
            quantize_llr(interval_l(z.x, rcp, INV_W_2_256, 16, SLOPE_23_256, INTERCEPT_23_256), 20.0f);
        llrs[8 * sym + 3] =
            quantize_llr(interval_l(z.y, rcp, INV_W_2_256, 16, SLOPE_23_256, INTERCEPT_23_256), 20.0f);
        llrs[8 * sym + 4] =
            quantize_llr(interval_l(z.x, rcp, INV_W_2_256, 16, SLOPE_45_256, INTERCEPT_45_256), 20.0f);
        llrs[8 * sym + 5] =
            quantize_llr(interval_l(z.y, rcp, INV_W_2_256, 16, SLOPE_45_256, INTERCEPT_45_256), 20.0f);
        llrs[8 * sym + 6] =
            quantize_llr(interval_l(z.x, rcp, INV_W_4_256, 8, SLOPE_67_256, INTERCEPT_67_256), 20.0f);
        llrs[8 * sym + 7] =
            quantize_llr(interval_l(z.y, rcp, INV_W_4_256, 8, SLOPE_67_256, INTERCEPT_67_256), 20.0f);
        return;
    }
}
