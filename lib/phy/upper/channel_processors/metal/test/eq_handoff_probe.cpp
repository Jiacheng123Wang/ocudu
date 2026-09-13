// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Minimal reproducer of the deferred-group stage hand-off defect.
///
/// The PUSCH demodulator's deferred chain accumulates the equalization of a whole group of OFDM
/// symbols and encodes it when the demapping of the same group opens the burst, so the equalization
/// and the demapping of a group share one command buffer. That hand-off is bit-exact in the chain
/// probe and in every single-threaded unit test, but the concurrency test of the demodulator (four
/// demodulations at the same time, one engine each) reads zeros for every symbol of the group but
/// the first one.
///
/// This probe reproduces the sequence alone: several rounds of "submit the group, then demap it,
/// then wait once", against a reference that waits between the two stages. Every round reuses the
/// same buffers, exactly like the demodulator's group slots, so a wrap mapping created by one round
/// is still cached when the next one binds the same output regions.
///
/// Environment overrides: OCUDU_HANDOFF_RE (default 204), OCUDU_HANDOFF_SYMBOLS (default 12),
/// OCUDU_HANDOFF_THREADS (default 1) and OCUDU_HANDOFF_ROUNDS (default 3).

#include "../channel_equalizer_metal.h"
#include "demodulation_mapper_metal.h"

#include "ocudu/adt/bf16.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/support/macos_compat.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <thread>
#include <vector>

using namespace ocudu;

namespace {

/// Page-aligned buffer, rounded up to whole pages (what the Metal no-copy wrap needs).
struct aligned_buffer {
  void*  ptr = nullptr;
  size_t cap = 0;

  void allocate(size_t bytes)
  {
    cap = ((bytes + compat::page_size() - 1) / compat::page_size()) * compat::page_size();
    ptr = compat::aligned_alloc(compat::page_size(), cap);
    std::memset(ptr, 0, cap);
  }
  ~aligned_buffer() { compat::aligned_free(ptr); }

