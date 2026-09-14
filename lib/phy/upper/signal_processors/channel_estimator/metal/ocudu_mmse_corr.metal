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
    uint  npt;      // DM-RS symbols of the hop (the time dimension of the block)
    uint  npf;      // pilots per DM-RS symbol (the frequency dimension of the block)
    uint  ncomb;    // pilots per PRB of a DM-RS symbol (the DM-RS comb size)
    uint  nf;       // subcarriers of the block (nout = nf * 14)
    uint  L;        // matrix order: npt * npf
    uint  Ls;       // row stride of the destination slots (>= L; the pad is left untouched)
    uint  Ns;       // row stride of the R_hp slot (>= nout; the pad is left untouched)
    float ts;       // slot symbol period in seconds, 1 / (scs_hz * 14)
    float scs_hz;   // subcarrier spacing in hertz
    float fd_hz;    // maximum Doppler shift (time correlation)
    float tau_rms_s;// RMS delay spread (frequency correlation)
    float sigma2;   // noise variance (diagonal loading of A)
    float ridge;    // diagonal ridge, identical to the host's (A's diagonal is 1 + sigma2 + ridge)
    // Slot symbols carrying DM-RS in this hop, ascending (npt entries).
    uint  dmrs_slots[4];
    // Pilot positions within a PRB, ascending (npf / nprb entries).
    uint  pilot_re[12];
};

/// Real part of the exponential-PDP time correlation (identical to the host's rt_corr).
static inline float mmse_rt_corr(float delta_t_s, float fd_hz)
{
    const float x = 2.0F * M_PI_F * delta_t_s * fd_hz;
    return 1.0F / (1.0F + x * x);
}

/// Real part of the exponential-PDP frequency correlation (identical to the host's rf_corr).
static inline float mmse_rf_corr(float delta_f_hz, float tau_rms_s)
{
    const float x = 2.0F * M_PI_F * delta_f_hz * tau_rms_s;
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
kernel void mmse_corr_a(device float* a [[buffer(0)]],
                        constant mmse_corr_params& p [[buffer(1)]],
                        uint i [[thread_position_in_grid]])
{
    if (i >= p.L * p.L) {
        return;
    }
    const uint row = i / p.L;
    const uint col = i % p.L;
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
    float v = rt * rf;
    if (row == col) {
        v += p.sigma2 + p.ridge;
    }
    a[(ulong)row * p.Ls + col] = v;
}

/// \brief Fills the R_hp slot (row stride \c p.Ns ) of one system: R_hp[o][k] = rt(sym(o) - t_k) * rf(sc(o) - f_k).
///
/// Rows are (slot symbol, subcarrier) in symbol-major order, columns are (DM-RS symbol, pilot) -
/// the same indexing the host's build_correlation_matrices() uses.
kernel void mmse_corr_r_hp(device float* r_hp [[buffer(0)]],
                           constant mmse_corr_params& p [[buffer(1)]],
                           uint i [[thread_position_in_grid]])
{
    const uint nout = p.nf * 14u;
    if (i >= nout * p.L) {
        return;
    }
    const uint o   = i / p.L;
    const uint col = i % p.L;
    const uint sym = o / p.nf;
    const uint sc  = o % p.nf;
    const uint t2  = col / p.npf;
    const uint f2  = col % p.npf;

    const int dt = (int)sym - (int)p.dmrs_slots[t2];
    const float rt = mmse_rt_corr((float)abs(dt) * p.ts, p.fd_hz);

    const int df = (int)sc - (int)mmse_corr_pilot_subcarrier(p, f2);
    const float rf = mmse_rf_corr((float)abs(df) * p.scs_hz, p.tau_rms_s);

    r_hp[(ulong)o * p.Ns + col] = rt * rf;
}
