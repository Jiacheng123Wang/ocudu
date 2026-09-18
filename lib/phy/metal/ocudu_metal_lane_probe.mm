// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_metal_lane_clock.h"
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
  /// Host reading of this command buffer's commit, on the same clock as the estimator's stage entry
  /// (see ocudu_metal_lane_clock.h). The GPU timestamps say when a command buffer RAN; only this says
  /// when the host let it run. The distance between two of these readings is what the host spent
  /// between the two stages, and read against the device's own span for the same interval it tells
  /// "the device was busy" apart from "the device had nothing to run yet" - which the gap, being a
  /// single number, cannot.
  ocudu::metal::lane_host_clock::clock::time_point commit_time{};
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
  /// The part of gap_us the HOST owns, measured on the host clock: "the estimator's stage ran this
  /// long before the lane's first command buffer was committed". Until that commit exists the back
  /// end has nothing queued for this lane, however idle it is. What remains of gap_us is the fences
  /// the lane burst encodes (front_end_wait / backend_stage_wait) plus the command queue's ordering.
  /// (The other host leg - how long the slot waited before the estimator's stage began - is the
  /// [ul_channel_estimation] phase; see the note in ocudu_metal_lane_clock.h for why it is not
  /// measured here.)
  std::vector<double> handover_us;

  /// \brief How long the lane's first command buffer waited to be STARTED by the device, after the
  /// host had committed it: its GPUStartTime minus the GPUStartTime of the estimator command buffer
  /// that carries this lane's work (the earliest one - the first command buffer of a lane, see
  /// register_commit()).
  ///
  /// Unlike "the burst waited on the extraction's fence", this is not a restatement of the gap: the
  /// fence delay is forced by the dependency (the burst reads what the estimator wrote, so it can
  /// never start before it ends), while this one is the QUEUE's cost - the device getting to the
  /// lane's work at all. A small value here says the remaining gap is the dependency itself (i.e. the
  /// estimator's own pipeline is what to shorten); a large one says the target is submission.
  std::vector<double> start_delay_us;
  /// \brief The same distance for the weights and the burst command buffers (see close_lane()).
  ///
  /// Kept as three series and not one: they are submitted at very different moments of the hop, so
  /// they see different queue states, and reading them against each other is what separates "the
  /// device was busy" from "the host handed this buffer over late".
  std::vector<double> queue_to_weights_us;
  std::vector<double> queue_to_burst_us;
  std::vector<double> period_us;

  /// \brief The device's idle time inside the lane, split by WHERE it sits: the distance between one
  /// stage's last GPU end and the next stage's first GPU start. Together the two account for the whole
  /// of gap_us whenever the lane holds one command buffer per stage, which is what every air leg since
  /// the extraction/weights split measured (3.00 cbs/lane, max 3) - and on air they close it exactly
  /// (measured: 125.3 + 78.0 = 203.3us).
  ///
  /// Why they are kept apart: they do not have the same owner, and the difference decides what to
  /// shorten.
  ///
  ///   * \c hole_to_weights_us is the HOST's. The extraction command buffer is committed, runs its own
  ///     span, and the device is then idle until the weights command buffer is committed - measured at
  ///     125.3us, against a 238.3us extraction-commit-to-weights-commit distance and a 116.6us
  ///     extraction. The queue accounts for ~4us of it, so the hole IS "the host had not handed the
  ///     second command buffer over yet", and there is a real dependency behind it: the weights
  ///     command buffer's parameters carry the CFO the host reads OUT of the extraction's buffer, so
  ///     it cannot be encoded before that command buffer completed (see build_pilots_lse() and
  ///     reformat_stage::noise_stage_t::cfo);
  ///   * \c hole_to_burst_us is the DEVICE's. The burst is committed long before the weights command
  ///     buffer can have finished (measured: 86.5us after the weights' commit, against that command
  ///     buffer's 340.6us span), so it is already queued and waiting on the fence when the weights
  ///     command buffer ends.
  std::vector<double> hole_to_weights_us;
  std::vector<double> hole_to_burst_us;

  /// \brief The same two transitions on the HOST clock: the extraction's commit to the weights', and
  /// the weights' to the burst's. Read next to the device figures above, not instead of them.
  ///
  /// The comparison that decides the question: the weights command buffer cannot END sooner than its
  /// own GPU span after its commit, so a burst committed inside that span was handed over before the
  /// device could possibly have needed it - and a burst committed after it was the device waiting for
  /// the host. Both readings are host-vs-host and GPU-vs-GPU, so no assumption is made about the two
  /// clocks sharing a time base.
  std::vector<double> host_to_weights_us;
  std::vector<double> host_to_burst_us;

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
    case gpu_lane_probe::stage::channel_estimator_weights:
      return "ch_wt";
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
  thread_state().pending.push_back(lane_entry{cb, which, ocudu::metal::lane_host_clock::clock::now()});
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
  // The entries this lane actually accounted for, kept so the queue diagnosis below can read the
  // estimator command buffer's own GPU start (see its comment).
  std::vector<lane_entry> entries_for_starts;
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
    entries_for_starts.push_back(entry);
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

  // ---- Where the lane's gap actually sits (see the two series' comments in lane_stats_t) ----------
  //
  // Per stage, the boundaries of its command buffers: the earliest GPU start, the latest GPU end and
  // the earliest host commit. Taken from the entries rather than from the running totals above so a
  // stage that ever commits more than one command buffer still yields one boundary.
  {
    const auto   idx_of = [](stage which) { return static_cast<unsigned>(which); };
    const size_t n      = static_cast<size_t>(stage::count);
    std::vector<double> stage_start(n, 0.0);
    std::vector<double> stage_end(n, 0.0);
    std::vector<double> stage_host(n, 0.0);
    std::vector<bool>   stage_gpu(n, false);

    // The host reference is the lane's earliest commit - the extraction's, committed first - so every
    // host reading below is an offset from the instant the 'entry -> extraction commit' series ends at.
    // entries_for_starts is non-empty here: it is filled in the same branch that sets \c any.
    lane_host_clock::clock::time_point host_ref = entries_for_starts.front().commit_time;
    for (const lane_entry& entry : entries_for_starts) {
      host_ref = std::min(host_ref, entry.commit_time);
    }
    const auto host_us = [&](const lane_entry& entry) {
      return std::chrono::duration<double, std::micro>(entry.commit_time - host_ref).count();
    };

    for (const lane_entry& entry : entries_for_starts) {
      const unsigned i     = idx_of(entry.which);
      const double   start = entry.cb.GPUStartTime;
      const double   end   = entry.cb.GPUEndTime;
      if (!stage_gpu[i]) {
        stage_gpu[i]   = true;
        stage_start[i] = start;
        stage_host[i]  = host_us(entry);
      } else {
        stage_start[i] = std::min(stage_start[i], start);
        stage_host[i]  = std::min(stage_host[i], host_us(entry));
      }
      stage_end[i] = std::max(stage_end[i], end);
    }

    const auto transition = [&](stage from, stage to, std::vector<double>& hole,
                                std::vector<double>& host_span) {
      const unsigned f = idx_of(from);
      const unsigned t = idx_of(to);
      if (!stage_gpu[f] || !stage_gpu[t]) {
        return;
      }
      const double h = (stage_start[t] - stage_end[f]) * 1e6;
      // A stage's commands may overlap in principle; a negative distance would not be a hole, so it is
      // left out rather than counted as zero.
      if (h >= 0.0) {
        hole.push_back(h);
      }
      host_span.push_back(stage_host[t] - stage_host[f]);
    };
    transition(stage::channel_estimator, stage::channel_estimator_weights, s.hole_to_weights_us,
               s.host_to_weights_us);
    transition(stage::channel_estimator_weights, stage::equalizer_demapper, s.hole_to_burst_us,
               s.host_to_burst_us);
  }

  // The host side of this lane's gap (see ocudu_metal_lane_clock.h). -1 means unmeasured (a route
  // without the estimator probe), and those samples are left out instead of counted as zero.
  if (metal::lane_clock.handover_us >= 0.0) {
    s.handover_us.push_back(metal::lane_clock.handover_us);
  }
  // ---- When the DEVICE got to each of the lane's command buffers (the queue's share) -------------
  //
  // This is the quantity the 'gap: commit -> first command buffer starts (queue)' series always
  // claimed to report, and never did. Its first version differenced the extraction's GPU start
  // against the lane's EARLIEST GPU start - and the extraction IS the lane's earliest command
  // buffer, by construction (the receiving chain estimates before it demodulates), so the series was
  // identically zero. Measured over 1978 air lanes: min = max = 0.0. An identity cannot testify
  // about the queue, and for a while it was read as "the queue costs nothing".
  //
  // What the number has to be is the distance between the host COMMITTING a command buffer and the
  // device STARTING it - which is exactly where a back-end command buffer waits when the GPU is
  // busy, and the question this probe exists to answer: the lane's gap is ~200us, its command
  // buffers are only ~530us of work in a 59s leg, and the front end submits one indivisible 423us
  // transform per slot (~42% of every 1ms slot), so "the device was late getting to this buffer" is
  // a live hypothesis that nothing has ever tested.
  //
  // \note This is the ONE reading in this file that mixes clocks: GPUStartTime is a host-time-base
  // reading (CACurrentMediaTime), and commit_time is steady_clock. On Darwin both are
  // mach_absolute_time, so the difference is meaningful - but a wrong epoch would not look subtle,
  // it would be off by seconds, which is why the magnitude is its own check. Printed for all three
  // stages so the three can be read against each other.
  const auto commit_seconds = [](const lane_entry& entry) {
    return std::chrono::duration<double>(entry.commit_time.time_since_epoch()).count();
  };
  const auto queue_of = [&](stage which, std::vector<double>& series) {
    for (const lane_entry& entry : entries_for_starts) {
      if (entry.which != which) {
        continue;
      }
      const double start = entry.cb.GPUStartTime;
      if (start > 0.0) {
        const double delay = (start - commit_seconds(entry)) * 1e6;
        // Negative would mean the device started a command buffer before the host committed it,
        // i.e. the two clocks are not on a shared base: reported as a sample so the leg shows it
        // instead of hiding it behind a clamp.
        series.push_back(delay);
      }
      break;
    }
  };
  queue_of(stage::channel_estimator, s.start_delay_us);
  queue_of(stage::channel_estimator_weights, s.queue_to_weights_us);
  queue_of(stage::equalizer_demapper, s.queue_to_burst_us);
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
  std::vector<double> queue_to_weights;
  std::vector<double> queue_to_burst;
  std::vector<double> hole_to_weights;
  std::vector<double> hole_to_burst;
  std::vector<double> host_to_weights;
  std::vector<double> host_to_burst;
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
    queue_to_weights = s.queue_to_weights_us;
    queue_to_burst   = s.queue_to_burst_us;
    hole_to_weights = s.hole_to_weights_us;
    hole_to_burst   = s.hole_to_burst_us;
    host_to_weights = s.host_to_weights_us;
    host_to_burst   = s.host_to_burst_us;
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
  // The host's share of that gap (see ocudu_metal_lane_clock.h): gap = this + (the fences and the
  // command queue). Printed next to the gap it belongs to instead of being inferred from it.
  print_series("gap: stage entry -> extraction commit (host)", s.handover_us);
  print_series("gap: commit -> first command buffer starts (queue)", s.start_delay_us);
  // The same distance for the other two command buffers of the lane. The three together say whether
  // the gap is the device being busy when a buffer arrives (all three large) or one buffer being
  // handed over late (one large, the others small). See close_lane() for why the first of them used
  // to read identically zero.
  print_series("queue: weights commit -> weights start", queue_to_weights);
  print_series("queue: burst commit -> burst start", queue_to_burst);
  // The lane's gap split by where it sits, with the host's own reading of the same two transitions
  // next to each device hole (see lane_stats_t). The two device figures add up to gap whenever the
  // lane holds one command buffer per stage; the host figures say whether the host had handed that
  // command buffer over by then.
  print_series("gap: extraction end -> weights start (device)", hole_to_weights);
  print_series("gap: weights end -> burst start (device)", hole_to_burst);
  print_series("host: extraction commit -> weights commit", host_to_weights);
  // NOT "extraction commit -> burst commit" - the transition this series is filled from starts at the
  // WEIGHTS commit, and the two are far apart (measured: 238.3us vs 94.6us on air), which is exactly
  // the mistake this label spelled out once.
  print_series("host: weights commit -> burst commit", host_to_burst);
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
