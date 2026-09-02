// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Pure LDPC decode BLER benchmark: ocudu CPU decoder vs the Metal GPU decoder.
//
// For every (base graph, lifting size, code rate, SNR) point:
//   - encode a random message (CRC16 attached, MSB-first like the segmenter),
//   - transmit the first E = round(K / rate) codeblock bits (RV0 ordering: the 2Z
//     punctured columns are skipped, parity is truncated to reach the rate - the same
//     erasure/shortening semantics the decoders receive from the real rate dematcher),
//   - BPSK + AWGN at Es/N0 = SNR, quantized to int8 LLRs (clamped +/-64),
//   - decode the same LLRs with the CPU (generic) and the GPU (metal) decoder,
//   - record the per-point pass counts and, for the CRC-OK decodes only, the per-decoder
//     latency distribution (mean/median/min/max/p95/p99) into a CSV for the plotting script.
//
// The first GPU decode builds the engine slot and compiles the shaders, so it is not
// representative: --warmup N (default 3) rounds run before the SNR sweep and are excluded
// from every statistic. Each packet has a fixed size (K bits), so the correctly received
// data per point is simply the CRC-OK count times the packet size.
//
// Usage: ldpc_metal_bler_test [--outdir DIR] [--gpu-type metal] [--bg 1|2] [--z Z]
//        [--rates 0.333,0.5,...] [--snrs 0:2:10] [--snrs-cpu 0:2:8] [--trials N]
//        [--max-iter N] [--cpu-max-iter N] [--norm A] [--beta B] [--latency N] [--warmup N]
//
// metal_lls tuning knobs (PLAN.md 4.15, Phase 1; --norm doubles as alpha):
//   --lls-beta X    self-prior damping delta -= X*|LLR| (default 0)
//   --lls-p X       unsatisfied-ratio exponent (default 2)
//   --lls-gamma X   post-flip magnitude: 1 overshoot (legacy), 0 reset to the evidence
//   --lls-eps X     post-update magnitude floor (default 0)
//   --lls-norm N    evidence normalization: 0 /s_cnt (legacy), 1 /e_cnt, 2 /tc
//   --lls-k N       suspects per unsatisfied row (2-4; 3/4 = Phase 2)
//   --lls-evidence M evidence assignment: 0 E-self, 1 E-peel, 2 E-uniform, 3 rank-damped E-peel
//   --lls-cooldown 1 enable the 1-round flip-immunity oscillation guard
//   --lls-stall-escape 1 enable the stall-escape hard multi-flip (C4)
//   --lls-stall-theta X hard-flip threshold: e_cnt >= X * column weight (default 0.75)
//   --lls-stall-rounds N consecutive stall rounds before the escape (default 3)
//
// --snrs selects the GPU decoder's SNR sweep; --snrs-cpu overrides the CPU decoder's sweep
// (default: the CPU shares --snrs). The sweeps may differ (e.g. the GPU needs higher SNRs
// than the CPU): the CSV contains one row per point of the merged sweep, and the cells of
// the decoder that was not evaluated at a point stay empty.
// --max-iter sets the GPU iteration cap; --cpu-max-iter overrides the CPU cap (default 0 =
// the CPU shares --max-iter).
//
// --gpu-type selects the decoder through the factory: metal (layered NMS), metal_flooding,
// metal_persistent, metal_async or metal_lls (LLS bit-flipping, restored from the git history,
// ~5-8 dB weaker on BLER - its waterfall lies well above the NMS curves).

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ldpc_decoder_metal.h"
#include "ocudu/adt/bit_buffer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

