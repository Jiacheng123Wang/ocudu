// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Layered normalized min-sum LDPC decoder (fused CN+VN kernel). Layer =
// base-graph check row (BG1: 46, BG2: 42); each layer = Z lifted check rows,
// which are independent and fully parallel.
//
// The CN and VN updates are fused into one kernel: within one layer no two
// lifted rows connect to the same variable node (3GPP base-graph property),
// so the VN soft-bit update llr[vn] += c2v_new - c2v_old is a race-free bare
// write. One dispatch per layer (instead of two) halves the serialized
// dispatch chain, and the per-VN delta buffer is gone entirely.
//
// GPU-internal early termination: each round ends with a full syndrome refresh
// (nmsl_final_syndrome) whose unsatisfied-row count feeds the ET gate
// (nmsl_et_gate); once a round's syndrome is clean, every kernel of the
// following rounds returns immediately (single command buffer, no CPU polling).
//
// Buffer indices are disjoint per kernel (CN 0-8, syndrome 10-14, gate 15) so
// the host encoder binds every static argument once for the whole decode and
// only setBytes(layer_start) + dispatch remain inside the per-layer loop.
//
// Convention: negative LLR -> bit 1 (fp16 sign bit is the hard decision).

#include <metal_stdlib>
using namespace metal;

// Must match decode_ctrl_t in ocudu_metal_decoder_engine.mm.
struct DecodeCtrl {
    atomic_uint error_count;
    atomic_uint early_terminate;
    atomic_uint actual_iters;
};

// Phase A: reset the control block and zero the c2v messages (dispatch
// no_edges threads over the flat c2v array).
kernel void nmsl_init(
    device half* c2v [[buffer(0)]],
    device DecodeCtrl* ctrl [[buffer(1)]],
    uint e [[thread_position_in_grid]])
{
    if (e == 0) {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }
    c2v[e] = (half)0.0f;
}

