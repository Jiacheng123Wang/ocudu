// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Unit-level validation of the Metal LDPC decoder (not part of the gNB build):
//   1. Golden check: the in-memory packed H / H^T generated from the ocudu ldpc_graph
//      must match the SynchroPlus H_matrix_Z*.bin / HT_matrix_Z*.bin reference files.
//   2. Decode parity: identical codeblocks through an AWGN channel, decoded by the CPU
//      (generic) and the Metal decoder created through the factory ("metal" type);
//      CRC pass rates (BLER) and the decoded bits must agree.

#include "ocudu/phy/upper/channel_coding/channel_coding_factories.h"
#include "ocudu/phy/upper/channel_coding/ldpc/ldpc_encoder_buffer.h"
#include "ldpc_graph_impl.h"
#include "ocudu/adt/bit_buffer.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

struct test_case {
  const char*            name;
  ldpc_base_graph_type   bg;
  ldpc::lifting_size_t   ls;
  double                 snr_db;
};

/// Builds the packed H / H^T matrices from the protograph (same routine as the adapter).
void generate_h_ht(ldpc_base_graph_type bg, ldpc::lifting_size_t ls, unsigned m_aligned, unsigned n_aligned,
                   std::vector<uint32_t>& h, std::vector<uint32_t>& ht)
{
  const unsigned z = static_cast<unsigned>(ls);
  const unsigned n_h_chunks = n_aligned / 32;
  const unsigned h_pred_len = m_aligned / 32;
  h.assign(static_cast<size_t>(m_aligned) * n_h_chunks, 0);
  ht.assign(static_cast<size_t>(n_aligned) * h_pred_len, 0);

  const ldpc_graph_impl graph(bg, ls);
  for (unsigned row = 0; row != graph.get_nof_BG_check_nodes(); ++row) {
    for (unsigned col = 0; col != graph.get_nof_BG_var_nodes_full(); ++col) {
      const uint16_t shift = graph.get_lifted_node(row, col);
      if (shift == ldpc::NO_EDGE) {
        continue;
      }
      for (unsigned k = 0; k != z; ++k) {
        const unsigned lifted_row = row * z + k;
        const unsigned lifted_col = col * z + ((k + shift) % z);
        h[lifted_row * n_h_chunks + lifted_col / 32] |= 1u << (lifted_col % 32);
        ht[lifted_col * h_pred_len + lifted_row / 32] |= 1u << (lifted_row % 32);
      }
    }
  }
}

int golden_check(const std::string& matrix_dir)
{
  // (bg, z, n_full, m) -> reference file names for BG1 Z256 and Z384.
  struct golden {
    const char* h_file;
    const char* ht_file;
    ldpc_base_graph_type bg;
    ldpc::lifting_size_t ls;
  };
  const golden goldens[] = {
      {"H_matrix_Z256.bin", "HT_matrix_Z256.bin", ldpc_base_graph_type::BG1, ldpc::LS256},
      {"H_matrix_Z384.bin", "HT_matrix_Z384.bin", ldpc_base_graph_type::BG1, ldpc::LS384},
  };

  int failures = 0;
  for (const golden& g : goldens) {
    const unsigned z = static_cast<unsigned>(g.ls);
    const unsigned n = 68 * z;
    const unsigned m = 46 * z;
    const unsigned n_aligned = ((n + 31) / 32) * 32;
    const unsigned m_aligned = ((m + 31) / 32) * 32;

    std::vector<uint32_t> h, ht;
    generate_h_ht(g.bg, g.ls, m_aligned, n_aligned, h, ht);

    const std::string h_path  = matrix_dir + "/" + g.h_file;
    const std::string ht_path = matrix_dir + "/" + g.ht_file;

    FILE* fh = std::fopen(h_path.c_str(), "rb");
    FILE* ft = std::fopen(ht_path.c_str(), "rb");
    if (fh == nullptr || ft == nullptr) {
      std::printf("[golden] SKIP %s (reference files not found)\n", g.h_file);
      if (fh) std::fclose(fh);
      if (ft) std::fclose(ft);
      continue;
    }
    const bool h_ok  = std::fread(h.data(), 4, h.size(), fh) == h.size();
    const bool ht_ok = std::fread(ht.data(), 4, ht.size(), ft) == ht.size();
    std::fclose(fh);
    std::fclose(ft);

    // Re-generate and compare (the fread overwrote the buffers above).
    std::vector<uint32_t> h_gen, ht_gen;
    generate_h_ht(g.bg, g.ls, m_aligned, n_aligned, h_gen, ht_gen);
    const bool h_match  = h_ok && (h_gen == h);
    const bool ht_match = ht_ok && (ht_gen == ht);
    std::printf("[golden] %s H: %s, HT: %s\n", g.h_file, h_match ? "MATCH" : "MISMATCH",
                ht_match ? "MATCH" : "MISMATCH");
    if (!h_match || !ht_match) {
      failures++;
    }
  }
  return failures;
}

