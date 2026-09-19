// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief MMSE correlation matrices, built on the device (K0-d).
///
/// The 2D MMSE weights are W = R_hp . A^-1, and both matrices are analytic: every element is the
/// product of a time correlation and a frequency correlation, each a closed-form function of the
/// distance between two DM-RS symbols and two pilot subcarriers. The host used to evaluate those
/// products in nested loops per hop - O(L^2) time-correlations times O(nout * L) frequency ones,
/// with the memory traffic of two matrices per system - and then copy the result into the engine
/// slots, which was the single largest CPU block of the estimator ([mmse_time_sum] stage + corr).
///
/// Nothing in the product needs the received signal: the geometry (which slot symbols carry DM-RS,
/// which subcarriers carry pilots) and the statistics (the maximum Doppler shift and the RMS delay
/// spread, both fixed constants in v1, and the noise variance, which K4 already produces on the
/// device) are the whole input. So the matrices are built where they are consumed, and the host
/// never touches them.
///
/// The kernel reproduces the host arithmetic expression for expression - the same integer
/// difference cast to float, the same order of the multiplies, the same literals - so that A and
/// R_hp are bit-identical to build_correlation_matrices().

#include <metal_stdlib>
using namespace metal;

struct mmse_corr_params {
    uint  nof_systems; // systems of the batch (the second grid dimension)
    uint  npt;      // DM-RS symbols of the hop (the time dimension of the block)
    uint  npf;      // pilots per DM-RS symbol (the frequency dimension of the block)
    uint  ncomb;    // pilots per PRB of a DM-RS symbol (the DM-RS comb size)
    uint  nf;       // subcarriers of the block (nout = nf * 14)
    uint  L;        // matrix order: npt * npf
    // Row stride of BOTH destination slots: A is [Ls][Ls] and R_hp is [Ns][Ls]. The two matrices
    // share the stride because the host stages them that way (stage_engine_group(): `row = rp_slot +
    // o * Ls`) and because every consumer reads them that way (mmse_weights.metal: `rp = r_hp + sys *
    // nout * L + row * L`; mmse_weights_matrix.metal: row stride Lp). It is the SLOT's stride, which
    // is >= L: a merged batch tucks a narrower edge block (order L_e) into the standard group's slots
    // (stride L_std), and the matrix flavor pads the block to ceil8. The pad is left untouched by
    // this kernel - the caller zeroes it.
    uint  Ls;       // row stride of the A and R_hp slots (>= L)
    // Distance between two systems of the batch. The engine call that consumes these slots uses the
    // PACKED order the weights kernel expects ([sys][L][L] and [sys][nout][L]), while the slot's own
    // row stride may belong to a different geometry (a merged batch puts a narrower edge block into
    // the standard slot: Ls = L_std but the block order is L_e). Stepping the systems by L*L there
    // would walk into the middle of the standard group.
    uint  a_sys;    // spacing between A systems    (>= Ls * Ls)
    uint  r_sys;    // spacing between R_hp systems (>= Ns * Ls, Ns being the slot's output rows)
    float ts;       // slot symbol period in seconds, 1 / (scs_hz * 14)
    float scs_hz;   // subcarrier spacing in hertz
    float fd_hz;    // maximum Doppler shift (time correlation)
    float tau_rms_s;// RMS delay spread (frequency correlation)
    float sigma2;   // noise variance (diagonal loading of A): the host's value, or scalars[2] when asked
    float ridge;    // diagonal ridge, identical to the host's (A's diagonal is 1 + sigma2 + ridge)
    // When 1, the diagonal loading is read from the DEVICE buffer the A kernel receives as buffer(2)
    // (its element 2, the ratio the extraction's own command buffer computed) instead of from the
    // float above. That is what keeps the host out of the chain from the received grid to A: with the
    // device LSE and the device noise variance, sigma2 and its mean-power divisor are both device
    // scalars, and the ratio between them is the only thing A's diagonal wants. Only the A kernel
    // reads it; the R_hp kernel does not depend on the noise.
    //
    // scalars[2] is the host's quotient bit for bit, and that is a MEASURED property of the build, not
    // of the expression: ocudu_mmse_pilots.metal (which computes it) is compiled with -fno-fast-math,
    // because under Metal's default fast math that divide rounds differently from the host's in 28.4%
    // of 2^20 pairs - and this loading is amplified by cond_2(A) ~ 2e4 (S-7g-20). See that file.
    uint  sigma2_from_device;
    // Which element of that buffer holds the ratio (the estimator's is 2). A caller with no device
    // buffer says so with sigma2_from_device = 0; the engine binds the buffer either way, because an
    // unbound device pointer is undefined in MSL.
    uint  sigma2_slot;
    // Slot symbols carrying DM-RS in this hop, ascending (npt entries).
    uint  dmrs_slots[4];
    // Pilot positions within a PRB, ascending (npf / nprb entries).
    uint  pilot_re[12];
};

