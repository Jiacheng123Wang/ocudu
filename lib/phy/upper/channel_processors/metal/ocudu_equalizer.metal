// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Channel equalizer: one thread per resource element, scalar per-RE math replicating the
/// CPU generic ZF/MMSE implementation.
///
/// Two topologies are covered:
/// - single Tx layer (1 x P SIMO, equalize_zf_1xn): per-port noise-variance validity is
///   resolved by the host (ports with non-positive or non-finite noise variance are dropped
///   before the call, like the CPU reduction) and the kernel combines the remaining ports.
///   Channel estimates are passed UNSCALED - tx_scaling enters the denominator, as in the
///   CPU 1 x n path. Both ZF and MMSE use this path (for a single layer the two algorithms
///   are equivalent once the LLR scaling is included).
/// - 2..4 Tx layers x 2/4/8 Rx ports: Gram matrix inversion plus the matched filter.
///
/// Both input grids arrive as raw bf16 pairs (cbf16_t, 4 bytes per complex sample) and are
/// widened in the kernel, so the host never converts them and only copies half the bytes.

#include <metal_stdlib>
using namespace metal;

constant uint MAX_LAYERS = 4;
constant uint MAX_PORTS  = 8;

struct equalize_params {
    uint  nof_re;       // resource elements
    uint  nof_ports;    // receive ports (1..8; 2/4/8 on the multi-layer path)
    uint  nof_layers;   // transmit layers (1..4, <= nof_ports)
    uint  algo;         // 0 = ZF, 1 = MMSE (the single-layer path is algorithm-independent)
    float noise_var;    // noise variance estimate (max across ports, multi-layer path)
    float tx_scaling;   // single-layer path: folded into the pseudo-inverse denominator
    float h_scaling;    // multi-layer path: scales the channel estimates (1 on the single-layer path)
    uint  h_offset;     // first channel estimate of the dispatch, in cbf16_t elements
    uint  h_layer_stride; // elements between two consecutive transmission layers
};

// bf16 (cbf16_t) widening: the value is the upper half of the IEEE-754 single, so the
// conversion is a 16-bit left shift (identical to the CPU to_float(bf16_t)).
static inline float bf16_to_f(ushort v)
{
    return as_type<float>((uint)v << 16);
}

static inline float2 load_cbf16(device const ushort2* p, uint idx)
{
    const ushort2 v = p[idx];
    return float2(bf16_to_f(v.x), bf16_to_f(v.y));
}

// Channel estimate of one port and layer. The caller either staged the estimates packed as
// [port][layer][re] (offset 0, layer stride nof_re) or bound the buffer they were produced in - the
// channel estimator's device output, whose layers are total_re apart and whose symbol starts at an
// offset inside it - so the layout travels in the parameters instead of being copied into one.
static inline float2 load_h(device const ushort2* h, constant equalize_params& p, uint port, uint layer, uint re)
{
    return load_cbf16(h, p.h_offset + (port * p.nof_layers + layer) * p.h_layer_stride + re) * p.h_scaling;
}

// Complex multiply / multiply-conjugate helpers.
static inline float2 cmul(float2 a, float2 b)
{
    return float2(a.x * b.x - a.y * b.y, a.x * b.y + a.y * b.x);
}
static inline float2 conjmul(float2 a, float2 b) // conj(a) * b
{
    return float2(a.x * b.x + a.y * b.y, a.x * b.y - a.y * b.x);
}

