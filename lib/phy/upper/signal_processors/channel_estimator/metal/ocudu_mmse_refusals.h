// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <optional>

namespace ocudu {
namespace metal {

/// \brief Why a device-side stage of the MMSE channel estimator did NOT run for a hop (batch S13-P1).
///
/// Every stage of the estimator has a device route and a host fallback, and the fallback is a
/// LEGITIMATE answer - a geometry the kernels do not cover, a knob that turns them off, a metallib
/// without the kernel. What was missing is the count: the crossing contract can only say THAT the lane
/// touched the host (a read, a write), and the estimator could not say WHICH gate had refused. The
/// fused lane's whole claim is about the paths it covers, and a claim whose exceptional paths are
/// invisible is a claim about the hops that happened to be measured.
///
/// \note The reasons are deliberately per-reason and not per-stage: "the device LSE did not run" is not
///       actionable, while "its geometry was refused" (an allocation the kernels do not cover, i.e. an
///       applicability limit) and "the device is switched off" (a knob, i.e. a deliberate A/B arm) are
///       two different findings. Several of these count DECISIONS rather than hops: the y scatter is
///       asked once per group of a batch, and the geometry gates of a stage are evaluated once per
///       hop. The names below say which side of that line each one is on.
enum class mmse_refusal : unsigned {
  /// The device least-squares build is switched off (OCUDU_CE_CPU_LS=1). Per hop.
  ls_disabled,
  /// The hop's geometry is outside the extraction kernel's contract (non-contiguous allocation,
  /// pilot count mismatch, no device grid view). Per hop - and this is the APPLICABILITY limit the
  /// sparse-RB-mask work (S13-P3) is about.
  ls_geometry,
  /// The geometry was fine and the engine call still refused (no engine, a metallib without the
  /// kernels, or the call's own failure - which also logs). Per hop.
  ls_build,
  /// The device pilot scatter (glue #2) is switched off (OCUDU_CE_DEV_Y=0). Per group.
  y_disabled,
  /// The device did not build this hop's least-squares pilots, which the scatter reads. Per group.
  y_no_ls,
  /// No engine or no scatter kernel in the metallib. Per group.
  y_no_kernel,
  /// The group's geometry is outside the scatter kernel's contract. Per group.
  y_geometry,
  /// The descriptor table is full (too many groups in flight). Per group.
  y_capacity,
  /// The direct LSE read (lever C of dev doc 6.61) is switched off (OCUDU_CE_Y_DIRECT=0). Per batch.
  /// A knob, i.e. the A/B arm of the route that removes the scatter dispatch.
  y_direct_disabled,
  /// The metallib does not carry mmse_apply_lse. Per batch.
  y_direct_no_kernel,
  /// This batch staged no y descriptor at all, so there is no group geometry to read the LSE with
  /// (the host staged y itself, or every group was refused above). Per batch.
  y_direct_no_source,
  /// The staged groups do not agree on ONE LSE buffer, or their system ranges do not cover this batch
  /// (a gap, an overlap, or a group that belongs to another batch). Per batch.
  y_direct_coverage,
  /// A group's geometry is outside mmse_apply_lse's contract: a zero npf / nof_symb / n_blk_real, a
  /// block walk leaving the hop's pilots, or a read past the LSE buffer. Per batch.
  y_direct_geometry,
  /// The device correlation build (K0-d) is switched off (OCUDU_CE_CORR_DEV=0). Per group.
  corr_disabled,
  /// The hop has no device statistics to build the matrices from. Per group.
  corr_geometry,
  /// The device time alignment is switched off (OCUDU_CE_DEV_TA=0), or there is no engine. Per hop.
  ta_disabled,
  /// The hop's RB mask is not contiguous, or its pilot comb is not one the kernel reproduces. Per hop.
  ta_stride,
  /// The transform the host's own estimator would use is wider than the fused TA chain supports
  /// (get_idft() > 2048) - the S13 G3b applicability limit. Per hop.
  ta_dft_too_wide,
  /// A zero transform size or a zero search window: a geometry the host estimator itself refuses.
  ta_geometry,
  /// The device noise variance (K2/K4) is switched off (OCUDU_CE_DEV_SIGMA2=0). Per hop.
  sigma2_disabled,
  /// The sigma2 block's geometry or inputs are outside its contract (the engine skips the stage and
  /// says so through sigma2_done; it also logs the geometry once). Per hop.
  sigma2_geometry,
  /// The equalizer's device estimates (K3) are switched off (OCUDU_CE_CPU_CE=1). Per hop.
  k3_disabled,
  /// The hop is not covered by K3 (no RE masks, the matrix flavor, or an empty allocation). Per hop.
  k3_geometry,
  /// Number of reasons. Keep last.
  count
};

/// Reason name, as printed. Must cover every enumerator (no default: a new reason must be named).
constexpr const char* to_string(mmse_refusal reason)
{
  switch (reason) {
    case mmse_refusal::ls_disabled:
      return "ls_disabled";
    case mmse_refusal::ls_geometry:
      return "ls_geometry";
    case mmse_refusal::ls_build:
      return "ls_build";
    case mmse_refusal::y_disabled:
      return "y_disabled";
    case mmse_refusal::y_no_ls:
      return "y_no_ls";
    case mmse_refusal::y_no_kernel:
      return "y_no_kernel";
    case mmse_refusal::y_geometry:
      return "y_geometry";
    case mmse_refusal::y_capacity:
      return "y_capacity";
    case mmse_refusal::y_direct_disabled:
      return "y_direct_disabled";
    case mmse_refusal::y_direct_no_kernel:
      return "y_direct_no_kernel";
    case mmse_refusal::y_direct_no_source:
      return "y_direct_no_source";
    case mmse_refusal::y_direct_coverage:
      return "y_direct_coverage";
    case mmse_refusal::y_direct_geometry:
      return "y_direct_geometry";
    case mmse_refusal::corr_disabled:
      return "corr_disabled";
    case mmse_refusal::corr_geometry:
      return "corr_geometry";
    case mmse_refusal::ta_disabled:
      return "ta_disabled";
    case mmse_refusal::ta_stride:
      return "ta_stride";
    case mmse_refusal::ta_dft_too_wide:
      return "ta_dft_too_wide";
    case mmse_refusal::ta_geometry:
      return "ta_geometry";
    case mmse_refusal::sigma2_disabled:
      return "sigma2_disabled";
    case mmse_refusal::sigma2_geometry:
      return "sigma2_geometry";
    case mmse_refusal::k3_disabled:
      return "k3_disabled";
    case mmse_refusal::k3_geometry:
      return "k3_geometry";
    case mmse_refusal::count:
      break;
  }
  return "unknown";
}

/// \brief Whether a reason is a KNOB the operator turned, rather than a hop the device cannot serve.
///
/// The fused lane treats the two differently (user ruling, 2026-09-20, design document section 1.8):
/// an arm the operator asked for - `OCUDU_CE_CPU_LS=1`, `OCUDU_CE_DEV_Y=0`, `OCUDU_CE_CORR_DEV=0`,
/// `OCUDU_CE_DEV_TA=0`, `OCUDU_CE_DEV_SIGMA2=0`, `OCUDU_CE_CPU_CE=1` - keeps its host route, because
/// that host route IS the arm. A hop the device could not serve is a different finding: in `mode=gpu`
/// the host must not cover it, so the consumer fails the grant instead (see phy_pipeline_strict.h).
///
/// This is the whole classification: the enum's own `*_disabled` entries are knobs, everything else
/// (geometry, missing kernel, full table, a build that failed) is the device being unable.
constexpr bool is_knob_refusal(mmse_refusal reason)
{
  switch (reason) {
    case mmse_refusal::ls_disabled:
    case mmse_refusal::y_disabled:
    case mmse_refusal::y_direct_disabled:
    case mmse_refusal::corr_disabled:
    case mmse_refusal::ta_disabled:
    case mmse_refusal::sigma2_disabled:
    case mmse_refusal::k3_disabled:
      return true;
    default:
      return false;
  }
}

/// \brief Counters of the estimator's device-path refusals, printed with the engine statistics.
///
/// Never destroyed on purpose: the report runs from an atexit handler, which runs after the static
/// destructors of this translation unit (the same reason phy_pipeline_crossings heap-allocates its
/// counters).
class mmse_refusals
{
public:
  static void count(mmse_refusal reason)
  {
    counters()[static_cast<unsigned>(reason)].fetch_add(1, std::memory_order_relaxed);
    hop_reasons() |= (1U << static_cast<unsigned>(reason));
  }

