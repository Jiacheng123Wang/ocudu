// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// Multi-layer (2..4 Tx layers x 2/4/8 Rx ports) MIMO channel equalizer: one thread per
/// resource element, scalar per-RE closed-form math replicating the CPU generic ZF/MMSE
/// implementation (equalize_zf_mxn_simd / equalize_mmse_mxn_simd). The single-layer path
/// keeps the CPU implementation (its per-port noise-validity reduction semantics are not
/// part of this kernel).

#include <metal_stdlib>
using namespace metal;

constant uint MAX_LAYERS = 4;
constant uint MAX_PORTS  = 8;

struct equalize_params {
    uint  nof_re;       // resource elements
    uint  nof_ports;    // receive ports (2, 4 or 8)
    uint  nof_layers;   // transmit layers (2..4, <= nof_ports)
    uint  algo;         // 0 = ZF, 1 = MMSE
    float noise_var;    // noise variance estimate (the max across ports, CPU convention)
    float tx_scaling;   // already applied to H by the host in this kernel's path
};

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

kernel void equalize_mxn(device const float2* h  [[buffer(0)]], // [port][layer][re]
                         device const float2* y  [[buffer(1)]], // [port][re]
                         device float2*       eq  [[buffer(2)]], // [re][layer] interleaved
                         device float*        nv  [[buffer(3)]], // [re][layer]
                         constant equalize_params& p [[buffer(4)]],
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
            H[port][layer] = h[((port * L + layer) * p.nof_re) + re];
        }
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
            const float2 yv = y[port * p.nof_re + re];
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
