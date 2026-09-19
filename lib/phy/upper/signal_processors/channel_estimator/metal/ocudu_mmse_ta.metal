// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K6: the hop's time alignment, reduced on the DEVICE out of the per-symbol IDFTs.
//
// ---- Why this exists ----
// The hop's timing advance is the last REPORTING value a fused lane still reads the estimated grid
// back for. The host derives it from the pilot estimates with
// time_alignment_estimator_dft_impl::estimate_ta_correlation(), whose four steps are:
//
//   1. the pilots of each DM-RS symbol/layer go into a zero-padded IDFT input, first pilot first;
//   2. every slice is transformed and |.|^2 accumulated: the hop's POWER DELAY PROFILE;
//   3. the peak is searched in HALF A CYCLIC PREFIX either side - the delayed taps at the START of
//      the circular profile, the advanced ones at its END - and the larger of the two wins;
//   4. the peak is refined by a parabolic fit over five taps, and divided by the sampling rate the
//      transform size and the pilot stride imply.
//
// Steps 1 and 2's transform are the only part that needs an FFT, and the lane already has one: the
// device DFT kernel (ocudu_dft.metal, inverse direction) turns the pilots into the slices. This
// kernel is everything AFTER them - accumulation, search and refinement - so the value never has to
// come back to the host at all.
//
// ---- Layout ----
// Reads the transform outputs as the DFT engine leaves them: `nof_slices` consecutive transforms of
// `size` complex values each, which is how submit()/run() lay a batch out. There is no re-indexing,
// so the caller cannot disagree with this kernel about which slice is which.
//
// ---- Output ----
// One float: the time alignment in SECONDS - the same quantity and unit the host publishes. The peak
// index and the fractional part are this kernel's business; a caller that wants to check the search
// compares the seconds against the host's own estimate, at the resolution the sampling rate sets.

#include <metal_stdlib>
using namespace metal;

// The transform the fused chain runs: the very butterflies dft_dit uses, from the DFT engine's own
// directory, so there is one implementation of them (see the header).
#include "../../../../generic_functions/metal/ocudu_dft_butterflies.h"

/// ---- Compile-time bounds. NO loop in this file may be bounded by a PARAMETER. ------------------
///
/// A kernel whose termination depends on its inputs does not fail as "a wrong number" - it fails as a
/// dispatch that never finishes, and on macOS that takes the whole machine down: the WindowServer
/// watchdog fires, `kill` does not release the GPU, and even `reboot` can hang (full_gpu_chain
/// §48.131 and §48.133 - it has happened twice, both times costing a hard power cycle). So every
/// loop below is bounded by one of these constants and the parameter block only ever BREAKS out
/// early. A parameter that is out of range therefore yields a WRONG RESULT, which is visible and
/// testable, and never a hang.
///
/// They mirror the estimator's own limits; the unit test asserts that they agree with the host's.
constant uint mmse_ta_max_size   = 4096;  // dft_metal_engine::max_size (one tap per transform output)
constant uint mmse_ta_max_slices = 16;    // MAX_DMRS_SYMBOLS(4) * MAX_LAYERS(4), with room to spare
constant uint mmse_ta_max_taps   = 5;     // the parabolic fit's widest window
constant uint mmse_ta_max_size_min = 2;   // a transform is at least two points

/// Threadgroup size this file's kernels are dispatched with. A COMPILE-TIME constant on purpose: a
/// kernel that reads [[threads_per_threadgroup]] and uses it as a stride stops if that read is ever
/// zero, which is exactly the failure §48.131 documents.
constant uint mmse_ta_tg_size = 256;

struct mmse_ta_params {
  uint  size;            // Transform size of each slice (a power of two).
  uint  nof_slices;      // Slices = DM-RS symbols x layers of the hop.
  uint  stride;          // Pilot spacing in subcarriers: 1 PUCCH f1/3/4, 2 PUSCH, 3 PUCCH f2.
  float scs_hz;          // Subcarrier spacing of the hop, in Hz.
  uint  max_ta_samples;  // Half-cyclic-prefix search window, in taps (clamped to `size`).
  uint  nof_taps;        // 5 or 3: the taps the parabolic refinement fits over.
  uint  pad0;
  uint  pad1;
};

// (mmse_ta_tg_size and mmse_ta_max_size are defined once, at the top of this file.)

