// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "demodulation_mapper_metal.h"
#include "ocudu_demod_metal_engine.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"
#include <cstdint>
#include <cstring>

using namespace ocudu;

namespace {

/// True when the buffer can be handed to the Metal no-copy wrap (page-aligned base address;
/// the engine maps a page-rounded length, so the caller's allocation must cover the tail -
/// page_aligned_allocator does, and only the symbol range within the span is touched).
bool is_page_aligned_buffer(const void* ptr)
{
  return (ptr != nullptr) && ((reinterpret_cast<uintptr_t>(ptr) % compat::page_size()) == 0);
}

} // namespace

struct demodulation_mapper_metal::impl {
  metal::demod_metal_engine engine;
  bool                      engine_ok   = false;
  bool                      path_logged = false;

  // Staging buffers (page-aligned, grown on demand and reused across calls).
  void*  sym_buf = nullptr;
  void*  nv_buf  = nullptr;
  void*  llr_buf = nullptr;
  size_t sym_cap = 0; // bytes
  size_t nv_cap  = 0;
  size_t llr_cap = 0;

  ~impl()
  {
    compat::aligned_free(sym_buf);
    compat::aligned_free(nv_buf);
    compat::aligned_free(llr_buf);
  }

  static void* ensure(void*& buf, size_t& cap, size_t needed)
  {
    if (cap >= needed) {
      return buf;
    }
    compat::aligned_free(buf);
    buf = compat::aligned_alloc(compat::page_size(), needed);
    ocudu_assert(buf != nullptr, "Demapper staging allocation failed.");
    cap = needed;
    return buf;
  }
};

demodulation_mapper_metal::demodulation_mapper_metal() : impl_(std::make_unique<impl>())
{
  impl_->engine_ok = impl_->engine.init();
}

demodulation_mapper_metal::~demodulation_mapper_metal() = default;

bool demodulation_mapper_metal::is_supported(modulation_scheme mod) const
{
  switch (mod) {
    case modulation_scheme::QPSK:
    case modulation_scheme::QAM16:
    case modulation_scheme::QAM64:
    case modulation_scheme::QAM256:
      return impl_->engine_ok;
    default:
      return false;
  }
}

void demodulation_mapper_metal::demodulate_soft(span<log_likelihood_ratio> llrs,
                                                span<const cf_t>           symbols,
                                                span<const float>          noise_vars,
                                                modulation_scheme          mod)
{
  ocudu_assert(symbols.size() == noise_vars.size(), "Inputs symbols and noise_vars must have the same length.");
  ocudu_assert(symbols.size() * get_bits_per_symbol(mod) == llrs.size(), "Input and output lengths are incompatible.");
  ocudu_assert(is_supported(mod), "Unsupported modulation scheme for the Metal demapper.");

  unsigned mod_id = 0;
  switch (mod) {
    case modulation_scheme::QPSK:
      mod_id = 0;
      break;
    case modulation_scheme::QAM16:
      mod_id = 1;
      break;
    case modulation_scheme::QAM64:
      mod_id = 2;
      break;
    case modulation_scheme::QAM256:
      mod_id = 3;
      break;
    default:
      ocudu_assertion_failure("Invalid modulation scheme.");
  }

  const size_t nof_symbols = symbols.size();
  const size_t nof_bits    = nof_symbols * get_bits_per_symbol(mod);
  const size_t sym_bytes   = nof_symbols * 2 * sizeof(float);
  const size_t nv_bytes    = nof_symbols * sizeof(float);
  const size_t llr_bytes   = nof_bits * sizeof(int8_t);

  // The equalized symbols and their noise variances are consumed as-is when they are page
  // aligned (the PUSCH demodulator allocates them that way), otherwise they are staged.
  const bool sym_direct = is_page_aligned_buffer(symbols.data());
  const bool nv_direct  = is_page_aligned_buffer(noise_vars.data());
  const void* sym_ptr = sym_direct ? static_cast<const void*>(symbols.data())
                                   : impl::ensure(impl_->sym_buf, impl_->sym_cap, sym_bytes);
  const void* nv_ptr  = nv_direct ? static_cast<const void*>(noise_vars.data())
                                  : impl::ensure(impl_->nv_buf, impl_->nv_cap, nv_bytes);
  // The LLR destination is the UL-SCH demultiplexer buffer (not page aligned): stage it.
  auto* llr_ptr = static_cast<int8_t*>(impl::ensure(impl_->llr_buf, impl_->llr_cap, llr_bytes));

  if (!sym_direct) {
    std::memcpy(const_cast<void*>(sym_ptr), symbols.data(), sym_bytes);
  }
  if (!nv_direct) {
    std::memcpy(const_cast<void*>(nv_ptr), noise_vars.data(), nv_bytes);
  }

  const bool ok = impl_->engine.demodulate(sym_ptr, nv_ptr, llr_ptr, nof_symbols, mod_id);
  if (!ok) {
    // Engine failure: zero LLRs (the CPU's ill-formed input semantics) instead of stale data.
    std::memset(llrs.data(), 0, llr_bytes);
    return;
  }

  if (!impl_->path_logged) {
    impl_->path_logged = true;
    ocudulog::fetch_basic_logger("PHY").info(
        "Metal demapper: inputs {} (symbols {} noise {}), LLRs staged, engine no-copy wrap {}",
        (sym_direct && nv_direct) ? "read in place" : "staged",
        sym_direct ? "direct" : "staged",
        nv_direct ? "direct" : "staged",
        impl_->engine.last_call_used_no_copy() ? "OK" : "FELL BACK TO COPY");
  }

  std::memcpy(llrs.data(), llr_ptr, llr_bytes);
}

double demodulation_mapper_metal::engine_gpu_wait_us() const
{
  return impl_->engine.last_gpu_wait_us();
}
