// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/phy/phy_pipeline_contract.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace ocudu {

/// \brief The host <-> device DATA crossings left in the uplink chain, counted where they happen.
///
/// ---- Why this exists ----
/// The `gpu` pipeline mode states its goal as a CROSSING COUNT, not as a latency. From
/// phy_pipeline_mode.h: the fused lane runs the whole IQ -> LLR chain "inside one device-side pipeline
/// with only two host <-> device *data* crossings (the IQ upload and the LLR download)".
///
/// Nothing measured that claim. The contract's other checks count SAMPLES ("host sample assembly"),
/// the GRID ("zero-copy wraps") and a compensation round trip ("cfo compensation"), and every one of
/// them passed on every air leg while the estimator was still reading three device scalars per hop and
/// handing the result back as kernel parameters - a dev -> host -> dev round trip that is neither of
/// the two crossings the mode allows. A mode whose stated property is not measured cannot be reached,
/// only asserted; and the property that was being asserted instead (sample copies) had already been
/// achieved and held for 24 consecutive legs.
///
/// ---- What counts ----
/// A crossing is the host TAKING device-produced data away, or handing device-DERIVED data back.
/// That is the leg which makes a device-side pipeline impossible, and it is what has to go to zero.
///
/// It does NOT count: submitting a command buffer, fencing, choosing a backend, or reading
/// configuration - control, not data. Neither does it count the IQ upload or the LLR download, which
/// the mode explicitly allows; those are the two the count is driven TO, not FROM.
///
/// \note Deliberately a plain counter and not an inferred quantity: the earlier attempts to attribute
/// the lane's gap by subtracting one measurement from another produced two wrong conclusions in a row
/// (see the design document). This one is incremented at the read, so it cannot be argued with.
class phy_pipeline_crossings
{
public:
  /// Counts one host read of data the device produced.
  ///
  /// \param[in] bytes How many bytes the host read, or 0 when the caller does not know. As on the
  ///            write side the COUNT is what the verdict uses and the byte total is context.
  static void count_host_read(uint64_t bytes = 0)
  {
    // The total always counts the touch; inside a DEBUG scope it is ALSO counted as debug, so the
    // judged number is (total - debug) and the two can never disagree. (An earlier revision routed the
    // debug touches away from the total instead, which made the subtraction underflow the moment a
    // capture was on - the totals here are "everything the counters saw", by design.)
    host_reads().fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
      host_read_bytes().fetch_add(bytes, std::memory_order_relaxed);
    }
    if (in_debug_scope()) {
      debug_reads().fetch_add(1, std::memory_order_relaxed);
      if (bytes != 0) {
        debug_read_bytes().fetch_add(bytes, std::memory_order_relaxed);
      }
    }
  }

  /// Counts one host write of data the device will consume.
  ///
  /// \param[in] bytes How many bytes the host wrote, or 0 when the caller does not know. Only the
  ///            BYTE total is optional; the COUNT is what the verdict uses, because one host store
  ///            into a buffer the device reads is a crossing whether it moves one float or a whole
  ///            slot. The write side is the easy one to miss: the lane's buffers are zero-copy
  ///            mappings, so a host store into one is an ordinary assignment in the source and a
  ///            device-visible transfer at runtime, with no memcpy to find.
  static void count_host_write(uint64_t bytes = 0)
  {
    // See count_host_read(): the total counts everything, the debug counter marks the subset that a
    // development capture caused.
    host_writes().fetch_add(1, std::memory_order_relaxed);
    if (bytes != 0) {
      host_write_bytes().fetch_add(bytes, std::memory_order_relaxed);
    }
    if (in_debug_scope()) {
      debug_writes().fetch_add(1, std::memory_order_relaxed);
      if (bytes != 0) {
        debug_write_bytes().fetch_add(bytes, std::memory_order_relaxed);
      }
    }
  }

  /// \brief Counts one host write and attributes it to \p site (batch 5e).
  ///
  /// The TOTAL is what the contract judges; this says WHICH store it was. "0.27 writes per hop" is not
  /// actionable on its own - the four live write sites have nothing in common (a pad memset, a scalar
  /// copy, a 56-byte table upload, and a demapper fallback that only fires when a buffer is not
  /// page-aligned), and moving the wrong one is a batch of work for nothing. Bounded table, no
  /// allocation on the path, and it increments the same total as count_host_write(), so the breakdown
  /// can never disagree with the verdict.
  static void count_host_write_site(const char* site, uint64_t bytes = 0)
  {
    count_host_write(bytes);
    if (site == nullptr) {
      return;
    }
    site_entry* e = site_for(site);
    if (e == nullptr) {
      return;
    }
    e->writes.fetch_add(1, std::memory_order_relaxed);
    if (in_debug_scope()) {
      e->debug_writes.fetch_add(1, std::memory_order_relaxed);
    }
    if (bytes != 0) {
      e->write_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  /// \brief Counts one host read and attributes it to \p site (batch S13-P1).
  ///
  /// The mirror of count_host_write_site(), and it exists for the same reason the write table does:
  /// the read side has MORE sites than the write side (the extraction of the received pilots, the y
  /// staging out of the device's least-squares pilots, the fallback sigma2), they have nothing in
  /// common, and "1.00 read per hop" does not say which of them a lane is paying for. It increments
  /// the same total as count_host_read(), so the breakdown can never disagree with the verdict.
  static void count_host_read_site(const char* site, uint64_t bytes = 0)
  {
    count_host_read(bytes);
    if (site == nullptr) {
      return;
    }
    site_entry* e = site_for(site);
    if (e == nullptr) {
      return;
    }
    e->reads.fetch_add(1, std::memory_order_relaxed);
    if (in_debug_scope()) {
      e->debug_reads.fetch_add(1, std::memory_order_relaxed);
    }
    if (bytes != 0) {
      e->read_bytes.fetch_add(bytes, std::memory_order_relaxed);
    }
  }

  /// Prints the per-site WRITE breakdown, biggest first, to \p out.
  ///
  /// Printed under the crossings line so the byte totals there can be read as "which store", which is
  /// the question batch 5e starts from. A site that never fired is not printed.
  static void print_write_sites(std::FILE* out) { print_sites(out, /*reads=*/false); }

  /// Prints the per-site READ breakdown (batch S13-P1), in the same shape as the write one.
  static void print_read_sites(std::FILE* out) { print_sites(out, /*reads=*/true); }

  /// \brief How many reads the named sites account for.
  ///
  /// The crossings line prints the TOTAL; the table below it lists the named ones. A total larger than
  /// this sum means a read nobody has named yet - which is a finding, not a rounding difference, and
  /// is why both numbers are printed.
  static uint64_t get_named_reads()
  {
    std::lock_guard<std::mutex> lock(sites_mutex());
    uint64_t                    sum = 0;
    for (std::size_t i = 0; i != nof_sites(); ++i) {
      sum += sites()[i].reads.load(std::memory_order_relaxed);
    }
    return sum;
  }

  /// \brief Declares that \p module has audited its host <-> device data touches and counts them here.
  ///
  /// The report lists the declarers NEXT TO the number, so the scope of a zero is visible with it.
  /// This is not decoration: the first version of this file counted the channel estimator's four read
  /// sites and nothing else, while the message said "the fused lane (mode=gpu) allows 0" - and that
  /// green OK was read (by its author) as "the whole lane is clean". It meant one module of four. A
  /// check that overstates its scope is worse than no check, because it ends the search.
  ///
  /// A module that has NOT audited its touches must not call this.
  static void declare_reporter(const char* module)
  {
    std::lock_guard<std::mutex> lock(reporters_mutex());
    for (std::size_t i = 0; i != nof_reporters(); ++i) {
      const char* r = reporters()[i];
      if ((r != nullptr) && (std::strcmp(r, module) == 0)) {
        return;
      }
    }
    if (nof_reporters() < kMaxReporters) {
      reporters()[nof_reporters()] = module;
    }
  }

  /// Prints the declarers, comma-separated, to \p out.
  ///
  /// Prints rather than returning a std::string on purpose: this header is included from translation
  /// units that sit INSIDE a namespace, where pulling in <string>/<vector>/<algorithm> makes libc++
  /// fail with errors like "no template named 'basic_ostream'". Keep this header to
  /// <atomic>/<cstdint>/<cstdio>/<cstring>/<mutex> and no more.
  static void print_reporters(std::FILE* out)
  {
    std::lock_guard<std::mutex> lock(reporters_mutex());
    if (nof_reporters() == 0) {
      std::fprintf(out, "<none>");
      return;
    }
    for (std::size_t i = 0; i != nof_reporters(); ++i) {
      std::fprintf(out, "%s%s", (i == 0) ? "" : ", ", reporters()[i]);
    }
  }

  /// \brief Marks every host touch inside its scope as DEBUG, so the contract does not judge it.
  ///
  /// A development capture (OCUDU_UL_DUMP) writes device-produced data to a file. That is not the CPU
  /// participating in the lane - it is a debug aid, and a release build does not even compile the
  /// machinery (ENABLE_UL_CAPTURE is off by default, see ul_capture.h) - so it must not count as a
  /// crossing. Dropping it silently would be worse than counting it: "0.00" has to stay checkable, so
  /// the debug touches are counted SEPARATELY and printed next to the judged number.
  ///
  /// The scope is thread-local: a lane is processed by one thread, and a capture opened on that thread
  /// must not reclassify another thread's lane touches.
  class scoped_debug_touches
  {
  public:
    scoped_debug_touches() { debug_depth() += 1; }
    ~scoped_debug_touches() { debug_depth() -= 1; }
    scoped_debug_touches(const scoped_debug_touches&)            = delete;
    scoped_debug_touches& operator=(const scoped_debug_touches&) = delete;
  };

  static bool in_debug_scope() { return debug_depth() != 0; }

  static uint64_t get_debug_reads() { return debug_reads().load(std::memory_order_relaxed); }
  static uint64_t get_debug_read_bytes() { return debug_read_bytes().load(std::memory_order_relaxed); }
  static uint64_t get_debug_writes() { return debug_writes().load(std::memory_order_relaxed); }
  static uint64_t get_debug_write_bytes() { return debug_write_bytes().load(std::memory_order_relaxed); }

  /// Counts one hop that consumed device output, so a total can be read as "per hop".
  static void count_device_hop() { device_hops().fetch_add(1, std::memory_order_relaxed); }

  static uint64_t get_host_reads() { return host_reads().load(std::memory_order_relaxed); }
  static uint64_t get_host_read_bytes() { return host_read_bytes().load(std::memory_order_relaxed); }
  static uint64_t get_host_writes() { return host_writes().load(std::memory_order_relaxed); }
  static uint64_t get_host_write_bytes() { return host_write_bytes().load(std::memory_order_relaxed); }
  static uint64_t get_device_hops() { return device_hops().load(std::memory_order_relaxed); }

private:
  /// Thread-local depth of scoped_debug_touches: a capture is opened and closed on the thread that
  /// writes the file, and one thread's capture must not reclassify another thread's lane touches.
  static unsigned& debug_depth()
  {
    static thread_local unsigned d = 0;
    return d;
  }

  // Never destroyed: the report runs from an atexit handler, which runs after the static destructors
  // of this translation unit (the same reason ul_host_stats and the lane probe heap-allocate theirs).
  static std::atomic<uint64_t>& host_reads()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& debug_reads()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& debug_read_bytes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& debug_writes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& debug_write_bytes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& device_hops()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& host_read_bytes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& host_writes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  static std::atomic<uint64_t>& host_write_bytes()
  {
    static std::atomic<uint64_t>* n = new std::atomic<uint64_t>(0);
    return *n;
  }
  /// One named crossing site: a host read or a host write of device data (batches 5e and S13-P1).
  struct site_entry {
    const char*           name = nullptr;
    std::atomic<uint64_t> reads{0};
    std::atomic<uint64_t> read_bytes{0};
    std::atomic<uint64_t> writes{0};
    std::atomic<uint64_t> write_bytes{0};
    /// Touches of the same site that a DEBUG scope asked for (see scoped_debug_touches). Kept per site
    /// so the table can say which store was the capture's - and so the judged total stays checkable
    /// against the table instead of silently losing a line.
    std::atomic<uint64_t> debug_reads{0};
    std::atomic<uint64_t> debug_writes{0};
  };
  static constexpr std::size_t kMaxSites = 8;
  static site_entry* sites()
  {
    static site_entry* v = new site_entry[kMaxSites];
    return v;
  }

  /// Finds (or adds) the entry of \p site, or nullptr when the table is full - in which case the
  /// caller's total has already counted the crossing and only the breakdown loses the name.
  static site_entry* site_for(const char* site)
  {
    std::lock_guard<std::mutex> lock(sites_mutex());
    const std::size_t           n = nof_sites();
    std::size_t                 i = 0;
    for (; i != n; ++i) {
      const char* name = sites()[i].name;
      if ((name != nullptr) && (std::strcmp(name, site) == 0)) {
        return &sites()[i];
      }
    }
    if (i == kMaxSites) {
      return nullptr;
    }
    sites()[i].name = site;
    ++nof_sites_stored();
    return &sites()[i];
  }

  /// Shared printer of the two tables: same shape, different counter (see print_write_sites()).
  static void print_sites(std::FILE* out, bool reads)
  {
    std::lock_guard<std::mutex> lock(sites_mutex());
    const std::size_t           n     = nof_sites();
    bool                        any   = false;
    bool                        printed[kMaxSites] = {};
    for (std::size_t round = 0; round != n; ++round) {
      std::size_t best     = kMaxSites;
      uint64_t    best_cnt = 0;
      for (std::size_t i = 0; i != n; ++i) {
        if (printed[i]) {
          continue;
        }
        const uint64_t c = reads ? sites()[i].reads.load(std::memory_order_relaxed)
                                 : sites()[i].writes.load(std::memory_order_relaxed);
        if (c == 0) {
          printed[i] = true;
          continue;
        }
        if ((best == kMaxSites) || (c > best_cnt)) {
          best     = i;
          best_cnt = c;
        }
      }
      if (best == kMaxSites) {
        break;
      }
      printed[best] = true;
      any           = true;
      const uint64_t bytes = reads ? sites()[best].read_bytes.load(std::memory_order_relaxed)
                                   : sites()[best].write_bytes.load(std::memory_order_relaxed);
      // The debug part of a site is shown NEXT TO it, not merged into it: the judged number above has
      // to be checkable against this table, and a site that is hit by both the lane and a capture would
      // otherwise look like a lane cost.
      const uint64_t dbg = reads ? sites()[best].debug_reads.load(std::memory_order_relaxed)
                                 : sites()[best].debug_writes.load(std::memory_order_relaxed);
      std::fprintf(out,
                   "\n    %-38s %8llu %s, %10llu bytes",
                   sites()[best].name,
                   static_cast<unsigned long long>(best_cnt),
                   reads ? "read(s)" : "call(s)",
                   static_cast<unsigned long long>(bytes));
      if (dbg != 0) {
        std::fprintf(out, "   [%llu of them debug]", static_cast<unsigned long long>(dbg));
      }
    }
    if (!any) {
      std::fprintf(out, reads ? "\n    <no host read was attributed to a site>"
                              : "\n    <no host write was attributed to a site>");
    }
  }
  static std::size_t& nof_sites_stored()
  {
    static std::size_t* n = new std::size_t(0);
    return *n;
  }
  static std::size_t nof_sites() { return nof_sites_stored(); }
  static std::mutex& sites_mutex()
  {
    static std::mutex* m = new std::mutex();
    return *m;
  }

  // Heap-allocated for the same reason as the counters (the report runs from an atexit handler), and
  // behind a mutex because modules declare from different threads.
  static constexpr std::size_t kMaxReporters = 8;
  static const char**          reporters()
  {
    static const char** v = new const char*[kMaxReporters]();
    return v;
  }
  /// Declares fill from 0 without holes, so scanning for the first null is exact and needs no second
  /// counter to keep in step.
  static std::size_t nof_reporters()
  {
    std::size_t n = 0;
    while ((n != kMaxReporters) && (reporters()[n] != nullptr)) {
      ++n;
    }
    return n;
  }
  static std::mutex& reporters_mutex()
  {
    static std::mutex* m = new std::mutex();
    return *m;
  }
};

/// \brief Registers the contract check that states the mode's crossing claim. Safe to call more than
/// once (a gNB builds one estimator per concurrent PUSCH thread).
///
/// The COUNT is printed in every published mode - it is the number this work has to drive to zero,
/// and hiding it in the modes that have not reached it yet would be exactly the mistake this check
/// exists to correct. The VERDICT belongs to `gpu` alone: cpu and cpu_gpu keep a crossing at every
/// module boundary by their own definition, so they claim nothing here and report "not applicable"
/// rather than passing or failing a requirement they never made.
inline void register_phy_pipeline_crossing_check()
{
  static std::once_flag once;
  std::call_once(once, []() {
    register_phy_pipeline_check(
        {"host device data crossings", []() -> std::optional<bool> {
           // WHAT IS JUDGED: the lane's own touches. The debug capture's are subtracted - a capture
           // (OCUDU_UL_DUMP) writing device-produced data to a file is a development aid, not the CPU
           // participating in the lane, and a release build does not compile it (ENABLE_UL_CAPTURE).
           // They are PRINTED rather than dropped: "0.00" has to stay checkable, and a reader has to be
           // able to see how much was set aside and why.
           const uint64_t          total_reads  = phy_pipeline_crossings::get_host_reads();
           const uint64_t          total_writes = phy_pipeline_crossings::get_host_writes();
           const uint64_t          dbg_reads    = phy_pipeline_crossings::get_debug_reads();
           const uint64_t          dbg_writes   = phy_pipeline_crossings::get_debug_writes();
           const uint64_t          reads        = total_reads - dbg_reads;
           const uint64_t          writes       = total_writes - dbg_writes;
           // The BYTES are judged on the same split as the counts - a line that said "0 read(s) (5376
           // bytes)" would be self-contradictory.
           const uint64_t          bytes = phy_pipeline_crossings::get_host_write_bytes() -
                                  phy_pipeline_crossings::get_debug_write_bytes();
           const uint64_t          hops         = phy_pipeline_crossings::get_device_hops();
           const phy_pipeline_mode mode         = phy_pipeline_mode_registry::get();
           const auto              per_hop      = [hops](uint64_t n) {
             return (hops != 0) ? (static_cast<double>(n) / static_cast<double>(hops)) : 0.0;
           };
           const uint64_t          rbytes = phy_pipeline_crossings::get_host_read_bytes() -
                                  phy_pipeline_crossings::get_debug_read_bytes();
           std::fprintf(stderr,
                        "%llu host read(s) (%llu bytes) and %llu host write(s) (%llu bytes) of device data "
                        "over %llu device hop(s) = %.2f read(s) + %.2f write(s) per hop; the fused lane "
                        "(mode=gpu) allows 0 of each (its two crossings are the IQ upload and the LLR "
                        "download, which this counts neither of)",
                        static_cast<unsigned long long>(reads),
                        static_cast<unsigned long long>(rbytes),
                        static_cast<unsigned long long>(writes),
                        static_cast<unsigned long long>(bytes),
                        static_cast<unsigned long long>(hops),
                        per_hop(reads),
                        per_hop(writes));
           if ((dbg_reads + dbg_writes) != 0) {
             std::fprintf(stderr,
                          "\n    NOT JUDGED: %llu read(s) (%llu bytes) and %llu write(s) (%llu bytes) are the "
                          "debug capture's own (OCUDU_UL_DUMP: a development aid, absent from a release "
                          "build) - see scoped_debug_touches()",
                          static_cast<unsigned long long>(dbg_reads),
                          static_cast<unsigned long long>(phy_pipeline_crossings::get_debug_read_bytes()),
                          static_cast<unsigned long long>(dbg_writes),
                          static_cast<unsigned long long>(phy_pipeline_crossings::get_debug_write_bytes()));
           }
           // The SCOPE of the number, printed with the number. Whoever reads a 0 here has to be able
           // to see how much of the lane it covers - see declare_reporter().
           std::fprintf(stderr,
                        "  counted by the module(s) that audited their host <-> device data touches: ");
           phy_pipeline_crossings::print_reporters(stderr);
           std::fprintf(stderr, " (a module NOT listed here is not covered by this number)");
           // WHICH store, when a module named itself (batch 5e), and WHICH read (batch S13-P1). A
           // breakdown that does not add up to the total above is itself a finding: it means a site
           // is still anonymous. The read table prints its own coverage line for that reason.
           phy_pipeline_crossings::print_write_sites(stderr);
           phy_pipeline_crossings::print_read_sites(stderr);
           // Compared against the TOTAL, debug included: the named sites cover the debug touches too,
           // so this line keeps meaning "every read is attributable" rather than "the judged ones are".
           if (total_reads != phy_pipeline_crossings::get_named_reads()) {
             std::fprintf(stderr,
                          "\n    (%llu of the %llu read(s) above are NOT named by a site - every read the "
                          "lane makes has to be attributable)",
                          static_cast<unsigned long long>(total_reads - phy_pipeline_crossings::get_named_reads()),
                          static_cast<unsigned long long>(total_reads));
           }
           std::fprintf(stderr, "\n");
           if (!phy_pipeline_mode_registry::is_published() || (hops == 0)) {
             return std::optional<bool>{};
           }
           if (mode != phy_pipeline_mode::gpu) {
             return std::optional<bool>{};
           }
           return std::optional<bool>((reads == 0) && (writes == 0));
         }});
  });
}

} // namespace ocudu
