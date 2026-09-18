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

// -------------------------------------------------------------------------------------------
// K4: the equalizer's noise variance, reduced on the GPU out of the same h K2 has just written.
//
// The value the equalizer scales its soft bits with is the residual energy of the received DM-RS
// pilots against the ones regenerated from the channel estimate:
//
//   noise = SUM over (DM-RS symbol s, pilot sc) | SUM over the layers of s's CDM group
//                    ( beta / nof_lse_symbols * SUM over DM-RS symbols s2 of H[s2][l][sc] )
//                    * X[s][l][sc] * exp(j 2 pi f_cfo t_s)  -  Y[s][g][sc] |^2
//
// with H the filtered (MMSE) estimates at the pilot positions - the only term that comes from the
// estimator's OUTPUT, which is why this belongs in the estimator's own command buffer: the host
// then never has to read the grid before the equalizer can be dispatched. X (the transmitted
// pilots) and Y (the received ones) are inputs the host already holds, so they are staged once per
// hop and the kernel reads them from the device.
//
// One threadgroup per hop: the reduction spans a few hundred pilots per DM-RS symbol, so a single
// threadgroup with a register accumulator and one tree reduction beats a grid-wide atomic. The
// summation order is not the host's, so the result matches it to floating-point reassociation
// (~1e-7 relative on float), not bit for bit - the value is a statistic, and the resulting soft
// bits are compared separately.
struct mmse_noise_params {
  // Block geometry of h, addressed exactly as K3 does.
  uint  nout_stride;
  uint  n_blk;
  uint  nf_std;
  uint  sc_tail_base;
  uint  nf_tail;
  uint  sys_tail;
  uint  nof_layers;
  // Pilots.
  uint  npt;              // DM-RS symbols of the hop (the time average runs over them).
  uint  nof_cdm_groups;   // CDM groups without data (one per pair of layers).
  uint  npf;              // Pilots per DM-RS symbol and layer.
  uint  nof_prb;          // PRBs of the allocation (pilot index -> subcarrier).
  uint  comb_size;        // DM-RS REs per PRB.
  uint  dmrs_re_bits;     // DM-RS RE positions within a PRB (12 bits, ascending).
  uint  dmrs_slots[4];    // Slot symbols carrying DM-RS in this hop, ascending (the estimator
                          // produces at most MAX_DMRS_SYMBOLS = 4 per hop).
  float beta;             // DM-RS to data amplitude scaling.
  float cfo;              // Estimated carrier frequency offset of the hop: the HOST's estimate, used
                          // when the device did not build the pilots (see cfo_from_device).
  uint  compensate_cfo;   // 0 or 1.
  uint  cfo_from_device;  // 1: take the CFO out of cfo_dev instead of the field above. The extraction
                          //    wrote it in the command buffer this stage is ordered after, so the host
                          //    never has to read that scalar back before this one can be encoded.
  // Finalization of the value the equalizer consumes (identical to the host's).
  uint  nof_dmrs_pilots;  // Pilots of the hop (all DM-RS symbols and layers).
  uint  nof_cdm;          // CDM groups of the transmission (ceil(nof_layers / 2)).
  float min_snr_power;    // convert_dB_to_power(MAX_SINR_DB): the SINR ceiling the variance is
                          // bounded by (a noiseless-synthetic guard).
};

/// Subcarrier of the \c sc -th pilot within the hop: pilots are PRB-major, comb ascending.
inline uint mmse_noise_pilot_subcarrier(constant mmse_noise_params& p, uint sc)
{
  const uint prb    = sc / p.comb_size;
  uint       within = sc % p.comb_size;
  uint       pos    = 0;
  for (uint i_re = 0; i_re != 12; ++i_re) {
    if (((p.dmrs_re_bits >> i_re) & 1u) == 0) {
      continue;
    }
    if (within == 0) {
      pos = i_re;
      break;
    }
    --within;
  }
  return prb * 12 + pos;
}

/// Filtered estimate at (slot symbol, layer, pilot subcarrier), out of the block layout of h.
inline float2 mmse_noise_load_h(device const float* h, constant mmse_noise_params& p, uint sym, uint layer, uint sc)
{
  const uint sc_h = mmse_noise_pilot_subcarrier(p, sc);
  uint       nf, b, local_sc, sys;
  if (sc_h < p.sc_tail_base) {
    nf       = p.nf_std;
    b        = sc_h / nf;
    local_sc = sc_h % nf;
    sys      = layer;
  } else {
    nf       = p.nf_tail;
    b        = 0;
    local_sc = sc_h - p.sc_tail_base;
    sys      = p.sys_tail + layer;
  }
  device const float* hp =
      h + (sys * p.n_blk + b) * (2 * p.nout_stride) + 2 * (sym * nf + local_sc);
  return float2{hp[0], hp[1]};
}

