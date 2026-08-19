// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Asynchronous residual (delta) belief propagation for LDPC: a BARRIER-FREE
// persistent grid. One threadgroup per lifted check row loops generation
// after generation; each generation reduces the row's min-sum over the
// extrinsic messages Q = P - R_old, computes the new per-edge messages
// R_new, and injects the residual delta R_new - R_old into the global
// posterior pool P with a commutative atomic add. Because the delta update
// P += R_new - R_old is the exact c2v-delta formulation, the atomic
// accumulation is equivalent to the flooding VN sum P = L + sum(R) at the
// fixed point, while torn reads/writes cannot occur: whether a worker reads
// a fresh or a stale P, its delta still moves the pool toward the same
// fixed point (stale reads only cost a few extra generations, which is the
// documented race-tolerant property of asynchronous message passing).
//
// There are NO threadgroup_barrier calls and no cross-threadgroup waits:
// threadgroups are never required to be co-resident, so the kernel cannot
// deadlock (the failure mode of the metal_persistent grid-barrier attempt).
// The grid is bounded by max_iter generations per worker, so the kernel
// always terminates even without convergence (GPU watchdog safety).
//
// A dedicated poller threadgroup (wid == m_aligned) waits for each full
// generation of row updates (a relaxed atomic counter), then polls the
// parity equations; a clean syndrome raises the stop_flag that every worker
// checks at the top of each generation.
//
// Fixed point: the posterior pool is int32 Q16.16 (|P| up to ~2000 is well
// inside +-32767; Q8.8 would overflow). Metal has no atomic floating-point
// RMW, so the pool is a plain uint32 buffer accessed with relaxed atomics.
// The per-edge old-message array R_old is private to its owner lane (each
// edge belongs to exactly one row, and one lane owns the edges it updates).

#include <metal_stdlib>
using namespace metal;

// Must match async_ctrl_t in ocudu_metal_decoder_engine.mm.
struct AsyncCtrl {
    atomic_uint stop_flag;    // 1 = converged, all workers exit
    atomic_uint row_updates;  // total finished row updates (generation tracker)
    atomic_uint actual_gens;  // completed generations at stop (poller-written)
};

// Q16.16 conversion helpers (the pool is 32-bit fixed point).
inline int llr_to_fixed(float v)
{
    return static_cast<int>(clamp(v * 65536.0f, -2147483647.0f, 2147483647.0f));
}

inline float fixed_to_llr(int v)
{
    return static_cast<float>(v) * (1.0f / 65536.0f);
}

// Phase A (one dispatch): reset the control block, quantize the host int8
// LLRs into the Q16.16 pool, and zero the per-edge old messages.
kernel void nmsa_init(
    device atomic_uint* P [[buffer(0)]],
    device float* R_old [[buffer(1)]],
    device AsyncCtrl* ctrl [[buffer(2)]],
    device const int8_t* in_i8 [[buffer(3)]],
    constant uint32_t& n_aligned [[buffer(4)]],
    constant uint32_t& no_edges [[buffer(5)]],
    uint id [[thread_position_in_grid]])
{
    if (id == 0) {
        atomic_store_explicit(&ctrl->stop_flag, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->row_updates, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_gens, 0, memory_order_relaxed);
    }
    if (id < n_aligned) {
        atomic_store_explicit(&P[id], static_cast<uint>((int16_t)in_i8[id] * 65536), memory_order_relaxed);
    }
    if (id < no_edges) {
        R_old[id] = 0.0f;
    }
}

