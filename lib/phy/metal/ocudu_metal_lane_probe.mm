// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_lane_probe.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

using namespace ocudu;

namespace ocudu {
namespace metal {

#if defined(OCUDU_METAL_STATS)

namespace {

/// A command buffer of the open lane, waiting for its GPU timestamps to become valid.
struct lane_entry {
  id<MTLCommandBuffer> cb    = nil;
  gpu_lane_probe::stage which = gpu_lane_probe::stage::other;
};

/// Command buffers of the lane this thread is filling. One lane per thread: the chained stages of
/// one slot (estimator, equalizer, demapper) run on the same thread, and a thread that processes
/// one slot after another closes a lane before it opens the next one.
struct lane_thread_state {
  std::vector<lane_entry> pending;
};

lane_thread_state& thread_state()
{
  static thread_local lane_thread_state s;
  return s;
}

/// The transforms of the slot currently being submitted by this thread, waiting to be accounted.
///
/// Thread local for the same reason as the lane state, and lockless for a stronger one: these
/// registrations happen on the radio thread, per symbol, on the path the real-time uplink depends on.
struct front_end_state {
  uint64_t                         slot = 0;
  bool                             open = false;
  std::vector<id<MTLCommandBuffer>> cbs;
};

front_end_state& front_end_thread_state()
{
  // Never destroyed on purpose, for the same reason as stats(): the report runs from an atexit handler,
  // which runs AFTER the thread-local destructors of the main thread - a function-local object would be
  // an empty vector by the time the front end is reported (measured: the series came out as "no lanes
  // recorded" with the command buffers already gone).
  static thread_local front_end_state* s = new front_end_state();
  return *s;
}

/// Process-wide statistics, accumulated by every thread's closed lanes.
struct lane_stats_t {
  std::mutex mutex;

  std::vector<double> residency_us;
  std::vector<double> busy_us;
  std::vector<double> gap_us;
  std::vector<double> period_us;

  /// Busy time and command buffers per stage (index = stage).
  double   stage_busy_us[static_cast<unsigned>(gpu_lane_probe::stage::count)] = {};
  uint64_t stage_cbs[static_cast<unsigned>(gpu_lane_probe::stage::count)]     = {};

  uint64_t lanes        = 0;
  uint64_t cbs          = 0;
  uint64_t cbs_max      = 0;
  uint64_t dropped_cbs  = 0; ///< completed without usable GPU timestamps
  uint64_t carried_over = 0; ///< not completed, or not started, at the lane close

  /// End of the last lane published, for the period series. Lanes close on several threads, so an
  /// out-of-order publication is dropped from the series rather than reported as a negative period.
  double last_lane_end_s = 0.0;
  bool   has_last_end    = false;
  uint64_t period_dropped = 0;

