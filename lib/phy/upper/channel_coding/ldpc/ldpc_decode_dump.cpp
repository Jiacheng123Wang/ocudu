// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ldpc_decode_dump.h"
#include "ocudu/support/ocudu_assert.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace ocudu;

bool ldpc_decode_dump::enabled_flag = false;

ldpc_decode_dump& ldpc_decode_dump::instance()
{
  static ldpc_decode_dump the_instance;
  return the_instance;
}

ldpc_decode_dump::ldpc_decode_dump()
{
  const char* dir = std::getenv("OCUDU_LDPC_DUMP");
  if ((dir == nullptr) || (dir[0] == '\0')) {
    return;
  }
  std::snprintf(directory, sizeof(directory), "%s", dir);
  dumping_enabled = true;
  enabled_flag    = true;

  if (const char* iters = std::getenv("OCUDU_LDPC_DUMP_ITERS")) {
    dump_iterations = (iters[0] != '0');
  }
  if (const char* count = std::getenv("OCUDU_LDPC_DUMP_COUNT")) {
    max_blocks = static_cast<unsigned>(std::strtoul(count, nullptr, 10));
  }
  std::fprintf(stderr,
               "[ldpc-dump] recording decoder inputs and soft outputs into '%s'%s%s\n",
               directory,
               dump_iterations ? ", with per-iteration soft bits" : "",
               (max_blocks != 0) ? ", until the block cap" : ", without a block cap");
}

void ldpc_decode_dump::record(span<const log_likelihood_ratio> input,
                              span<const log_likelihood_ratio> soft_final,
                              span<const int8_t>               soft_per_iter,
                              span<const uint16_t>             syndrome_weight,
                              unsigned                         iterations,
                              bool                             success,
                              unsigned                         lifting_size,
                              unsigned                         bg_K)
{
  if (!dumping_enabled) {
    return;
  }
  if ((max_blocks != 0) && (written >= max_blocks)) {
    return;
  }

  char path[640];
  std::snprintf(path, sizeof(path), "%s/ldpc_%06u.bin", directory, written);
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    // A dump that cannot be written must not break the decode: this path is instrumentation.
    return;
  }

  // A tiny, self-describing header. Everything is little-endian and 32-bit on purpose: the reader is a few lines of
  // Python and the files are small.
  struct header_t {
    uint32_t magic;
    uint32_t nof_llrs;
    uint32_t iterations;
    uint32_t success;
    uint32_t lifting_size;
    uint32_t bg_K;
    uint32_t nof_iteration_entries;
  } header = {0x44335031U, // "D3P1"
              static_cast<uint32_t>(input.size()),
              iterations,
              success ? 1U : 0U,
              lifting_size,
              bg_K,
              static_cast<uint32_t>(syndrome_weight.size())};
  std::fwrite(&header, sizeof(header), 1, f);

  // The LLRs the decoder was handed, as bytes: the decoder's own type is a signed byte and re-encoding it would lose
  // exactly the saturation behaviour the network has to learn.
  std::fwrite(input.data(), sizeof(int8_t), input.size(), f);
  std::fwrite(soft_final.data(), sizeof(int8_t), soft_final.size(), f);
  if (!syndrome_weight.empty()) {
    std::fwrite(syndrome_weight.data(), sizeof(uint16_t), syndrome_weight.size(), f);
  }
  if (!soft_per_iter.empty()) {
    std::fwrite(soft_per_iter.data(), sizeof(int8_t), soft_per_iter.size(), f);
  }
  std::fclose(f);
  ++written;
}
