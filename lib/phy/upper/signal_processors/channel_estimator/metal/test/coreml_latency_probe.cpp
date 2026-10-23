// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Time ONE Core ML bundle through the SAME zero-copy engine the receiver uses, so two networks can be compared
// without the arm, the chain or the corpus in between (design section 19.171).
//
// Why a probe and not the receiver: the arm's `forward` segment contains plane assembly and delivery, and a
// reception also carries a resource grid, a PDU and a staged capture. This program touches none of that. It loads
// `coreml_nn_engine` exactly as the receiver does, feeds a fixed buffer, and reports the engine's own
// `last_predict_us()` percentiles -- the same reading that appears in the arm's `[depth3-timing]` line.
//
// Both entry points are here because the comparison the design needed is between the two SHIPPED networks: the
// AI channel estimator (HELENA, the depth-1 path, 2 channels of H in and out) and the depth-3 receiver (16
// channels of plane out to four heads). Same machine, same engine, same measurement point.
//
// Usage:
//   coreml_latency_probe --model <bundle.mlmodelc> [--iterations N] [--warmup M] [--nsc 612] [--subcarriers N]
//     --model            a depth-1 (HELENA) bundle or a depth-3 bundle; the path is chosen from what loads
//     --iterations       timed predictions (default 300)
//     --warmup           untimed predictions first (default 20)
//     --subcarriers      grid width for a depth-1 bundle (default 612)
//     --repeat-batch     call predict this many times per timed sample and report the per-call time (default 1)

#include "ocudu_coreml_nn_engine.h"
#include "coreml_compute_plan.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace ocudu::metal;

namespace {

double percentile(const std::vector<double>& sorted, double fraction)
{
  if (sorted.empty()) {
    return 0.0;
  }
  const size_t index = std::min(static_cast<size_t>(fraction * static_cast<double>(sorted.size())),
                                sorted.size() - 1);
  return sorted[index];
}

void report(const char* what, std::vector<double> samples)
{
  std::sort(samples.begin(), samples.end());
  double sum = 0.0;
  for (double value : samples) {
    sum += value;
  }
  std::printf("%-14s n=%zu  p50=%.1fus  p90=%.1fus  p99=%.1fus  max=%.1fus  mean=%.1fus\n", what, samples.size(),
              percentile(samples, 0.50), percentile(samples, 0.90), percentile(samples, 0.99),
              samples.empty() ? 0.0 : samples.back(), sum / static_cast<double>(std::max<size_t>(samples.size(), 1)));
}

} // namespace