// The host mirrors this layout in mmse_corr_params_t (ocudu_metal_mmse_engine.mm) and passes it with
// setBytes, so a field added on one side only would silently shift every field after it.
static_assert(sizeof(mmse_corr_params) == 132, "mmse_corr_params must stay in step with its host mirror");

/// Two pi, as the float the host's TWOPI constant holds.
///
/// The host's TWOPI is `2.0F * static_cast<float>(M_PI)` (include/ocudu/support/math/math_utils.h),
/// and this is the same value bit for bit: (float)M_PI is 0x40490FDB and doubling it is exact, so the
/// product here reproduces the host's float multiply exactly. The kernels are compiled with strict
/// IEEE semantics (-fno-fast-math, see CMakeLists.txt) because the default fast math is free to
/// contract, reassociate and approximate these expressions - and a 1-ulp difference from the host is
/// amplified by the matrix inverse into ~1% of W and h.
constant float MMSE_TWOPI = as_type<float>(0x40C90FDBu);

/// Real part of the exponential-PDP time correlation (identical to the host's rt_corr).
static inline float mmse_rt_corr(float delta_t_s, float fd_hz)
{
    const float x = MMSE_TWOPI * delta_t_s * fd_hz;
    return 1.0F / (1.0F + x * x);
}

/// Real part of the exponential-PDP frequency correlation (identical to the host's rf_corr).
static inline float mmse_rf_corr(float delta_f_hz, float tau_rms_s)
{
    const float x = MMSE_TWOPI * delta_f_hz * tau_rms_s;
    return 1.0F / (1.0F + x * x);
}

/// Subcarrier of the \c i_pilot -th pilot within the block: pilots are PRB-major, comb ascending,
/// exactly like the host's pilot_sc[].
static inline uint mmse_corr_pilot_subcarrier(constant mmse_corr_params& p, uint i_pilot)
{
    return (i_pilot / p.ncomb) * 12u + p.pilot_re[i_pilot % p.ncomb];
}