  aligned_buffer()                                = default;
  aligned_buffer(const aligned_buffer&)           = delete;
  aligned_buffer& operator=(const aligned_buffer&) = delete;
};

unsigned env_unsigned(const char* name, unsigned fallback)
{
  const char* env = std::getenv(name);
  return (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : fallback;
}

/// One worker: its own adapters and its own per-symbol buffers, like a demodulator instance.
struct worker {
  unsigned nof_re    = 0;
  unsigned nof_syms  = 0;
  unsigned nof_ports = 0;
  unsigned nof_layers = 1;

  std::unique_ptr<channel_equalizer_metal> equalizer;
  std::unique_ptr<demodulation_mapper_metal> demapper;

  // Inputs, alive for the whole run (the demodulator's grid views).
  std::vector<std::vector<cbf16_t>>                 y;
  std::vector<std::vector<cbf16_t>>                 h;
  std::vector<modular_re_buffer_reader<cbf16_t, 8>> ch_symbols;
  std::vector<modular_ch_est_list<8 * 4>>           ch_est;
  std::vector<float>                                nv_est;

  // Outputs, one page-aligned slot per symbol and stage output.
  aligned_buffer eq;
  aligned_buffer nv;
  aligned_buffer llr_ref;
  aligned_buffer llr_dut;

  size_t eq_gap  = 0;
  size_t nv_gap  = 0;
  size_t llr_gap = 0;
  unsigned bits_per_re = 0;

  void init(unsigned re, unsigned symbols, unsigned ports, unsigned layers, unsigned seed)
  {
    nof_re     = re;
    nof_syms   = symbols;
    nof_ports  = ports;
    nof_layers = layers;

    const size_t page = compat::page_size();
    eq_gap  = ((static_cast<size_t>(nof_re) * layers * sizeof(cf_t) + page - 1) / page) * page / sizeof(cf_t);
    nv_gap  = ((static_cast<size_t>(nof_re) * layers * sizeof(float) + page - 1) / page) * page / sizeof(float);
    bits_per_re = layers * get_bits_per_symbol(modulation_scheme::QPSK);
    llr_gap = ((static_cast<size_t>(nof_re) * bits_per_re + page - 1) / page) * page;

    equalizer = std::make_unique<channel_equalizer_metal>(false);
    demapper  = std::make_unique<demodulation_mapper_metal>();

    std::mt19937                    rgen(seed);
    std::normal_distribution<float> dist(0.0F, 0.3F);
    std::uniform_real_distribution<float> ch_dist(-0.5F, 0.5F);

    y.assign(nof_ports, std::vector<cbf16_t>(nof_re));
    h.assign(static_cast<size_t>(nof_ports) * layers, std::vector<cbf16_t>(nof_re));
    for (auto& slice : y) {
      for (cbf16_t& v : slice) {
        v = cbf16_t(dist(rgen), dist(rgen));
      }
    }
    for (auto& slice : h) {
      for (cbf16_t& v : slice) {
        v = cbf16_t(1.0F + ch_dist(rgen), ch_dist(rgen));
      }
    }

    ch_symbols.reserve(nof_syms);
    ch_est.reserve(nof_syms);
    for (unsigned s = 0; s != nof_syms; ++s) {
      ch_symbols.emplace_back(nof_ports, nof_re);
      for (unsigned p = 0; p != nof_ports; ++p) {
        ch_symbols.back().set_slice(p, y[p]);
      }
      ch_est.emplace_back(nof_re, nof_ports, layers);
      for (unsigned p = 0; p != nof_ports; ++p) {
        for (unsigned l = 0; l != layers; ++l) {
          ch_est.back().set_channel(h[static_cast<size_t>(p) * layers + l], p, l);
        }
      }
    }
    nv_est.assign(nof_ports, 0.02F);

    eq.allocate(static_cast<size_t>(eq_gap) * nof_syms * sizeof(cf_t));
    nv.allocate(static_cast<size_t>(nv_gap) * nof_syms * sizeof(float));
    llr_ref.allocate(static_cast<size_t>(llr_gap) * nof_syms);
    llr_dut.allocate(static_cast<size_t>(llr_gap) * nof_syms);
  }

  span<cf_t> eq_of(unsigned s)
  {
    return span<cf_t>(static_cast<cf_t*>(eq.ptr) + static_cast<size_t>(s) * eq_gap,
                      static_cast<size_t>(nof_re) * nof_layers);
  }
  span<float> nv_of(unsigned s)
  {
    return span<float>(static_cast<float*>(nv.ptr) + static_cast<size_t>(s) * nv_gap,
                       static_cast<size_t>(nof_re) * nof_layers);
  }
  span<log_likelihood_ratio> llr_of(aligned_buffer& buf, unsigned s)
  {
    return span<log_likelihood_ratio>(static_cast<log_likelihood_ratio*>(buf.ptr) +
                                          static_cast<size_t>(s) * llr_gap,
                                      static_cast<size_t>(nof_re) * bits_per_re);
  }

  /// Reference: the serial chain, one wait per symbol and stage.
  void run_reference()
  {
    for (unsigned s = 0; s != nof_syms; ++s) {
      equalizer->equalize(eq_of(s), nv_of(s), ch_symbols[s], ch_est[s], nv_est, 1.0F);
      demapper->demodulate_soft(llr_of(llr_ref, s), eq_of(s), nv_of(s), modulation_scheme::QPSK);
    }
  }

  /// Unit under test: the deferred group, one wait per stage and group.
  void run_deferred()
  {
    for (unsigned s = 0; s != nof_syms; ++s) {
      equalizer->submit(eq_of(s), nv_of(s), ch_symbols[s], ch_est[s], nv_est, 1.0F);
    }
    for (unsigned s = 0; s != nof_syms; ++s) {
      demapper->submit(llr_of(llr_dut, s), eq_of(s), nv_of(s), modulation_scheme::QPSK);
    }
    demapper->wait();
    equalizer->wait();
  }

  /// True when the deferred group produced the reference soft bits.
  bool matches() const
  {
    for (unsigned s = 0; s != nof_syms; ++s) {
      const auto* a = static_cast<const log_likelihood_ratio*>(llr_ref.ptr) + static_cast<size_t>(s) * llr_gap;
      const auto* b = static_cast<const log_likelihood_ratio*>(llr_dut.ptr) + static_cast<size_t>(s) * llr_gap;
      const size_t n = static_cast<size_t>(nof_re) * bits_per_re;
      if (std::memcmp(a, b, n) != 0) {
        return false;
      }
    }
    return true;
  }
};

} // namespace

int main()
{
  const unsigned nof_re   = env_unsigned("OCUDU_HANDOFF_RE", 204);
  const unsigned nof_syms = env_unsigned("OCUDU_HANDOFF_SYMBOLS", 12);
  const unsigned threads  = std::max(1U, env_unsigned("OCUDU_HANDOFF_THREADS", 1));
  const unsigned rounds   = std::max(1U, env_unsigned("OCUDU_HANDOFF_ROUNDS", 3));
  const unsigned ports    = 2;
  const unsigned layers   = 1;

  std::printf("[handoff] re=%u symbols=%u ports=%u threads=%u rounds=%u deferred=%s\n",
              nof_re,
              nof_syms,
              ports,
              threads,
              rounds,
              (std::getenv("OCUDU_EQ_DEFER_ENCODE") != nullptr) ? "yes" : "no");

  std::atomic<unsigned> failures{0};
  std::atomic<bool>     go{false};
  std::vector<std::unique_ptr<worker>> workers;
  for (unsigned w = 0; w != threads; ++w) {
    workers.push_back(std::make_unique<worker>());
    workers.back()->init(nof_re, nof_syms, ports, layers, 0x1234 + w);
  }

  std::vector<std::thread> pool;
  for (unsigned w = 0; w != threads; ++w) {
    pool.emplace_back([&, w]() {
      while (!go.load(std::memory_order_acquire)) {
      }
      for (unsigned r = 0; r != rounds; ++r) {
        workers[w]->run_reference();
        workers[w]->run_deferred();
        if (!workers[w]->matches()) {
          failures.fetch_add(1, std::memory_order_relaxed);
          std::fprintf(stderr, "[handoff] worker %u round %u: MISMATCH\n", w, r);
        }
      }
    });
  }
  go.store(true, std::memory_order_release);
  for (std::thread& t : pool) {
    t.join();
  }

  std::printf("[handoff] %u of %u rounds mismatched -> %s\n",
              failures.load(),
              threads * rounds,
              (failures.load() == 0) ? "OK" : "MISMATCH");
  return (failures.load() == 0) ? 0 : 1;
}
