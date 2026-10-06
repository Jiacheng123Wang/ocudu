// LANE GRID -> LLR: the fused equalizer + demapper kernel of the metal_kernel_fusion workstream (M1).
//
// WHAT IT REPLACES (plan doc §2, M1.0 §2.6). The fused lane runs, per hop, two kernels in two dispatches of one
// command buffer: `equalize_mxn_batch` (grid + weights -> equalized symbols eq[] and per-RE noise nv[]) followed
// by `demod_soft` (eq[] + nv[] -> int8 LLRs). This kernel does both in one dispatch with the intermediate values
// in REGISTERS: nothing but the LLRs leaves the kernel, so the eq/nv buffers and the page-aligned per-symbol
// slots they live in are not touched at all.
//
// WHY IT CAN BE A LINE-BY-LINE MERGE. The two kernels' grids already agree: `equalize_mxn_batch` uses
// `uint2 gid` with gid.x = resource element and gid.y = OFDM symbol, and `demod_soft` uses `uint2 pos` with the
// same meaning; both address a run of symbols through per-symbol stride tables for the same reason (the deferred
// chain's slots). So the merged kernel keeps the batch equalizer's indexing (`st.h_starts[]`, `st.y_starts[]`,
// `p.h_offset`) and the demapper's output addressing (`llr_stride`), and the per-element math is copied verbatim
// from the two originals.
//
// SCOPE (M1.1 decision, plan doc §2.8): MOD_QAM16 ONLY. Every judging leg runs MCS 13 (16QAM), so the first
// version covers exactly the modulation the flights exercise and every line of it is covered by a leg; the other
// modulations stay on the existing two-stage route, which the caller selects. Extending it means adding branches
// the same way the demapper has them.
//
// WHY THE HELPERS BELOW ARE COPIES. Factoring them into a shared header would rewrite the two kernels that are
// part of the shipped delivery shape, and this workstream's rules require its own A/B for that. A copy inside a
// route that is off by default cannot change what ships, so the extraction is deferred and recorded as a
// follow-up. The copies must stay verbatim - the fused route is judged by CRC and by the pipeline readings, not
// by bit-exactness (user's ruling), but "the same computation" is what makes those readings mean anything.

#include <metal_stdlib>
using namespace metal;

// ---- verbatim from ocudu_demod.metal ---------------------------------------------------------------
constant float LLR_MAX_F = 120.0f;
constant float NEAR_ZERO = 1e-9f;

constant float GAIN_FIRST_16 = 1.26491106f;  // 4 * M_SQRT1_10
constant float THR_16        = 0.632455528f; // 2 * M_SQRT1_10
constant float CONST_0_8     = 0.8f;

static inline char quantize_llr(float value, float range_limit)
{
    const float scaled = value * (LLR_MAX_F / range_limit);
    float       v      = clamp(scaled, -LLR_MAX_F, LLR_MAX_F);
    if (isnan(v)) {
        return 0;
    }
    // (the original's rounding: ties to even, then a saturating cast - see ocudu_demod.metal)
    const float rounded = rint(v);
    return static_cast<char>(clamp(rounded, -LLR_MAX_F, LLR_MAX_F));
}

static inline float rcp_noise_safe(float noise_var)
{
    return (noise_var > 0.0f) ? precise::divide(1.0f, noise_var) : 0.0f;
}

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

// ---- verbatim from ocudu_equalizer.metal -----------------------------------------------------------
#define MAX_PORTS 8

constant uint eq_max_run_symbols = 32; // must match the equalizer's table size

struct equalize_params {
    uint  nof_re;
    uint  nof_ports;
    uint  nof_layers;
    uint  algo;
    float noise_var;
    float tx_scaling;
    float h_scaling;
    uint  h_offset;
    uint  h_layer_stride;
};

struct equalize_strides {
    uint nof_symbols;
    uint h_stride;
    uint eq_stride;
    uint nv_stride;
    uint h_starts[eq_max_run_symbols];
    uint y_starts[eq_max_run_symbols];
};