struct params {
  std::string outdir    = ".";
  std::string gpu_type  = "metal"; // the layered NMS decoder
  float       norm      = -1.0F;
  float       beta      = -1.0F;
  unsigned    bg        = 1;
  unsigned    z         = 64;
  std::vector<double> rates = {1.0 / 3.0, 0.5, 2.0 / 3.0};
  /// GPU decoder SNR sweep (--snrs). The CPU shares it unless --snrs-cpu is given.
  std::vector<double> snrs  = {0.0, 2.0, 4.0, 6.0, 8.0, 10.0};
  /// CPU decoder SNR sweep (--snrs-cpu); empty = reuse --snrs.
  std::vector<double> snrs_cpu;
  unsigned    trials   = 200;
  unsigned    max_iter = 6;
  unsigned    cpu_max_iter = 0; // 0 = same as --max-iter (the GPU and the CPU share the value)
  unsigned    latency  = 0; // >0: decode-latency mode, N timed decodes per decoder
  unsigned    warmup   = 3; // BLER mode: warm-up rounds before the SNR sweep (GPU init excluded)
  // LLS tuning overrides (PLAN.md 4.15); -1 = legacy default. metal_lls only.
  float       lls_beta  = -1.0F;
  float       lls_p     = -1.0F;
  float       lls_gamma = -1.0F;
  float       lls_eps   = -1.0F;
  int         lls_norm  = -1; // 0: /s_cnt (legacy), 1: /e_cnt, 2: /tc
  unsigned    lls_k     = 2; // suspects per unsatisfied row (2-4, Phase 2)
  int         lls_evidence = -1; // 0: E-self (legacy k=2), 1: E-peel, 2: E-uniform, 3: rank-damped E-peel
  int         lls_cooldown  = -1; // 1: 1-round flip-immunity guard
  int         lls_stall_escape = -1; // 1: stall-escape hard multi-flip (C4)
  float       lls_stall_theta  = -1.0F; // hard-flip threshold (default 0.75)
  unsigned    lls_stall_rounds = 0;  // consecutive stall rounds (default 3)
};

/// Parses "a:b:c" into a sequence, or a single value.
std::vector<double> parse_seq(const std::string& s)
{
  std::vector<double> out;
  std::stringstream   ss(s);
  std::string         tok;
  std::vector<std::string> parts;
  while (std::getline(ss, tok, ',')) {
    const size_t c1 = tok.find(':');
    if (c1 == std::string::npos) {
      out.push_back(std::stod(tok));
      continue;
    }
    const size_t c2 = tok.find(':', c1 + 1);
    if (c2 == std::string::npos) {
      std::fprintf(stderr, "bad range '%s' (expected a:b:c)\n", tok.c_str());
      std::exit(1);
    }
    const double a = std::stod(tok.substr(0, c1));
    const double b = std::stod(tok.substr(c1 + 1, c2 - c1 - 1));
    const double c = std::stod(tok.substr(c2 + 1));
    if (b <= 0.0) {
      std::fprintf(stderr, "bad range '%s': step must be positive (a zero step loops forever)\n", tok.c_str());
      std::exit(1);
    }
    for (double v = a; v <= c + 1e-9; v += b) {
      out.push_back(v);
    }
  }
  return out;
}

params parse_args(int argc, char** argv)
{
  params p;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto              next = [&](const char* what) -> std::string {
      if (++i >= argc) {
        std::fprintf(stderr, "missing value for %s\n", what);
        std::exit(1);
      }
      return argv[i];
    };
    if (a == "--outdir") {
      p.outdir = next(a.c_str());
    } else if (a == "--gpu-type") {
      p.gpu_type = next(a.c_str());
    } else if (a == "--norm") {
      p.norm = std::stof(next(a.c_str()));
    } else if (a == "--beta") {
      p.beta = std::stof(next(a.c_str()));
    } else if (a == "--bg") {
      p.bg = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--z") {
      p.z = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--rates") {
      p.rates = parse_seq(next(a.c_str()));
    } else if (a == "--snrs") {
      p.snrs = parse_seq(next(a.c_str()));
    } else if (a == "--snrs-cpu") {
      p.snrs_cpu = parse_seq(next(a.c_str()));
    } else if (a == "--trials") {
      p.trials = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--max-iter") {
      p.max_iter = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--cpu-max-iter") {
      p.cpu_max_iter = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--latency") {
      p.latency = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--warmup") {
      p.warmup = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--lls-beta") {
      p.lls_beta = std::stof(next(a.c_str()));
    } else if (a == "--lls-p") {
      p.lls_p = std::stof(next(a.c_str()));
    } else if (a == "--lls-gamma") {
      p.lls_gamma = std::stof(next(a.c_str()));
    } else if (a == "--lls-eps") {
      p.lls_eps = std::stof(next(a.c_str()));
    } else if (a == "--lls-norm") {
      p.lls_norm = std::stoi(next(a.c_str()));
    } else if (a == "--lls-k") {
      p.lls_k = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--lls-evidence") {
      p.lls_evidence = std::stoi(next(a.c_str()));
    } else if (a == "--lls-cooldown") {
      p.lls_cooldown = std::stoi(next(a.c_str()));
    } else if (a == "--lls-stall-escape") {
      p.lls_stall_escape = std::stoi(next(a.c_str()));
    } else if (a == "--lls-stall-theta") {
      p.lls_stall_theta = std::stof(next(a.c_str()));
    } else if (a == "--lls-stall-rounds") {
      p.lls_stall_rounds = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", a.c_str());
      std::exit(1);
    }
  }
  return p;
}

