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
  uint n_blk;          // Blocks per system in the batch.
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

kernel void mmse_rsrp(device const float*         h [[buffer(0)]],
                      device float*               out [[buffer(1)]], // [n_blk_slots][nof_layers][2]: {sum, count}
                      constant mmse_rsrp_params&  p [[buffer(2)]],
                      uint                        tgid [[threadgroup_position_in_grid]],
                      uint                        tid [[thread_position_in_threadgroup]])
{
  // One threadgroup per (block slot, layer), flattened into tgid: MSL requires a kernel's inputs to
  // be uniformly scalar or uniformly vector, and the threadgroup position is the reduction's only
  // two-dimensional index, so it is unrolled here.
  const uint lay = tgid % p.nof_layers;
  const uint blk = tgid / p.nof_layers;
  if ((lay >= p.nof_layers) || (blk >= p.n_blk)) {
    return;
  }

  const uint nof_sub = p.sc_tail_base + p.nf_tail;
  const uint comb    = p.pilot_re_bits[lay];

  // This block's geometry, exactly as mmse_reformat derives it: the standard blocks are b in
  // [0, n_blk) with nf_std subcarriers each, the edge block is b = 0 of the tail system with nf_tail.
  // A block index that is neither (a slot the merged batch carries but does not fill) contributes
  // nothing, and the early exit keeps the reduction from reading another block's rows.
  const bool is_std = (blk * p.nf_std) < p.sc_tail_base;
  const bool is_tail = (p.nf_tail != 0u) && (blk == 0u);
  if (!is_std && !is_tail) {
    if (tid == 0u) {
      const uint z = (blk * p.nof_layers + lay) * 2u;
      out[z]     = 0.0F;
      out[z + 1] = 0.0F;
    }
    return;
  }
  const uint nf  = is_std ? p.nf_std : p.nf_tail;
  const uint sys = is_std ? lay : (p.sys_tail + lay);
  const uint sc0 = is_std ? (blk * p.nf_std) : p.sc_tail_base;

  threadgroup float partial[mmse_rsrp_tg_size];
  threadgroup uint  counts[mmse_rsrp_tg_size];
  float             acc = 0.0F;
  uint              nre = 0U;

  // One thread walks a strided slice of this block's (symbol, subcarrier) space. Only the DM-RS
  // symbols' comb positions contribute; the rest of the hop is data REs K3 keeps for the equalizer.
  for (uint sym = 0; sym != p.nof_symbols; ++sym) {
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
      device const float* hp = h + (static_cast<ulong>(sys) * p.n_blk + blk) * (2 * p.nout_stride) +
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
    const uint o = (blk * p.nof_layers + lay) * 2u;
    out[o]     = partial[0];
    out[o + 1] = static_cast<float>(counts[0]);
  }
}