// In-place inversion mirroring the CPU squared_matrix_inverse recursion (NO row pivoting,
// identical operation order): the left half of the augmented matrix is consumed (row ops) and
// the right half becomes A^-1. Row stride is fixed at 2 * MAX_LAYERS. Returns 0 on success, the
// 1-based failing column index on a zero/non-finite pivot. n = 2..4.
static uint invert_aug(thread float2* a, uint n)
{
    const uint stride = 2 * MAX_LAYERS;
    for (uint col = 0; col != n; ++col) {
        // Pivot: the current diagonal element of the (partially reduced) left half.
        const float2 pivot     = a[col * stride + col];
        const float  pivot_nrm = pivot.x * pivot.x + pivot.y * pivot.y;
        if (!(pivot_nrm > 0.0f) || isinf(pivot_nrm) || isnan(pivot_nrm)) {
            return col + 1;
        }
        // pivot_inv = conj(pivot) / |pivot|^2 (plain division: MSL emits an rcp + Newton
        // refinement, the closest analogue to the CPU's Newton-refined reciprocal).
        const float  rcp       = 1.0f / pivot_nrm;
        const float2 pivot_inv = float2(pivot.x * rcp, -pivot.y * rcp);

        // Normalize the pivot row: right half entries j <= col (identity entry j == col
        // included), left half entries j > col. The left diagonal keeps the raw pivot (the
        // CPU consumes it untouched in the elimination below).
        a[col * stride + col + n] = float2(1.0f, 0.0f);
        for (uint j = col + 1; j != n; ++j) {
            a[col * stride + j] = cmul(a[col * stride + j], pivot_inv);
        }
        for (uint j = 0; j != col + 1; ++j) {
            a[col * stride + n + j] = cmul(a[col * stride + n + j], pivot_inv);
        }

        // Make every other row zero in the current column (left half), applying the same
        // operations to the right half.
        for (uint k = 0; k != n; ++k) {
            if (k == col) {
                continue;
            }
            const float2 factor = a[k * stride + col];
            for (uint j = col; j != n; ++j) {
                a[k * stride + j] -= cmul(factor, a[col * stride + j]);
            }
            for (uint j = 0; j != col; ++j) {
                a[k * stride + n + j] -= cmul(factor, a[col * stride + n + j]);
            }
            a[k * stride + n + col] = -cmul(factor, a[col * stride + n + col]);
        }
    }
    return 0;
}