/// \brief The parabolic refinement, term for term the host's curve_fitting_fractional_max().
///
/// The host solves a least-squares parabola through the taps around a peak with two fixed weight
/// vectors and returns its vertex. Reproduced with the SAME weights and the same guards (a result
/// that is NaN, infinite or outside [-1, 1] means "no refinement"): the two sides are compared
/// against each other, and a tidier reformulation of the same fit is a different rounding.
inline float mmse_ta_frac_from_taps(const thread float* y, uint nof_taps)
{
  float result;
  if (nof_taps == 5u) {
    // num_weights_5 / den_weights_5 of curve_fitting_find_max.cpp, correction 1.0.
    const float num = -0.4F * y[0] - 0.2F * y[1] + 0.2F * y[3] + 0.4F * y[4];
    const float den = 0.571429F * y[0] - 0.285714F * y[1] - 0.571429F * y[2] - 0.285714F * y[3] +
                      0.571429F * y[4];
    result = -num / den;
  } else {
    // num_weights_3 / den_weights_3, whose correction factor is 0.5.
    const float num = -0.5F * y[0] + 0.5F * y[2];
    const float den = 0.5F * y[0] - 1.0F * y[1] + 0.5F * y[2];
    result = -0.5F * num / den;
  }
  if (isnan(result) || isinf(result) || (fabs(result) > 1.0F)) {
    return 0.0F;
  }
  return result;
}

