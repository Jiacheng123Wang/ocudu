// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K5: the hop's per-layer reference signal received power, reduced on the DEVICE.
//
// ---- Why this exists ----
// The estimator's hop statistics (rsrp, and the noise variance derived from the same pilots) were
// computed on the HOST out of a grid the host had read back from the device - pending_fill::fill()
// samples that grid at the pilot resource elements and the base class reduces it. Reading the grid
// back is a device -> host crossing on EVERY hop, and it is the last one the fused lane still makes.
// The values it produces are REPORTING values: they reach the CSI/FAPI report and the debug dump,
// never the LLR path.
//
// The device already holds the same estimates: this kernel reads the very h the reformat kernel (K3)
// has just written, at the pilot positions the equalizer's layout excludes, and reduces them. So the
// host reads one float per layer instead of the grid.
//
// ---- Layout ----
// Reads the reformat's SOURCE, not its destination: `h` is [nof_systems][n_blk][2 * nout_stride]
// real/imag interleaved floats, exactly the buffer and the indexing mmse_reformat_params already
// describes - so the two kernels cannot disagree about which subcarrier is which.
//
// The pilot positions are the DM-RS resource elements of the DM-RS symbols: the layer's comb inside
// each PRB. K3 SKIPS those REs (they carry no data and the equalizer does not index by them), so
// they are reachable in h and nowhere in dst - which is why this kernel walks h directly.
//
// ---- Output ----
// Two floats per (block slot, layer): {SUM over the hop's DM-RS REs of |h|^2, how many REs were
// summed}. The count is what makes a mismatch diagnosable: a power ratio alone cannot tell
// "the wrong REs" from "the right REs, wrong scaling", and the host reports its own count
// next to this one.
// The sum is RAW, so that the host applies the same normalization it always has
// (mean over symbols, times beta^2) and the two remain comparable term by term. The SUM is done in
// this kernel's threadgroup and the ORDER is the device's, so the value matches the host's
// sequential summation to floating-point reassociation, not bit for bit - which is what the
// reporting values are allowed, and what makes them worth declaring as a tolerance rather than
// pretending they are exact.

#include <metal_stdlib>
using namespace metal;

struct mmse_rsrp_params {
  uint nout_stride;    // Output positions per block in the batch (row length of h).
  uint n_blk;          // STANDARD blocks per system in the batch. The edge block, when the batch
                       // carries one, is addressed as the block slots from n_blk on (its systems are
                       // [sys_tail, sys_tail + nof_layers)) - it is NOT an out-of-range block.
  uint nf_std;         // Subcarriers per standard block.
  uint sc_tail_base;   // First subcarrier of the edge block (n_blk * nf_std; nof_sub when there is none).
  uint nf_tail;        // Subcarriers of the edge block (0 when the hop has no edge block).
  uint sys_tail;       // First system of the edge block (== nof_layers).
  uint nof_layers;
  uint nof_symbols;
  uint dc_sc;          // DC subcarrier of the hop (>= nof_sub when there is none).
  uint dmrs_sym_bits;  // DM-RS symbols of the slot (one bit per symbol).
  // The layer's own pilot comb within a PRB, one 12-bit mask per layer. NOT the union the reformat
  // uses for its "is this RE a pilot" test: a two-layer hop has two combs and each layer's rsrp is
  // reduced over its OWN pilots.
  uint pilot_re_bits[4];
};

/// Reduction target size: a power of two so the tree below is a plain shift sequence.
constant uint mmse_rsrp_tg_size = 64;

