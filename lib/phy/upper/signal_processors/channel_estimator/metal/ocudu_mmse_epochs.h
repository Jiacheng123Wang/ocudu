// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// The slot's symbol start epochs, as a function of the numerology and the cyclic prefix ALONE.
//
// ---- Why this is a header shared by the host and the Metal kernels ----
//
// port_channel_estimator_average_impl::initialize_symbol_start_epochs() builds the 14 (12 for extended
// CP) start times of the slot once per configuration and hands them to the estimator's stage in
// fd_td_estimation_stage_args::symbol_start_epochs. Until batch 5g the Metal route copied that array
// into a zero-copy device buffer (gpu_epochs) - a host -> device write of a CONSTANT, once per
// configuration, and it was the last one left: the crossing contract asks for zero reads AND zero
// writes per hop, and this single upload is what kept the counter at 7 of 8 checks.
//
// The values are needed on the device in four kernels, and only for the few symbols the hop's DM-RS
// occupies:
//   * K4 (mmse_noise, ocudu_mmse_reformat.metal), which rotates the regenerated pilots by
//     2*pi*cfo*epoch[sym];
//   * mmse_pilots_cfo (the phase ramp between the hop's first two DM-RS symbols),
//     mmse_pilots_apply_cfo (one phasor per DM-RS symbol) and mmse_pilots_sigma2 (the same phasor on
//     the pilots it regenerates), all in ocudu_mmse_pilots.metal.
//
// MSL cannot include the host header this rule lives in: cyclic_prefix.h pulls in phy_time_unit.h,
// subcarrier_spacing.h, ocudu_assert.h and <string_view>, and its to_seconds() is double - which an
// Apple GPU does not have. The cp-length rule therefore has to be RESTATED here (it is TS 38.211
// 5.3.1, and cyclic_prefix::get_length() is its other, host-side statement). A restatement that
// nothing compares is drift waiting to happen, so this one is compared, twice:
//   * `[epoch_check]` (port_channel_estimator_metal_mmse_impl.cpp) recomputes the array with THIS
//     function on the first hop of every configuration the estimator sees and compares it with the
//     host's own symbol_start_epochs, bit for bit, printing the verdict once per leg;
//   * the unit test (test/port_channel_estimator_metal_mmse_unit_test.cpp) sweeps the ENTIRE domain -
//     both CP types x all five numerologies x every symbol of the slot - against
//     cyclic_prefix::get_length(), so the restatement is proved equivalent to the host rule, not
//     sampled.
//
// ---- Why the device can reproduce the host's floats exactly ----
//
// The host builds (initialize_symbol_start_epochs()):
//     e[0] = cp.get_length(0, scs).to_seconds() * scs_to_khz(scs) * 1000;
//     e[i] = e[i - 1] + cp.get_length(i, scs).to_seconds() * scs_to_khz(scs) * 1000 + 1.0F;
// phy_time_unit::to_seconds() converts units of kappa to seconds as value_kappa * KAPPA * T_C, with
// KAPPA * T_C = 64 / (480000 * 4096) = 1 / 30720000 s. So one symbol's prefix contributes
//     cp_len_kappa * 1000 * scs_khz / 30720000 = cp_len_kappa * scs_khz / 30720 symbols,
// and since scs_khz = 15 << mu and 30720 = 15 * 2048, that is exactly
//     (cp_len_kappa << mu) / 2048.
// Every quantity in that expression is EXACT in single precision for every value the SCS enum can
// carry: `cp_len_kappa << mu` is an integer of at most 512 (a kappa length is 144 or 160 for normal
// CP, 512 for extended, each divided then multiplied by 2^mu), and the division is by 2048 = 2^11, a
// power of two. The accumulation is exact as well: it adds 1 per symbol and prefixes that are
// multiples of 2^-11, so every epoch is a multiple of 2^-11 below 16 - representable, with room to
// spare. That is the whole argument: not one operation here rounds, so no compiler is free to change
// the answer, and a kernel built with Metal's default fast math reproduces the host's array bit for
// bit. (The host computes the same values through double, but the exact result is a float, and the
// double intermediates are within 1e-16 of it - eight orders of magnitude below the float rounding
// boundary at 3.7e-9 for values of this size.)
//
// ---- The one place this deliberately does NOT follow the host ----
//
// For an index at or past the slot's symbol count (extended CP has 12 symbols, normal has 14) the
// result is 0.0F. That is not arbitrary: it is what the retired upload put in the padded tail of its
// 14-float array, and keeping it means the device's value for EVERY index a caller can form is
// identical to the one the device used to read. No caller asks for those indices (a DM-RS symbol is
// always inside the slot), and the clamp below is what makes a caller that does read out of range.