kernel void equalize_mxn(device const ushort2* h [[buffer(0)]], // cbf16 [port][layer][re]
                         device const ushort2* y [[buffer(1)]], // cbf16 [port][re]
                         device float2*       eq  [[buffer(2)]], // [re][layer] interleaved
                         device float*        nv  [[buffer(3)]], // [re][layer]
                         constant equalize_params& p [[buffer(4)]],
                         device const float* sigma2 [[buffer(5)]], // [port] (single-layer path)
                         uint re [[thread_position_in_grid]])
{
    if (re >= p.nof_re) {
        return;
    }
    const uint L = p.nof_layers;
    const uint P = p.nof_ports;

    float2 H[MAX_PORTS][MAX_LAYERS];
    for (uint port = 0; port != P; ++port) {
        for (uint layer = 0; layer != L; ++layer) {
            H[port][layer] = load_h(h, p, port, layer, re);
        }
    }

    // ---- Single Tx layer: 1 x P SIMO combiner (CPU equalize_zf_1xn) ----
    if (L == 1u) {
        float  ch_mod_sq = 0.0f; // sum of |h|^2 over the valid ports
        float  nvar_acc  = 0.0f; // sum of |h|^2 * noise_var over the valid ports
        float2 re_out    = float2(0.0f);
        for (uint port = 0; port != P; ++port) {
            const float2 hv  = H[port][0];
            const float  nrm     = hv.x * hv.x + hv.y * hv.y;
            const float  nv_port = sigma2[port];
            // CPU per-port mask: the port takes part only when its channel square norm is finite
            // (NaN fails the compare too) and its noise variance is positive and finite. The host
            // applies the same predicate to compact the ports when it owns the variances; a backend
            // that hands the kernel the device ones lets it apply the predicate here. When no port
            // passes, ch_mod_sq stays zero and the output takes the invalid branch below, which is
            // exactly what the host produces for an ill-formed noise variance.
            if ((nrm < INFINITY) && (nv_port > 0.0f) && (nv_port < INFINITY)) {
                ch_mod_sq += nrm;
                nvar_acc += nrm * nv_port;
                // Matched filter: conjprod(re_in, ch_est) = re_in * conj(ch_est).
                const float2 yv = load_cbf16(y, port * p.nof_re + re);
                re_out += cmul(yv, float2(hv.x, -hv.y));
            }
        }

        // Denominator of the pseudo-inverse (tx_scaling is NOT folded into H here).
        const float d = p.tx_scaling * ch_mod_sq;
        // CPU validity: (d > 0) && (infinity > d), i.e., strictly positive and finite.
        if ((d > 0.0f) && !isinf(d) && !isnan(d)) {
            const float rcp = 1.0f / d;
            eq[re] = re_out * rcp;
            nv[re] = nvar_acc * (rcp * rcp);
        } else {
            eq[re] = 0;
            nv[re] = INFINITY;
        }
        return;
    }

    // Gram matrix G[i][j] = sum_p h[p][i] * conj(h[p][j]). Every entry (including the lower
    // triangle) is accumulated independently with its own rounding, exactly like the CPU
    // squared_gram_matrix, so that the downstream inverse stays numerically aligned with it.
    float2 G[MAX_LAYERS][MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            float2 acc = float2(0.0f);
            for (uint port = 0; port != P; ++port) {
                acc += conjmul(H[port][i], H[port][j]);
            }
            G[i][j] = acc;
        }
    }

    bool diag_ok = true;
    if (p.algo == 1u) {
        // MMSE: the CPU marks the result invalid when any Gram diagonal is not > 0 BEFORE
        // adding the noise term; the noise is added to the diagonal afterwards.
        for (uint i = 0; i != L; ++i) {
            diag_ok = diag_ok && (G[i][i].x > 0.0f);
        }
        for (uint i = 0; i != L; ++i) {
            G[i][i].x += p.noise_var;
        }
    }

    // Invert the Gram matrix via the augmented [G | I] Gauss-Jordan; Gi = right half.
    float2 aug[MAX_LAYERS][2 * MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            aug[i][j]     = G[i][j];
            aug[i][j + L] = (i == j) ? float2(1.0f, 0.0f) : float2(0.0f);
        }
    }
    const uint ok = invert_aug(&aug[0][0], L);
    float2    Gi[MAX_LAYERS][MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            Gi[i][j] = aug[i][j + L];
        }
    }

    if (ok != 0u || !diag_ok) {
        // Invalid (singular Gram matrix / ill-formed diagonal): zero symbols, infinite noise
        // variances, matching the CPU semantics.
        for (uint layer = 0; layer != L; ++layer) {
            eq[re * L + layer] = 0;
            nv[re * L + layer] = INFINITY;
        }
        return;
    }

    // W = Gi * H^H: the CPU chain reads conjprod(a, b) = a * conj(b), so with the Hermitian
    // Gram inverse Gi, W[port][layer] = sum_k conj(H[port][k]) * Gi[layer][k] (the textbook
    // matched filter; equivalently W[port][layer] = sum_k Gi[layer][k] * conj(H[port][k])).
    float2 W[MAX_PORTS][MAX_LAYERS];
    for (uint layer = 0; layer != L; ++layer) {
        for (uint port = 0; port != P; ++port) {
            float2 acc = float2(0.0f);
            for (uint k = 0; k != L; ++k) {
                acc += conjmul(H[port][k], Gi[layer][k]);
            }
            W[port][layer] = acc;
        }
    }

    for (uint layer = 0; layer != L; ++layer) {
        float2 eq_acc  = 0;
        float  corr    = 0.0f;
        for (uint port = 0; port != P; ++port) {
            const float2 yv = load_cbf16(y, port * p.nof_re + re);
            eq_acc += cmul(W[port][layer], yv);
            if (p.algo == 1u) {
                const float2 wh = cmul(W[port][layer], H[port][layer]);
                corr += wh.x;
            }
        }
        if (p.algo == 0u) {
            eq[re * L + layer] = eq_acc;
            nv[re * L + layer] = Gi[layer][layer].x * p.noise_var;
        } else {
            // MMSE LLR rescaling (CPU: correction = 1 / real(W . H); eq *= correction;
            // noise_var = correction - 1).
            const float c = 1.0f / corr;
            eq[re * L + layer] = eq_acc * c;
            nv[re * L + layer] = c - 1.0f;
        }
    }
}

/// \brief Per-symbol strides of a batched dispatch, in elements of the bound buffers.
struct equalize_strides {
    uint nof_symbols;
    uint h_stride;   // cbf16 elements per symbol
    uint y_stride;   // cbf16 elements per symbol
    uint eq_stride;  // float2 elements per symbol
    uint nv_stride;  // float elements per symbol
};

/// \brief Where one OFDM symbol of the HOP starts in the gather plan.
struct gather_tap {
    uint symbol; // OFDM symbol index in the grid
    uint offset; // first gather entry of the symbol, within the plan
    uint nof_re; // gather entries of the symbol, i.e. its number of resource elements
};