/// ---- Compile-time bounds. NO loop in this file may be bounded by a PARAMETER. ------------------
///
/// A kernel whose termination depends on its inputs does not fail as "a wrong number" - it fails as
/// a dispatch that never finishes, and on macOS that takes the whole machine down (the WindowServer
/// watchdog fires, and `kill` does not release the GPU: see full_gpu_chain §48.131/§48.133 - it has
/// happened twice). So the bounds below are constants, and the parameter block is only ever used to
/// BREAK out early or to skip work. A parameter that is out of range therefore yields a WRONG RESULT,
/// which is visible and testable, and never a hang.
///
/// They mirror the estimator's own limits (port_channel_estimator_metal_mmse_impl.h) and the ring
/// this kernel writes into; keeping them the same numbers as the host is a checked assumption, not a
/// coincidence: see the static assertions in the unit test.
constant uint mmse_rsrp_max_slot_symb = 14;  // MAX_NSYMB_PER_SLOT
constant uint mmse_rsrp_max_layers    = 4;   // MAX_LAYERS
constant uint mmse_rsrp_max_prb       = 275; // MAX_NOF_PRBS: nf_std/nf_tail are PRBs x 12
constant uint mmse_rsrp_max_scr       = mmse_rsrp_max_prb * 12u;
constant uint mmse_rsrp_max_ring      = 16u * 4u; // kRsrpBlocks * kRsrpSlots

/// Floats per BLOCK SLOT of the ring this kernel writes into: two per layer ({sum, count}).
///
/// The layout is the K2 block layout the rest of the estimator already uses - block slot `b` and
/// layer `l` land at float `(b * nof_layers + l) * 2` - so it needs no extra parameter and cannot
/// drift from the caller's indexing. The caller reserves (n_blk + tail_slots) block slots, i.e.
/// (n_blk + tail_slots) * nof_layers * 2 floats, and reads them the same way.
constant uint kRsrpLayerFloats = 2;

