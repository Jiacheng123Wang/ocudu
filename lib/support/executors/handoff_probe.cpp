// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu/support/executors/handoff_probe.h"
#include "ocudu/support/scheduling/thread_sched_snapshot.h" // this_thread_cpu_ns(): Mach on macOS, RUSAGE_THREAD on Linux
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#if defined(__APPLE__)
#include <mach/mach_time.h>
#else
#include <chrono>
#endif

using namespace ocudu;

namespace {

/// The probe is a two-key instrument: compiled in always, switched on by the environment. Reading the variable once
/// is what makes the disabled cost a single predictable branch.
bool probe_enabled()
{
  static const bool enabled = []() {
    const char* v = std::getenv("OCUDU_UL_HANDOFF_PROBE");
    return v != nullptr and v[0] != '\0' and v[0] != '0';
  }();
  return enabled;
}

uint64_t now_ns()
{
#if defined(__APPLE__)
  static const double ns_per_tick = []() {
    mach_timebase_info_data_t tb{};
    mach_timebase_info(&tb);
    return static_cast<double>(tb.numer) / static_cast<double>(tb.denom);
  }();
  return static_cast<uint64_t>(static_cast<double>(mach_absolute_time()) * ns_per_tick);
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
#endif
}

/// 1 us resolution up to 4096 us. The hand-offs chased here are tens of microseconds (a poll phase, a wake-up), so a
/// microsecond of resolution over four milliseconds keeps both the body and the tail in one flat array, and the report
/// stays a scan instead of a sort.
constexpr unsigned nof_buckets = 4096;
constexpr unsigned bucket_ns   = 1000;

struct site_readings {
  std::atomic<uint64_t> buckets[nof_buckets];
  std::atomic<uint64_t> nof_samples{0};
  std::atomic<uint64_t> nof_overflow{0};
  std::atomic<uint64_t> sum_ns{0};
  std::atomic<uint64_t> max_ns{0};

  site_readings()
  {
    for (auto& b : buckets) {
      b.store(0, std::memory_order_relaxed);
    }
  }
};

site_readings& readings_of(handoff_site site)
{
  static site_readings sites[static_cast<unsigned>(handoff_site::nof_sites)];
  return sites[static_cast<unsigned>(site)];
}

/// The CPU one ACTIVATION burned, per site (dev doc 11.69). Same shape as the latency histogram, because the
/// question has the same form: what does one of these cost, in the body and in the tail. The number a
/// `computation` declaration is written from is the p99.9 of THIS, not the mean of a window that aggregated
/// several activations.
struct site_cpu_readings {
  std::atomic<uint64_t> buckets[nof_buckets];
  std::atomic<uint64_t> nof_samples{0};
  std::atomic<uint64_t> nof_overflow{0};
  std::atomic<uint64_t> sum_ns{0};
  std::atomic<uint64_t> max_ns{0};

