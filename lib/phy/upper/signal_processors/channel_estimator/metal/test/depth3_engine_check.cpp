// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Checks the depth-3 Core ML entry point against the contract's shapes.
///
/// Three questions, in the order that matters:
///   1. does the bundle carry the contract at all: one fp16 [1, nsc, 1, 64] input named `x` and the
///      four named heads (`init_depth3` refuses anything else);
///   2. did the forward pass actually WRITE the caller's buffers, checked by filling them with a
///      sentinel first. A pass that silently wrote nothing would leave a receiver reading stale
///      values that look like a result, which is the failure this workstream keeps meeting;
///   3. what the pass costs on the path a receiver would use, since the engine runs it on its own
///      worker thread rather than through a benchmarking harness.
///
/// Usage: depth3_engine_check <model.mlmodelc> [repeats] [subcarriers]

#include "ocudu_coreml_nn_engine.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

/// Fills fp16 values from a deterministic generator, so two runs of this check see the same input.
void fill_input(std::vector<__fp16>& plane, unsigned seed)
{
  std::mt19937                       generator(seed);
  std::uniform_real_distribution<float> distribution(-1.0F, 1.0F);
  for (__fp16& value : plane) {
    value = static_cast<__fp16>(distribution(generator));
  }
}

/// Counts how many values a head received, and how far they are from the sentinel.
struct head_report {
  size_t written     = 0;
  size_t nonzero     = 0;
  float  min_value   = 0.0F;
  float  max_value   = 0.0F;
};

head_report inspect(const std::vector<__fp16>& buffer, __fp16 sentinel)
{
  head_report report;
  for (__fp16 value : buffer) {
    const float as_float = static_cast<float>(value);
    if (value != sentinel) {
      ++report.written;
    }
    if (as_float != 0.0F) {
      ++report.nonzero;
    }
    report.min_value = std::min(report.min_value, as_float);
    report.max_value = std::max(report.max_value, as_float);
  }
  return report;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::printf("usage: depth3_engine_check <model.mlmodelc> [repeats] [subcarriers]\n");
    return 2;
  }
  const char*    model_path = argv[1];
  const unsigned repeats    = (argc > 2) ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 20;
  const unsigned nsc        = (argc > 3) ? static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10)) : 612;

  metal::coreml_nn_engine engine;
  if (!engine.init_depth3(model_path)) {
    std::printf("INIT_FAIL %s\n", model_path);
    return 1;
  }
  std::printf("init ok: %s (%u subcarriers)\n", model_path, nsc);

  // The main head is a function of the modulation, so the check binds the 256QAM shape.
  constexpr unsigned BITS_PER_SYMBOL = 8;
  constexpr unsigned NOF_SYMBOLS     = 14;

  std::vector<__fp16> plane(nsc * 64);
  std::vector<__fp16> llr(nsc * NOF_SYMBOLS * BITS_PER_SYMBOL);
  std::vector<__fp16> noise(nsc * NOF_SYMBOLS);
  std::vector<__fp16> ch_est(nsc * 2 * NOF_SYMBOLS);
  std::vector<__fp16> dmrs_noise(1);
  fill_input(plane, 20261013);

  metal::coreml_nn_engine::depth3_input input;
  input.plane            = reinterpret_cast<const uint16_t*>(plane.data());
  input.nof_subcarriers  = nsc;
  metal::coreml_nn_engine::depth3_output output;
  output.llr             = reinterpret_cast<uint16_t*>(llr.data());
  output.post_eq_noise   = reinterpret_cast<uint16_t*>(noise.data());
  output.ch_est          = reinterpret_cast<uint16_t*>(ch_est.data());
  output.dmrs_noise      = reinterpret_cast<uint16_t*>(dmrs_noise.data());
  output.bits_per_symbol = BITS_PER_SYMBOL;

  std::vector<double> timings;
  timings.reserve(repeats);
  const __fp16 sentinel = static_cast<__fp16>(-12345.0F);
  for (unsigned run = 0; run != repeats + 2; ++run) {
    // The sentinel is written before every pass, so the report below cannot be satisfied by a
    // buffer that was already holding a plausible value.
    std::fill(llr.begin(), llr.end(), sentinel);
    std::fill(noise.begin(), noise.end(), sentinel);
    std::fill(ch_est.begin(), ch_est.end(), sentinel);
    std::fill(dmrs_noise.begin(), dmrs_noise.end(), sentinel);
    if (!engine.predict_depth3(input, output)) {
      std::printf("PREDICT_FAIL on run %u\n", run);
      return 1;
    }
    if (run >= 2) {
      timings.push_back(engine.last_predict_us());
    }
  }

  const head_report llr_report   = inspect(llr, sentinel);
  const head_report noise_report = inspect(noise, sentinel);
  const head_report ch_report    = inspect(ch_est, sentinel);
  const head_report dmrs_report  = inspect(dmrs_noise, sentinel);

  std::sort(timings.begin(), timings.end());
  const double p50 = timings[timings.size() / 2];
  const double p99 = timings[std::min(timings.size() - 1, (timings.size() * 99) / 100)];

  std::printf("llr        %zu/%zu written, %zu nonzero, range [%.4f, %.4f]\n",
              llr_report.written, llr.size(), llr_report.nonzero, llr_report.min_value,
              llr_report.max_value);
  std::printf("noise      %zu/%zu written, %zu nonzero, range [%.4f, %.4f]\n",
              noise_report.written, noise.size(), noise_report.nonzero, noise_report.min_value,
              noise_report.max_value);
  std::printf("ch_est     %zu/%zu written, %zu nonzero, range [%.4f, %.4f]\n",
              ch_report.written, ch_est.size(), ch_report.nonzero, ch_report.min_value,
              ch_report.max_value);
  std::printf("dmrs_noise %zu/%zu written, %zu nonzero, range [%.4f, %.4f]\n",
              dmrs_report.written, dmrs_noise.size(), dmrs_report.nonzero, dmrs_report.min_value,
              dmrs_report.max_value);
  std::printf("p50 %.1f us, p99 %.1f us over %zu predictions\n", p50, p99, timings.size());

  // Every head must have been written, and the two that carry a bounded shape must not be a
  // constant: a constant noise head is what drives the scheduler's SINR to an extreme.
  const bool all_written = llr_report.written == llr.size() && noise_report.written == noise.size() &&
                           ch_report.written == ch_est.size() && dmrs_report.written == 1;
  const bool informative = llr_report.nonzero > 0 && ch_report.nonzero > 0;
  std::printf("%s\n", (all_written && informative) ? "PASS" : "FAIL");
  return (all_written && informative) ? 0 : 1;
}