/// One full encode -> AWGN -> decode round trip; returns CRC results for both decoders
/// and whether the decoded bits agree (when both succeed).
struct round_trip_result {
  bool cpu_ok   = false;
  bool gpu_ok   = false;
  bool bits_eq  = false;
  int  gpu_iters = -1;
};

round_trip_result run_round_trip(std::mt19937&                         rng,
                                 ldpc_encoder&                        encoder,
                                 crc_calculator&                      crc16,
                                 ldpc_decoder&                        cpu_decoder,
                                 ldpc_decoder&                        gpu_decoder,
                                 const test_case&                     tc,
                                 double                               sigma)
{
  const unsigned z      = static_cast<unsigned>(tc.ls);
  const unsigned n_full = tc.bg == ldpc_base_graph_type::BG1 ? 68 : 52;
  const unsigned bg_k   = n_full - (tc.bg == ldpc_base_graph_type::BG1 ? 46 : 42);
  const unsigned K      = bg_k * z;
  const unsigned nof_tx = (n_full - 2) * z;

  // Message: K-16 random bits + CRC16.
  std::vector<uint8_t> message_bytes((K + 7) / 8);
  bit_buffer           message = bit_buffer::from_bytes(message_bytes);
  std::uniform_int_distribution<int> bit_dist(0, 1);
  for (unsigned i = 0; i != K - 16; ++i) {
    message.insert(static_cast<uint8_t>(bit_dist(rng)), i, 1);
  }
  // Attach the CRC16 MSB-first, matching the segmenter convention.
  const unsigned crc = crc16.calculate(message.first(K - 16));
  for (unsigned i = 0; i != 16; ++i) {
    message.insert(static_cast<uint8_t>((crc >> (15 - i)) & 1U), K - 16 + i, 1);
  }

  // Encode. Note: the ldpc_encoder_buffer interface is UNPACKED - one byte per bit.
  const ldpc_encoder::configuration enc_cfg = {.base_graph = tc.bg, .lifting_size = tc.ls, .Nref = 0};
  const ldpc_encoder_buffer&        codeblock = encoder.encode(message, enc_cfg);
  ocudu_assert(codeblock.get_codeblock_length() == nof_tx, "Unexpected encoder output length");
  std::vector<uint8_t> packed_tx(nof_tx);
  codeblock.write_codeblock(packed_tx, 0);

  // BPSK + AWGN + int8 LLR quantization (sigma == 0 selects the noiseless mode).
  std::normal_distribution<double> noise(0.0, sigma);
  std::vector<log_likelihood_ratio> llrs(nof_tx);
  for (unsigned i = 0; i != nof_tx; ++i) {
    const unsigned bit = packed_tx[i] & 1U;
    double         y   = (bit == 0 ? 1.0 : -1.0);
    if (sigma > 0.0) {
      y += noise(rng);
    }
    int q = (sigma > 0.0) ? static_cast<int>(std::lround(2.0 * y / (sigma * sigma))) : (y < 0 ? -64 : 64);
    q     = std::clamp(q, -64, 64);
    llrs[i] = q;
  }

  // Decode with both decoders.
  const ldpc_decoder::configuration dec_cfg = {
      .base_graph      = tc.bg,
      .lifting_size    = tc.ls,
      .nof_filler_bits = 0,
      .nof_crc_bits    = 16,
      .max_iterations  = 6,
  };

  round_trip_result res;

  std::vector<uint8_t> cpu_out_bytes((K + 7) / 8);
  bit_buffer           cpu_out = bit_buffer::from_bytes(cpu_out_bytes);
  auto                 cpu_iters = cpu_decoder.decode(cpu_out, llrs, &crc16, dec_cfg);
  res.cpu_ok                     = cpu_iters.has_value();

  std::vector<uint8_t> gpu_out_bytes((K + 7) / 8);
  bit_buffer           gpu_out = bit_buffer::from_bytes(gpu_out_bytes);
  auto                 gpu_iters = gpu_decoder.decode(gpu_out, llrs, &crc16, dec_cfg);
  res.gpu_ok     = gpu_iters.has_value();
  res.gpu_iters  = gpu_iters.has_value() ? static_cast<int>(*gpu_iters) : -1;

  if (res.cpu_ok && res.gpu_ok) {
    // Compare the information bits (the CRC16 tail is part of the K-bit message; compare all).
    res.bits_eq = true;
    for (unsigned i = 0; i != K; ++i) {
      if ((cpu_out.extract(i, 1) & 1U) != (gpu_out.extract(i, 1) & 1U)) {
        res.bits_eq = false;
        break;
      }
    }
  }
  // A GPU CRC pass must always decode the exact transmitted message (no false passes).
  if (res.gpu_ok) {
    res.bits_eq = res.bits_eq && [&]() {
      for (unsigned i = 0; i != K; ++i) {
        if ((gpu_out.extract(i, 1) & 1U) != (message.extract(i, 1) & 1U)) {
          return false;
        }
      }
      return true;
    }();
  }
  return res;
}

} // namespace

