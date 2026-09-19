// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "demodulation_mapper_metal.h"
#include "ocudu_demod_metal_engine.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/support/macos_compat.h"
#include "ocudu/support/ocudu_assert.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace ocudu;

namespace {

/// \brief Declares the demapper's coverage to the fused-lane crossing counter (\c declare_reporter).
///
/// Audited 2026-09-19 against the lane route, and the audit found NO host <-> device data movement:
///
///   * its inputs are the equalizer's output, consumed in place when the buffers are page-aligned.
///     The PUSCH demodulator allocates them with page_aligned_allocator (temp_eq_re /
///     temp_eq_noise_vars) and hands a page-aligned slot per OFDM symbol, so is_page_aligned_buffer()
///     is true and no copy is staged;
///   * its output LLRs go to the kernel direct for the same reason (temp_llr is page-aligned too),
///     so there is no copy-back either;
///   * the hand-off from the equalization dispatches is a MEMORY BARRIER inside the shared command
///     buffer, not a wait - see pusch_demodulator_impl::demodulate's fused path;
///   * the only CPU involvement is the batch commit and the single wait for the group, which is what
///     the criterion allows.
///
/// \note The counters are on the FALLBACKS, not on the happy path, and that is deliberate: the
///       fallbacks are what a regression would newly exercise. Before this the demapper had no
///       counter at all, so if the PUSCH demodulator stopped allocating those buffers page-aligned
///       the entire equalized-symbol stream would cross the host mid-lane and the contract's
///       crossing check would still have read 0. Now that shows up as a write (the staging) plus a
///       read (the copy-back) against THIS module's name, which is what makes the number an upper
///       bound rather than "the modules we happened to look at".
struct demapper_crossing_declaration {
  demapper_crossing_declaration() { ocudu::phy_pipeline_crossings::declare_reporter("demapper"); }
} demapper_crossing_declaration_instance;

} // namespace


namespace {

/// True when the buffer can be handed to the Metal no-copy wrap (page-aligned base address;
/// the engine maps a page-rounded length, so the caller's allocation must cover the tail -
/// page_aligned_allocator does, and only the symbol range within the span is touched).
bool is_page_aligned_buffer(const void* ptr)
{
  return (ptr != nullptr) && ((reinterpret_cast<uintptr_t>(ptr) % compat::page_size()) == 0);
}

} // namespace

void demodulation_mapper_metal::staging::swap(staging& other) noexcept
{
  std::swap(ptr, other.ptr);
  std::swap(cap, other.cap);
}

demodulation_mapper_metal::staging& demodulation_mapper_metal::staging::operator=(staging&& other) noexcept
{
  if (this != &other) {
    compat::aligned_free(ptr);
    ptr       = other.ptr;
    cap       = other.cap;
    other.ptr = nullptr;
    other.cap = 0;
  }
  return *this;
}

demodulation_mapper_metal::staging::~staging()
{
  compat::aligned_free(ptr);
}

void* demodulation_mapper_metal::staging::ensure(size_t needed)
{
  if (cap >= needed) {
    return ptr;
  }
  compat::aligned_free(ptr);
  ptr = compat::aligned_alloc(compat::page_size(), needed);
  ocudu_assert(ptr != nullptr, "Demapper staging allocation failed.");
  cap = needed;
  return ptr;
}

struct demodulation_mapper_metal::impl {
  metal::demod_metal_engine engine;
  bool                      engine_ok   = false;
  bool                      path_logged = false;

  /// In-flight deferred submits, oldest first. All of them are committed to the shared back-end
  /// queue in order, so a single wait on the newest command buffer covers the whole FIFO.
  std::vector<std::unique_ptr<pending_entry>> pending;
  /// Entries released by wait(), kept so their staging buffers stay warm.
  std::vector<std::unique_ptr<pending_entry>> pool;
  /// Staging for the synchronous path (never in flight).
  pending_entry scratch;