  site_cpu_readings()
  {
    for (auto& b : buckets) {
      b.store(0, std::memory_order_relaxed);
    }
  }
};

site_cpu_readings& cpu_readings_of(handoff_site site)
{
  static site_cpu_readings sites[static_cast<unsigned>(handoff_site::nof_sites)];
  return sites[static_cast<unsigned>(site)];
}

const char* site_name(handoff_site site)
{
  switch (site) {
    case handoff_site::rx_to_ul:
      return "rx_to_ul";
    case handoff_site::ul_to_lane:
      return "ul_to_lane";
    default:
      return "?";
  }
}

/// Smallest hand-off seen, in microseconds (the first non-empty bucket).
uint64_t min_us(const site_readings& r)
{
  for (unsigned i = 0; i != nof_buckets; ++i) {
    if (r.buckets[i].load(std::memory_order_relaxed) != 0) {
      return static_cast<uint64_t>(i) + 1;
    }
  }
  return 0;
}

/// Percentile in microseconds, from the histogram (so it is resolved to 1 us and rounded up to the bucket).
uint64_t percentile_us(const site_readings& r, uint64_t nof, double q)
{
  const uint64_t target = static_cast<uint64_t>(static_cast<double>(nof) * q + 0.5);
  uint64_t       seen   = 0;
  for (unsigned i = 0; i != nof_buckets; ++i) {
    seen += r.buckets[i].load(std::memory_order_relaxed);
    if (seen >= target) {
      return static_cast<uint64_t>(i) + 1;
    }
  }
  return nof_buckets; // Everything that overflowed sits beyond the array; the caller prints the overflow count too.
}

void report_site(handoff_site site)
{
  const site_readings& r   = readings_of(site);
  const uint64_t       nof = r.nof_samples.load(std::memory_order_relaxed);
  if (nof == 0) {
    return;
  }

  const uint64_t sum      = r.sum_ns.load(std::memory_order_relaxed);
  const uint64_t max      = r.max_ns.load(std::memory_order_relaxed);
  const uint64_t overflow = r.nof_overflow.load(std::memory_order_relaxed);
  const uint64_t first    = min_us(r);
  const char*    name     = site_name(site);

  std::fprintf(stderr,
               "[ul_handoff] %s: n=%llu min=%lluus p50=%lluus p90=%lluus p99=%lluus p99.9=%lluus max=%lluus "
               "mean=%.1fus over4ms=%llu\n",
               name,
               static_cast<unsigned long long>(nof),
               static_cast<unsigned long long>(first),
               static_cast<unsigned long long>(percentile_us(r, nof, 0.50)),
               static_cast<unsigned long long>(percentile_us(r, nof, 0.90)),
               static_cast<unsigned long long>(percentile_us(r, nof, 0.99)),
               static_cast<unsigned long long>(percentile_us(r, nof, 0.999)),
               static_cast<unsigned long long>(max / bucket_ns),
               static_cast<double>(sum) / static_cast<double>(nof) / 1000.0,
               static_cast<unsigned long long>(overflow));

  // The shape, not just the percentiles: 8 us bins over the first 256 us. A consumer that polls shows a plateau whose
  // width is the poll period; one woken by a push shows a spike at the first bins. The two are the same p50.
  constexpr unsigned nof_bins     = 32;
  constexpr unsigned bin_width_us = 8;
  std::fprintf(stderr, "[ul_handoff] %s hist_us", name);
  for (unsigned bin = 0; bin != nof_bins; ++bin) {
    uint64_t count = 0;
    for (unsigned b = bin * bin_width_us; b != (bin + 1) * bin_width_us; ++b) {
      count += r.buckets[b].load(std::memory_order_relaxed);
    }
    std::fprintf(stderr, " %llu", static_cast<unsigned long long>(count));
  }
  std::fprintf(stderr, " (bins of %uus, from 0)\n", bin_width_us);
}

/// The per-activation CPU reading (see handoff_probe_note_cpu). Printed next to the latency line for the same
/// site, because they answer the two questions a declaration needs: how long the hand-off took, and how much CPU
/// the work it started actually burned.
void report_cpu_site(handoff_site site)
{
  const site_cpu_readings& r   = cpu_readings_of(site);
  const uint64_t           nof = r.nof_samples.load(std::memory_order_relaxed);
  if (nof == 0) {
    return;
  }
  const auto pct_us = [&r](uint64_t n, double q) -> uint64_t {
    const uint64_t target = static_cast<uint64_t>(q * static_cast<double>(n) + 0.5);
    uint64_t       seen   = 0;
    for (unsigned i = 0; i != nof_buckets; ++i) {
      seen += r.buckets[i].load(std::memory_order_relaxed);
      if (seen >= target) {
        return static_cast<uint64_t>(i) + 1;
      }
    }
    return nof_buckets;
  };
  std::fprintf(stderr,
               "[ul_handoff_cpu] %s: n=%llu p50=%lluus p90=%lluus p99=%lluus p99.9=%lluus max=%lluus mean=%.1fus "
               "over4ms=%llu  <- CPU of ONE activation; a `computation` declaration is written from p99.9\n",
               site_name(site),
               static_cast<unsigned long long>(nof),
               static_cast<unsigned long long>(pct_us(nof, 0.50)),
               static_cast<unsigned long long>(pct_us(nof, 0.90)),
               static_cast<unsigned long long>(pct_us(nof, 0.99)),
               static_cast<unsigned long long>(pct_us(nof, 0.999)),
               static_cast<unsigned long long>(r.max_ns.load(std::memory_order_relaxed) / bucket_ns),
               static_cast<double>(r.sum_ns.load(std::memory_order_relaxed)) / static_cast<double>(nof) / 1000.0,
               static_cast<unsigned long long>(r.nof_overflow.load(std::memory_order_relaxed)));
}

} // namespace