/// bf16 (cbf16_t) widening: the value is the upper half of the IEEE-754 single.
static inline float bf16_to_f(ushort v)
{
    return as_type<float>((uint)v << 16);
}

static inline float2 load_cbf16(device const ushort2* p, uint idx)
{
    const ushort2 v = p[idx];
    return float2(bf16_to_f(v.x), bf16_to_f(v.y));
}

static inline float2 load_h(device const ushort2* h, constant equalize_params& p, uint port, uint layer, uint re)
{
    return load_cbf16(h, p.h_offset + (port * p.nof_layers + layer) * p.h_layer_stride + re) * p.h_scaling;
}

static inline float2 cmul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}

// ---- the fused kernel -----------------------------------------------------------------------------
kernel void lane_grid_to_llr(device const ushort2* h [[buffer(0)]],  // cbf16 [symbol][port][layer][re]
                             device const ushort2* y [[buffer(1)]],  // cbf16 [symbol][port][re]
                             device const float* sigma2 [[buffer(2)]], // [port]
                             device char*        llrs_base [[buffer(3)]], // [symbol][re][bit]
                             constant equalize_params&  p [[buffer(4)]],
                             constant equalize_strides& st [[buffer(5)]],
                             constant uint&      llr_stride [[buffer(6)]], // bytes between two OFDM symbols
                             uint2               gid [[thread_position_in_grid]])
{
    const uint re  = gid.x;
    const uint sym = gid.y;
    if ((re >= p.nof_re) || (sym >= st.nof_symbols)) {
        return;
    }
    // The per-symbol starts, exactly as equalize_mxn_batch computes them (h absolute, y relative to the binding).
    h += st.h_starts[min(sym, eq_max_run_symbols - 1u)] - p.h_offset;
    y += st.y_starts[min(sym, eq_max_run_symbols - 1u)];

    const uint P = p.nof_ports;

    // ---- single Tx layer: the 1 x P SIMO combiner (copied from the equalizer's L == 1 branch) --------
    float  ch_mod_sq = 0.0f;
    float  nvar_acc  = 0.0f;
    float2 x_hat     = float2(0.0f);
    for (uint port = 0; port != P; ++port) {
        const float2 hv     = load_h(h, p, port, 0, re);
        const float  nrm    = hv.x * hv.x + hv.y * hv.y;
        const float  nv_port = sigma2[port];
        if ((nrm < INFINITY) && (nv_port > 0.0f) && (nv_port < INFINITY)) {
            ch_mod_sq += nrm;
            nvar_acc += nrm * nv_port;
            const float2 yv = load_cbf16(y, port * p.nof_re + re);
            x_hat += cmul(yv, float2(hv.x, -hv.y));
        }
    }
    float nvar = INFINITY;
    const float d = p.tx_scaling * ch_mod_sq;
    if ((d > 0.0f) && !isinf(d) && !isnan(d)) {
        const float rcp_eq = 1.0f / d;
        x_hat *= rcp_eq;
        nvar = nvar_acc * (rcp_eq * rcp_eq);
    } else {
        // The equalizer's degenerate branch: eq = 0, nv = INFINITY. Kept so the LLRs come out the same way
        // (the zero rcp below then zeroes every LLR through the NEAR_ZERO guards).
        x_hat = float2(0.0f);
        nvar  = INFINITY;
    }

    // ---- 16QAM soft demodulation (copied from demod_soft's MOD_QAM16 branch) ------------------------
    const float  rcp  = rcp_noise_safe(nvar);
    device char* llrs = llrs_base + sym * llr_stride;
    llrs[4 * re + 0] = quantize_llr(qam16_01(x_hat.x, rcp), 20.0f);
    llrs[4 * re + 1] = quantize_llr(qam16_01(x_hat.y, rcp), 20.0f);
    llrs[4 * re + 2] = quantize_llr(qam16_23(x_hat.x, rcp), 20.0f);
    llrs[4 * re + 3] = quantize_llr(qam16_23(x_hat.y, rcp), 20.0f);
}