kernel void mmse_rsrp(device const float*         h [[buffer(0)]],
                      device float*               out [[buffer(1)]], // [n_blk_slots][nof_layers][2]: {sum, count}
                      constant mmse_rsrp_params&  p [[buffer(2)]],
                      uint                        tgid [[threadgroup_position_in_grid]],
                      uint                        tid [[thread_position_in_threadgroup]])
{
  // Every parameter is first CLAMPED into a compile-time range (see the bounds above). From here on
  // the parameters only decide what is skipped and what is written; they never decide whether the
  // kernel ends. An out-of-range geometry therefore produces a wrong sum, which the probe sees.
  const uint nof_layers_c = (p.nof_layers <= mmse_rsrp_max_layers) ? p.nof_layers : 0u;
  if (nof_layers_c == 0u) {
    return; // no barrier below this point's reach: this return precedes every barrier in the kernel
  }
  // One threadgroup per (block slot, layer), flattened into tgid: MSL requires a kernel's inputs to
  // be uniformly scalar or uniformly vector, and the threadgroup position is the reduction's only
  // two-dimensional index, so it is unrolled here.
  const uint lay = tgid % nof_layers_c;
  const uint blk = tgid / nof_layers_c;
  // The grid is [standard block slot][layer] followed by [edge slot][layer], so a slot at or past
  // n_blk is the EDGE geometry - not an out-of-range block. `n_blk` counts the batch's STANDARD
  // blocks, and the edge block lives in the systems from sys_tail on at block 0 (see the geometry
  // below). Rejecting blk >= n_blk (the first version) is what made the edge block reduce nothing:
  // its threadgroups returned before reading a single RE.
  const bool is_edge_slot = (blk >= p.n_blk);
  const uint edge_slot    = is_edge_slot ? (blk - p.n_blk) : 0u;
  if ((lay >= nof_layers_c) || (is_edge_slot && ((p.nf_tail == 0u) || (edge_slot != 0u)))) {
    return;
  }

  const uint comb = p.pilot_re_bits[lay];

  // This block's geometry, exactly as mmse_reformat derives it: the standard blocks are b in
  // [0, n_blk) with nf_std subcarriers each, the edge block is b = 0 of the tail system with nf_tail.
  // A block index that is neither (a slot the merged batch carries but does not fill) contributes
  // nothing, and the early exit keeps the reduction from reading another block's rows.
  const bool is_std = !is_edge_slot;
  // Clamped: `nf` multiplies into every index below, and a subcarrier count wider than a block is a
  // geometry error, not something to walk off the end of the row for.
  const uint nf_raw = is_std ? p.nf_std : p.nf_tail;
  const uint nf     = (nf_raw <= mmse_rsrp_max_scr) ? nf_raw : 0u;
  if (nf == 0u) {
    return;
  }
  const uint sys    = is_std ? lay : (p.sys_tail + lay);
  const uint sc0    = is_std ? (blk * p.nf_std) : p.sc_tail_base;
  // The row of h this block lives in. `blk` is the GRID's block slot (n_blk + slot for the edge),
  // so it cannot be used as the block index inside the system: the edge block is block 0 of the
  // systems from sys_tail on. Using `blk` there read the row past the edge block's own - memory K2
  // never wrote - which is why the edge reduction summed zeros while its RE count was right.
  const uint blk_in_sys = is_std ? blk : 0u;

  threadgroup float partial[mmse_rsrp_tg_size];
  threadgroup uint  counts[mmse_rsrp_tg_size];
  float             acc = 0.0F;
  uint              nre = 0U;

  // One thread walks a strided slice of this block's (symbol, subcarrier) space. Only the DM-RS
  // symbols' comb positions contribute; the rest of the hop is data REs K3 keeps for the equalizer.
  for (uint sym = 0; sym != mmse_rsrp_max_slot_symb; ++sym) {
    if (sym >= p.nof_symbols) {
      break; // the parameter breaks the loop; it cannot extend it past the constant
    }
    if (((p.dmrs_sym_bits >> sym) & 1u) == 0u) {
      continue;
    }
    for (uint local_sc = tid; local_sc < nf; local_sc += mmse_rsrp_tg_size) {
      const uint i_re = local_sc % 12u;
      if (((comb >> i_re) & 1u) == 0u) {
        continue;
      }
      // The DC subcarrier carries no data and K3 writes a zero estimate there; skip it so the two
      // sides agree on what is excluded rather than summing a zero the host never wrote.
      if ((sc0 + local_sc) == p.dc_sc) {
        continue;
      }
      device const float* hp =
          h + (static_cast<ulong>(sys) * p.n_blk + blk_in_sys) * (2 * p.nout_stride) +
          2 * (sym * nf + local_sc);
      acc += hp[0] * hp[0] + hp[1] * hp[1];
      nre += 1U;
    }
  }

  partial[tid] = acc;
  counts[tid]  = nre;
  threadgroup_barrier(mem_flags::mem_threadgroup);

  // Tree reduction. The order is this kernel's, not the host's - see the note in the header.
  for (uint stride = mmse_rsrp_tg_size / 2u; stride != 0u; stride >>= 1u) {
    if (tid < stride) {
      partial[tid] += partial[tid + stride];
      counts[tid] += counts[tid + stride];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }

  if (tid == 0u) {
    // Output slot: [block slot][layer][2] within this batch's reserved ring region. The block slot
    // is the threadgroup's own: the standard blocks are slots [0, n_blk) and the edge block is the
    // slots from n_blk on (the grid the caller dispatches is laid out exactly that way).
    // Clamped by construction: blk < n_blk + tail slots and lay < nof_layers, so the unclamped
    // product is already inside the ring the caller reserves. The `min` is the belt to that
    // suspenders - an out-of-range write is as fatal as a loop that does not end (see the bounds
    // above), and a wrap keeps a wrong geometry a wrong VALUE rather than a dead machine.
    const uint o = min((blk * nof_layers_c + lay) * kRsrpLayerFloats, mmse_rsrp_max_ring - 2u);
    out[o]      = partial[0];
    out[o + 1u] = static_cast<float>(counts[0]);
  }
}