kernel void mmse_noise(device const float*  h [[buffer(0)]],
                       device const float2* pilots [[buffer(1)]],    // [npt][nof_layers][npf]
                       device const float2* rx_pilots [[buffer(2)]], // [npt][nof_cdm_groups][npf]
                       device float*        nv [[buffer(3)]],        // one value per estimator
                       constant mmse_noise_params& p [[buffer(4)]],
                       constant float*      epochs [[buffer(5)]],    // symbol start times, in symbols
                       device const float*  cfo_dev [[buffer(6)]],   // this hop's CFO (one value)
                       uint tid [[thread_position_in_threadgroup]],
                       uint tg_size [[threads_per_threadgroup]])
{
  // The CFO the rotation below uses, read ONCE up front. On the device route it comes out of the
  // extraction's own buffer - the slot the estimator reserved for THIS hop, which is what lets the
  // caller encode this stage without first reading that scalar back to the host; on the host route
  // the pre-stage's answer arrives in the parameter block. Hoisting it also keeps the load out of
  // the per-symbol loop.
  const float cfo = (p.cfo_from_device != 0) ? cfo_dev[0] : p.cfo;

  float acc      = 0.0F;
  float rsrp_acc = 0.0F; // SUM |H|^2 over the hop's DM-RS symbols, layers and pilots

  for (uint sc = tid; sc < p.npf; sc += tg_size) {
    // The filtered estimates are loaded ONCE per (layer, DM-RS symbol) and reused for every DM-RS
    // symbol of the hop: they are the same values, and the layout - a DM-RS comb, one subcarrier
    // every twelve - makes each load an uncoalesced, high-latency fetch. Loading them per symbol
    // (the first version did) multiplied the latency of the whole kernel by the number of symbols.
    float2 h_avg[4];
    for (uint l = 0; l != 4; ++l) {
      h_avg[l] = float2{0.0F, 0.0F};
    }
    for (uint l = 0; l != p.nof_layers; ++l) {
      float2 sum = float2{0.0F, 0.0F};
      for (uint s2 = 0; s2 != p.npt; ++s2) {
        const float2 h_sym = mmse_noise_load_h(h, p, p.dmrs_slots[s2], l, sc);
        sum += h_sym;
        rsrp_acc += h_sym.x * h_sym.x + h_sym.y * h_sym.y;
      }
      h_avg[l] = sum * (p.beta / static_cast<float>(p.npt));
    }

    for (uint i_dmrs = 0; i_dmrs != p.npt; ++i_dmrs) {
      const uint sym = p.dmrs_slots[i_dmrs];
      for (uint g = 0; g != p.nof_cdm_groups; ++g) {
        const uint layer_begin = 2 * g;
        const uint layer_end   = min(layer_begin + 2, p.nof_layers);
        float2     predicted   = float2{0.0F, 0.0F};
        for (uint l = layer_begin; l != layer_end; ++l) {
          // Regenerate the observation of this symbol out of the time-averaged estimate.
          const float2 x   = pilots[(i_dmrs * p.nof_layers + l) * p.npf + sc];
          const float2 hav = h_avg[l];
          predicted += float2{hav.x * x.x - hav.y * x.y, hav.x * x.y + hav.y * x.x};
        }
        if (p.compensate_cfo != 0) {
          // The same rotation the host applies: the CFO times the start time of the symbol, the
          // latter being an input (it depends on the cyclic prefix, not on the symbol index).
          const float phase = 2.0F * M_PI_F * cfo * epochs[sym];
          const float c     = cos(phase);
          const float s     = sin(phase);
          predicted = float2{predicted.x * c - predicted.y * s, predicted.x * s + predicted.y * c};
        }

        const float2 y = rx_pilots[(i_dmrs * p.nof_cdm_groups + g) * p.npf + sc];
        const float2 e = predicted - y;
        acc += e.x * e.x + e.y * e.y;
      }
    }
  }

  // Threadgroup tree reduction (one threadgroup covers the whole hop).
  threadgroup float2 partial[256];
  partial[tid] = float2{acc, rsrp_acc};
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint stride = tg_size / 2; stride != 0; stride /= 2) {
    if (tid < stride) {
      partial[tid] += partial[tid + stride];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
  }
  if (tid == 0) {
    // The host's finalization: normalize by the number of independent noise samples, and bound the
    // result from below by the SINR ceiling (a guard for noiseless synthetic inputs).
    const float rsrp_avg = (partial[0].y * p.beta * p.beta) /
                           (static_cast<float>(p.nof_dmrs_pilots) * static_cast<float>(p.nof_layers));
    const float min_nv = rsrp_avg / p.min_snr_power;
    const float energy = partial[0].x / (static_cast<float>(p.nof_dmrs_pilots * p.nof_cdm) - 1.0F);
    nv[0]              = max(min_nv, energy);
  }
}