  // ---- The hop-local view -----------------------------------------------------------------------
  //
  // WHY it exists: the process-wide counters answer "how many refusals did this run have", which is
  // the wrong question for a consumer that has to decide about ONE hop - it needs to know WHICH hop
  // was refused and WHY, and it needs to know whether the refusal was a knob (an arm the operator
  // asked for) or the device being unable. A delta over the process-wide counters cannot answer that
  // when several PUSCH workers run at once; a thread_local mask can, because one hop is processed by
  // one thread.
  //
  // WHAT is recorded: the reason with the LOWEST enumerator index, and the enum is ordered by STAGE
  // (ls -> y -> corr -> ta -> sigma2 -> k3), with the `*_disabled` knob of a stage before its geometry
  // reasons. So "lowest index" is "earliest stage", which is the ROOT CAUSE: a refused stage cascades
  // (no device LSE makes the y scatter refuse, which makes the correlation refuse), and a knob that
  // disabled an early stage explains everything downstream of it. Both matter, because the consumer's
  // decision is "was this an arm the operator asked for, or a hop the device could not serve".

  /// \brief Marks the start of a hop: clears the thread-local reason mask. Called once per hop.
  static void begin_hop() { hop_reasons() = 0; }

  /// \brief The first reason counted since begin_hop(), or nullopt when the hop was not refused.
  static std::optional<mmse_refusal> first_hop_reason()
  {
    const uint32_t mask = hop_reasons();
    for (unsigned i = 0; i != static_cast<unsigned>(mmse_refusal::count); ++i) {
      if ((mask & (1U << i)) != 0) {
        return static_cast<mmse_refusal>(i);
      }
    }
    return std::nullopt;
  }