/// \brief Fills the A slot (row stride \c p.Ls ) of one system: A = kron(R_t_pp, R_f_pp) + (sigma2 + ridge) I.
///
/// The slot is assumed to be zeroed by the caller: this writes the L x L block only, and the
/// diagonal loading is added to the first L diagonal entries.
///
/// \c scalars is the caller's sigma2 buffer; the kernel reads \c scalars[p.sigma2_slot] when
/// \c p.sigma2_from_device is set, and \c p.sigma2 otherwise. The element is the noise-to-pilot-power
/// ratio the extraction's own command buffer produced, i.e. the same float the host would have passed
/// in \c p.sigma2 (see mmse_pilots_power) - reading it here is what lets A be built with the host
/// never having read the hop's noise variance.
/// \note The buffer is the BASE of the caller's allocation, never the element's own address: the
/// kernel indexes it with \c sigma2_slot, so an element address would read past the end.
kernel void mmse_corr_a(device float* a [[buffer(0)]],
                        constant mmse_corr_params& p [[buffer(1)]],
                        device const float* scalars [[buffer(2)]],
                        uint2 gid [[thread_position_in_grid]])
{
    // One thread per matrix element of one system: the second grid dimension is the SYSTEM, so the
    // whole batch is one dispatch (a dispatch per system cost more than the host loops it replaces).
    // EXPERIMENT (not committed): the grid covers the whole slot and this kernel writes the
    // blockdiag identity in the pad, so the pad's writer and the inversion's read are both inside the
    // command buffer instead of depending on the host store's visibility.
    const uint Ls = (p.Ls != 0u) ? p.Ls : p.L;
    if ((gid.x >= Ls * Ls) || (gid.y >= p.nof_systems)) {
        return;
    }
    device float* a_sys = a + (ulong)gid.y * p.a_sys;
    const uint    i     = gid.x;
    const uint    row   = i / Ls;
    const uint    col   = i % Ls;
    if ((row >= p.L) || (col >= p.L)) {
        a_sys[(ulong)row * Ls + col] = (row == col) ? 1.0f : 0.0f;
        return;
    }
    const uint t1  = row / p.npf;
    const uint f1  = row % p.npf;
    const uint t2  = col / p.npf;
    const uint f2  = col % p.npf;

    const int dt = (int)p.dmrs_slots[t1] - (int)p.dmrs_slots[t2];
    const float rt = mmse_rt_corr((float)abs(dt) * p.ts, p.fd_hz);

    const int df = (int)mmse_corr_pilot_subcarrier(p, f1) - (int)mmse_corr_pilot_subcarrier(p, f2);
    const float rf = mmse_rf_corr((float)abs(df) * p.scs_hz, p.tau_rms_s);

    // The host adds the loading to the correlation's own diagonal (R_pp has unit diagonal), so the
    // value there is 1 + sigma2 + ridge.
    const float sigma2 = (p.sigma2_from_device != 0u) ? scalars[p.sigma2_slot] : p.sigma2;
    float v = rt * rf;
    if (row == col) {
        v += sigma2 + p.ridge;
    }
    a_sys[(ulong)row * p.Ls + col] = v;
}

/// \brief Fills the R_hp slot (row stride \c p.Ls , like A) of one system: R_hp[o][k] = rt(sym(o) - t_k) * rf(sc(o) - f_k).
///
/// Rows are (slot symbol, subcarrier) in symbol-major order, columns are (DM-RS symbol, pilot) -
/// the same indexing the host's build_correlation_matrices() uses.
kernel void mmse_corr_r_hp(device float* r_hp [[buffer(0)]],
                           constant mmse_corr_params& p [[buffer(1)]],
                           uint2 gid [[thread_position_in_grid]])
{
    const uint nout = p.nf * 14u;
    if ((gid.x >= nout * p.L) || (gid.y >= p.nof_systems)) {
        return;
    }
    device float* r_sys = r_hp + (ulong)gid.y * p.r_sys;
    const uint    i     = gid.x;
    const uint    o     = i / p.L;
    const uint    col   = i % p.L;
    const uint sym = o / p.nf;
    const uint sc  = o % p.nf;
    const uint t2  = col / p.npf;
    const uint f2  = col % p.npf;

    const int dt = (int)sym - (int)p.dmrs_slots[t2];
    const float rt = mmse_rt_corr((float)abs(dt) * p.ts, p.fd_hz);

    const int df = (int)sc - (int)mmse_corr_pilot_subcarrier(p, f2);
    const float rf = mmse_rf_corr((float)abs(df) * p.scs_hz, p.tau_rms_s);

    // Row stride Ls, NOT the slot's output-row count: R_hp[o][k] is stored row-major with the same
    // stride as A (see the struct). Stepping the rows by the output count wrote o * Ns instead, so
    // only the first L rows landed inside the slot and every row past the first L*L entries was
    // written elsewhere in the buffer (measured: 2916 of 27216 non-zero, 2916 = L * L).
    r_sys[(ulong)o * p.Ls + col] = rt * rf;
}
