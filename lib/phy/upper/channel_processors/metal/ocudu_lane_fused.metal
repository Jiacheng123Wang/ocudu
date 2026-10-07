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
// SCOPE: the three modulations the PUSCH actually carries, each branch copied verbatim from the
// demapper. 16QAM was the first version's scope because the judging legs were believed to be
// MCS 13 = 16QAM; the first pair measured that they are 64QAM (the PUSCH runs MCS table 2, where
// index 13 is 64QAM - see the implementation doc's memo), so 64QAM was added. QPSK is the last one
// the scheduler reaches in practice: with the MCS pin removed (the AMC arm) the link adaptation
// walks QPSK/16QAM/64QAM, and without this branch every cell-edge hop fell back to the two-stage
// route. What is left out - 256QAM, and any topology with more than one layer - stays on that route,
// which the caller selects per hop (see channel_equalizer::supports_fused_demapping()).
//
// WHY THE HELPERS BELOW ARE COPIES. Factoring them into a shared header would rewrite the two kernels that are
// part of the shipped delivery shape, and this workstream's rules require its own A/B for that. A copy inside a
// route that is off by default cannot change what ships, so the extraction is deferred and recorded as a
// follow-up. The copies must stay verbatim - the fused route is judged by CRC and by the pipeline readings, not
// by bit-exactness (user's ruling), but "the same computation" is what makes those readings mean anything.

#include <metal_stdlib>
using namespace metal;

// ---- verbatim from ocudu_demod.metal ---------------------------------------------------------------
// Modulation ids: the SAME numbering the demapper engine passes to demod_soft (see its run_demodulate).
constant uint MOD_QPSK  = 0;
constant uint MOD_QAM16 = 1;
constant uint MOD_QAM64 = 2;

constant float LLR_MAX_F = 120.0f;
constant float NEAR_ZERO = 1e-9f;

// ---- QPSK (range limit 24: see its branch below, which is NOT 20 like the QAM ones) ----
constant float GAIN_QPSK = 2.82842708f; // 2 * M_SQRT2f32

constant float GAIN_FIRST_16 = 1.26491106f;  // 4 * M_SQRT1_10
constant float THR_16        = 0.632455528f; // 2 * M_SQRT1_10
constant float CONST_0_8     = 0.8f;

constant float SLOPE_01_64[8] = {2.46885371f, 1.85164022f, 1.23442686f, 0.617213428f,
                                 0.617213428f, 1.23442686f, 1.85164022f, 2.46885371f};
constant float INTERCEPT_01_64[8] = {1.14285719f, 0.571428597f, 0.190476194f, 0.0f,
                                     0.0f, -0.190476194f, -0.571428597f, -1.14285719f};
constant float SLOPE_23_64[8] = {1.23442686f, 0.617213428f, 0.617213428f, 1.23442686f,
                                 -1.23442686f, -0.617213428f, -0.617213428f, -1.23442686f};
constant float INTERCEPT_23_64[8] = {0.952380955f, 0.380952388f, 0.380952388f, 0.571428597f,
                                     0.571428597f, 0.380952388f, 0.380952388f, 0.952380955f};
constant float SLOPE_45_64[8]     = {0.617213428f, -0.617213428f, 0.617213428f, -0.617213428f, 0.0f, 0.0f, 0.0f, 0.0f};
constant float INTERCEPT_45_64[8] = {0.571428597f, -0.190476194f, -0.190476194f,
                                     0.571428597f, 0.0f, 0.0f, 0.0f, 0.0f};
constant float INV_W_2_64 = 3.24037027f; // 1 / (2 * M_SQRT1_42)
constant float INV_W_4_64 = 1.62018514f; // 1 / (4 * M_SQRT1_42)

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