  static uint64_t get(mmse_refusal reason)
  {
    return counters()[static_cast<unsigned>(reason)].load(std::memory_order_relaxed);
  }

  static uint64_t total()
  {
    uint64_t sum = 0;
    for (unsigned i = 0; i != static_cast<unsigned>(mmse_refusal::count); ++i) {
      sum += counters()[i].load(std::memory_order_relaxed);
    }
    return sum;
  }

  /// Prints the non-zero reasons, biggest first, or an explicit marker when there are none. The
  /// marker matters: "no line" would be indistinguishable from "the counters were not built".
  static void print(std::FILE* out)
  {
    bool any   = false;
    bool first = true;
    bool printed[static_cast<unsigned>(mmse_refusal::count)] = {};
    for (unsigned round = 0; round != static_cast<unsigned>(mmse_refusal::count); ++round) {
      unsigned best     = static_cast<unsigned>(mmse_refusal::count);
      uint64_t best_cnt = 0;
      for (unsigned i = 0; i != static_cast<unsigned>(mmse_refusal::count); ++i) {
        if (printed[i]) {
          continue;
        }
        const uint64_t c = counters()[i].load(std::memory_order_relaxed);
        if (c == 0) {
          printed[i] = true;
          continue;
        }
        if ((best == static_cast<unsigned>(mmse_refusal::count)) || (c > best_cnt)) {
          best     = i;
          best_cnt = c;
        }
      }
      if (best == static_cast<unsigned>(mmse_refusal::count)) {
        break;
      }
      printed[best] = true;
      any           = true;
      std::fprintf(out,
                   "%s%s=%llu",
                   first ? "" : " ",
                   to_string(static_cast<mmse_refusal>(best)),
                   static_cast<unsigned long long>(best_cnt));
      first = false;
    }
    if (!any) {
      std::fprintf(out, "<none>");
    }
  }

private:
  static std::atomic<uint64_t>* counters()
  {
    static std::atomic<uint64_t>* v = new std::atomic<uint64_t>[static_cast<unsigned>(mmse_refusal::count)];
    return v;
  }

  static uint32_t& hop_reasons()
  {
    static thread_local uint32_t mask = 0;
    return mask;
  }
};

} // namespace metal
} // namespace ocudu