  /// The front-end (DFT) timeline, one group per slot (see register_front_end_commit()).
  std::vector<double> fe_residency_us;
  std::vector<double> fe_busy_us;
  std::vector<double> fe_gap_us;
  uint64_t            fe_slots       = 0;
  uint64_t            fe_cbs         = 0;
  uint64_t            fe_carried     = 0;
};

lane_stats_t& stats()
{
  // Never destroyed on purpose: the report runs from an atexit handler, which runs AFTER the static
  // destructors of this translation unit, so a function-local object (its mutex among the first
  // things to go) would already be gone by the time the statistics are read. A few hundred bytes
  // are deliberately left unreclaimed for the lifetime of the process.
  static lane_stats_t* s = new lane_stats_t();
  return *s;
}

namespace {

/// Registered at namespace scope, not on first use: a run that recorded no lane at all must still
/// say so (the probe being silent is indistinguishable from the probe not being compiled in).
const bool report_registered = []() {
  std::atexit(gpu_lane_probe::report);
  return true;
}();

} // namespace

const char* stage_name(gpu_lane_probe::stage which)
{
  switch (which) {
    case gpu_lane_probe::stage::dft:
      return "dft";
    case gpu_lane_probe::stage::channel_estimator:
      return "ch_est";
    case gpu_lane_probe::stage::equalizer_demapper:
      return "eq_demap";
    case gpu_lane_probe::stage::other:
      return "other";
    case gpu_lane_probe::stage::count:
      break;
  }
  return "?";
}

/// One percentile of an already sorted series.
double percentile(const std::vector<double>& sorted, double p)
{
  return sorted[static_cast<size_t>((sorted.size() - 1) * p)];
}

void print_series(const char* name, std::vector<double>& sorted)
{
  if (sorted.empty()) {
    std::fprintf(stderr, "[ul_gpu_lane] %s: no samples\n", name);
    return;
  }
  std::sort(sorted.begin(), sorted.end());
  double sum = 0;
  for (double v : sorted) {
    sum += v;
  }
  std::fprintf(stderr,
               "[ul_gpu_lane] %s samples=%zu mean=%.1fus median=%.1fus min=%.1fus max=%.1fus p95=%.1fus p99=%.1fus\n",
               name,
               sorted.size(),
               sum / static_cast<double>(sorted.size()),
               percentile(sorted, 0.5),
               sorted.front(),
               sorted.back(),
               percentile(sorted, 0.95),
               percentile(sorted, 0.99));
}

} // namespace

/// Accumulates one front-end group (a slot's transforms) into the series. Command buffers that did not
/// complete are counted as carried, never reported: a half-visible group would read as a gap.
static void close_front_end_group(front_end_state& fe, lane_stats_t& st)
{
  if (!fe.open || fe.cbs.empty()) {
    fe.cbs.clear();
    fe.open = false;
    return;
  }

  double   first_start = 0.0;
  double   last_end    = 0.0;
  double   busy        = 0.0;
  bool     any         = false;
  uint64_t carried     = 0;
  uint64_t counted     = 0;
  for (id<MTLCommandBuffer> cb : fe.cbs) {
    if ((cb == nil) || (cb.status != MTLCommandBufferStatusCompleted)) {
      ++carried;
      continue;
    }
    const double start = cb.GPUStartTime;
    const double end   = cb.GPUEndTime;
    if (!(start > 0.0) || !(end >= start)) {
      ++carried;
      continue;
    }
    if (!any) {
      first_start = start;
      any         = true;
    }
    last_end = std::max(last_end, end);
    busy += (end - start) * 1e6;
    ++counted;
  }
  fe.cbs.clear();
  fe.open = false;
  st.fe_carried += carried;
  if (!any) {
    return;
  }
  const double residency = (last_end - first_start) * 1e6;
  st.fe_residency_us.push_back(residency);
  st.fe_busy_us.push_back(busy);
  st.fe_gap_us.push_back(residency - busy);
  ++st.fe_slots;
  st.fe_cbs += counted;
}

void gpu_lane_probe::register_front_end_commit(id<MTLCommandBuffer> cb, uint64_t slot_index)
{
  if (cb == nil) {
    return;
  }
  front_end_state& fe = front_end_thread_state();
  if (fe.open && (fe.slot != slot_index)) {
    std::lock_guard<std::mutex> lock(stats().mutex);
    close_front_end_group(fe, stats());
  }
  if (!fe.open) {
    fe.slot = slot_index;
    fe.open = true;
  }
  fe.cbs.push_back(cb);
}

void gpu_lane_probe::register_commit(id<MTLCommandBuffer> cb, stage which)
{
  if (cb == nil) {
    return;
  }
  thread_state().pending.push_back(lane_entry{cb, which});
}

void gpu_lane_probe::close_lane()
{
  lane_thread_state& ts = thread_state();
  if (ts.pending.empty()) {
    return;
  }

  const auto idx = [](stage which) { return static_cast<unsigned>(which); };

  double   first_start = 0.0;
  double   last_end    = 0.0;
  double   busy        = 0.0;
  double   stage_busy[static_cast<unsigned>(stage::count)] = {};
  uint64_t stage_cbs[static_cast<unsigned>(stage::count)]  = {};
  bool     any      = false;
  unsigned resolved = 0;
  uint64_t dropped  = 0;

  std::vector<lane_entry> carry;
  carry.reserve(ts.pending.size());
  for (const lane_entry& entry : ts.pending) {
    if (entry.cb.status != MTLCommandBufferStatusCompleted) {
      // Still running (or scheduled but not started): its timestamps are not final, so it belongs to
      // the next lane instead of showing up as gap in this one.
      carry.push_back(entry);
      continue;
    }
    const double start = entry.cb.GPUStartTime;
    const double end   = entry.cb.GPUEndTime;
    if (!(start > 0.0) || !(end >= start)) {
      // Completed without GPU timestamps: nothing to account for (it never ran on the device).
      ++dropped;
      continue;
    }
    if (!any) {
      first_start = start;
      last_end    = end;
      any         = true;
    } else {
      first_start = std::min(first_start, start);
      last_end    = std::max(last_end, end);
    }
    const double span = end - start;
    busy += span;
    stage_busy[idx(entry.which)] += span;
    ++stage_cbs[idx(entry.which)];
    ++resolved;
  }

  // A lane whose command buffers are all still pending stays open: the next close will account for
  // them. The cap only exists so that a command buffer that never completes cannot grow the list.
  static constexpr size_t max_carried = 64;
  if (carry.size() > max_carried) {
    dropped += carry.size() - max_carried;
    carry.erase(carry.begin(), carry.begin() + (carry.size() - max_carried));
  }
  const unsigned carried = static_cast<unsigned>(carry.size());
  ts.pending.swap(carry);

  if (!any) {
    return;
  }

  lane_stats_t& s = stats();
  std::lock_guard<std::mutex> lock(s.mutex);
  const double residency = (last_end - first_start) * 1e6;
  const double busy_us   = busy * 1e6;
  s.residency_us.push_back(residency);
  s.busy_us.push_back(busy_us);
  s.gap_us.push_back(residency - busy_us);
  for (unsigned i = 0; i != static_cast<unsigned>(stage::count); ++i) {
    s.stage_busy_us[i] += stage_busy[i] * 1e6;
    s.stage_cbs[i] += stage_cbs[i];
  }
  ++s.lanes;
  s.cbs += resolved;
  s.cbs_max         = std::max<uint64_t>(s.cbs_max, resolved);
  s.carried_over += carried;
  s.dropped_cbs += dropped;
  if (s.has_last_end) {
    if (last_end > s.last_lane_end_s) {
      s.period_us.push_back((last_end - s.last_lane_end_s) * 1e6);
      s.last_lane_end_s = last_end;
    } else {
      ++s.period_dropped;
    }
  } else {
    s.last_lane_end_s = last_end;
    s.has_last_end    = true;
  }
}

void gpu_lane_probe::report()
{
  lane_stats_t& s = stats();

  // The last front-end group of a run has no successor slot to close it: close it here. (In a leg the
  // transforms are submitted by the radio thread and this runs on the main one at exit, so the group
  // that is still open at that moment - the final slot - is the one that may be missed; every earlier
  // one was closed when its successor arrived.)
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    close_front_end_group(front_end_thread_state(), s);
  }