  std::unique_ptr<pending_entry> acquire()
  {
    if (pool.empty()) {
      return std::make_unique<pending_entry>();
    }
    std::unique_ptr<pending_entry> entry = std::move(pool.back());
    pool.pop_back();
    return entry;
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
  run_demodulate(llrs, symbols, noise_vars, mod, false);
}

void demodulation_mapper_metal::submit(span<log_likelihood_ratio> llrs,
                                       span<const cf_t>           symbols,
                                       span<const float>          noise_vars,
                                       modulation_scheme          mod)
{
  run_demodulate(llrs, symbols, noise_vars, mod, true);
}

void demodulation_mapper_metal::wait()
{
  // Close the shared burst of this group and wait for it: it holds the dispatches of every stage.
  if (impl_->engine.burst_open()) {
    (void)impl_->engine.burst_commit();
  }
  (void)impl_->engine.burst_wait_committed();
  if (impl_->pending.empty()) {
    return;
  }
  for (std::unique_ptr<pending_entry>& entry : impl_->pending) {
    if (!entry->llr_direct) {
      // The kernel wrote the LLRs into the staging buffer: copy them into the caller's span,
      // exactly like the synchronous path does.
      //
      // CROSSING (device -> host), counted only when it happens: the LLRs the GPU produced coming
      // back through host memory while the lane is still running. With the demodulator's
      // page-aligned LLR slots it does not happen - see the input-side note in run_demodulate().
      phy_pipeline_crossings::count_host_read(entry->llr_staged);
      std::memcpy(entry->llrs.data(), entry->llr_ptr, entry->llr_sz);
    }
    impl_->pool.push_back(std::move(entry));
  }
  impl_->pending.clear();
}

void demodulation_mapper_metal::run_demodulate(span<log_likelihood_ratio> llrs,
                                               span<const cf_t>           symbols,
                                               span<const float>          noise_vars,
                                               modulation_scheme          mod,
                                               bool                       defer)
{
  ocudu_assert(symbols.size() == noise_vars.size(), "Inputs symbols and noise_vars must have the same length.");
  ocudu_assert(symbols.size() * get_bits_per_symbol(mod) == llrs.size(), "Input and output lengths are incompatible.");
  ocudu_assert(is_supported(mod), "Unsupported modulation scheme for the Metal demapper.");

  // Batched group encoding of the deferred chain (the default). OCUDU_DEMOD_DEFER_ENCODE=0 restores
  // the per-symbol encoding, which stays bit-exact - it is what the offline A/B gate compares the
  // batched path against.
  static const bool defer_encode = []() {
    const char* env = std::getenv("OCUDU_DEMOD_DEFER_ENCODE");
    return (env == nullptr) || (std::strtoul(env, nullptr, 10) != 0);
  }();

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
  std::unique_ptr<pending_entry> owned;
  pending_entry*                 entry = &impl_->scratch;
  if (defer) {
    owned = impl_->acquire();
    entry = owned.get();
  }

  const bool sym_direct = is_page_aligned_buffer(symbols.data());
  const bool nv_direct  = is_page_aligned_buffer(noise_vars.data());
  const void* sym_ptr = sym_direct ? static_cast<const void*>(symbols.data()) : entry->sym.ensure(sym_bytes);
  const void* nv_ptr  = nv_direct ? static_cast<const void*>(noise_vars.data()) : entry->nv.ensure(nv_bytes);
  // The LLR destination is usually the UL-SCH demultiplexer buffer (not page aligned) and is
  // staged. A page-aligned destination (the deferred chain's per-symbol staging) is written by
  // the kernel directly, which removes the copy-back entirely.
  const bool llr_direct = is_page_aligned_buffer(llrs.data());
  void*      llr_ptr    = llr_direct ? static_cast<void*>(llrs.data()) : entry->llr.ensure(llr_bytes);

  if (!sym_direct) {
    std::memcpy(const_cast<void*>(sym_ptr), symbols.data(), sym_bytes);
  }
  if (!nv_direct) {
    std::memcpy(const_cast<void*>(nv_ptr), noise_vars.data(), nv_bytes);
  }
  // CROSSING (host -> device), counted only when it HAPPENS: the two memcpys above move the
  // equalizer's symbols and their noise variances through host memory, and the kernel then reads
  // what the host wrote. On the fused lane both are page-aligned slots of the PUSCH demodulator's
  // group buffers (page_aligned_allocator), so this stays zero and the wrap is in place - which is
  // exactly why it is counted here rather than assumed away.
  //
  // \note The demapper had NO crossing counter at all before this, so a PUSCH demodulator that
  //       stopped allocating those buffers page-aligned would have moved every equalized symbol
  //       through the host - float data, in the middle of the lane - while the contract's crossing
  //       check still read 0. A silent fallback is the shape of defect these counters exist to
  //       refuse, so the fallback is measured, not assumed absent.
  if (!sym_direct) {
    phy_pipeline_crossings::count_host_write_site("demapper: equalized symbols staged (host)", sym_bytes);
    entry->sym_staged += sym_bytes;
  }
  if (!nv_direct) {
    phy_pipeline_crossings::count_host_write_site("demapper: noise variances staged (host)", nv_bytes);
    entry->nv_staged += nv_bytes;
  }

  if (defer) {
    // A group is accumulated and the burst's flush hook encodes a run of its symbols as ONE
    // dispatch: the per-symbol slots of the demodulator's group buffers are page-aligned, so the
    // kernel is handed their strides and nothing is staged (see enqueue_burst_deferred). The memory
    // barrier that the pipeline change inserts orders this stage after the equalization.
    const bool ok = defer_encode ? impl_->engine.enqueue_burst_deferred(sym_ptr, nv_ptr, llr_ptr, nof_symbols, mod_id)
                                 : impl_->engine.enqueue_burst(sym_ptr, nv_ptr, llr_ptr, nof_symbols, mod_id);
    if (!ok) {
      // Engine failure: zero LLRs (the CPU's ill-formed input semantics) instead of stale data.
      std::memset(llrs.data(), 0, llr_bytes);
      return;
    }
  } else {
    // Synchronous path: never share a command buffer with an unfinished burst.
    if (impl_->engine.burst_open()) {
      (void)impl_->engine.burst_commit();
      (void)impl_->engine.burst_wait_committed();
    }
    if (!impl_->engine.batch_open()) {
      (void)impl_->engine.begin_batch();
    }
    const bool ok = impl_->engine.enqueue(sym_ptr, nv_ptr, llr_ptr, nof_symbols, mod_id);
    if (!ok) {
      // Engine failure: zero LLRs (the CPU's ill-formed input semantics) instead of stale data.
      (void)impl_->engine.commit_batch();
      (void)impl_->engine.wait_committed();
      std::memset(llrs.data(), 0, llr_bytes);
      return;
    }
  }

  if (!impl_->path_logged) {
    impl_->path_logged = true;
    ocudulog::fetch_basic_logger("PHY").info("Metal demapper: inputs {} (symbols {} noise {}), LLRs {}, engine "
                                             "no-copy wrap {}",
                                             (sym_direct && nv_direct) ? "read in place" : "staged",
                                             sym_direct ? "direct" : "staged",
                                             nv_direct ? "direct" : "staged",
                                             llr_direct ? "written in place" : "staged",
                                             impl_->engine.last_call_used_no_copy() ? "OK" : "FELL BACK TO COPY");
  }

  if (defer) {
    // Submitted for the fused chain: neither the commit nor the wait happen here. The dispatch
    // stays encoded in the shared burst (one command buffer for every stage of the group) and
    // wait() closes it, also copying back the staged LLRs.
    entry->llrs       = llrs;
    entry->llr_ptr    = llr_ptr;
    entry->llr_sz     = llr_bytes;
    entry->llr_direct = llr_direct;
    // 0 when the kernel wrote the caller's own page-aligned slot; the byte count when it wrote the
    // staging buffer, which wait() must then bring back through the host.
    entry->llr_staged = llr_direct ? 0 : llr_bytes;
    impl_->pending.push_back(std::move(owned));
    return;
  }

  (void)impl_->engine.commit_batch();
  impl_->engine.wait_committed();

  if (!llr_direct) {
    // The same crossing as in wait(), on the route that does not defer.
    phy_pipeline_crossings::count_host_read(llr_bytes);
    std::memcpy(llrs.data(), llr_ptr, llr_bytes);
  }
}

double demodulation_mapper_metal::engine_gpu_wait_us() const
{
  return impl_->engine.last_gpu_wait_us();
}