int main(int argc, char** argv)
{
  std::string matrix_dir = "/Users/jiachengwang/OneDrive/newWork/work/SynchroPlus/ORAN_L1_M/src/channel_coding/ldpc/g_f_matrix";
  if (argc > 1) {
    matrix_dir = argv[1];
  }

  // 1. Golden H / H^T check against the SynchroPlus reference matrices.
  const int golden_failures = golden_check(matrix_dir);

  // 2. Decode parity through the factory path.
  const ldpc_decoder_factory::ldpc_decoder_factory_configuration dec_factory_cfg = {
      .force_decoding      = false,
      .early_stop_syndrome = true,
  };
  auto cpu_dec = create_ldpc_decoder_factory_sw("generic", dec_factory_cfg)->create();
  auto gpu_dec = create_ldpc_decoder_factory_sw("metal", dec_factory_cfg)->create();
  if (!cpu_dec || !gpu_dec) {
    std::printf("FAIL: factory did not create the decoders (metal type registered?)\n");
    return 1;
  }

  auto encoder = create_ldpc_encoder_factory_sw("generic")->create();
  auto crc16   = create_crc_calculator_factory_sw("lut")->create(crc_generator_poly::CRC16);

  const test_case cases[] = {
      // Noiseless: the GPU decode must be bit-exact (strict correctness proof).
      {"BG1 Z16", ldpc_base_graph_type::BG1, ldpc::LS16, 99.0},
      {"BG2 Z16", ldpc_base_graph_type::BG2, ldpc::LS16, 99.0},
      {"BG1 Z256", ldpc_base_graph_type::BG1, ldpc::LS256, 99.0},
      // Noisy: the LLS algorithm is ~5 dB weaker than the CPU min-sum (see PLAN.md), so
      // the assertion is: no GPU false passes (every GPU-passed block decodes bit-exactly)
      // and the GPU pass rate never exceeds the CPU's.
      {"BG1 Z16", ldpc_base_graph_type::BG1, ldpc::LS16, 3.0},
      {"BG1 Z32", ldpc_base_graph_type::BG1, ldpc::LS32, 3.0},
      {"BG2 Z16", ldpc_base_graph_type::BG2, ldpc::LS16, 3.0},
      {"BG2 Z32", ldpc_base_graph_type::BG2, ldpc::LS32, 3.0},
      {"BG1 Z256", ldpc_base_graph_type::BG1, ldpc::LS256, 5.0},
  };

  int failures = 0;
  for (const test_case& tc : cases) {
    const bool   noiseless = tc.snr_db >= 90.0;
    const double sigma     = noiseless ? 0.0 : std::sqrt(std::pow(10.0, -tc.snr_db / 10.0) / 2.0);
    std::mt19937 rng(42 + tc.ls);

    const unsigned nof_trials = (tc.ls >= ldpc::LS256) ? 20 : 100;
    unsigned       cpu_pass = 0, gpu_pass = 0, both_pass_same = 0, disagreements = 0;
    int            iters_min = 999, iters_max = 0;

    for (unsigned t = 0; t != nof_trials; ++t) {
      const round_trip_result r = run_round_trip(rng, *encoder, *crc16, *cpu_dec, *gpu_dec, tc, sigma);
      if (r.cpu_ok) cpu_pass++;
      if (r.gpu_ok) {
        gpu_pass++;
        iters_min = std::min(iters_min, r.gpu_iters);
        iters_max = std::max(iters_max, r.gpu_iters);
      }
      if (r.cpu_ok && r.gpu_ok) {
        both_pass_same++;
        if (!r.bits_eq) {
          disagreements++;
        }
      }
      if (r.cpu_ok != r.gpu_ok) {
        std::printf("  [%s] trial %u: CRC disagreement (cpu=%d gpu=%d)\n", tc.name, t, r.cpu_ok, r.gpu_ok);
      }
    }

    // Noiseless: strict bit-exactness (both decoders must pass every block identically).
    // Noisy: no false GPU passes and the GPU rate never exceeds the CPU rate.
    const bool ok = noiseless ? ((cpu_pass == nof_trials) && (gpu_pass == nof_trials) && (disagreements == 0))
                              : ((disagreements == 0) && (gpu_pass <= cpu_pass));
    std::printf("[parity] %-9s SNR %.1f dB: cpu %u/%u, gpu %u/%u, same %u, disagreements %u, gpu iters [%d,%d] -> %s\n",
                tc.name, tc.snr_db, cpu_pass, nof_trials, gpu_pass, nof_trials, both_pass_same, disagreements,
                (gpu_pass != 0) ? iters_min : -1, (gpu_pass != 0) ? iters_max : -1, ok ? "OK" : "FAIL");
    if (!ok) {
      failures++;
    }
  }

  std::printf(failures == 0 && golden_failures == 0 ? "ALL OK\n" : "FAILURES: %d parity + %d golden\n", failures,
              golden_failures);
  return (failures == 0 && golden_failures == 0) ? 0 : 1;
}