// Phase B (one dispatch): the barrier-free asynchronous decode grid.
// Threadgroups 0..m_aligned-1 are row workers (one per lifted check row);
// threadgroup m_aligned is the syndrome poller.
kernel void nmsa_delta_bp(
    device atomic_uint* P [[buffer(0)]],
    device float* R_old [[buffer(1)]],
    device AsyncCtrl* ctrl [[buffer(2)]],
    device const uint32_t* row_start [[buffer(3)]],
    device const uint32_t* edge_vn [[buffer(4)]],
    device uint32_t* h_pred_bits [[buffer(5)]],
    device const uint32_t* h_matrix [[buffer(6)]],
    constant uint32_t& n_h_chunks [[buffer(7)]],
    constant uint32_t& m_aligned [[buffer(8)]],
    constant float& norm [[buffer(9)]],
    constant float& beta [[buffer(10)]],
    constant uint32_t& max_iter [[buffer(11)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    if (wid == m_aligned) {
        // ---- Poller: wait for a full generation of row updates, then poll
        // the parity equations. A clean syndrome raises stop_flag; the
        // poller also bounds itself by the workers' max_iter generations so
        // the kernel always terminates. ----
        uint last = 0;
        while (atomic_load_explicit(&ctrl->stop_flag, memory_order_relaxed) == 0) {
            const uint cur = atomic_load_explicit(&ctrl->row_updates, memory_order_relaxed);
            if (cur - last < m_aligned) {
                continue;
            }
            last = cur;

            // Poll every row's parity from the pool's hard bits.
            uint errors = 0;
            for (uint row = 0; row < m_aligned; ++row) {
                uint parity = 0;
                const uint row_base = row * n_h_chunks;
                for (uint i = tid; i < n_h_chunks; i += 32) {
                    uint32_t mask = h_matrix[row_base + i];
                    while (mask != 0) {
                        const uint bit = ctz(mask);
                        parity ^= ((int32_t)atomic_load_explicit(&P[i * 32 + bit], memory_order_relaxed) < 0)
                                      ? 1u
                                      : 0u;
                        mask &= (mask - 1);
                    }
                }
                for (uint offset = 16; offset > 0; offset >>= 1) {
                    parity ^= simd_shuffle_xor(parity, offset);
                }
                if (tid == 0) {
                    h_pred_bits[row] = parity & 1u;
                    errors += parity & 1u;
                }
            }
            if (errors == 0) {
                atomic_store_explicit(&ctrl->stop_flag, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&ctrl->actual_gens, cur / m_aligned, memory_order_relaxed);
            if (cur >= max_iter * m_aligned) {
                break;
            }
        }
        return;
    }

    if (wid >= m_aligned) {
        return;
    }

    // ---- Row worker: generation loop over the row's edges. ----
    const uint e0 = row_start[wid];
    const uint e1 = row_start[wid + 1];

    for (uint gen = 0; gen < max_iter; ++gen) {
        if (atomic_load_explicit(&ctrl->stop_flag, memory_order_relaxed) != 0) {
            break;
        }

        // Pass 1: extrinsic Q = P - R_old (relaxed reads), reduce the row's
        // min1/min2/idx/sign and the row parity over the hard bits.
        float m1 = 65504.0f;
        float m2 = 65504.0f;
        uint i1 = 0;
        uint sign = 0;
        uint parity = 0;

        for (uint e = e0 + tid; e < e1; e += 32) {
            const uint vn = edge_vn[e];
            const float pv = fixed_to_llr(
                static_cast<int32_t>(atomic_load_explicit(&P[vn], memory_order_relaxed)));
            const float q = pv - R_old[e];
            const float val = abs(q);
            sign ^= (q < 0.0f) ? 1u : 0u;
            parity ^= (pv < 0.0f) ? 1u : 0u;
            if (val < m1) {
                m2 = m1;
                m1 = val;
                i1 = vn;
            } else if (val < m2) {
                m2 = val;
            }
        }

        // Butterfly reduction across the 32 lanes. Ties keep the current winner.
        for (uint offset = 16; offset > 0; offset >>= 1) {
            const float om1 = simd_shuffle_xor(m1, offset);
            const uint oi1 = simd_shuffle_xor(i1, offset);
            const float om2 = simd_shuffle_xor(m2, offset);
            const uint osign = simd_shuffle_xor(sign, offset);
            const uint oparity = simd_shuffle_xor(parity, offset);
            sign ^= osign;
            parity ^= oparity;
            if (om1 < m1) {
                m2 = min(m1, om2);
                m1 = om1;
                i1 = oi1;
            } else {
                m2 = min(m2, om1);
            }
        }

        // Pass 2: per-edge new message R_new = extrinsic sign x
        // norm x max(min - beta, 0); inject the residual delta R_new -
        // R_old with a commutative atomic add and store the new message.
        for (uint e = e0 + tid; e < e1; e += 32) {
            const uint vn = edge_vn[e];
            const float pv = fixed_to_llr(
                static_cast<int32_t>(atomic_load_explicit(&P[vn], memory_order_relaxed)));
            const float q = pv - R_old[e];
            float mag = (vn == i1) ? m2 : m1;
            mag = max(mag - beta, 0.0f);
            mag *= norm;
            const bool neg = ((sign ^ ((q < 0.0f) ? 1u : 0u)) != 0u);
            const float r_new = neg ? -mag : mag;
            const int delta = llr_to_fixed(r_new - R_old[e]);
            atomic_fetch_add_explicit(&P[vn], static_cast<uint>(delta), memory_order_relaxed);
            R_old[e] = r_new;
        }

        if (tid == 0) {
            atomic_fetch_add_explicit(&ctrl->row_updates, 1, memory_order_relaxed);
        }
    }
}