/// One encode -> channel -> decode round trip. With the split SNR sweeps (--snrs vs --snrs-cpu)
/// each decoder can be skipped at a point via \c do_cpu / \c do_gpu.
struct round_trip {
  bool cpu_ok = false;
  bool gpu_ok = false;
  int  gpu_iters = -1; // GPU rounds actually executed (-1 = not run / failed before the GPU)
};

round_trip run_once(std::mt19937&          rng,
                    ldpc_encoder&         encoder,
                    crc_calculator&       crc16,
                    ldpc_decoder&         cpu_dec,
                    ldpc_decoder&         gpu_dec,
                    unsigned              z,
                    unsigned              k,
                    unsigned              n_short,
                    unsigned              e,
                    double                sigma,
                    unsigned              max_iter,
                    unsigned              cpu_max_iter,
                    ldpc_base_graph_type  bg,
                    ldpc::lifting_size_t  ls,
                    double*               cpu_us = nullptr,
                    double*               gpu_us = nullptr,
                    bool                  do_cpu = true,
                    bool                  do_gpu = true)
{
  // Message: K-16 random bits + CRC16 (MSB-first, segmenter convention).
  std::vector<uint8_t> msg_bytes((k + 7) / 8);
  bit_buffer           msg = bit_buffer::from_bytes(msg_bytes);
  std::uniform_int_distribution<int> bit_dist(0, 1);
  for (unsigned i = 0; i != k - 16; ++i) {
    msg.insert(static_cast<uint8_t>(bit_dist(rng)), i, 1);
  }
  const unsigned crc = crc16.calculate(msg.first(k - 16));
  for (unsigned i = 0; i != 16; ++i) {
    msg.insert(static_cast<uint8_t>((crc >> (15 - i)) & 1U), k - 16 + i, 1);
  }

  // Encode. The encoder buffer is UNPACKED (one byte per bit).
  const ldpc_encoder::configuration enc_cfg = {.base_graph = bg, .lifting_size = ls, .Nref = 0};
  const ldpc_encoder_buffer&        cb      = encoder.encode(msg, enc_cfg);
  ocudu_assert(cb.get_codeblock_length() == n_short * z, "Unexpected codeblock length");
  std::vector<uint8_t> packed(cb.get_codeblock_length());
  cb.write_codeblock(packed, 0);

  // Transmit the first E bits (RV0: punctured columns skipped, parity truncated).
  ocudu_assert(e >= k + 2 * z && e <= n_short * z, "Invalid rate-matched length");
  std::normal_distribution<double> noise(0.0, sigma);
  std::vector<log_likelihood_ratio> llrs(e);
  for (unsigned i = 0; i != e; ++i) {
    const double y = ((packed[i] & 1U) == 0 ? 1.0 : -1.0) + noise(rng);
    const double l = 2.0 * y / (sigma * sigma);
    int          q = static_cast<int>(std::lround(l));
    q              = std::clamp(q, -64, 64);
    llrs[i]        = q;
  }

  const ldpc_decoder::configuration dec_cfg = {
      .base_graph      = bg,
      .lifting_size    = ls,
      .nof_filler_bits = 0,
      .nof_crc_bits    = 16,
      .max_iterations  = max_iter,
  };
  // Decoupled CPU iterations (the flooding evaluation runs the CPU at its
  // production 6 iterations while the GPU sweeps higher counts).
  const ldpc_decoder::configuration cpu_dec_cfg = {
      .base_graph      = bg,
      .lifting_size    = ls,
      .nof_filler_bits = 0,
      .nof_crc_bits    = 16,
      .max_iterations  = cpu_max_iter != 0 ? cpu_max_iter : max_iter,
  };

  round_trip res;
  if (do_cpu) {
    std::vector<uint8_t> out_bytes((k + 7) / 8);
    bit_buffer           out = bit_buffer::from_bytes(out_bytes);
    const auto           t0  = std::chrono::steady_clock::now();
    res.cpu_ok              = cpu_dec.decode(out, llrs, &crc16, cpu_dec_cfg).has_value();
    const auto t1 = std::chrono::steady_clock::now();
    if (cpu_us != nullptr) {
      *cpu_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
  } else if (cpu_us != nullptr) {
    *cpu_us = 0.0;
  }
  if (do_gpu) {
    std::vector<uint8_t> out_bytes((k + 7) / 8);
    bit_buffer           out = bit_buffer::from_bytes(out_bytes);
    const auto           t0  = std::chrono::steady_clock::now();
    const auto           it  = gpu_dec.decode(out, llrs, &crc16, dec_cfg);
    res.gpu_ok              = it.has_value();
    res.gpu_iters           = it.has_value() ? static_cast<int>(*it) : -1;
    const auto t1 = std::chrono::steady_clock::now();
    if (gpu_us != nullptr) {
      *gpu_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
  } else if (gpu_us != nullptr) {
    *gpu_us = 0.0;
  }
  return res;
}

/// Latency distribution of one decoder over one SNR point (CRC-OK decodes only).
struct latency_stats {
  double mean   = 0.0;
  double median = 0.0;
  double min    = 0.0;
  double max    = 0.0;
  double p95    = 0.0;
  double p99    = 0.0;
};

/// Sorts the sample vector in place and returns its distribution (requires a non-empty vector).
latency_stats compute_stats(std::vector<double>& v)
{
  ocudu_assert(!v.empty(), "Empty latency vector.");
  std::sort(v.begin(), v.end());
  auto pct = [&v](double p) { return v[static_cast<size_t>((v.size() - 1) * p)]; };
  double sum = 0;
  for (double x : v) {
    sum += x;
  }
  return {.mean   = sum / static_cast<double>(v.size()),
          .median = pct(0.5),
          .min    = v.front(),
          .max    = v.back(),
          .p95    = pct(0.95),
          .p99    = pct(0.99)};
}

} // namespace

int main(int argc, char** argv)
{
  const params p = parse_args(argc, argv);

  // The CSVs go to --outdir; create it when missing so the benchmark never fails
  // silently later (std::ofstream does not report a missing directory).
  {
    std::error_code ec;
    std::filesystem::create_directories(p.outdir, ec);
    if (ec) {
      std::fprintf(stderr, "cannot create outdir '%s': %s\n", p.outdir.c_str(), ec.message().c_str());
      return 1;
    }
  }

  const ldpc_base_graph_type bg = (p.bg == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
  ldpc::lifting_size_t ls = static_cast<ldpc::lifting_size_t>(p.z);

  const unsigned n_full  = (p.bg == 1) ? 68 : 52;
  const unsigned n_short = n_full - 2;
  const unsigned k       = (n_full - ((p.bg == 1) ? 46 : 42)) * p.z;

  std::fprintf(stderr,
               "BLER benchmark: GPU=%s BG%d Z%u K=%u codeblock=%u rates=%zu snrs=%zu snrs_cpu=%zu trials=%u "
               "max_iter=%u cpu_max_iter=%u\n",
               p.gpu_type.c_str(), p.bg, p.z, k, n_short * p.z, p.rates.size(), p.snrs.size(), p.snrs_cpu.size(),
               p.trials, p.max_iter, p.cpu_max_iter);

  const ldpc_decoder_factory::ldpc_decoder_factory_configuration dec_factory_cfg = {
      .force_decoding      = false,
      .early_stop_syndrome = true,
  };
  auto cpu_dec = create_ldpc_decoder_factory_sw("auto", dec_factory_cfg)->create();
  std::unique_ptr<ldpc_decoder> gpu_dec;
  if ((p.gpu_type == "metal") && ((p.norm >= 0.0F) || (p.beta >= 0.0F))) {
    // Norm/offset experiments bypass the factory defaults (layered norm 0.7, beta 0.5).
    gpu_dec = std::make_unique<ldpc_decoder_metal>(dec_factory_cfg.force_decoding,
                                                   dec_factory_cfg.early_stop_syndrome,
                                                   ocudu::metal::decoder_engine::algo::layered, p.norm, p.beta);
  } else if ((p.gpu_type == "metal_flooding") && ((p.norm >= 0.0F) || (p.beta >= 0.0F))) {
    // Flooding experiments bypass the factory defaults (flooding norm 0.45, beta 0).
    gpu_dec = std::make_unique<ldpc_decoder_metal>(dec_factory_cfg.force_decoding,
                                                   dec_factory_cfg.early_stop_syndrome,
                                                   ocudu::metal::decoder_engine::algo::flooding, p.norm, p.beta);
  } else if ((p.gpu_type == "metal_persistent") && ((p.norm >= 0.0F) || (p.beta >= 0.0F))) {
    // Persistent experiments bypass the factory defaults (layered norm 0.7, beta 0.5).
    gpu_dec = std::make_unique<ldpc_decoder_metal>(dec_factory_cfg.force_decoding,
                                                   dec_factory_cfg.early_stop_syndrome,
                                                   ocudu::metal::decoder_engine::algo::layered_persistent, p.norm,
                                                   p.beta);
  } else if ((p.gpu_type == "metal_async") && ((p.norm >= 0.0F) || (p.beta >= 0.0F))) {
    // Async delta-BP experiments bypass the factory defaults (flooding-family norm 0.45, beta 0).
    gpu_dec = std::make_unique<ldpc_decoder_metal>(dec_factory_cfg.force_decoding,
                                                   dec_factory_cfg.early_stop_syndrome,
                                                   ocudu::metal::decoder_engine::algo::async_delta, p.norm, p.beta);
  } else if (p.gpu_type == "metal_lls") {
    // LLS experiments bypass the factory defaults (LLS step size 0.8, beta 0).
    gpu_dec = std::make_unique<ldpc_decoder_metal>(dec_factory_cfg.force_decoding,
                                                   dec_factory_cfg.early_stop_syndrome,
                                                   ocudu::metal::decoder_engine::algo::lls, p.norm, p.beta);
    if (p.lls_k > 4) {
      std::fprintf(stderr, "--lls-k %u: 2-4 supported (PLAN.md 4.15 Phase 2)\n", p.lls_k);
      return 1;
    }
    if (p.lls_evidence > 3) {
      std::fprintf(stderr, "--lls-evidence %d: 0 (E-self), 1 (E-peel) or 2 (E-uniform)\n", p.lls_evidence);
      return 1;
    }
    // PLAN.md 4.15 Phase 1/2 tuning knobs: any of them switches the engine to
    // the full parameter struct (alpha defaults to --norm or the champion 1.5).
    const bool lls_flags = (p.lls_beta >= 0.0F) || (p.lls_p >= 0.0F) || (p.lls_gamma >= 0.0F) ||
                           (p.lls_eps >= 0.0F) || (p.lls_norm >= 0) || (p.lls_k != 2) ||
                           (p.lls_evidence >= 0) || (p.lls_cooldown >= 0) ||
                           (p.lls_stall_escape >= 0) || (p.lls_stall_theta >= 0.0F) || (p.lls_stall_rounds != 0);
    if (lls_flags) {
      ocudu::metal::decoder_engine::lls_params lp;
      lp.alpha = (p.norm >= 0.0F) ? p.norm : 1.5F;
      if (p.lls_beta >= 0.0F) lp.beta = p.lls_beta;
      if (p.lls_p >= 0.0F) lp.p = p.lls_p;
      if (p.lls_gamma >= 0.0F) lp.gamma = p.lls_gamma;
      if (p.lls_eps >= 0.0F) lp.eps = p.lls_eps;
      if (p.lls_norm >= 0) lp.norm_mode = static_cast<uint32_t>(p.lls_norm);
      lp.k_suspects    = p.lls_k;
      lp.evidence_mode = (p.lls_evidence >= 0) ? static_cast<uint32_t>(p.lls_evidence) : 0;
      lp.cooldown      = (p.lls_cooldown >= 1) ? 1u : 0u;
      lp.stall_escape  = (p.lls_stall_escape >= 1) ? 1u : 0u;
      if (p.lls_stall_theta >= 0.0F) lp.theta = p.lls_stall_theta;
      if (p.lls_stall_rounds != 0) lp.stall_rounds = p.lls_stall_rounds;
      static_cast<ldpc_decoder_metal&>(*gpu_dec).set_lls_params(lp);
    }
  } else {
    gpu_dec = create_ldpc_decoder_factory_sw(p.gpu_type, dec_factory_cfg)->create();
  }
  auto encoder = create_ldpc_encoder_factory_sw("generic")->create();
  auto crc16   = create_crc_calculator_factory_sw("lut")->create(crc_generator_poly::CRC16);
  if (!cpu_dec || !gpu_dec || !encoder || !crc16) {
    std::fprintf(stderr, "Failed to create the codec instances\n");
    return 1;
  }

  std::mt19937 rng(20260816);

  if (p.latency > 0) {
    const double rate  = p.rates.front();
    const double snr   = p.snrs.front();
    const unsigned e   = std::min(static_cast<unsigned>(std::lround(k / rate)), n_short * p.z);
    const double sigma = std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);

    // Warm-up: the first GPU decode builds the engine slot and compiles the shaders.
    for (unsigned w = 0; w != 3; ++w) {
      run_once(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, p.z, k, n_short, e, sigma, p.max_iter,
               p.cpu_max_iter, bg, ls);
    }

    std::vector<double> cpu_us, gpu_us, gpu_wait_us;
    cpu_us.reserve(p.latency);
    gpu_us.reserve(p.latency);
    for (unsigned t = 0; t != p.latency; ++t) {
      double c_us = 0.0, g_us = 0.0;
      run_once(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, p.z, k, n_short, e, sigma, p.max_iter,
               p.cpu_max_iter, bg, ls, &c_us, &g_us);
      cpu_us.push_back(c_us);
      gpu_us.push_back(g_us);
      if (p.gpu_type.rfind("metal", 0) == 0) {
        gpu_wait_us.push_back(static_cast<ldpc_decoder_metal&>(*gpu_dec).last_gpu_wait_us());
      }
    }
    auto stats = [](std::vector<double>& v) {
      std::sort(v.begin(), v.end());
      auto pct = [&v](double p) { return v[static_cast<size_t>((v.size() - 1) * p)]; };
      double sum = 0;
      for (double x : v) {
        sum += x;
      }
      return std::array<double, 4>{sum / static_cast<double>(v.size()), pct(0.5), pct(0.95), pct(0.99)};
    };
    const auto cs = stats(cpu_us);
    const auto gs = stats(gpu_us);
    std::fprintf(stderr,
                 "[latency] %s BG%d Z%u rate=%.4f snr=%.1f n=%u: cpu mean=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus | "
                 "gpu mean=%.1fus p50=%.1fus p95=%.1fus p99=%.1fus",
                 p.gpu_type.c_str(), p.bg, p.z, rate, snr, p.latency, cs[0], cs[1], cs[2], cs[3], gs[0], gs[1], gs[2],
                 gs[3]);
    if (!gpu_wait_us.empty()) {
      const auto gws = stats(gpu_wait_us);
      std::fprintf(stderr, " | gpu_wait mean=%.1fus (cpu-side overhead mean=%.1fus)", gws[0], gs[0] - gws[0]);
    }
    std::fprintf(stderr, "\n");
    return 0;
  }

  // Warm-up: the first GPU decode builds the engine slot and compiles the shaders,
  // so it is orders of magnitude slower than the steady state and must not leak
  // into the per-point latency statistics. Discarded (--warmup 0 to disable).
  if (p.warmup > 0) {
    const double   warm_rate  = p.rates.front();
    const double   warm_snr   = p.snrs.front();
    const unsigned warm_e     = std::min(static_cast<unsigned>(std::lround(k / warm_rate)), n_short * p.z);
    const double   warm_sigma = std::sqrt(std::pow(10.0, -warm_snr / 10.0) / 2.0);
    std::fprintf(stderr, "warm-up: %u decode(s) at rate %.4f snr %.1f dB (discarded)\n", p.warmup, warm_rate, warm_snr);
    for (unsigned w = 0; w != p.warmup; ++w) {
      run_once(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, p.z, k, n_short, warm_e, warm_sigma, p.max_iter,
               p.cpu_max_iter, bg, ls);
    }
  }

  // The CPU shares the GPU sweep unless --snrs-cpu overrides it; the CSV covers the merged sweep,
  // with empty cells for the decoder that was not evaluated at a given point.
  const std::vector<double>& cpu_snrs = p.snrs_cpu.empty() ? p.snrs : p.snrs_cpu;
  std::vector<double>        all_snrs;
  for (const std::vector<double>* list : {&p.snrs, &cpu_snrs}) {
    for (double snr : *list) {
      if (std::none_of(all_snrs.begin(), all_snrs.end(), [&](double v) { return std::abs(v - snr) < 1e-9; })) {
        all_snrs.push_back(snr);
      }
    }
  }
  std::sort(all_snrs.begin(), all_snrs.end());
  const auto contains = [](const std::vector<double>& list, double v) {
    return std::any_of(list.begin(), list.end(), [&](double x) { return std::abs(x - v) < 1e-9; });
  };

  for (double rate : p.rates) {
    // Clamp the rate-matched length to the codeblock (the rounding of k/rate can
    // overshoot the shortened codeblock by one bit at low rates).
    const unsigned e = std::min(static_cast<unsigned>(std::lround(k / rate)), n_short * p.z);

    char fname[256];
    std::snprintf(fname, sizeof(fname), "%s/bler_%s_bg%u_z%u_r%.4f.csv", p.outdir.c_str(), p.gpu_type.c_str(), p.bg, p.z, rate);
    std::ofstream csv(fname);
    if (!csv) {
      std::fprintf(stderr, "cannot open '%s' for writing\n", fname);
      return 1;
    }
    csv << "# gpu=" << p.gpu_type << " bg=" << p.bg << " z=" << p.z << " k=" << k << " rate=" << rate << " e=" << e
        << " max_iter=" << p.max_iter << " cpu_max_iter=" << p.cpu_max_iter << " trials=" << p.trials
        << " warmup=" << p.warmup << " snrs_cpu_split=" << (p.snrs_cpu.empty() ? 0 : 1);
    if (p.gpu_type == "metal_lls") {
      // Effective LLS parameters (PLAN.md 4.15): the engine uses these unless all
      // the --lls-* knobs are at their legacy defaults.
      csv << " lls_params=alpha=" << ((p.norm >= 0.0F) ? p.norm : 1.5F)
          << ",beta=" << ((p.lls_beta >= 0.0F) ? p.lls_beta : 0.0F)
          << ",p=" << ((p.lls_p >= 0.0F) ? p.lls_p : 2.0F)
          << ",gamma=" << ((p.lls_gamma >= 0.0F) ? p.lls_gamma : 0.0F)
          << ",eps=" << ((p.lls_eps >= 0.0F) ? p.lls_eps : 0.0F)
          << ",norm=" << ((p.lls_norm >= 0) ? p.lls_norm : 0) << ",k=" << p.lls_k
          << ",evidence=" << ((p.lls_evidence >= 0) ? p.lls_evidence : 0)
          << ",cooldown=" << ((p.lls_cooldown >= 1) ? 1 : 0)
          << ",stall_escape=" << ((p.lls_stall_escape >= 1) ? 1 : 0)
          << ",stall_theta=" << ((p.lls_stall_theta >= 0.0F) ? p.lls_stall_theta : 0.75F)
          << ",stall_rounds=" << ((p.lls_stall_rounds != 0) ? p.lls_stall_rounds : 3);
    }
    csv << "\n";
    csv << "snr_db,cpu_pass,gpu_pass,total,time_cpu_s,time_gpu_s,"
           "cpu_mean_us,cpu_median_us,cpu_min_us,cpu_max_us,cpu_p95_us,cpu_p99_us,"
           "gpu_mean_us,gpu_median_us,gpu_min_us,gpu_max_us,gpu_p95_us,gpu_p99_us,"
           "gpu_mean_iters_ok,gpu_mean_iters_fail\n";
    std::fprintf(stderr, "rate %.4f (e=%u):", rate, e);

    for (double snr : all_snrs) {
      const bool run_cpu = contains(cpu_snrs, snr);
      const bool run_gpu = contains(p.snrs, snr);
      const double sigma = std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);
      unsigned     cpu_pass = 0, gpu_pass = 0;
      // GPU rounds executed (CRC-OK vs CRC-fail), averaged per point: the
      // stall distribution shows whether a decoder fails by hitting max_iter
      // or by converging to a wrong codeword.
      unsigned gpu_iters_ok_sum = 0, gpu_iters_fail_sum = 0;
      unsigned gpu_iters_ok_n = 0, gpu_iters_fail_n = 0;
      // Per-decoder wall time accumulated over the point's trials (run_once
      // times each decode separately, so the CPU and GPU curves carry their
      // own run times).
      double cpu_us_sum = 0.0, gpu_us_sum = 0.0;
      // Per-decoder latency samples of the CRC-OK decodes only: the data burst
      // size is fixed (K bits), so cpu_pass/gpu_pass times the packet size is
      // the correctly received data of the point.
      std::vector<double> cpu_ok_us;
      std::vector<double> gpu_ok_us;
      cpu_ok_us.reserve(p.trials);
      gpu_ok_us.reserve(p.trials);
      for (unsigned t = 0; t != p.trials; ++t) {
        double c_us = 0.0, g_us = 0.0;
        const round_trip r = run_once(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, p.z, k, n_short, e, sigma,
                                      p.max_iter, p.cpu_max_iter, bg, ls, &c_us, &g_us, run_cpu, run_gpu);
        if (run_cpu) {
          cpu_pass += r.cpu_ok ? 1 : 0;
          cpu_us_sum += c_us;
          if (r.cpu_ok) {
            cpu_ok_us.push_back(c_us);
          }
        }
        if (run_gpu) {
          gpu_pass += r.gpu_ok ? 1 : 0;
          gpu_us_sum += g_us;
          if (r.gpu_iters >= 0) {
            if (r.gpu_ok) {
              gpu_ok_us.push_back(g_us);
              gpu_iters_ok_sum += static_cast<unsigned>(r.gpu_iters);
              ++gpu_iters_ok_n;
            } else {
              gpu_iters_fail_sum += static_cast<unsigned>(r.gpu_iters);
              ++gpu_iters_fail_n;
            }
          }
        }
      }
      csv << snr << "," << (run_cpu ? std::to_string(cpu_pass) : "") << "," << (run_gpu ? std::to_string(gpu_pass) : "")
          << "," << p.trials << "," << (run_cpu ? std::to_string(cpu_us_sum / 1e6) : "")
          << "," << (run_gpu ? std::to_string(gpu_us_sum / 1e6) : "");
      std::fprintf(stderr, " %gdB: gpu_pass=%s cpu_pass=%s", snr, run_gpu ? std::to_string(gpu_pass).c_str() : "-",
                   run_cpu ? std::to_string(cpu_pass).c_str() : "-");
      std::fprintf(stderr, " (cpu %.1fs gpu %.1fs", cpu_us_sum / 1e6, gpu_us_sum / 1e6);
      // CRC-OK-only latency statistics, one column block per decoder; empty when a decoder has no CRC-OK sample.
      if (!cpu_ok_us.empty()) {
        const latency_stats cs = compute_stats(cpu_ok_us);
        csv << "," << cs.mean << "," << cs.median << "," << cs.min << "," << cs.max << "," << cs.p95 << ","
            << cs.p99;
        std::fprintf(stderr, " cpu mean=%.1fus p50=%.1fus max=%.1fus", cs.mean, cs.median, cs.max);
      } else {
        csv << ",,,,,,";
      }
      if (!gpu_ok_us.empty()) {
        const latency_stats gs = compute_stats(gpu_ok_us);
        csv << "," << gs.mean << "," << gs.median << "," << gs.min << "," << gs.max << "," << gs.p95 << ","
            << gs.p99;
        std::fprintf(stderr, " gpu mean=%.1fus p50=%.1fus max=%.1fus", gs.mean, gs.median, gs.max);
      } else {
        csv << ",,,,,,";
      }
      // Mean GPU rounds for CRC-OK vs CRC-fail decodes (empty when not run / no samples).
      csv << "," << (run_gpu && gpu_iters_ok_n ? std::to_string(static_cast<double>(gpu_iters_ok_sum) / gpu_iters_ok_n) : "")
          << "," << (run_gpu && gpu_iters_fail_n ? std::to_string(static_cast<double>(gpu_iters_fail_sum) / gpu_iters_fail_n) : "");
      if (run_gpu && (gpu_iters_ok_n || gpu_iters_fail_n)) {
        std::fprintf(stderr, " iters ok=%.2f fail=%.2f",
                     gpu_iters_ok_n ? static_cast<double>(gpu_iters_ok_sum) / gpu_iters_ok_n : 0.0,
                     gpu_iters_fail_n ? static_cast<double>(gpu_iters_fail_sum) / gpu_iters_fail_n : 0.0);
      }
      csv << "\n";
      std::fprintf(stderr, ")");
    }
    csv.close();
    std::fprintf(stderr, " -> %s\n", fname);
  }
  std::fprintf(stderr, "done\n");
  return 0;
}