// Fused CN+VN update for one layer: Z threadgroups, one per lifted check row
// (row = layer_start + wid). Pass 1 reduces min1/min2/idx/sign over the row's
// edges (v2c = soft - c2v_old); pass 2 writes c2v_new and applies the VN
// update to the soft bits in place (race-free, see the header comment), and
// records the row parity for the final syndrome readback.
kernel void nmsl_cn_update(
    device const uint32_t* row_start [[buffer(0)]],
    device const uint32_t* edge_vn [[buffer(1)]],
    device half* c2v [[buffer(2)]],
    device half* llr [[buffer(3)]],
    device uint32_t* h_pred_bits [[buffer(4)]],
    constant uint32_t& layer_start [[buffer(5)]],
    constant float& norm [[buffer(6)]],
    constant float& beta [[buffer(7)]],
    device DecodeCtrl* ctrl [[buffer(8)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    const uint row = layer_start + wid;
    const uint e0 = row_start[row];
    const uint e1 = row_start[row + 1];

    half m1 = 65504.0h;
    half m2 = 65504.0h;
    uint i1 = 0;
    uint sign = 0;
    uint parity = 0;

    const half norm_h = (half)norm;
    const half beta_h = (half)beta;

    // 核心优化：静态展开 (Static Unrolling) 锁定寄存器
    // 基于 3GPP 38.212 规范，LDPC BG1/BG2 最大行度数为 68。
    // SIMD32 线程组内，单个线程最多处理 3 个 Node。纯标量声明彻底杜绝 Register Spilling。
    uint vn_0 = 0, vn_1 = 0, vn_2 = 0;
    half cv_0 = 0, cv_1 = 0, cv_2 = 0;
    half v_0  = 0, v_1  = 0, v_2  = 0;

    const uint e_0 = e0 + tid;
    const uint e_1 = e_0 + 32;
    const uint e_2 = e_1 + 32;

    // Pass 1: 静态分支展开，消除 for 循环和动态数组寻址
    if (e_0 < e1) {
        vn_0 = edge_vn[e_0];
        cv_0 = c2v[e_0];
        v_0  = llr[vn_0];
        half v2c = v_0 - cv_0;
        half val = abs(v2c);
        sign ^= (v2c < 0.0h) ? 1u : 0u;
        parity ^= (v_0 < 0.0h) ? 1u : 0u;
        if (val < m1) { m2 = m1; m1 = val; i1 = vn_0; }
        else if (val < m2) { m2 = val; }
    }
    
    if (e_1 < e1) {
        vn_1 = edge_vn[e_1];
        cv_1 = c2v[e_1];
        v_1  = llr[vn_1];
        half v2c = v_1 - cv_1;
        half val = abs(v2c);
        sign ^= (v2c < 0.0h) ? 1u : 0u;
        parity ^= (v_1 < 0.0h) ? 1u : 0u;
        if (val < m1) { m2 = m1; m1 = val; i1 = vn_1; }
        else if (val < m2) { m2 = val; }
    }
    
    if (e_2 < e1) {
        vn_2 = edge_vn[e_2];
        cv_2 = c2v[e_2];
        v_2  = llr[vn_2];
        half v2c = v_2 - cv_2;
        half val = abs(v2c);
        sign ^= (v2c < 0.0h) ? 1u : 0u;
        parity ^= (v_2 < 0.0h) ? 1u : 0u;
        if (val < m1) { m2 = m1; m1 = val; i1 = vn_2; }
        else if (val < m2) { m2 = val; }
    }

    // Butterfly reduction (原生 half 并行规约)
    for (uint offset = 16; offset > 0; offset >>= 1) {
        const half om1 = simd_shuffle_xor(m1, offset);
        const uint oi1 = simd_shuffle_xor(i1, offset);
        const half om2 = simd_shuffle_xor(m2, offset);
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

    if (tid == 0) {
        h_pred_bits[row] = parity & 1u;
    }

    // Pass 2: 完全依赖物理寄存器内的数据 (v_0/v_1/v_2)，真正实现 0 全局内存读取
    if (e_0 < e1) {
        half mag = (vn_0 == i1) ? m2 : m1;
        mag = max(mag - beta_h, (half)0.0h) * norm_h;
        bool edge_neg = (v_0 - cv_0 < 0.0h);
        bool msg_neg = (sign ^ (edge_neg ? 1u : 0u)) != 0u;
        half c2v_new = msg_neg ? -mag : mag;
        c2v[e_0] = c2v_new;
        llr[vn_0] = v_0 + (c2v_new - cv_0);
    }
    
    if (e_1 < e1) {
        half mag = (vn_1 == i1) ? m2 : m1;
        mag = max(mag - beta_h, (half)0.0h) * norm_h;
        bool edge_neg = (v_1 - cv_1 < 0.0h);
        bool msg_neg = (sign ^ (edge_neg ? 1u : 0u)) != 0u;
        half c2v_new = msg_neg ? -mag : mag;
        c2v[e_1] = c2v_new;
        llr[vn_1] = v_1 + (c2v_new - cv_1);
    }
    
    if (e_2 < e1) {
        half mag = (vn_2 == i1) ? m2 : m1;
        mag = max(mag - beta_h, (half)0.0h) * norm_h;
        bool edge_neg = (v_2 - cv_2 < 0.0h);
        bool msg_neg = (sign ^ (edge_neg ? 1u : 0u)) != 0u;
        half c2v_new = msg_neg ? -mag : mag;
        c2v[e_2] = c2v_new;
        llr[vn_2] = v_2 + (c2v_new - cv_2);
    }
}

// Final syndrome refresh: recomputes the per-row parities from the final soft
// bits (one threadgroup per check row) so the CPU-side readback is exact, and
// accumulates the unsatisfied-row count for the ET gate.
kernel void nmsl_final_syndrome(
    device const uint32_t* row_start [[buffer(10)]],
    device const uint32_t* edge_vn [[buffer(11)]],
    device const half* llr [[buffer(12)]],
    device uint32_t* h_pred_bits [[buffer(13)]],
    device DecodeCtrl* ctrl [[buffer(14)]],
    uint tid [[thread_index_in_threadgroup]],
    uint wid [[threadgroup_position_in_grid]])
{
if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }

    const uint row = wid;
    uint parity = 0;
    const uint e0 = row_start[row];
    const uint e1 = row_start[row + 1];

    // Walk the CSR edge list of the row (identical edge set as the packed H row,
    // so the XOR parity is bit-exact with the previous H-matrix walk).
    for (uint e = e0 + tid; e < e1; e += 32) {
        parity ^= (llr[edge_vn[e]] < 0.0h) ? 1u : 0u;
    }

    for (uint offset = 16; offset > 0; offset >>= 1) {
        parity ^= simd_shuffle_xor(parity, offset);
    }

    if (tid == 0) {
        h_pred_bits[row] = parity & 1u;
        if ((parity & 1u) != 0u) {
            atomic_fetch_add_explicit(&ctrl->error_count, 1, memory_order_relaxed);
        }
    }
}

// ET gate: runs once per round, after the final syndrome refresh. If the whole
// syndrome was clean, raise early_terminate (all kernels of the following
// rounds then return immediately). actual_iters counts executed rounds only.
kernel void nmsl_et_gate(
    device DecodeCtrl* ctrl [[buffer(15)]],
    uint tid [[thread_position_in_grid]])
{
    if (tid != 0) {
        return;
    }
    if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
        return;
    }
    if (atomic_load_explicit(&ctrl->error_count, memory_order_relaxed) == 0) {
        atomic_store_explicit(&ctrl->early_terminate, 1, memory_order_relaxed);
    }
    // Reset for the next round (when ET did not fire).
    atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
    atomic_fetch_add_explicit(&ctrl->actual_iters, 1, memory_order_relaxed);
}

// One-time preprocessing: converts the host int8 LLR buffer into the fp16
// working buffer (one dispatch per decode; the hot CN/VN loop keeps its
// native half loads and float ALU, while the host side stays zero-conversion
// with a plain memcpy into the int8 buffer).
kernel void nmsl_i8_to_fp16(
    device const int8_t* in [[buffer(16)]],
    device half* out [[buffer(17)]],
    uint i [[thread_position_in_grid]])
{
    out[i] = (half)((int16_t)in[i]);
}

// ---------------------------------------------------------------------------
// Persistent variant (metal_persistent): ONE dispatch per decode.

// Forward declaration: MSL resolves function calls C-style, and the fused
// row body is defined after this kernel.
inline void nmsl_fused_row_update(
    device const uint32_t* row_start,
    device const uint32_t* edge_vn,
    device half*           c2v,
    device half*           llr,
    device uint32_t*       h_pred_bits,
    uint                   row,
    constant float&        norm,
    constant float&        beta,
    uint                   tid);

//
// ONE resident threadgroup of 1024 threads (32 simdgroups) runs the whole
// (iteration, layer) loop inside the kernel and the host submits a single
// dispatchThreadgroups call. The Gauss-Seidel dependency between layers is
// serialized by threadgroup_barrier - the one cross-thread synchronization
// primitive this platform guarantees. This MSL target exposes ONLY relaxed
// atomics (no acquire/release/seq_cst, no atomic_fence), so cross-THREADGROUP
// software barriers cannot be made correct here (a device-scope memory
// ordering primitive does not exist); within one threadgroup,
// threadgroup_barrier(mem_device) provides the guaranteed cross-simdgroup
// visibility the layered chain needs.
//
// The layer's lifted rows are independent (3GPP base-graph property), so
// each simdgroup takes every 32nd row serially; the prologue folds in the
// work of the former nmsl_init + nmsl_i8_to_fp16 dispatches (ctrl reset,
// c2v zeroing, int8->fp16 conversion) and the syndrome refresh + ET gate of
// each round run in-kernel, so decode() = exactly one dispatch.
// ---------------------------------------------------------------------------

kernel void nmsl_persistent_decode(
    device const uint32_t* row_start [[buffer(0)]],
    device const uint32_t* edge_vn [[buffer(1)]],
    device half* c2v [[buffer(2)]],
    device half* llr [[buffer(3)]],
    device uint32_t* h_pred_bits [[buffer(4)]],
    constant uint32_t& z [[buffer(5)]],
    constant float& norm [[buffer(6)]],
    constant float& beta [[buffer(7)]],
    device DecodeCtrl* ctrl [[buffer(8)]],
    constant uint32_t& n_layers [[buffer(9)]],
    constant uint32_t& max_iter [[buffer(10)]],
    constant uint32_t& no_edges [[buffer(11)]],
    constant uint32_t& n_aligned [[buffer(12)]],
    constant uint32_t& m_aligned [[buffer(13)]],
    device const int8_t* in_i8 [[buffer(14)]],
    uint tid [[thread_index_in_threadgroup]])
{
    const uint lane = tid & 31u;
    const uint sg   = tid >> 5;

    // ---- Prologue (former nmsl_init + nmsl_i8_to_fp16): ctrl reset by
    // thread 0, c2v zeroing and int8 -> fp16 conversion by all 1024 threads.
    // The barrier after the prologue publishes the reset and the LLR writes
    // to every simdgroup before the first layer reads them. ----
    if (tid == 0) {
        atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->early_terminate, 0, memory_order_relaxed);
        atomic_store_explicit(&ctrl->actual_iters, 0, memory_order_relaxed);
    }
    for (uint e = tid; e < no_edges; e += 1024) {
        c2v[e] = (half)0.0f;
    }
    for (uint i = tid; i < n_aligned; i += 1024) {
        llr[i] = (half)((int16_t)in_i8[i]);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);

    // ---- Decode rounds (the former CPU-side it/l loop, now on the GPU). ----
    for (uint it = 0; it < max_iter; ++it) {
        // Rounds after the ET gate fired are skipped entirely (the flag is
        // written by thread 0 behind the gate barrier, so every thread reads
        // the same value - the breaks stay uniform).
        if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
            break;
        }

        for (uint l = 0; l < n_layers; ++l) {
            if (atomic_load_explicit(&ctrl->early_terminate, memory_order_relaxed)) {
                break;
            }
            // Round-robin over the layer's lifted rows: one simdgroup per row,
            // every 32nd row serially (the fused row body above).
            for (uint r = sg; r < z; r += 32) {
                nmsl_fused_row_update(row_start, edge_vn, c2v, llr, h_pred_bits, l * z + r, norm, beta, lane);
            }
            // Gauss-Seidel: layer l+1 reads the LLRs layer l just wrote.
            threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
        }

        // ---- Final syndrome refresh (former nmsl_final_syndrome): each
        // simdgroup scans its rows and accumulates the unsatisfied count.
        // CSR walk: identical edge set as the packed H row, so the XOR parity
        // is bit-exact with the previous H-matrix version. ----
        for (uint row = sg; row < m_aligned; row += 32) {
            uint parity = 0;
            const uint e0 = row_start[row];
            const uint e1 = row_start[row + 1];
            for (uint e = e0 + lane; e < e1; e += 32) {
                parity ^= (llr[edge_vn[e]] < 0.0h) ? 1u : 0u;
            }
            for (uint offset = 16; offset > 0; offset >>= 1) {
                parity ^= simd_shuffle_xor(parity, offset);
            }
            if (lane == 0) {
                h_pred_bits[row] = parity & 1u;
                if ((parity & 1u) != 0u) {
                    atomic_fetch_add_explicit(&ctrl->error_count, 1, memory_order_relaxed);
                }
            }
        }
        // Wait for every error contribution before the gate reads the total.
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);

        // ---- ET gate (former nmsl_et_gate), executed by thread 0. ----
        if (tid == 0) {
            if (atomic_load_explicit(&ctrl->error_count, memory_order_relaxed) == 0) {
                atomic_store_explicit(&ctrl->early_terminate, 1, memory_order_relaxed);
            }
            atomic_store_explicit(&ctrl->error_count, 0, memory_order_relaxed);
            atomic_store_explicit(&ctrl->actual_iters, it + 1, memory_order_relaxed);
        }
        // Publish the gate's writes (early_terminate) before the next round
        // reads them.
        threadgroup_barrier(mem_flags::mem_threadgroup | mem_flags::mem_device);
    }
}