bool ocudu::handoff_probe_enabled()
{
  return probe_enabled();
}

uint64_t ocudu::handoff_probe_now_ns()
{
  return probe_enabled() ? now_ns() : 0;
}

void ocudu::handoff_probe_note(handoff_site site, uint64_t pushed_ns)
{
  if (pushed_ns == 0) {
    return;
  }
  const uint64_t delta = now_ns() - pushed_ns;
  site_readings& r     = readings_of(site);

  r.nof_samples.fetch_add(1, std::memory_order_relaxed);
  r.sum_ns.fetch_add(delta, std::memory_order_relaxed);
  uint64_t prev_max = r.max_ns.load(std::memory_order_relaxed);
  while (delta > prev_max and not r.max_ns.compare_exchange_weak(prev_max, delta, std::memory_order_relaxed)) {
  }

  const uint64_t idx = delta / bucket_ns;
  if (idx < nof_buckets) {
    r.buckets[idx].fetch_add(1, std::memory_order_relaxed);
  } else {
    r.nof_overflow.fetch_add(1, std::memory_order_relaxed);
  }
}

uint64_t ocudu::handoff_probe_thread_cpu_ns()
{
  // The portable reader is this workflow's own (Mach THREAD_BASIC_INFO on macOS, RUSAGE_THREAD on Linux, behind
  // one interface - see thread_sched_snapshot.h). Reusing it is what keeps this file free of platform code.
  return probe_enabled() ? static_cast<uint64_t>(this_thread_cpu_ns()) : 0;
}

void ocudu::handoff_probe_note_cpu(handoff_site site, uint64_t cpu_begin_ns)
{
  if (cpu_begin_ns == 0) {
    return;
  }
  const uint64_t now_cpu_ns = static_cast<uint64_t>(this_thread_cpu_ns());
  if (now_cpu_ns < cpu_begin_ns) {
    return; // A thread CPU counter that went backwards is not a sample this histogram can carry.
  }
  const uint64_t     delta = now_cpu_ns - cpu_begin_ns;
  site_cpu_readings& r     = cpu_readings_of(site);

  r.nof_samples.fetch_add(1, std::memory_order_relaxed);
  r.sum_ns.fetch_add(delta, std::memory_order_relaxed);
  uint64_t prev_max = r.max_ns.load(std::memory_order_relaxed);
  while (delta > prev_max and not r.max_ns.compare_exchange_weak(prev_max, delta, std::memory_order_relaxed)) {
  }
  const uint64_t idx = delta / bucket_ns;
  if (idx < nof_buckets) {
    r.buckets[idx].fetch_add(1, std::memory_order_relaxed);
  } else {
    r.nof_overflow.fetch_add(1, std::memory_order_relaxed);
  }
}

void ocudu::handoff_probe_report()
{
  for (unsigned i = 0; i != static_cast<unsigned>(handoff_site::nof_sites); ++i) {
    report_site(static_cast<handoff_site>(i));
    report_cpu_site(static_cast<handoff_site>(i));
  }
}
