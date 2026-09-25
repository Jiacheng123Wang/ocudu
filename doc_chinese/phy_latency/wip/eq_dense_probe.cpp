// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Offline probe of ch_gather_desc's "the entries ARE the grid" decision (dev doc 6.48).
///
/// WHY IT EXISTS. The equalizer's direct-grid path (dev doc 6.47 variant A) drops the gather dispatch
/// for every run whose received symbols are a contiguous run of the resource grid itself. That is a
/// property of the PLAN, decided by `ch_gather_desc::build()` from the RE mask, and the air leg's
/// counterexample reading depends on it being right: a run the plan calls dense but which really has
/// holes would make the equalization read a neighbour's subcarriers (wrong values, not a crash).
///
/// The capture-driven arms CANNOT test it: the replay tool's holed-allocation corpus builds a
/// `vrb_bitmap(max_prb + 1)` while the BWP stays at `bwp_size_rb`, so `get_crb_mask()` flattens the
/// hole before the plan ever sees it - measured, an `alloc_prb=0,1,3` capture came back with every
/// symbol dense (dev doc 6.48 (4) 1). So the predicate is tested where it lives: on the builder.
///
/// WHAT IT PRINTS. For each mask, per OFDM symbol of the hop: the number of entries, the symbol's
/// first grid subcarrier and the dense flag. The three masks that matter:
///   * a contiguous allocation      -> data symbols dense, DM-RS-without-data symbols empty;
///   * a single gap in the middle   -> EVERY data symbol NOT dense (the entries skip subcarriers);
///   * two clusters                 -> same, and the gap is wider than one PRB.
///
/// Build (from `build/lib/phy/upper/channel_processors/metal/`, i.e. with the library's own link set -
/// compiling the single .cpp on its own does NOT link, it needs the PHY support archives):
///   clang++ -std=c++17 -I $ROOT/include -I $ROOT/external/fmt/include $ROOT/doc_chinese/phy_latency/wip/eq_dense_probe.cpp \
///     ../../equalization/libocudu_channel_equalizer.a ../../../../support/libocudu_support.a \
///     ../../../support/libocudu_phy_support.a ../../../../instrumentation/libocudu_instrumentation.a \
///     ../../../../../utils/macos_compat/libocudu_macos_compat.a ../../../../support/math/libocudu_support_math.a \
///     ../../../../ran/libocudu_ran.a -framework Metal -framework Foundation ../../../../ocuduvec/libocuduvec.a \
///     ../../../../ocudulog/libocudulog.a ../../../../../external/fmt/libfmt.a -o /tmp/eq_dense_probe && /tmp/eq_dense_probe

#include "ocudu/phy/upper/equalization/channel_equalizer_device_grid.h"
#include "ocudu/support/macos_compat.h"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace ocudu;

/// Builds one plan over a synthetic grid and prints what its builder decided per symbol.
static void run(const char* name, const std::vector<unsigned>& prbs)
{
  const unsigned nof_subc  = 6 * NOF_SUBCARRIERS_PER_RB;
  const unsigned nof_symb  = MAX_NSYMB_PER_SLOT;
  const unsigned nof_ports = 1;
  const size_t   bytes     = static_cast<size_t>(nof_subc) * nof_symb * nof_ports * sizeof(cbf16_t);
  const size_t   cap       = ((bytes + compat::page_size() - 1) / compat::page_size()) * compat::page_size();
  auto*          buf       = static_cast<cbf16_t*>(compat::aligned_alloc(compat::page_size(), cap));
  std::memset(buf, 0, cap);
  const resource_grid_device_view view =
      make_resource_grid_device_view(span<cbf16_t>(buf, cap / sizeof(cbf16_t)), nof_subc, nof_symb, nof_ports);

  crb_bitmap rb(6);
  for (unsigned prb : prbs) {
    rb.set(prb);
  }
  // DM-RS on three symbols of the slot, with NO data on them: that is the shape the air leg has
  // (0x000 == "every subcarrier of a DM-RS symbol is DM-RS"), so a hop built here has the same
  // 14-symbols-minus-DM-RS structure the counterexample table in dev doc 6.48 (5) describes.
  bounded_bitset<MAX_NSYMB_PER_SLOT> dmrs(MAX_NSYMB_PER_SLOT);
  dmrs.set(2);
  dmrs.set(7);
  dmrs.set(11);
  const ch_gather_desc plan(view, rb, 0, nof_symb, nof_ports, dmrs, 0xFFFu, 0x000u);

  std::printf("[%s] prbs={", name);
  for (unsigned prb : prbs) {
    std::printf("%u,", prb);
  }
  std::printf("} plan_entries=%u\n", plan.size());
  for (unsigned i = 0; i != plan.nof_symbols; ++i) {
    const ch_gather_symbol& s = plan.symbols[i];
    std::printf("   sym=%2u nof_entries=%3u subc_base=%3u dense=%d\n",
                s.symbol,
                s.nof_entries,
                s.subc_base,
                s.dense ? 1 : 0);
  }
  compat::aligned_free(buf);
}

int main()
{
  run("contiguous {0,1,2}", {0, 1, 2});
  run("one gap {0,1,3}", {0, 1, 3});
  run("two clusters {0,1,4,5}", {0, 1, 4, 5});
  return 0;
}