// Fused CN+VN update of one lifted check row (the body of nmsl_cn_update,
// shared by the persistent kernel; the per-row result is order-independent
// within a layer, so serial reuse of one threadgroup per row is bit-exact).
inline void nmsl_fused_row_update(
    device const uint32_t* row_start,
    device const uint32_t* edge_vn,
    device half*           c2v,
    device half*           llr,
    device uint32_t*       h_pred_bits,
    uint                   row,
    constant float&        norm,
    constant float&        beta,
    uint                   tid)
{
    const uint e0 = row_start[row];
    const uint e1 = row_start[row + 1];

    float m1 = 65504.0f;
    float m2 = 65504.0f;
    uint i1 = 0;
    uint sign = 0;
    uint parity = 0;

    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const float v = (float)llr[vn];
        const float v2c = v - (float)c2v[e];
        const float val = abs(v2c);
        sign ^= (v2c < 0.0f) ? 1u : 0u;
        parity ^= (v < 0.0f) ? 1u : 0u;
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

    if (tid == 0) {
        h_pred_bits[row] = parity & 1u;
    }

    // Second pass: write c2v_new and apply the VN update in place.
    for (uint e = e0 + tid; e < e1; e += 32) {
        const uint vn = edge_vn[e];
        const float old_c2v = (float)c2v[e];
        const float v2c = (float)llr[vn] - old_c2v;
        float mag = (vn == i1) ? m2 : m1;
        mag = max(mag - beta, 0.0f);
        mag *= norm;
        const bool neg = ((sign ^ ((v2c < 0.0f) ? 1u : 0u)) != 0u);
        const float c2v_new = neg ? -mag : mag;
        c2v[e] = (half)c2v_new;
        llr[vn] = (half)((float)llr[vn] + c2v_new - old_c2v);
    }
}