int main(int argc, char** argv)
{
  std::string model_path;
  unsigned    iterations  = 300;
  unsigned    warmup      = 20;
  unsigned    subcarriers = 612;
  unsigned    repeat      = 1;
  unsigned    gap_ms      = 0;
  for (int i = 1; i != argc; ++i) {
    const std::string arg = argv[i];
    if ((arg == "--model") && (i + 1 < argc)) {
      model_path = argv[++i];
    } else if ((arg == "--iterations") && (i + 1 < argc)) {
      iterations = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--warmup") && (i + 1 < argc)) {
      warmup = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--subcarriers") && (i + 1 < argc)) {
      subcarriers = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--repeat-batch") && (i + 1 < argc)) {
      repeat = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else if ((arg == "--gap-ms") && (i + 1 < argc)) {
      gap_ms = static_cast<unsigned>(std::strtoul(argv[++i], nullptr, 10));
    } else {
      std::fprintf(stderr, "usage: %s --model <bundle.mlmodelc> [--iterations N] [--warmup M] "
                           "[--subcarriers N] [--repeat-batch N]\n",
                   argv[0]);
      return 1;
    }
  }
  if (model_path.empty()) {
    std::fprintf(stderr, "no --model\n");
    return 1;
  }

  coreml_nn_engine engine;
  // Depth-3 first: a depth-3 bundle and a depth-1 bundle are told apart by which entry point accepts them, and a
  // probe that guessed wrong would time the wrong network.
  if (engine.init_depth3(model_path.c_str())) {
    std::printf("bundle %s: DEPTH-3 (plane %u channels, %u subcarrier(s))\n", model_path.c_str(),
                engine.depth3_input_channels(), engine.depth3_nsc());
    const unsigned channels = engine.depth3_input_channels();
    const unsigned nsc      = (engine.depth3_nsc() != 0) ? engine.depth3_nsc() : subcarriers;
    std::vector<__fp16> plane(static_cast<size_t>(nsc) * 14 * channels, 0.0F);
    std::vector<__fp16> llr(static_cast<size_t>(nsc) * 14 * 8);
    std::vector<__fp16> noise(static_cast<size_t>(nsc) * 14);
    std::vector<__fp16> ch(static_cast<size_t>(nsc) * 28);
    __fp16              dmrs = 0.0F;
    // A non-zero, non-degenerate input: an all-zero plane can take a different path through a fused convolution,
    // and the timing would then describe a graph nobody runs.
    for (size_t i = 0; i != plane.size(); ++i) {
      plane[i] = static_cast<__fp16>(0.01F * static_cast<float>(i % 101));
    }
    coreml_nn_engine::depth3_input in;
    in.plane            = reinterpret_cast<const uint16_t*>(plane.data());
    in.nof_subcarriers  = nsc;
    coreml_nn_engine::depth3_output out;
    out.llr             = reinterpret_cast<uint16_t*>(llr.data());
    out.post_eq_noise   = reinterpret_cast<uint16_t*>(noise.data());
    out.ch_est          = reinterpret_cast<uint16_t*>(ch.data());
    out.dmrs_noise      = reinterpret_cast<uint16_t*>(&dmrs);
    out.bits_per_symbol = engine.depth3_bits_per_symbol();
    for (unsigned i = 0; i != warmup; ++i) {
      if (!engine.predict_depth3(in, out)) {
        std::fprintf(stderr, "warm-up prediction failed\n");
        return 1;
      }
    }
    std::vector<double> samples;
    samples.reserve(iterations);
    const auto whole_start = std::chrono::steady_clock::now();
    for (unsigned i = 0; i != iterations; ++i) {
      for (unsigned r = 0; r != repeat; ++r) {
        if (!engine.predict_depth3(in, out)) {
          std::fprintf(stderr, "prediction failed\n");
          return 1;
        }
      }
      // `last_predict_us` is the engine's own reading of the Core ML call; dividing by `repeat` keeps the reported
      // number per call when a batch of calls is timed as one sample.
      samples.push_back(engine.last_predict_us() / static_cast<double>(repeat));
      if (gap_ms != 0) {
        // The DEPLOYED cadence: one reception per slot, i.e. milliseconds apart. The arm reads 1065 us for this same
        // bundle where a back-to-back loop reads 937, and the gap is the difference between "the engine is hot from
        // the previous call" and "it is called again after the rest of the chain ran".
        std::this_thread::sleep_for(std::chrono::milliseconds(gap_ms));
      }
    }
    const auto whole_stop = std::chrono::steady_clock::now();
    const double wall_us  = std::chrono::duration<double, std::micro>(whole_stop - whole_start).count();
    report("predict", samples);
    // WHERE THIS PROCESS GRAPH WENT. The state being hunted is a placement, so it has to be read
    // from the process that measured it, right after the measurement (design section 19.348).
    ocudu_print_compute_plan(model_path.c_str());
    std::printf("%-14s wall %.1f ms for %u call(s) => %.1f us/call (includes the loop, so it is an upper bound)\n",
                "process", wall_us / 1000.0, iterations * repeat, wall_us / static_cast<double>(iterations * repeat));
    return 0;
  }

  if (engine.init(model_path.c_str())) {
    std::printf("bundle %s: DEPTH-1 (HELENA, %u subcarrier(s))\n", model_path.c_str(), subcarriers);
    std::vector<float> in(static_cast<size_t>(subcarriers) * 14 * 2, 0.0F);
    std::vector<float> out(in.size());
    for (size_t i = 0; i != in.size(); ++i) {
      in[i] = 0.01F * static_cast<float>(i % 101);
    }
    for (unsigned i = 0; i != warmup; ++i) {
      if (!engine.predict(in.data(), out.data(), subcarriers)) {
        std::fprintf(stderr, "warm-up prediction failed\n");
        return 1;
      }
    }
    std::vector<double> samples;
    samples.reserve(iterations);
    for (unsigned i = 0; i != iterations; ++i) {
      for (unsigned r = 0; r != repeat; ++r) {
        if (!engine.predict(in.data(), out.data(), subcarriers)) {
          std::fprintf(stderr, "prediction failed\n");
          return 1;
        }
      }
      samples.push_back(engine.last_predict_us() / static_cast<double>(repeat));
    }
    report("predict", samples);
    return 0;
  }

  std::fprintf(stderr, "the bundle loaded as neither a depth-1 nor a depth-3 model: %s\n", model_path.c_str());
  return 1;
}