kernel void mmse_ta_profile(device const float*      slices [[buffer(0)]], // [nof_slices][size] complex pairs
                            device float*            out [[buffer(1)]],    // one value: seconds
                            constant mmse_ta_params& p [[buffer(2)]],
                            uint                     tid [[thread_position_in_threadgroup]])
{
  threadgroup float profile[mmse_ta_max_size];

  // Every parameter is CLAMPED into its compile-time range first (see the bounds at the top of this
  // file). From here on they only decide what is skipped and what is written - never whether the
  // kernel ends. A geometry outside the bounds therefore produces a wrong estimate, which the probe
  // sees, instead of a dispatch that never returns.
  const uint size = ((p.size <= mmse_ta_max_size) && (p.size >= mmse_ta_max_size_min)) ? p.size : 0u;
  if (size == 0u) {
    // Nothing to reduce. Written as a zero so the caller reads a value rather than a stale slot; the
    // return precedes the kernel's only barrier, so every thread in the group takes it together.
    if (tid == 0u) {
      out[0] = 0.0F;
    }
    return;
  }
  const uint nof_slices = (p.nof_slices <= mmse_ta_max_slices) ? p.nof_slices : 0u;
  // The window cannot exceed the profile: the host slices correlation.first(max_ta_samples) off a
  // buffer of `size`, so a wider window would read past it - and the parabolic fit below indexes
  // relative to the peak, so that overflow would become a negative index, not a clamped one.
  const uint window = (p.max_ta_samples < size) ? p.max_ta_samples : size;

  // Power delay profile: |.|^2 accumulated over the hop's slices, one tap per thread per pass.
  // `tap < size` cannot run away: size is clamped to mmse_ta_max_size, and the profile is that big.
  for (uint tap = tid; tap < size; tap += mmse_ta_tg_size) {
    float acc = 0.0F;
    for (uint s = 0; s != mmse_ta_max_slices; ++s) {
      if (s >= nof_slices) {
        break;
      }
      device const float* z = slices + (static_cast<ulong>(s) * size + tap) * 2ul;
      acc += z[0] * z[0] + z[1] * z[1];
    }
    profile[tap] = acc;
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // The search and the refinement are one thread's work: the window is tens of taps (35 at 30 kHz,
  // 100 MHz of sampling rate), so a scan is cheaper than the reduction that would parallelise it -
  // and it keeps the "which tap won" index, which a value-only reduction cannot carry.
  threadgroup uint tg_idx_biased; // winning tap, biased by the window so it is never negative
  threadgroup uint tg_delayed;    // 1 when the delayed side won (ties included, like the host's >=)
  if (tid == 0u) {
    // Delayed taps start the circular profile; advanced taps end it. Ties go to the delayed side,
    // as the host's >= does.
    uint  delay_idx    = 0u;
    uint  advance_idx  = 0u;
    float delay_best   = profile[0];
    float advance_best = profile[size - window];
    for (uint tap = 1u; tap < window; ++tap) {
      if (profile[tap] > delay_best) {
        delay_best = profile[tap];
        delay_idx  = tap;
      }
      const float a = profile[size - window + tap];
      if (a > advance_best) {
        advance_best = a;
        advance_idx  = tap;
      }
    }
    const bool delayed_wins = (delay_best >= advance_best);
    // Publish the signed index BIASED by the window, so it is never negative, plus which side won.
    // Nothing signed crosses a threadgroup variable - that is what the first version got wrong (it
    // packed a signed index into a uint and every negative delay read back as 0) - and the bias is
    // exactly the window, so the distance from either end is recoverable without a second value.
    const int idx = delayed_wins ? static_cast<int>(delay_idx) : -static_cast<int>(window - advance_idx);
    tg_idx_biased = static_cast<uint>(idx + static_cast<int>(window));
    tg_delayed    = delayed_wins ? 1u : 0u;

  }
  threadgroup_barrier(mem_flags::mem_threadgroup);

  if (tid == 0u) {
    // Undo the bias. The host's convention, kept verbatim: a delayed tap is the profile's index, an
    // advanced tap is negative and measured from the end.
    const int idx = static_cast<int>(tg_idx_biased) - static_cast<int>(window);

    // The refinement, gated exactly as the host gates it: only when the profile is NOT the full
    // transform (at the full size the integer tap is the resolution by construction).
    float fractional = 0.0F;
    if (size != mmse_ta_max_size) {
      float taps[mmse_ta_max_taps];
      // The tap count is a parameter, so it is clamped to the two the fit accepts (see
      // curve_fitting_find_max.cpp: three or five, and anything else means "no refinement").
      const uint nof_taps = ((p.nof_taps == 3u) || (p.nof_taps == 5u)) ? p.nof_taps : 0u;
      // NOTE: not 'half' - that is an MSL builtin type name, and shadowing it is a compile error.
      const uint nof_half = nof_taps / 2u;
      for (uint i = 0u; i != mmse_ta_max_taps; ++i) {
        if (i >= nof_taps) {
          break;
        }
        const int t = idx + static_cast<int>(i) - static_cast<int>(nof_half);
        // The profile is circular - and `size` is clamped, so the modulus is never by zero and the
        // wrapped index is inside the threadgroup array by construction.
        const int size_i  = static_cast<int>(size);
        const int wrapped = ((t % size_i) + size_i) % size_i;
        taps[i]           = profile[wrapped];
      }
      fractional = (nof_taps != 0u) ? mmse_ta_frac_from_taps(taps, nof_taps) : 0.0F;
    }

    // Sampling rate the transform size, the subcarrier spacing and the pilot stride imply - the same
    // expression (and therefore the same resolution) the host's estimator uses.
    // The sampling rate the transform size, the spacing and the stride imply. A zero rate is a
    // parameter error: publish zero rather than an infinity the caller would have to guard.
    const float rate_hz = static_cast<float>(size) * p.scs_hz * static_cast<float>(p.stride);
    out[0]              = (rate_hz > 0.0F) ? ((static_cast<float>(idx) + fractional) / rate_hz) : 0.0F;
  }
}

// ---------------------------------------------------------------------------------------------
// K7: places a hop's pilots into a DFT input, zero-padded, in the positions the host's estimator
// uses - so that the existing transform kernel (ocudu_dft.metal, dft_dit, INVERSE) can turn them
// into the slices K6 reduces.
//
// ---- Why this is a kernel and not a host step ----
// The transform is the DFT engine's, and it reads its input as `in[batch_offset + perm[i]]`: it has
// no subcarrier stride it could gather with. That leaves two ways to get the pilots in - assemble
// them on the host (a per-hop copy of every pilot plus a blit that has to commit and wait, which is
// the wait moving the value was meant to remove), or place them on the device. This is the second.
//
// ---- The input is h, the very buffer K5 reads -----------------------------------------------
// The host's TA input is `filtered_pilots_lse`, which for this backend is filled by
// pending_fill::fill() out of the estimated grid - and that grid is a host copy of THIS buffer
// (port_channel_estimator_metal_mmse_impl::unpack_engine_group() copies gpu_h into grid_est, and the
// pilot REs are then sampled at the layer's comb). So the two sides read the same estimates at the
// same places, and the placement below is the host's, term for term:
//
//   * the pilots of one DM-RS symbol and one layer are a slice;
//   * a pilot's transform POSITION is (subcarrier - lowest_comb_bit) / stride, where `stride` is the
//     value the host hands its estimator: the comb spacing for a contiguous RB allocation (1 PUCCH
//     f1/3/4, 2 PUSCH comb-2, 3 PUCCH f2), and 1 for the sparse-mask route, whose POSITIONS are the
//     subcarrier offsets themselves (estimate_time_alignment() in port_channel_estimator_helpers.cpp).
//     Both routes are the same expression, which is why one kernel serves both;
//   * every other position of the slice is ZERO. This kernel writes them: the host zeroes its input
//     buffer every call, and reproducing that here is what keeps the transform input self-contained -
//     a caller that only filled the pilot positions would transform the previous hop's pilots as well.
//
// ---- The DC subcarrier is NOT skipped, unlike K5 ---------------------------------------------
// K5 leaves the DC RE out of its rsrp sum, but the host's TA input is a copy of the grid at every
// comb position of every DM-RS symbol, DC included. So this kernel copies it too: its job is to
// reproduce the host's TA input exactly, not to decide which REs are meaningful.
//
// ---- The order is NATURAL, and that was established by measurement, not by reading. --------------
// The transform's load looks like a digit-reversal gather (`buf[i] = in[perm[i]]`), which reads as
// "the input must be stored digit-reversed" - and the first version of this kernel believed it. It is
// wrong: the gathered `buf` is then walked by DIT butterflies, so it is the GATHER that produces the
// reversed order the first stage needs. Feeding it reversed input double-reverses the transform.
//
// Measured, on the same pilots: stored natural -> the transform is flat (a unit tone's IDFT is a
// constant, flatness 1.0000); stored reversed -> flatness 330. So: pilot for position j goes to j.

/// Compile-time geometry bounds of this kernel (they mirror the estimator's own limits, and the unit
/// test asserts that they agree with the host's).
constant uint mmse_ta_max_layers    = 4;    // MAX_LAYERS
constant uint mmse_ta_max_slot_symb = 14;   // MAX_NSYMB_PER_SLOT
constant uint mmse_ta_max_scr       = 3300; // MAX_NOF_SUBCARRIERS: a block's subcarrier count
constant uint mmse_ta_max_stride    = 3;    // the widest comb the host's estimator is called with
constant uint mmse_ta_max_dmrs      = 4;    // MAX_DMRS_SYMBOLS: DM-RS symbols of ONE hop

/// The geometry of the hop, field for field the leading part of mmse_rsrp_params: the two kernels
/// read the SAME buffer with the same indexing, so a field that drifts between them is a silent
/// disagreement about which subcarrier is which. \c dc_sc is unused here (see the note above) and
/// kept so that the two structs stay comparable field by field.
struct mmse_ta_place_params {
  uint nout_stride;    // Output positions per block in the batch (row length of h, in complex values).
  uint n_blk;          // STANDARD blocks per system in the batch (the edge block is slot n_blk on).
  uint nf_std;         // Subcarriers per standard block.
  uint sc_tail_base;   // First subcarrier of the edge block (no tail: nof_sub).
  uint nf_tail;        // Subcarriers of the edge block (0 when there is none).
  uint sys_tail;       // First system of the edge block.
  uint nof_layers;
  uint nof_symbols;
  uint dc_sc;          // Unused here: the host's TA input includes the DC comb position.
  uint dmrs_sym_bits;  // DM-RS symbols of the slot (one bit per symbol).
  uint pilot_re_bits[4]; // The layer's own pilot comb within a PRB.
  uint size;           // Transform size of one slice (positions per slice, a power of two).
  uint stride;         // Pilot spacing in subcarriers, as the host's estimator is called with.
  // The hop's own DM-RS symbols, ascending - NOT the slot's (dmrs_sym_bits above): a hop of a
  // frequency-hopping slot carries only some of them, and the host's estimator enumerates exactly the
  // hop's (pilots_lse.size().nof_symbols). Slice s of this dispatch is dmrs_slots[s].
  uint nof_dmrs_symbols;
  uint dmrs_slots[mmse_ta_max_dmrs];
  uint pad0;
};

kernel void mmse_ta_place(device const float*             h [[buffer(0)]],  // [nof_systems][n_blk slots][2*nout_stride]
                          device float*                   dst [[buffer(2)]], // [nof_slices][size] complex
                          constant mmse_ta_place_params&  p [[buffer(3)]],
                          uint                            tgid [[threadgroup_position_in_grid]],
                          uint                            tid [[thread_position_in_threadgroup]])
{
  // ---- THE HANG THAT MOTIVATED EVERY LINE BELOW -------------------------------------------------
  // The first version of this kernel bounded its loops by the parameter block (nof_slices,
  // nof_pilots) and read the threadgroup size from [[threads_per_threadgroup]]. Its first dispatch
  // never returned: the GPU stayed busy, the WindowServer watchdog fired, `kill` could not release
  // the GPU and even `reboot` hung - the machine had to be powered off (full_gpu_chain §48.131,
  // which documents the first occurrence of exactly this, and §48.133 for why nothing recovers it).
  //
  // So: the parameters are clamped into compile-time ranges FIRST, every loop is bounded by a
  // constant with the parameter only breaking out or skipping, and every index is bounded by
  // construction. A parameter error now yields a WRONG RESULT - visible, testable - and never a hang.
  //
  // The subcarrier count is also a DIVISOR below, so a zero or out-of-range one has to be rejected
  // rather than clamped to something that still divides: an integer division by zero is exactly the
  // kind of undefined behaviour that ends as a dispatch that never returns.
  const uint nof_layers_c = (p.nof_layers <= mmse_ta_max_layers) ? p.nof_layers : 0u;
  const uint size         = ((p.size <= mmse_ta_max_size) && (p.size >= mmse_ta_max_size_min)) ? p.size : 0u;
  const uint stride       = ((p.stride >= 1u) && (p.stride <= mmse_ta_max_stride)) ? p.stride : 0u;
  const uint nf_std       = ((p.nf_std >= 1u) && (p.nf_std <= mmse_ta_max_scr)) ? p.nf_std : 0u;
  const uint nf_tail      = (p.nf_tail <= mmse_ta_max_scr) ? p.nf_tail : 0u;
  if ((nof_layers_c == 0u) || (size == 0u) || (stride == 0u) || (nf_std == 0u)) {
    return; // no barrier below this point's reach: this return precedes every barrier in the kernel
  }

  // One threadgroup per SLICE: the s-th DM-RS symbol of the HOP crossed with the layers, in the
  // host's own slice order (symbol-major, see estimate_time_alignment()).
  const uint nof_dmrs = (p.nof_dmrs_symbols <= mmse_ta_max_dmrs) ? p.nof_dmrs_symbols : 0u;
  if (nof_dmrs == 0u) {
    return;
  }
  const uint lay = tgid % nof_layers_c;
  const uint idx = tgid / nof_layers_c; // which of the hop's DM-RS symbols this slice is
  if (idx >= nof_dmrs) {
    return; // a slice past the hop's DM-RS symbols: nothing to place
  }
  const uint sym = p.dmrs_slots[idx];
  if (sym >= mmse_ta_max_slot_symb) {
    return; // a slot symbol index outside the slot: a parameter error, and nothing to read
  }

  const uint comb = p.pilot_re_bits[lay];
  if (comb == 0u) {
    return; // a layer without a comb has no pilots to place
  }
  // The lowest set bit of the comb: the subcarrier offset a pilot's position is measured from. Both
  // sides derive their positions from it, so it must come out of the SAME mask the host samples.
  const uint low_bit = ctz(comb);

  const uint std_sc   = p.n_blk * nf_std; // subcarriers covered by the standard blocks
  const uint tail_end = (nf_tail != 0u) ? (p.sc_tail_base + nf_tail) : 0u;
  device float* slice = dst + static_cast<ulong>(tgid) * (static_cast<ulong>(size) * 2ul);

  // Every position of the slice is written: a pilot position gets the estimate, every other one a
  // zero (see the header). The loop is bounded by the transform's compile-time maximum and the
  // parameter only breaks it - see the bounds note above.
  for (uint j = tid; j != mmse_ta_max_size; j += mmse_ta_tg_size) {
    if (j >= size) {
      break;
    }
    float re = 0.0F;
    float im = 0.0F;
    // The subcarrier this position holds a pilot of, and where the host's estimator would read it.
    const uint sc = j * stride + low_bit;
    if (sc < std_sc) {
      // A standard block: block sc / nf_std, local subcarrier sc % nf_std. nf_std is non-zero (it is
      // one of the rejections above), so the divisions are defined.
      const uint b     = sc / nf_std;
      const uint local = sc - b * nf_std;
      // Only a comb position carries a pilot - the positions between two pilots of a sparse hop are
      // zeros on both sides, and this is what makes one kernel serve the sparse route too.
      if (((comb >> (local % 12u)) & 1u) != 0u) {
        device const float* hp = h + (static_cast<ulong>(lay) * p.n_blk + b) * (2u * p.nout_stride) +
                                 2u * (sym * nf_std + local);
        re = hp[0];
        im = hp[1];
      }
    } else if ((nf_tail != 0u) && (sc >= p.sc_tail_base) && (sc < tail_end)) {
      // The edge block: block 0 of the systems from sys_tail on (see mmse_rsrp, which reads the same
      // slot the same way).
      const uint local = sc - p.sc_tail_base;
      if (((comb >> (local % 12u)) & 1u) != 0u) {
        device const float* hp =
            h + (static_cast<ulong>(p.sys_tail + lay) * p.n_blk) * (2u * p.nout_stride) + 2u * (sym * nf_tail + local);
        re = hp[0];
        im = hp[1];
      }
    }
    // NATURAL order (see the note above): position j is transform input j.
    slice[2u * j]      = re;
    slice[2u * j + 1u] = im;
  }
}

// ---------------------------------------------------------------------------------------------
// The FUSED chain (batch 5d): placement + transform + profile + peak, in ONE dispatch.
//
// ---- Why one kernel instead of three ----
//
// The port started as three dispatches (K7 places the pilots, the DFT engine's dft_dit transforms
// them, K6 reduces the profile), which is the natural decomposition and the one the ladder validates
// kernel by kernel. On air the three of them cost the lane's weights command buffer ~50us/lane
// (measured: ch_wt 352.3us before the port, 399.8/406.1us with it - design doc 17.10.5), while the
// arithmetic is nothing: a 128-point transform is a few thousand flops. Two experiments removed the
// obvious candidates for that cost - the kernel's 32 KB static threadgroup scratch (a threadgroup
// ARGUMENT version measured the same GPU window) and the two scope-wide memory barriers (removing
// them left the offline per-hop delta unchanged) - which leaves the dispatches themselves.
//
// So they become one. The whole chain needs ONE threadgroup anyway: production transforms 2..3 slices
// of 128/256 points, and the peak search is a single thread's scan over ~9 taps. Doing all of it in
// one threadgroup also removes the intermediate spectra buffer (13 KB per hop that no longer has to be
// written and read), the two cross-dispatch dependencies, and the switch between two metallibs.
//
// ---- Bounds (unchanged discipline, see the note at the top of this file) ----
//
// Every loop is bounded by a compile-time constant and the parameters only break or skip. The one
// place this kernel is narrower than the three-dispatch route is the transform size: the profile it
// keeps in threadgroup memory is what caps it at mmse_ta_chain_max_size (2048 = the largest size the
// estimator's get_idft() can ask for, since 275 PRB x 6 pilots scales to exactly 2048). A caller with
// a wider transform keeps the three-dispatch route (or the host).

constant uint mmse_ta_chain_max_size = 2048;

struct mmse_ta_chain_params {
  // The hop's geometry, field for field mmse_ta_place_params' leading part (and K5's): the same h is
  // indexed the same way by every kernel that reads it.
  uint  nout_stride;
  uint  n_blk;
  uint  nf_std;
  uint  sc_tail_base;
  uint  nf_tail;
  uint  sys_tail;
  uint  nof_layers;
  uint  nof_symbols;
  uint  dc_sc;           // unused here (the host's TA input includes the DC comb position)
  uint  dmrs_sym_bits;   // unused here (the hop's own symbols travel in dmrs_slots)
  uint  pilot_re_bits[4];
  uint  size;            // transform size of one slice
  uint  stride;          // pilot spacing in subcarriers, as the host's estimator is called with
  uint  nof_dmrs_symbols; // slices = nof_dmrs_symbols x nof_layers
  uint  dmrs_slots[mmse_ta_max_dmrs];
  uint  radix2;          // log2(size): the twiddle table is the DFT engine's, built for this size
  uint  max_ta_samples;  // half-cyclic-prefix search window, in taps
  float scs_hz;
  uint  nof_taps;        // 5 or 3: the taps the parabolic refinement fits over
  uint  pad0;
  uint  pad1;
};

kernel void mmse_ta_chain(device const float*              h [[buffer(0)]],  // [nof_systems][n_blk slots][2*nout_stride]
                          device float*                    out [[buffer(1)]], // one value: seconds
                          device const float2*             twiddle [[buffer(2)]], // N/2 roots of unity
                          constant mmse_ta_chain_params&   p [[buffer(3)]],
                          device const uint*               perm [[buffer(4)]], // digit-reversed input index
                          uint                             tid [[thread_position_in_threadgroup]])
{
  // One threadgroup holds both the transform scratch and the profile. 2048 complex values (16 KB) +
  // 2048 floats (8 KB) = 24 KB, inside the 32 KB a threadgroup may use on an Apple GPU.
  threadgroup float2 work[mmse_ta_chain_max_size];
  threadgroup float  profile[mmse_ta_chain_max_size];

  const uint nof_layers_c = (p.nof_layers <= mmse_ta_max_layers) ? p.nof_layers : 0u;
  const uint size = ((p.size >= mmse_ta_max_size_min) && (p.size <= mmse_ta_chain_max_size)) ? p.size : 0u;
  const uint stride = ((p.stride >= 1u) && (p.stride <= mmse_ta_max_stride)) ? p.stride : 0u;
  const uint nf_std = ((p.nf_std >= 1u) && (p.nf_std <= mmse_ta_max_scr)) ? p.nf_std : 0u;
  const uint nf_tail = (p.nf_tail <= mmse_ta_max_scr) ? p.nf_tail : 0u;
  const uint nof_dmrs = (p.nof_dmrs_symbols <= mmse_ta_max_dmrs) ? p.nof_dmrs_symbols : 0u;
  const uint radix2 = (p.radix2 <= 12u) ? p.radix2 : 0u; // 2^12 = 4096 = the widest table there is
  if ((nof_layers_c == 0u) || (size == 0u) || (stride == 0u) || (nf_std == 0u) || (nof_dmrs == 0u) ||
      (radix2 == 0u) || ((1u << radix2) != size)) {
    if (tid == 0u) {
      out[0] = 0.0F; // a parameter error publishes a zero, not a stale slot (see mmse_ta_profile)
    }
    return; // precedes every barrier below
  }
  // The window cannot exceed the profile: the host slices correlation.first(max_ta_samples) off a
  // buffer of `size` (see mmse_ta_profile, where the same clamp is a correctness requirement of the
  // parabolic fit's negative indices).
  const uint window = (p.max_ta_samples < size) ? p.max_ta_samples : size;

  const uint threads = min(size, 1024u);
  const uint std_sc = p.n_blk * nf_std; // subcarriers covered by the standard blocks
  const uint tail_end = (nf_tail != 0u) ? (p.sc_tail_base + nf_tail) : 0u;
  const uint nof_slices = nof_dmrs * nof_layers_c;
  const float tw_sign = -1.0F; // INVERSE: the estimator's IDFT (see dft_dit's `inverse`)

  if (tid >= threads) {
    // No barrier below this point's reach: the early return precedes the slice loop's first barrier,
    // and every barrier in this kernel is inside that loop (all threads of the group reach this test
    // with the same answer, so the group stays uniform).
    return;
  }

  for (uint slice = 0u; slice != mmse_ta_max_slices; ++slice) {
    if (slice >= nof_slices) {
      break;
    }
    // Slice s is the s-th DM-RS symbol of the HOP (not of the slot: a hopping hop carries a subset)
    // crossed with the layers, which is the order the host's estimator enumerates them in.
    const uint lay = slice % nof_layers_c;
    const uint sym = p.dmrs_slots[slice / nof_layers_c];
    if (sym >= mmse_ta_max_slot_symb) {
      break; // a slot symbol index outside the slot: a parameter error, and nothing to read
    }
    const uint comb = p.pilot_re_bits[lay];
    const uint low_bit = (comb != 0u) ? ctz(comb) : 0u;

    // ---- 1) place: element j holds the pilot of subcarrier perm[j]*stride + low_bit, or a zero ----
    //
    // The permutation is the one thing the fused kernel must reproduce from the route it replaces:
    // dft_dit loads its scratch with `buf[i] = in[perm[i]]`, so the butterflies below (the same
    // function, ocudu_dft_butterflies.h) expect the input to have been gathered that way. Writing the
    // pilots in natural order - which is what the input BUFFER holds, and what mmse_ta_place writes -
    // would double-reverse the transform (see mmse_ta_place's note: measured as a flatness of 330
    // instead of 1).
    for (uint j = tid; j != mmse_ta_max_size; j += threads) {
      if (j >= size) {
        break;
      }
      float re = 0.0F;
      float im = 0.0F;
      if (comb != 0u) {
        const uint sc = perm[j] * stride + low_bit;
        if (sc < std_sc) {
          const uint b = sc / nf_std;
          const uint local = sc - b * nf_std;
          if (((comb >> (local % 12u)) & 1u) != 0u) {
            device const float* hp = h + (static_cast<ulong>(lay) * p.n_blk + b) * (2u * p.nout_stride) +
                                     2u * (sym * nf_std + local);
            re = hp[0];
            im = hp[1];
          }
        } else if ((nf_tail != 0u) && (sc >= p.sc_tail_base) && (sc < tail_end)) {
          const uint local = sc - p.sc_tail_base;
          if (((comb >> (local % 12u)) & 1u) != 0u) {
            device const float* hp = h + (static_cast<ulong>(p.sys_tail + lay) * p.n_blk) * (2u * p.nout_stride) +
                                     2u * (sym * nf_tail + local);
            re = hp[0];
            im = hp[1];
          }
        }
      }
      work[j] = float2(re, im);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ---- 2) the transform, in place: the very butterflies dft_dit runs (ocudu_dft_butterflies) ----
    ocudu_dft_butterflies(work, size, radix2, /*radix3=*/0u, twiddle, tid, threads, tw_sign);
    threadgroup_barrier(mem_flags::mem_threadgroup);

    // ---- 3) accumulate the power delay profile ----
    for (uint i = tid; i != mmse_ta_max_size; i += threads) {
      if (i >= size) {
        break;
      }
      const float p2 = work[i].x * work[i].x + work[i].y * work[i].y;
      profile[i] = (slice == 0u) ? p2 : (profile[i] + p2);
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  // ---- 4) the half-cyclic-prefix search and the parabolic refinement (one thread) ----
  if (tid != 0u) {
    return;
  }
  // Delayed taps start the circular profile, advanced taps end it; ties go to the delayed side, as
  // the host's >= does.
  uint  delay_idx = 0u;
  uint  advance_idx = 0u;
  float delay_best = profile[0];
  float advance_best = profile[size - window];
  for (uint tap = 1u; tap != window; ++tap) {
    if (profile[tap] > delay_best) {
      delay_best = profile[tap];
      delay_idx = tap;
    }
    const float a = profile[size - window + tap];
    if (a > advance_best) {
      advance_best = a;
      advance_idx = tap;
    }
  }
  const bool delayed_wins = (delay_best >= advance_best);
  const int  idx = delayed_wins ? static_cast<int>(delay_idx) : -static_cast<int>(window - advance_idx);

  // The refinement, gated exactly as the host gates it: only when the profile is NOT the full
  // transform (at the full size the integer tap is the resolution by construction).
  float fractional = 0.0F;
  if (size != mmse_ta_max_size) {
    float taps[mmse_ta_max_taps];
    const uint nof_taps = ((p.nof_taps == 3u) || (p.nof_taps == 5u)) ? p.nof_taps : 0u;
    const uint nof_half = nof_taps / 2u; // NOTE: not 'half', an MSL builtin type name
    for (uint i = 0u; i != mmse_ta_max_taps; ++i) {
      if (i >= nof_taps) {
        break;
      }
      const int t = idx + static_cast<int>(i) - static_cast<int>(nof_half);
      const int size_i = static_cast<int>(size);
      const int wrapped = ((t % size_i) + size_i) % size_i;
      taps[i] = profile[wrapped];
    }
    fractional = (nof_taps != 0u) ? mmse_ta_frac_from_taps(taps, nof_taps) : 0.0F;
  }

  const float rate_hz = static_cast<float>(size) * p.scs_hz * static_cast<float>(stride);
  out[0] = (rate_hz > 0.0F) ? ((static_cast<float>(idx) + fractional) / rate_hz) : 0.0F;
}