#pragma once

/// \brief Cyclic prefix length of slot symbol \c sym, in units of the constant kappa (TS 38.211 5.3.1).
///
/// The device-side statement of cyclic_prefix::get_length(): 144 (normal) or 512 (extended) kappa,
/// each shifted right by the numerology, plus 16 for the first symbol of a half-subframe under normal
/// CP - which covers symbol 0 always and symbol 7 * 2^mu when the slot has one (it does not beyond
/// 15 kHz: 7 * 2^mu >= 14 leaves the slot).
///
/// \param[in] numerology  Numerology index mu of the transmission, clamped to the SCS enum's range.
/// \param[in] cp_extended Non-zero for extended cyclic prefix (no long prefix; 12 symbols per slot).
/// \param[in] sym         Slot symbol index, in [0, 14).
static inline unsigned ocudu_mmse_cp_length_kappa(unsigned numerology, unsigned cp_extended, unsigned sym)
{
    // Every parameter is a value the caller controls, so none of them may select a length of its own:
    // the numerology is clamped to the five the subcarrier_spacing enum carries (mu <= 4, so the
    // shifts below stay inside a word) and the CP type is reduced to a flag.
    const unsigned mu = (numerology < 4u) ? numerology : 4u;

    if (cp_extended != 0u) {
        return 512u >> mu;
    }
    return (144u >> mu) + (((sym == 0u) || (sym == (7u << mu))) ? 16u : 0u);
}

/// \brief Start time of slot symbol \c sym, in units of OFDM symbol duration.
///
/// The device-side statement of initialize_symbol_start_epochs(), and the reason this file exists.
/// The body mirrors that function line for line - the same seed for symbol 0, the same
/// `e = e + prefix + 1` recurrence - because a restatement that also changes the ORDER of the
/// additions invites a reader to wonder which one is authoritative. (Here it would not matter: see
/// the exactness argument in the file header. It matters in general, and the next mapping copied this
/// way may not be exact.)
///
/// \param[in] numerology  Numerology index mu of the transmission, clamped as above.
/// \param[in] cp_extended Non-zero for extended cyclic prefix.
/// \param[in] sym         Slot symbol index; at or past the slot's symbol count the result is 0.
/// \return The symbol's start time in symbol durations.
static inline float ocudu_mmse_symbol_start_epoch(unsigned numerology, unsigned cp_extended, unsigned sym)
{
    const unsigned mu               = (numerology < 4u) ? numerology : 4u;
    const unsigned ext              = (cp_extended != 0u) ? 1u : 0u;
    const unsigned nof_slot_symbols = (ext != 0u) ? 12u : 14u;

    // The retired upload's padded tail: an index the slot does not have reads as zero, not as a
    // length extrapolated past the cyclic prefix rule.
    if (sym >= nof_slot_symbols) {
        return 0.0F;
    }

    // The compiler-time bound is the slot's own maximum; `sym` only ends the walk early.
    float e = static_cast<float>(ocudu_mmse_cp_length_kappa(mu, ext, 0u) << mu) / 2048.0F;
    for (unsigned i = 1u; i != 14u; ++i) {
        if (i > sym) {
            break;
        }
        e = e + static_cast<float>(ocudu_mmse_cp_length_kappa(mu, ext, i) << mu) / 2048.0F + 1.0F;
    }
    return e;
}
