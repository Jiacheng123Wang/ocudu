// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// K3: device-side reformat of the K2 output into the equalizer's channel estimates.
//
// K2 writes one batch of time-frequency blocks: per (system, block) a row-major block of
// nout = nf * 14 complex outputs (real/imag interleaved floats), nf = b_prb * 12 subcarriers,
// symbol-major within the block. The equalizer does not consume that layout: for one OFDM symbol
// and one layer it wants the estimates of the allocated DATA resource elements only, packed in
// ascending subcarrier order and stored as cbf16 - see channel_equalizer::ch_est_list. This kernel
// performs that relayout and the float -> bfloat16 rounding on the GPU, so the host never walks
// the grid to build the equalizer's input (and, with the dispatch in the estimator's own command
// buffer, never has to read the grid back at all).
//
// The destination index is ARITHMETIC rather than a gather over a materialized mask: the estimator
// only produces these estimates for a contiguous allocation, every PRB of a symbol contributes the
// same number of data REs, and the DM-RS comb removes the same REs from every PRB. The rank of a
// subcarrier is therefore (PRB index) * (data REs per PRB) + (data REs below it inside its PRB),
// and the per-symbol offset is a prefix sum the caller passes. The first version staged a per-symbol
// bit mask instead, which cost more host time than the whole kernel whenever the allocation changed
// - and in the real chain the allocation changes from slot to slot.
//
// The merged standard + edge-block batch (S-5c) shows up as two geometries in one batch: the
// standard blocks cover subcarriers [0, sc_tail_base) in systems [0, nof_layers) with blocks
// 0 .. n_blk-1, and the edge block covers the remaining subcarriers in systems
// [sys_tail, sys_tail + nof_layers) at block 0. Both keep the batch's row stride (nout_stride),
// which is the standard block's nout because that is what the padded edge system was computed
// with (its real positions are the first nf_tail * 14 rows).

#include <metal_stdlib>
using namespace metal;

struct mmse_reformat_params {
  uint nout_stride;   // Output positions per block in the batch (row length of h).
  uint n_blk;         // Blocks per system in the batch.
  uint nf_std;        // Subcarriers per standard block.
  uint sc_tail_base;  // First subcarrier of the edge block (n_blk * nf_std; nof_sub when there is none).
  uint nf_tail;       // Subcarriers of the edge block (0 when the hop has no edge block).
  uint sys_tail;      // First system of the edge block (== nof_layers).
  uint nof_layers;
  uint nof_symbols;
  uint total_re;      // Compressed resource elements per layer (sum over symbols).
  uint dc_sc;         // DC subcarrier of the hop (>= nof_sub when there is none).
  uint drpp;          // Data REs per PRB of a symbol without DM-RS (12).
  uint drpp_dmrs;     // Data REs per PRB of a DM-RS symbol (12 minus the DM-RS comb size).
  uint dmrs_re_bits;  // DM-RS RE positions within a PRB (12 bits).
  uint dmrs_sym_bits; // DM-RS symbols of the slot (one bit per symbol).
};

/// Round-to-nearest-even conversion to bfloat16, bit-identical to ocudu::to_bf16(): the 16 least
/// significant fraction bits are dropped, the remaining 7 are rounded half to even.
inline ushort ocudu_to_bf16(float value)
{
  const uint bits = as_type<uint>(value);
  return static_cast<ushort>((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
}

kernel void mmse_reformat(device const float* h [[buffer(0)]],      // [nof_systems][n_blk][2 * nout_stride]
                          constant uint*      offsets [[buffer(1)]], // [nof_symbols] prefix RE counts
                          device ushort*      dst [[buffer(2)]],     // [nof_layers][total_re][2] bf16
                          constant mmse_reformat_params& p [[buffer(3)]],
                          uint gid [[thread_position_in_grid]])
{
  const uint nof_sub = p.sc_tail_base + p.nf_tail;
  const uint lay     = gid / (p.nof_symbols * nof_sub);
  const uint rem     = gid % (p.nof_symbols * nof_sub);
  const uint sym     = rem / nof_sub;
  const uint sc      = rem % nof_sub;
  if (lay >= p.nof_layers) {
    return;
  }

  const bool is_dmrs_sym = ((p.dmrs_sym_bits >> sym) & 1u) != 0;
  const uint i_re        = sc % 12u;

  // DM-RS resource elements of a DM-RS symbol carry no data and are not part of the compressed
  // layout the equalizer indexes by.
  if (is_dmrs_sym && (((p.dmrs_re_bits >> i_re) & 1u) != 0)) {
    return;
  }

  // Destination index: PRB index times the data REs a PRB contributes, plus the data REs below this
  // subcarrier within its PRB.
  const uint drpp       = is_dmrs_sym ? p.drpp_dmrs : p.drpp;
  const uint dmrs_below = popcount(p.dmrs_re_bits & ((1u << i_re) - 1u));
  const uint rank       = (sc / 12u) * drpp + (is_dmrs_sym ? (i_re - dmrs_below) : i_re);

  uint nf, b, local_sc, sys;
  if (sc < p.sc_tail_base) {
    nf       = p.nf_std;
    b        = sc / nf;
    local_sc = sc % nf;
    sys      = lay;
  } else {
    nf       = p.nf_tail;
    b        = 0;
    local_sc = sc - p.sc_tail_base;
    sys      = p.sys_tail + lay;
  }

  device const float* hp =
      h + (sys * p.n_blk + b) * (2 * p.nout_stride) + 2 * (sym * nf + local_sc);

  // The DC subcarrier carries no data (TS38.211 Section 6.3.1.7): the equalizer must see a zero
  // estimate there, and only the producer of this buffer can write it.
  const float re = (sc == p.dc_sc) ? 0.0F : hp[0];
  const float im = (sc == p.dc_sc) ? 0.0F : hp[1];

  const uint idx = lay * p.total_re + offsets[sym] + rank;
  dst[2 * idx]     = ocudu_to_bf16(re);
  dst[2 * idx + 1] = ocudu_to_bf16(im);
}