/// \brief Gather plan of the received symbols: where the equalizer's y input comes from.
///
/// The host used to gather y itself, one memcpy per port per symbol, because the resource elements
/// the equalizer consumes are not a contiguous run of the grid: the DM-RS comb and the per-PRB
/// active pattern leave holes. The plan describes those elements in the grid's own coordinates, one
/// entry per element (the subcarrier it comes from, the index it goes to), so the very same bytes
/// are produced on the device - a pure transport, no conversion, hence bit-identical to the host
/// gather.
///
/// One thread per (resource element, OFDM symbol of the run): the entries of a run were emitted
/// symbol by symbol, so a thread reads its own entry and copies it for every port - the same element
/// of every port, which is what the host's per-port memcpy did.
///
/// \note The tap table covers the WHOLE hop, not the run: the entries of a symbol do not depend on
///       which symbols a dispatch happens to carry, so one table serves every dispatch of the hop
///       and only the entry point moves (\c first_symbol). That is what keeps the table out of the
///       per-run cost - building and uploading it per run is tens of microseconds per slot.
struct gather_params {
    ulong grid_base;        // byte offset of grid element 0 of port 0, symbol 0
    uint  nof_symbols;      // OFDM symbols of the run
    uint  nof_ports;        // receive ports to gather (the run is [port][re] per symbol)
    uint  nof_dest;         // cbf16_t per port run (the dispatch's nof_re)
    uint  grid_subc_stride; // elements between two consecutive subcarriers
    uint  grid_symb_stride; // elements between two consecutive OFDM symbols
    uint  grid_port_stride; // elements between two consecutive ports
    uint  first_symbol;     // first symbol of the run within the hop's tap table
};

/// One entry of a gather plan: the grid subcarrier of a resource element and where it goes.
struct gather_entry {
    uint subc; // grid subcarrier index, within the symbol's row of the grid
    uint dest; // index within the symbol's output region
};

kernel void gather_ch_re(device const ushort2* grid [[buffer(0)]], // resource grid, element 0 of port 0, symbol 0
                         device ushort2*       y [[buffer(1)]],    // [symbol][port][re] (cbf16)
                         constant gather_params& p [[buffer(2)]],
                         device const gather_entry* table [[buffer(3)]],
                         constant gather_tap*  taps [[buffer(4)]],
                         uint2 gid [[thread_position_in_grid]])
{
    const uint dest_idx = gid.x;
    const uint sym      = gid.y;
    if (sym >= p.nof_symbols) {
        return;
    }
    // The hop's tap table, reached at the symbol this run starts at: a run is a contiguous window of
    // the hop, so its own symbols are the next nof_symbols taps.
    const gather_tap tap = taps[p.first_symbol + sym];
    if (dest_idx >= tap.nof_re) {
        return;
    }
    const gather_entry e = table[tap.offset + dest_idx];

    // The element of the grid this entry names, then the same element of every port: y is the run's
    // region, so its per-symbol stride is nof_ports * nof_dest elements.
    device const ushort2* src = (device const ushort2*)((device const char*)grid + p.grid_base) +
                                tap.symbol * p.grid_symb_stride + p.grid_subc_stride * e.subc;
    device ushort2* dst = y + (sym * p.nof_ports) * p.nof_dest + e.dest;
    for (uint port = 0; port != p.nof_ports; ++port) {
        dst[port * p.nof_dest] = src[port * p.grid_port_stride];
    }
}