  std::vector<double> residency;
  std::vector<double> busy;
  std::vector<double> gap;
  std::vector<double> period;
  double              stage_busy[static_cast<unsigned>(stage::count)] = {};
  uint64_t            stage_cbs[static_cast<unsigned>(stage::count)]  = {};
  uint64_t            lanes          = 0;
  uint64_t            cbs            = 0;
  uint64_t            cbs_max        = 0;
  uint64_t            dropped        = 0;
  uint64_t            carried        = 0;
  uint64_t            period_dropped = 0;
  std::vector<double> fe_residency;
  std::vector<double> fe_busy;
  std::vector<double> fe_gap;
  uint64_t            fe_slots   = 0;
  uint64_t            fe_cbs     = 0;
  uint64_t            fe_carried = 0;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    fe_residency = s.fe_residency_us;
    fe_busy      = s.fe_busy_us;
    fe_gap       = s.fe_gap_us;
    fe_slots     = s.fe_slots;
    fe_cbs       = s.fe_cbs;
    fe_carried   = s.fe_carried;
    residency = s.residency_us;
    busy      = s.busy_us;
    gap       = s.gap_us;
    period    = s.period_us;
    for (unsigned i = 0; i != static_cast<unsigned>(stage::count); ++i) {
      stage_busy[i] = s.stage_busy_us[i];
      stage_cbs[i]  = s.stage_cbs[i];
    }
    lanes          = s.lanes;
    cbs            = s.cbs;
    cbs_max        = s.cbs_max;
    dropped        = s.dropped_cbs;
    carried        = s.carried_over;
    period_dropped = s.period_dropped;
  }

  // The front end has its own series and does not need a lane to be worth reporting: a run of the
  // demodulator alone (its test) has transforms but no back-end lane at all.
  const auto print_front_end = [&]() {
    if (fe_slots == 0) {
      return;
    }
    std::fprintf(stderr,
                 "[ul_gpu_lane] dft slots=%llu cbs=%llu carried=%llu (front-end queue, one group per slot)\n",
                 static_cast<unsigned long long>(fe_slots),
                 static_cast<unsigned long long>(fe_cbs),
                 static_cast<unsigned long long>(fe_carried));
    print_series("dft residency", fe_residency);
    print_series("dft busy", fe_busy);
    print_series("dft gap", fe_gap);
  };

  if (lanes == 0) {
    if (fe_slots == 0) {
      std::fprintf(stderr, "[ul_gpu_lane] no lanes recorded\n");
    } else {
      print_front_end();
    }
    return;
  }

  // Header line first: how many lanes were seen and how many command buffers each of them held, plus
  // the accounting that says whether the four series below can be trusted.
  std::fprintf(stderr,
               "[ul_gpu_lane] lanes=%llu cbs/lane=%.2f (max=%llu) dropped=%llu carried=%llu period_dropped=%llu\n",
               static_cast<unsigned long long>(lanes),
               static_cast<double>(cbs) / static_cast<double>(lanes),
               static_cast<unsigned long long>(cbs_max),
               static_cast<unsigned long long>(dropped),
               static_cast<unsigned long long>(carried),
               static_cast<unsigned long long>(period_dropped));
  print_series("residency", residency);
  print_series("busy", busy);
  print_series("gap", gap);
  print_series("period", period);

  print_front_end();

  std::fprintf(stderr, "[ul_gpu_lane] busy split:");
  double busy_total = 0;
  for (unsigned i = 0; i != static_cast<unsigned>(stage::count); ++i) {
    busy_total += stage_busy[i];
  }
  for (unsigned i = 0; i != static_cast<unsigned>(stage::count); ++i) {
    if (stage_cbs[i] == 0) {
      continue;
    }
    std::fprintf(stderr,
                 " %s=%.1fus/lane (%.0f%% of busy, cbs/lane=%.2f)",
                 stage_name(static_cast<stage>(i)),
                 stage_busy[i] / static_cast<double>(lanes),
                 (100.0 * stage_busy[i]) / std::max(1.0, busy_total),
                 static_cast<double>(stage_cbs[i]) / static_cast<double>(lanes));
  }
  std::fprintf(stderr, "\n");
}

#endif // OCUDU_METAL_STATS

} // namespace metal
} // namespace ocudu
