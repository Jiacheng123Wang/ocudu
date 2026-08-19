// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
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
//   - record the per-point pass counts into a CSV for the plotting script.
//
// Usage: ldpc_metal_bler_test [--outdir DIR] [--gpu-type metal] [--bg 1|2] [--z Z]
//        [--rates 0.333,0.5,...] [--snrs 0:2:10] [--trials N] [--max-iter N] [--cpu-max-iter N]
//        [--norm A] [--beta B] [--latency N]

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ldpc_decoder_metal.h"
#include "ocudu/adt/bit_buffer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
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
  std::vector<double> snrs  = {0.0, 2.0, 4.0, 6.0, 8.0, 10.0};
  unsigned    trials   = 200;
  unsigned    max_iter = 6;
  unsigned    cpu_max_iter = 0; // 0 = same as max_iter (decoupled for the flooding evaluation)
  unsigned    latency  = 0; // >0: decode-latency mode, N timed decodes per decoder
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
    } else if (a == "--trials") {
      p.trials = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--max-iter") {
      p.max_iter = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--cpu-max-iter") {
      p.cpu_max_iter = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else if (a == "--latency") {
      p.latency = static_cast<unsigned>(std::stoul(next(a.c_str())));
    } else {
      std::fprintf(stderr, "unknown argument '%s'\n", a.c_str());
      std::exit(1);
    }
  }
  return p;
}

/// One encode -> channel -> decode round trip for both decoders.
struct round_trip {
  bool cpu_ok = false;
  bool gpu_ok = false;
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
                    double*               gpu_us = nullptr)
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
  {
    std::vector<uint8_t> out_bytes((k + 7) / 8);
    bit_buffer           out = bit_buffer::from_bytes(out_bytes);
    const auto           t0  = std::chrono::steady_clock::now();
    res.cpu_ok              = cpu_dec.decode(out, llrs, &crc16, cpu_dec_cfg).has_value();
    const auto t1 = std::chrono::steady_clock::now();
    if (cpu_us != nullptr) {
      *cpu_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
  }
  {
    std::vector<uint8_t> out_bytes((k + 7) / 8);
    bit_buffer           out = bit_buffer::from_bytes(out_bytes);
    const auto           t0  = std::chrono::steady_clock::now();
    res.gpu_ok              = gpu_dec.decode(out, llrs, &crc16, dec_cfg).has_value();
    const auto t1 = std::chrono::steady_clock::now();
    if (gpu_us != nullptr) {
      *gpu_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
    }
  }
  return res;
}

} // namespace

int main(int argc, char** argv)
{
  const params p = parse_args(argc, argv);

  const ldpc_base_graph_type bg = (p.bg == 1) ? ldpc_base_graph_type::BG1 : ldpc_base_graph_type::BG2;
  ldpc::lifting_size_t ls = static_cast<ldpc::lifting_size_t>(p.z);

  const unsigned n_full  = (p.bg == 1) ? 68 : 52;
  const unsigned n_short = n_full - 2;
  const unsigned k       = (n_full - ((p.bg == 1) ? 46 : 42)) * p.z;

  std::fprintf(stderr, "BLER benchmark: GPU=%s BG%d Z%u K=%u codeblock=%u rates=%zu snrs=%zu trials=%u max_iter=%u\n",
               p.gpu_type.c_str(), p.bg, p.z, k, n_short * p.z, p.rates.size(), p.snrs.size(), p.trials, p.max_iter);

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

  for (double rate : p.rates) {
    // Clamp the rate-matched length to the codeblock (the rounding of k/rate can
    // overshoot the shortened codeblock by one bit at low rates).
    const unsigned e = std::min(static_cast<unsigned>(std::lround(k / rate)), n_short * p.z);

    char fname[256];
    std::snprintf(fname, sizeof(fname), "%s/bler_%s_bg%u_z%u_r%.4f.csv", p.outdir.c_str(), p.gpu_type.c_str(), p.bg, p.z, rate);
    std::ofstream csv(fname);
    csv << "# gpu=" << p.gpu_type << " bg=" << p.bg << " z=" << p.z << " k=" << k << " rate=" << rate << " e=" << e
        << " max_iter=" << p.max_iter << " trials=" << p.trials << "\n";
    csv << "snr_db,cpu_pass,gpu_pass,total,time_cpu_s,time_gpu_s\n";
    std::fprintf(stderr, "rate %.4f (e=%u):", rate, e);

    for (double snr : p.snrs) {
      const double sigma = std::sqrt(std::pow(10.0, -snr / 10.0) / 2.0);
      unsigned     cpu_pass = 0, gpu_pass = 0;
      // Per-decoder wall time accumulated over the point's trials (run_once
      // times each decode separately, so the CPU and GPU curves carry their
      // own run times).
      double cpu_us_sum = 0.0, gpu_us_sum = 0.0;
      for (unsigned t = 0; t != p.trials; ++t) {
        double c_us = 0.0, g_us = 0.0;
        const round_trip r = run_once(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, p.z, k, n_short, e, sigma,
                                      p.max_iter, p.cpu_max_iter, bg, ls, &c_us, &g_us);
        cpu_pass += r.cpu_ok ? 1 : 0;
        gpu_pass += r.gpu_ok ? 1 : 0;
        cpu_us_sum += c_us;
        gpu_us_sum += g_us;
      }
      csv << snr << "," << cpu_pass << "," << gpu_pass << "," << p.trials << ","
          << cpu_us_sum / 1e6 << "," << gpu_us_sum / 1e6 << "\n";
      std::fprintf(stderr, " %gdB:%u/%u (cpu %.1fs gpu %.1fs)", snr, gpu_pass, p.trials, cpu_us_sum / 1e6,
                   gpu_us_sum / 1e6);
    }
    csv.close();
    std::fprintf(stderr, " -> %s\n", fname);
  }
  std::fprintf(stderr, "done\n");
  return 0;
}