/// \brief Batched equalizer: the SAME arithmetic as equalize_mxn(), one thread per (resource
/// element, OFDM symbol) instead of one dispatch per symbol.
///
/// The stages of a deferred group submit one dispatch per symbol today, and a dispatch costs about
/// 10 us while the work of a 25 PRB symbol is a couple of microseconds, so the dispatch itself is
/// most of `ul_equalization_demod`. Batching the symbols of a group into a single dispatch removes
/// that overhead without touching the math: the body below is the one above with the per-symbol
/// buffers offset by gid.y.
kernel void equalize_mxn_batch(device const ushort2* h [[buffer(0)]], // cbf16 [symbol][port][layer][re]
                               device const ushort2* y [[buffer(1)]], // cbf16 [symbol][port][re]
                               device float2*       eq  [[buffer(2)]], // [symbol][re][layer]
                               device float*        nv  [[buffer(3)]], // [symbol][re][layer]
                               constant equalize_params& p [[buffer(4)]],
                               device const float* sigma2 [[buffer(5)]],
                               constant equalize_strides& st [[buffer(6)]],
                               device const uint*   h_start [[buffer(7)]],
                               uint2 gid [[thread_position_in_grid]])
{
    const uint re  = gid.x;
    const uint sym = gid.y;
    if (re >= p.nof_re || sym >= st.nof_symbols) {
        return;
    }
    // The symbols of a run are NOT evenly spaced in the estimate buffer: a symbol carrying DM-RS
    // holds fewer data REs, so the estimator publishes its slices at irregular starts (its offsets
    // step by 72, 108, 72, ... elements). One stride cannot describe them, so the run carries the
    // per-symbol starts. h_start is absolute within the bound buffer and p.h_offset is the first
    // symbol's start - which the dispatch already bound - hence the subtraction.
    h += h_start[sym] - p.h_offset;
    y += sym * st.y_stride;
    eq += sym * st.eq_stride;
    nv += sym * st.nv_stride;

    const uint L = p.nof_layers;
    const uint P = p.nof_ports;

    float2 H[MAX_PORTS][MAX_LAYERS];
    for (uint port = 0; port != P; ++port) {
        for (uint layer = 0; layer != L; ++layer) {
            H[port][layer] = load_h(h, p, port, layer, re);
        }
    }

    // ---- Single Tx layer: 1 x P SIMO combiner (CPU equalize_zf_1xn) ----
    if (L == 1u) {
        float  ch_mod_sq = 0.0f;
        float  nvar_acc  = 0.0f;
        float2 re_out    = float2(0.0f);
        for (uint port = 0; port != P; ++port) {
            const float2 hv  = H[port][0];
            const float  nrm     = hv.x * hv.x + hv.y * hv.y;
            const float  nv_port = sigma2[port];
            // Same per-port mask as the per-symbol kernel above.
            if ((nrm < INFINITY) && (nv_port > 0.0f) && (nv_port < INFINITY)) {
                ch_mod_sq += nrm;
                nvar_acc += nrm * nv_port;
                const float2 yv = load_cbf16(y, port * p.nof_re + re);
                re_out += cmul(yv, float2(hv.x, -hv.y));
            }
        }
        const float d = p.tx_scaling * ch_mod_sq;
        if ((d > 0.0f) && !isinf(d) && !isnan(d)) {
            const float rcp = 1.0f / d;
            eq[re] = re_out * rcp;
            nv[re] = nvar_acc * (rcp * rcp);
        } else {
            eq[re] = 0;
            nv[re] = INFINITY;
        }
        return;
    }

    // Multi-layer path: identical to equalize_mxn().
    float2 G[MAX_LAYERS][MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            float2 acc = float2(0.0f);
            for (uint port = 0; port != P; ++port) {
                acc += conjmul(H[port][i], H[port][j]);
            }
            G[i][j] = acc;
        }
    }
    bool diag_ok = true;
    if (p.algo == 1u) {
        for (uint i = 0; i != L; ++i) {
            diag_ok = diag_ok && (G[i][i].x > 0.0f);
        }
        for (uint i = 0; i != L; ++i) {
            G[i][i].x += p.noise_var;
        }
    }
    float2 aug[MAX_LAYERS][2 * MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            aug[i][j]     = G[i][j];
            aug[i][j + L] = (i == j) ? float2(1.0f, 0.0f) : float2(0.0f);
        }
    }
    const uint ok = invert_aug(&aug[0][0], L);
    float2    Gi[MAX_LAYERS][MAX_LAYERS];
    for (uint i = 0; i != L; ++i) {
        for (uint j = 0; j != L; ++j) {
            Gi[i][j] = aug[i][j + L];
        }
    }
    if (ok != 0u || !diag_ok) {
        for (uint layer = 0; layer != L; ++layer) {
            eq[re * L + layer] = 0;
            nv[re * L + layer] = INFINITY;
        }
        return;
    }
    float2 W[MAX_PORTS][MAX_LAYERS];
    for (uint layer = 0; layer != L; ++layer) {
        for (uint port = 0; port != P; ++port) {
            float2 acc = float2(0.0f);
            for (uint k = 0; k != L; ++k) {
                acc += conjmul(H[port][k], Gi[layer][k]);
            }
            W[port][layer] = acc;
        }
    }
    for (uint layer = 0; layer != L; ++layer) {
        float2 eq_acc = 0;
        float  corr   = 0.0f;
        for (uint port = 0; port != P; ++port) {
            const float2 yv = load_cbf16(y, port * p.nof_re + re);
            eq_acc += cmul(W[port][layer], yv);
            if (p.algo == 1u) {
                const float2 wh = cmul(W[port][layer], H[port][layer]);
                corr += wh.x;
            }
        }
        if (p.algo == 0u) {
            eq[re * L + layer] = eq_acc;
            nv[re * L + layer] = Gi[layer][layer].x * p.noise_var;
        } else {
            const float c = 1.0f / corr;
            eq[re * L + layer] = eq_acc * c;
            nv[re * L + layer] = c - 1.0f;
        }
    }
}