// MUST match ocudu_equalizer.metal: the strides block is passed as raw bytes and its size is part of
// its layout, so a different table length here reads y_starts from the wrong offset - not a compile
// error, a silently wrong resource element. The C++ side asserts the same number (eq_strides_t).
constant uint eq_max_run_symbols = 14; // MAX_NSYMB_PER_SLOT

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
                             device float*       nv [[buffer(7)]], // [symbol][re] (single layer)
                             constant uint&      mod [[buffer(8)]], // MOD_QAM16 / MOD_QAM64
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

    // The post-equalization noise variance of this resource element, written where the equalizer would
    // have written it (nv[symbol * nv_stride + re], the single-layer layout of the batch kernel).
    //
    // WHY IT IS WRITTEN AT ALL, given that keeping it in registers is the point of the fusion: the
    // PUSCH demodulator's DEFAULT SINR method (`pusch_sinr_calc_method: post_equalization`) reduces
    // this array on the host after the group wait, and the value it reports feeds the uplink link
    // adaptation. A route that only produced LLRs would have to either refuse that configuration -
    // i.e. never run in the shipped one - or report an infinite SINR, which is worse than a slower
    // hop. The store is 4 of the 24 bytes per resource element the fusion removes (the equalized
    // symbol's write AND read, the noise variance's read, plus the second dispatch), so what the
    // fusion is being judged on is untouched; an arm that drops it is a follow-up A/B, not a default.
    nv[sym * st.nv_stride + re] = nvar;

    // ---- soft demodulation (copied from demod_soft's branches) --------------------------------------
    const float  rcp  = rcp_noise_safe(nvar);
    device char* llrs = llrs_base + sym * llr_stride;

    if (mod == MOD_QPSK) {
        // Two soft bits per resource element, one per component, and a RANGE LIMIT OF 24 rather than
        // the QAM branches' 20 (the demapper scales the two constellations differently).
        const float l0 = (GAIN_QPSK * x_hat.x) * rcp;
        const float l1 = (GAIN_QPSK * x_hat.y) * rcp;
        llrs[2 * re + 0] = quantize_llr(l0, 24.0f);
        llrs[2 * re + 1] = quantize_llr(l1, 24.0f);
        return;
    }
    if (mod == MOD_QAM16) {
        llrs[4 * re + 0] = quantize_llr(qam16_01(x_hat.x, rcp), 20.0f);
        llrs[4 * re + 1] = quantize_llr(qam16_01(x_hat.y, rcp), 20.0f);
        llrs[4 * re + 2] = quantize_llr(qam16_23(x_hat.x, rcp), 20.0f);
        llrs[4 * re + 3] = quantize_llr(qam16_23(x_hat.y, rcp), 20.0f);
        return;
    }
    if (mod == MOD_QAM64) {
        llrs[6 * re + 0] =
            quantize_llr(interval_l(x_hat.x, rcp, INV_W_2_64, 8, SLOPE_01_64, INTERCEPT_01_64), 20.0f);
        llrs[6 * re + 1] =
            quantize_llr(interval_l(x_hat.y, rcp, INV_W_2_64, 8, SLOPE_01_64, INTERCEPT_01_64), 20.0f);
        llrs[6 * re + 2] =
            quantize_llr(interval_l(x_hat.x, rcp, INV_W_2_64, 8, SLOPE_23_64, INTERCEPT_23_64), 20.0f);
        llrs[6 * re + 3] =
            quantize_llr(interval_l(x_hat.y, rcp, INV_W_2_64, 8, SLOPE_23_64, INTERCEPT_23_64), 20.0f);
        llrs[6 * re + 4] =
            quantize_llr(interval_l(x_hat.x, rcp, INV_W_4_64, 4, SLOPE_45_64, INTERCEPT_45_64), 20.0f);
        llrs[6 * re + 5] =
            quantize_llr(interval_l(x_hat.y, rcp, INV_W_4_64, 4, SLOPE_45_64, INTERCEPT_45_64), 20.0f);
        return;
    }
    // No other modulation reaches this kernel: the host refuses the route for them (see
    // channel_equalizer::supports_fused_demapping()) and the demapper is not called for these symbols.
    // The fall-through is therefore unreachable, and it is left explicit rather than silent for the
    // same reason demod_soft's own if-chain is: whoever adds a modulation has to add its branch, and
    // the host side (the predicate AND the run's bits-per-RE accounting) has to learn it too.
}
