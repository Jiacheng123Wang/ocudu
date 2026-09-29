// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief WHAT ONE CHANNEL-ESTIMATOR KERNEL COSTS: an offline micro-benchmark of the production kernels.
///
/// WHY IT EXISTS (dev doc 3.3, after 6.64). The dispatch-elimination arms are measured and spent: a
/// removed dispatch is worth 5-14 us (paired 10-14), the remaining candidates are worth ~30-42 us, and
/// what is left of the hop - `merged_hop` is ~534 us/lane - was written down as "the rest is real
/// compute + inter-dispatch waits" without anybody measuring either half. Deciding whether to rewrite a
/// KERNEL needs the kernels' own cost at the geometry the air interface runs.
///
/// WHY A STANDALONE BENCHMARK AND NOT THE ENGINE'S REPETITION KNOBS. The engine has one
/// (`mmse_engine_impl::stage_repeat()`), and driving it from the replay was tried first and does NOT
/// work on this host: the reading would be `[metal_stats] gpu busy (back_end)`, which is the UNION of
/// every commit of the run, and it varies by tens of percent run to run (19 commits, ~12.5 ms of busy,
/// 16 of them the CPU-reference hops) - the first sweep produced slopes of -130 us per dispatch, i.e.
/// noise. The per-commit alternative (`queue occupancy ... slowest commits`) is not it either: a
/// `ce_weights` commit carries an encoded WAIT on the extraction's fence, so its `start->end` is a
/// window and not execution (measured 745-750 us for a stage whose whole hop is 534 us). A kernel's
/// cost therefore has to be measured where nothing else is: its own command buffer, its own grid, the
/// production parameter block - which is what this file does, and the shape the DFT arm already proved
/// (doc_chinese/phy_latency/wip/dft_kernel_cost.mm).
///
/// THE GEOMETRY is the air leg's (configs/gnb_rf_b200_tdd_n78_20mhz.yml): a 3 PRB estimation block,
/// dmrs_type=1 with 2 CDM groups => 6 DM-RS REs per PRB per symbol (re_pattern.count() = 6, NOT the
/// comb SPACING of 2), 3 DM-RS symbols, 1 layer. So npf = 3 * 6 = 18 pilots per (block, symbol),
/// L = npt * npf = 54 rows per block slot, nf = 36 subcarriers, nout = nf * 14 = 504 output rows. That
/// is the geometry every leg since p27 reports ([y_direct] ... systems=1 blocks=1 L=54 groups=1), and
/// the one the 4 PRB corpus hop stages (syn004_4.txt: dmrs_symbols=2,7,11, alloc_nof_rb=4 ->
/// standard block + 1 PRB edge block).
///
/// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
///
///   clang++ -std=c++20 -fobjc-arc -framework Metal -framework Foundation -O2 \
///           doc_chinese/phy_latency/wip/ce_kernel_cost.mm -o /tmp/ce_kernel_cost
///   /tmp/ce_kernel_cost [metallib-path]     # default: the checkout's ocudu_mmse.metallib
///
/// WHAT IT REPORTS: GPU microseconds per dispatch (the command buffer's own timestamps, over 200
/// dispatches in ONE buffer, so the number is the dispatch and not the queue) for each kernel at the
/// production geometry, plus a dispatch-overhead baseline (the same pipeline, the smallest grid that
/// still runs it) and a block-size sweep for the two kernels whose cost scales with the block.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>
#include <vector>

namespace {

// ---- the parameter blocks, mirrored field for field (each with the same static_assert the engine's
//      own mirrors carry: a field that drifts is read as geometry, not as a compile error) -----------

struct weights_params {           // mmse_weights_params (ocudu_mmse_weights.metal)
  uint32_t nout;
  uint32_t L;
  uint32_t nof_systems;
};
static_assert(sizeof(weights_params) == 12, "");

struct apply_params {             // mmse_apply_params (ocudu_mmse_apply.metal)
  uint32_t nout;
  uint32_t L;
  uint32_t nof_systems;
  uint32_t nof_blocks;
};
static_assert(sizeof(apply_params) == 16, "");

struct apply_lse_batch {          // mmse_apply_lse_batch (ocudu_mmse_apply_lse.metal)
  uint32_t nout;
  uint32_t L;
  uint32_t nof_systems;
  uint32_t nof_blocks;
};
static_assert(sizeof(apply_lse_batch) == 16, "");

struct y_source {                 // mmse_y_source (ocudu_mmse_apply_lse.metal)
  uint32_t sys_lo;
  uint32_t sys_hi;
  uint32_t nof_layers;
  uint32_t nof_pilots;
  uint32_t nof_symb;
  uint32_t pilot_base;
  uint32_t npf;
  uint32_t n_blk_real;
  float    inv_beta;
};
static_assert(sizeof(y_source) == 36, "");

struct lse_params {               // mmse_lse_params (ocudu_mmse_apply_lse.metal)
  uint32_t nof_sources;
  y_source sources[2];
};
static_assert(sizeof(lse_params) == 76, "");

struct corr_params {              // mmse_corr_params (ocudu_mmse_corr.metal)
  uint32_t nof_systems;
  uint32_t npt;
  uint32_t npf;
  uint32_t ncomb;
  uint32_t nf;
  uint32_t L;
  uint32_t Ls;
  uint32_t a_sys;
  uint32_t r_sys;
  float    ts;
  float    scs_hz;
  float    fd_hz;
  float    tau_rms_s;
  float    sigma2;
  float    ridge;
  uint32_t sigma2_from_device;
  uint32_t sigma2_slot;
  uint32_t dmrs_slots[4];
  uint32_t pilot_re[12];
};
static_assert(sizeof(corr_params) == 132, "");

struct reformat_params {          // mmse_reformat_params (ocudu_mmse_reformat.metal)
  uint32_t nout_stride;
  uint32_t n_blk;
  uint32_t nf_std;
  uint32_t sc_tail_base;
  uint32_t nf_tail;
  uint32_t sys_tail;
  uint32_t nof_layers;
  uint32_t nof_symbols;
  uint32_t total_re;
  uint32_t dc_sc;
  uint32_t drpp;
  uint32_t drpp_dmrs;
  uint32_t dmrs_re_bits;
  uint32_t dmrs_sym_bits;
};
static_assert(sizeof(reformat_params) == 56, "");

struct equalize_params {          // equalize_params (ocudu_equalizer.metal)
  uint32_t nof_re;
  uint32_t nof_ports;
  uint32_t nof_layers;
  uint32_t algo;
  float    noise_var;
  float    tx_scaling;
  float    h_scaling;
  uint32_t h_offset;
  uint32_t h_layer_stride;
};
static_assert(sizeof(equalize_params) == 36, "");

struct demod_params {             // demod_params (ocudu_demod.metal)
  uint32_t nof_symbols;
  uint32_t nof_re;
  uint32_t mod;
  uint32_t sym_stride;
  uint32_t nv_stride;
  uint32_t llr_stride;
};
static_assert(sizeof(demod_params) == 24, "");

/// Per-symbol strides of the batched equalizer (equalize_strides in ocudu_equalizer.metal). The
/// per-symbol starts are what make a run's symbols addressable when they are not evenly spaced; an
/// arm that lays its buffer out densely sets them to k * nof_re, which is exactly the layout the
/// lane's direct-grid path produces.
struct equalize_strides_h {
  uint32_t nof_symbols;
  uint32_t h_stride;
  uint32_t eq_stride;
  uint32_t nv_stride;
  uint32_t h_starts[14];
  uint32_t y_starts[14];
};
static_assert(sizeof(equalize_strides_h) == 4 * sizeof(uint32_t) + 2 * 14 * sizeof(uint32_t), "");

/// The air interface's shape as every eq/demod arm uses it: MMSE, 1 Tx layer, 1 Rx port, unit scalings.
static equalize_params make_eq_params(uint32_t nof_re, uint32_t nof_ports, uint32_t nof_layers)
{
  equalize_params ep{};
  ep.nof_re         = nof_re;
  ep.nof_ports      = nof_ports;
  ep.nof_layers     = nof_layers;
  ep.algo           = 1; // MMSE
  ep.noise_var      = 0.1F;
  ep.tx_scaling     = 1.0F;
  ep.h_scaling      = 1.0F;
  ep.h_offset       = 0;
  ep.h_layer_stride = nof_re;
  return ep;
}

// ---- the production geometry ---------------------------------------------------------------------

/// One estimation block: 3 PRB, dmrs_type=1 with 2 CDM groups, `npt` DM-RS symbols.
struct geometry {
  uint32_t block_prb = 3;   // MAX_BLOCK_PRB
  uint32_t ncomb     = 6;   // DM-RS REs per PRB per symbol = re_pattern.count()
  uint32_t npt       = 3;   // DM-RS symbols of the hop
  uint32_t layers    = 1;
  uint32_t blocks    = 1;   // block slots of the batch (n_std_blocks)

  uint32_t npf() const { return block_prb * ncomb; }        // pilots per (block, symbol)
  uint32_t L() const { return npt * npf(); }                // pilot rows of one block slot
  uint32_t nf() const { return block_prb * 12u; }           // subcarriers of the block
  uint32_t nout() const { return nf() * 14u; }              // output rows of the block
  uint32_t systems() const { return 2u * layers; }          // the merged batch: standard + edge group
};

} // namespace

int main(int argc, char** argv)
{
  @autoreleasepool {
    NSString* lib_path = (argc > 1) ? @(argv[1])
                                    : @"lib/phy/upper/signal_processors/channel_estimator/metal/ocudu_mmse.metallib";
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::printf("FAIL: no Metal device\n");
      return 1;
    }
    NSError*       err = nil;
    id<MTLLibrary> lib = [device newLibraryWithFile:lib_path error:&err];
    if (lib == nil) {
      std::printf("FAIL: cannot load %s: %s\n", lib_path.UTF8String, err.localizedDescription.UTF8String);
      return 1;
    }

    const auto pipeline = [&](const char* name) -> id<MTLComputePipelineState> {
      id<MTLFunction> fn = [lib newFunctionWithName:@(name)];
      if (fn == nil) {
        std::printf("  (no %s in this library)\n", name);
        return nil;
      }
      return [device newComputePipelineStateWithFunction:fn error:&err];
    };

    id<MTLComputePipelineState> p_weights    = pipeline("mmse_weights");
    id<MTLComputePipelineState> p_apply      = pipeline("mmse_apply");
    id<MTLComputePipelineState> p_apply_lse  = pipeline("mmse_apply_lse");
    id<MTLComputePipelineState> p_corr_a     = pipeline("mmse_corr_a");
    id<MTLComputePipelineState> p_corr_rhp   = pipeline("mmse_corr_r_hp");
    id<MTLComputePipelineState> p_corr_merged = pipeline("mmse_corr_a_rhp");
    id<MTLComputePipelineState> p_reformat   = pipeline("mmse_reformat");
    // The equalizer and the demapper live in their own libraries; they are OPTIONAL here (a checkout
    // that has not built them still gets the CE table).
    id<MTLLibrary> eq_lib = [device newLibraryWithFile:@"lib/phy/upper/channel_processors/metal/ocudu_equalizer.metallib"
                                                 error:&err];
    id<MTLLibrary> dm_lib = [device newLibraryWithFile:@"lib/phy/upper/channel_modulation/metal/ocudu_demod.metallib"
                                                 error:&err];
    const auto from_lib = [&](id<MTLLibrary> l, const char* name) -> id<MTLComputePipelineState> {
      if (l == nil) {
        return nil;
      }
      id<MTLFunction> fn = [l newFunctionWithName:@(name)];
      return (fn != nil) ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
    };
    if (p_weights == nil || p_apply == nil || p_corr_a == nil || p_corr_rhp == nil) {
      std::printf("FAIL: the metallib does not carry the estimator's kernels\n");
      return 1;
    }

    id<MTLCommandQueue> q = [device newCommandQueue];
    // Generous shared buffers: the largest geometry below is 3x the production block.
    const NSUInteger big = 4u << 20; // 4 MB each
    id<MTLBuffer> b_w   = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_a   = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_rhp = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_y   = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_h   = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_lse = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_sc  = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_dst = [device newBufferWithLength:big options:MTLResourceStorageModeShared];
    id<MTLBuffer> b_off = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    std::memset(b_a.contents, 0, big);
    std::memset(b_y.contents, 0, big);
    std::memset(b_lse.contents, 0, big);
    std::memset(b_dst.contents, 0, big);
    std::memset(b_off.contents, 0, 4096);
    for (NSUInteger i = 0; i != 4096 / sizeof(float); ++i) {
      static_cast<float*>(b_sc.contents)[i] = 0.1F;
    }

    constexpr uint32_t reps = 200;
    /// Times `n` dispatches of one pipeline in ONE command buffer. GPU time comes from the buffer's own
    /// timestamps, so the queue is not in the number.
    ///
    /// \note WARM-UP FIRST, and it is not optional: Metal pays the shader's first use (upload/state
    ///       setup, ~ms) inside whichever command buffer it happens in, and with 200 reps that is tens
    ///       of microseconds per dispatch. Measured without it, the SAME arm read 28.7 us/dispatch when
    ///       it was the first of its pipeline in the file and 9.2 us in the sweep further down - a 3x
    ///       instrument artefact that would have been read as "the kernel is expensive".
    const auto time_it = [&](id<MTLComputePipelineState> pipe, NSUInteger tg, NSUInteger tpt,
                             void (^bind)(id<MTLComputeCommandEncoder>)) -> double {
      const auto encode = [&](id<MTLCommandBuffer> cb, uint32_t n) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        bind(enc);
        for (uint32_t r = 0; r != n; ++r) {
          [enc dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
        }
        [enc endEncoding];
      };
      {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, 20);
        [cb commit];
        [cb waitUntilCompleted];
      }
      // MIN OF THREE: the GPU's clock state is not constant over the first milliseconds of a run, and
      // an arm measured once read 21.6 and 58.6 us in two consecutive runs of the SAME binary (the
      // weights kernel). The minimum is the least-disturbed sample, which is what a cost benchmark
      // wants; the spread is printed by the caller when it matters.
      double best = 1e30;
      for (unsigned round = 0; round != 3; ++round) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, reps);
        [cb commit];
        [cb waitUntilCompleted];
        best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1e6 / static_cast<double>(reps));
      }
      return best;
    };

    /// The same measurement for the kernels their engines dispatch with `dispatchThreads` (a
    /// NON-UNIFORM grid, where the grid size IS the thread count). Passing such a kernel through
    /// time_it() instead multiplies its grid by the threadgroup size - measured: the equalizer read
    /// 4.0 us for 156 REs, i.e. 25.7 ns per thread, which was 156*256 threads doing nothing.
    const auto time_it_threads = [&](id<MTLComputePipelineState> pipe, NSUInteger n_threads, NSUInteger tpt,
                                     void (^bind)(id<MTLComputeCommandEncoder>)) -> double {
      const auto encode = [&](id<MTLCommandBuffer> cb, uint32_t n) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        bind(enc);
        for (uint32_t r = 0; r != n; ++r) {
          [enc dispatchThreads:MTLSizeMake(n_threads, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
        }
        [enc endEncoding];
      };
      {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, 20);
        [cb commit];
        [cb waitUntilCompleted];
      }
      double best = 1e30;
      for (unsigned round = 0; round != 3; ++round) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, reps);
        [cb commit];
        [cb waitUntilCompleted];
        best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1e6 / static_cast<double>(reps));
      }
      return best;
    };

    /// \brief The same measurement for a dispatch with an ARBITRARY grid (the equalizer's and the
    ///        demapper's real shape is two-dimensional: (nof_re, nof_symbols)).
    ///
    /// It exists because \c time_it_threads can only express a 1-D grid, and a 1-D grid is NOT the same
    /// dispatch when the kernel reads \c thread_position_in_grid.y: the lane's batched equalizer and
    /// demapper are both 2-D, and the difference is not cosmetic - the (256,1,1) threadgroup tiles the
    /// grid per dimension, so a 2-D grid pays a partial threadgroup PER SYMBOL ROW while a 1-D grid pays
    /// it once. Which of the two the device actually charges for is measured below, not assumed.
    const auto time_it_grid = [&](id<MTLComputePipelineState> pipe, MTLSize grid, MTLSize tg,
                                  void (^bind)(id<MTLComputeCommandEncoder>)) -> double {
      const auto encode = [&](id<MTLCommandBuffer> cb, uint32_t n) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        bind(enc);
        for (uint32_t r = 0; r != n; ++r) {
          [enc dispatchThreads:grid threadsPerThreadgroup:tg];
        }
        [enc endEncoding];
      };
      {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, 20);
        [cb commit];
        [cb waitUntilCompleted];
      }
      double best = 1e30;
      for (unsigned round = 0; round != 3; ++round) {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        encode(cb, reps);
        [cb commit];
        [cb waitUntilCompleted];
        best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1e6 / static_cast<double>(reps));
      }
      return best;
    };

    const geometry g{};

    std::printf("device=%s\n", device.name.UTF8String);
    std::printf("geometry: block_prb=%u ncomb=%u npt=%u -> npf=%u L=%u nf=%u nout=%u layers=%u "
                "systems=%u blocks=%u   (reps=%u per arm)\n\n",
                g.block_prb, g.ncomb, g.npt, g.npf(), g.L(), g.nf(), g.nout(), g.layers, g.systems(),
                g.blocks, reps);

    // ---- the corr parameters (both kernels share them) ----
    corr_params cp{};
    cp.nof_systems = g.systems();
    cp.npt         = g.npt;
    cp.npf         = g.npf();
    cp.ncomb       = g.ncomb;
    cp.nf          = g.nf();
    cp.L           = g.L();
    cp.Ls          = g.L();
    cp.a_sys       = g.L() * g.L();
    cp.r_sys       = g.nout() * g.L();
    cp.ts          = 1.0F / (30000.0F * 14.0F);
    cp.scs_hz      = 30000.0F;
    cp.fd_hz       = 10.0F;
    cp.tau_rms_s   = 1e-6F;
    cp.sigma2      = 0.1F;
    cp.ridge       = 1e-6F;
    cp.sigma2_from_device = 0;
    cp.sigma2_slot = 2;
    for (uint32_t i = 0; i != 4; ++i) {
      cp.dmrs_slots[i] = (i < g.npt) ? (2u + 5u * i) : 0u;
    }
    for (uint32_t i = 0; i != 12; ++i) {
      cp.pilot_re[i] = (i < g.ncomb) ? (2u * i) : 0u; // dmrs_type=1: every other subcarrier
    }

    weights_params wp{g.nout(), g.L(), g.systems()};
    apply_params   ap{g.nout(), g.L(), g.systems(), g.blocks};

    // GLOBAL WARM-UP before any measurement. The min-of-three inside time_it() was not enough: the
    // FIRST arm of a process is still inside the GPU's clock ramp, so a 1-threadgroup dispatch that
    // does nothing read 1.9 us in one run and 4.9 us in the next, and the weights kernel read 19.7 vs
    // 27.7. ~50 ms of real work before the table puts the device in its steady state; the two runs'
    // readings then agree to ~10%, which the arms below show.
    {
      const NSUInteger tgs_per_sys = (g.nout() * g.L() + 127u) / 128u;
      for (unsigned i = 0; i != 12; ++i) {
        id<MTLCommandBuffer>         cb  = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p_weights];
        [enc setBuffer:b_rhp offset:0 atIndex:0];
        [enc setBuffer:b_a offset:0 atIndex:1];
        [enc setBuffer:b_w offset:0 atIndex:2];
        [enc setBytes:&wp length:sizeof(wp) atIndex:3];
        for (unsigned r = 0; r != 200; ++r) {
          [enc dispatchThreadgroups:MTLSizeMake(g.systems() * tgs_per_sys, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
        }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      }
    }

    // ---- the arms ---------------------------------------------------------------------------------
    std::printf("%-34s %12s %14s\n", "kernel (production geometry)", "grid threads", "GPU us/dispatch");
    std::printf("%-34s %12s %14s\n", "---------------------------------", "------------", "--------------");

    const auto row = [](const char* label, NSUInteger threads, double us) {
      std::printf("%-34s %12llu %14.3f\n", label, (unsigned long long)threads, us);
    };

    // BASELINE: the weights pipeline with the smallest grid that still runs it. Everything above this is
    // the dispatch itself (encode, launch, retire), which is what the elimination arms remove.
    {
      weights_params tiny{1u, 1u, 1u};
      const double us = time_it(p_weights, 1, 128, ^(id<MTLComputeCommandEncoder> e) {
        [e setBuffer:b_rhp offset:0 atIndex:0];
        [e setBuffer:b_a offset:0 atIndex:1];
        [e setBuffer:b_w offset:0 atIndex:2];
        [e setBytes:&tiny length:sizeof(tiny) atIndex:3];
      });
      row("BASELINE: weights, 1 threadgroup", 128, us);
    }

    {
      const NSUInteger tgs_per_sys = (g.nout() * g.L() + 127u) / 128u;
      row("mmse_weights (K1b)",
          static_cast<NSUInteger>(g.systems()) * tgs_per_sys * 128u,
          time_it(p_weights, g.systems() * tgs_per_sys, 128, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_rhp offset:0 atIndex:0];
            [e setBuffer:b_a offset:0 atIndex:1];
            [e setBuffer:b_w offset:0 atIndex:2];
            [e setBytes:&wp length:sizeof(wp) atIndex:3];
          }));
    }

    {
      row("mmse_apply (K2, y route)",
          static_cast<NSUInteger>(g.blocks) * g.systems() * g.nout(),
          time_it(p_apply, g.blocks * g.systems(), g.nout(), ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_w offset:0 atIndex:0];
            [e setBuffer:b_y offset:0 atIndex:1];
            [e setBuffer:b_h offset:0 atIndex:2];
            [e setBytes:&ap length:sizeof(ap) atIndex:3];
          }));
    }

    if (p_apply_lse != nil) {
      // The merged batch's own shape: two groups, one system each (the standard block and the 1 PRB edge
      // block share the slot stride, which is why both read 54 rows out of the LSE).
      lse_params lp{};
      lp.nof_sources        = 2;
      lp.sources[0].sys_lo  = 0;
      lp.sources[0].sys_hi  = 1;
      lp.sources[1].sys_lo  = 1;
      lp.sources[1].sys_hi  = 2;
      for (auto& s : lp.sources) {
        s.nof_layers = 1;
        s.nof_pilots = 3u * g.L();
        s.nof_symb   = g.npt;
        s.pilot_base = 0;
        s.npf        = g.npf();
        s.n_blk_real = 1;
        s.inv_beta   = 1.0F;
      }
      row("mmse_apply_lse (K2, LSE route)",
          static_cast<NSUInteger>(g.blocks) * g.systems() * g.nout(),
          time_it(p_apply_lse, g.blocks * g.systems(), g.nout(), ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_w offset:0 atIndex:0];
            [e setBuffer:b_h offset:0 atIndex:1];
            [e setBuffer:b_lse offset:0 atIndex:2];
            [e setBytes:&ap length:sizeof(ap) atIndex:3];
            [e setBytes:&lp length:sizeof(lp) atIndex:4];
          }));
    }

    {
      const NSUInteger threads = static_cast<NSUInteger>(g.L()) * g.L() * g.systems();
      row("mmse_corr_a (K0-d)",
          threads,
          time_it(p_corr_a, static_cast<NSUInteger>(g.L()) * g.L(), g.systems(), ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_a offset:0 atIndex:0];
            [e setBytes:&cp length:sizeof(cp) atIndex:1];
            [e setBuffer:b_sc offset:0 atIndex:2];
          }));
    }

    {
      const NSUInteger threads = static_cast<NSUInteger>(g.nout()) * g.L() * g.systems();
      row("mmse_corr_r_hp (K0-d)",
          threads,
          time_it(p_corr_rhp, static_cast<NSUInteger>(g.nout()) * g.L(), g.systems(), ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_rhp offset:0 atIndex:0];
            [e setBytes:&cp length:sizeof(cp) atIndex:1];
          }));
    }

    if (p_reformat != nil) {
      reformat_params rp{};
      rp.nout_stride  = g.nout();
      rp.n_blk        = g.blocks;
      rp.nf_std       = g.nf();
      rp.sc_tail_base = 0;
      rp.nf_tail      = 0;
      rp.sys_tail     = 0;
      rp.nof_layers   = g.layers;
      rp.nof_symbols  = 14;
      rp.total_re     = 12u * rp.nf_std * 14u;
      rp.dc_sc        = 4096;
      rp.drpp         = 12;
      rp.drpp_dmrs    = 12 - g.ncomb;
      rp.dmrs_re_bits = 0;
      rp.dmrs_sym_bits = 0;
      const NSUInteger threads = static_cast<NSUInteger>(g.layers) * rp.nof_symbols * g.nf();
      row("mmse_reformat (K3)",
          threads,
          time_it(p_reformat, threads, 1, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_h offset:0 atIndex:0];
            [e setBuffer:b_off offset:0 atIndex:1];
            [e setBuffer:b_dst offset:0 atIndex:2];
            [e setBytes:&rp length:sizeof(rp) atIndex:3];
          }));
    }

    // ---- WHAT A STAGE BOUNDARY COSTS -----------------------------------------------------------------
    //
    // 6.65's finding was that the legs' 10-14 us per REMOVED dispatch is neither the kernel's execution
    // (1.5-6.5 us for the ones those arms removed) nor the fixed dispatch cost (1.3-1.4 us above). The
    // candidates for the rest are the three things that separate two dispatches of the estimator's chain
    // - a memory barrier, a pipeline switch, an encoder boundary - and each one can be priced here by
    // inserting it between two dispatches of the SAME kernel and reading the growth. This is the unit
    // the "fewer boundaries" lever (6.65 (4)) is bought in.
    {
      const NSUInteger tg = g.blocks * g.systems();
      const NSUInteger tpt = g.nout();
      lse_params lp{};
      lp.nof_sources       = 2;
      lp.sources[0].sys_lo = 0;
      lp.sources[0].sys_hi = 1;
      lp.sources[1].sys_lo = 1;
      lp.sources[1].sys_hi = 2;
      for (auto& src : lp.sources) {
        src.nof_layers = 1;
        src.nof_pilots = 3u * g.L();
        src.nof_symb   = g.npt;
        src.pilot_base = 0;
        src.npf        = g.npf();
        src.n_blk_real = 1;
        src.inv_beta   = 1.0F;
      }
      const auto bind_arm = ^(id<MTLComputeCommandEncoder> e) {
        [e setBuffer:b_w offset:0 atIndex:0];
        [e setBuffer:b_h offset:0 atIndex:1];
        [e setBuffer:b_lse offset:0 atIndex:2];
        [e setBytes:&ap length:sizeof(ap) atIndex:3];
        [e setBytes:&lp length:sizeof(lp) atIndex:4];
      };

      const auto time_boundary = [&](int kind) -> double {
        // kind 0 = back to back, 1 = a buffer barrier, 2 = a pipeline switch, 3 = an encoder boundary.
        double best = 1e30;
        for (unsigned round = 0; round != 3; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_apply_lse];
          bind_arm(enc);
          for (uint32_t r = 0; r != reps; ++r) {
            [enc dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
            switch (kind) {
              case 1:
                [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
                break;
              case 2:
                [enc setComputePipelineState:p_apply];
                [enc setComputePipelineState:p_apply_lse];
                break;
              case 3:
                [enc endEncoding];
                enc = [cb computeCommandEncoder];
                [enc setComputePipelineState:p_apply_lse];
                bind_arm(enc);
                break;
              default:
                break;
            }
          }
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1e6 / static_cast<double>(reps));
        }
        return best;
      };

      // ---- WHAT THE HOST PAYS PER DISPATCH ------------------------------------------------------
      //
      // The GPU cannot explain 10-14 us per removed dispatch (execution 1.5-6.5, floor 1.4, and the
      // three boundaries below measure ~0), so the candidate left is the HOST's own encode - which this
      // file has so far kept out of every number by keeping the GPU busy. That is the point of this arm:
      // encode `reps` dispatches and time ONLY the CPU's encoding (no commit, no wait inside the timer).
      const auto time_encode = [&]() -> double {
        double best = 1e30;
        for (unsigned round = 0; round != 5; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          const double         t0 = CFAbsoluteTimeGetCurrent();
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_apply_lse];
          bind_arm(enc);
          for (uint32_t r = 0; r != reps; ++r) {
            [enc dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(tpt, 1, 1)];
          }
          [enc endEncoding];
          const double t1 = CFAbsoluteTimeGetCurrent();
          [cb commit];
          [cb waitUntilCompleted];
          best = std::min(best, (t1 - t0) * 1e6 / static_cast<double>(reps));
        }
        return best;
      };

      const double base = time_boundary(0);
      std::printf("\nstage boundaries (mmse_apply_lse, %u dispatches, at the production geometry):\n", reps);
      std::printf("%-34s %14s %16s\n", "boundary between dispatches", "us/dispatch", "vs back-to-back");
      const char* names[] = {"none (back to back)", "buffer barrier", "pipeline switch", "encoder boundary"};
      for (int k = 0; k != 4; ++k) {
        const double us = time_boundary(k);
        std::printf("%-34s %14.3f %16.3f\n", names[k], us, us - base);
      }
      std::printf("\nhost cost of one more dispatch (CPU encode only, GPU not waited for): %.3f us\n",
                  time_encode());
    }

    // ---- THE EQUALIZER AND THE DEMAPPER (dev doc 8 Q20: completing the compute bill) ----------------
    //
    // Their geometry is not a block but the hop's DATA RESOURCE ELEMENTS, which the legs do not print,
    // so these arms are parameterised by nof_re and swept: the cost is linear in it (one thread per RE),
    // and whoever needs the hop's own number plugs in the RE count the hop geometry gives. Both use the
    // air interface's shape: 1 Tx layer, 1 Rx port, QPSK (the corpus' and the legs' modulation) and the
    // dispatch shape the engines use (dispatchThreads, 256 threads per threadgroup).
    {
      id<MTLComputePipelineState> p_eq    = from_lib(eq_lib, "equalize_mxn");
      id<MTLComputePipelineState> p_demod = from_lib(dm_lib, "demod_soft");
      if (p_eq == nil) {
        std::printf("\n(equalizer metallib not found - skipping the equalizer/demapper arms)\n");
      }
      const NSUInteger            big_c   = 4u << 20;
      id<MTLBuffer>               b_hc    = [device newBufferWithLength:big_c options:MTLResourceStorageModeShared];
      id<MTLBuffer>               b_yc    = [device newBufferWithLength:big_c options:MTLResourceStorageModeShared];
      id<MTLBuffer>               b_eq    = [device newBufferWithLength:big_c options:MTLResourceStorageModeShared];
      id<MTLBuffer>               b_nvc   = [device newBufferWithLength:big_c options:MTLResourceStorageModeShared];
      id<MTLBuffer>               b_llr   = [device newBufferWithLength:big_c options:MTLResourceStorageModeShared];
      std::memset(b_hc.contents, 0, big_c);
      std::memset(b_yc.contents, 0, big_c);
      for (NSUInteger i = 0; i != big_c / sizeof(float); ++i) {
        static_cast<float*>(b_nvc.contents)[i] = 0.1F; // noise variance > 0 (the equalizer's guard)
      }
      if (p_eq != nil) {
        std::printf("\nequalizer + demapper (1 layer / 1 port / QPSK; the cost is linear in nof_re):\n");
        std::printf("%-34s %12s %14s %14s\n", "nof_re (data REs)", "threads", "equalize us", "demod us");
        for (uint32_t re : {156u, 612u, 1224u, 2184u}) {
          equalize_params ep{};
          ep.nof_re         = re;
          ep.nof_ports      = 1;
          ep.nof_layers     = 1;
          ep.algo           = 1; // MMSE
          ep.noise_var      = 0.1F;
          ep.tx_scaling     = 1.0F;
          ep.h_scaling      = 1.0F;
          ep.h_offset       = 0;
          ep.h_layer_stride = re;
          const double eq_us = time_it_threads(p_eq, re, 256, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_hc offset:0 atIndex:0];
            [e setBuffer:b_yc offset:0 atIndex:1];
            [e setBuffer:b_eq offset:0 atIndex:2];
            [e setBuffer:b_nvc offset:0 atIndex:3];
            [e setBytes:&ep length:sizeof(ep) atIndex:4];
            [e setBuffer:b_nvc offset:0 atIndex:5];
          });
          double dem_us = 0.0;
          if (p_demod != nil) {
            demod_params dp{};
            dp.nof_symbols = 1;
            dp.nof_re      = re;
            dp.mod         = 0; // MOD_QPSK
            dp.sym_stride  = 1;
            dp.nv_stride   = 1;
            dp.llr_stride  = 2 * re; // QPSK: 2 bits per RE
            dem_us         = time_it_threads(p_demod, re, 256, ^(id<MTLComputeCommandEncoder> e) {
              [e setBuffer:b_eq offset:0 atIndex:0];
              [e setBuffer:b_nvc offset:0 atIndex:1];
              [e setBuffer:b_llr offset:0 atIndex:2];
              [e setBytes:&dp length:sizeof(dp) atIndex:3];
            });
          }
          std::printf("%-34u %12u %14.3f %14.3f\n", re, re, eq_us, dem_us);
        }
      }
    }

    // ---- 2026-09-30: EQ/DEMAP PARALLELISM AT THE LANE'S OWN GEOMETRY ------------------------------
    //
    // WHY THE ARMS ABOVE ARE NOT ENOUGH. They time ONE-DIMENSIONAL dispatches of the PER-SYMBOL kernels
    // (grid = nof_re threads, one symbol). The lane dispatches the BATCHED kernels over a 2-D grid.
    // p150's counters give the real shape: `eq_batch flushes=19558 symbols=234686 runs=19558
    // batched=19558 max_run=12` and `demod_batch flushes=19558 symbols=234686 dispatches=19558
    // max_run=12` over 19558 hops - i.e. ONE equalizer dispatch and ONE demapper dispatch per hop, each
    // carrying 12 OFDM symbols, grid (nof_re, 12) with a (256,1,1) threadgroup. Three questions follow
    // from the code, and none of them can be answered by the 1-D arms:
    //   (1) TILING: how many lanes does that grid launch, and how many of them do work? A (256,1,1)
    //       threadgroup tiles a 2-D grid once per dimension, so if the partial threadgroup is padded,
    //       the waste is paid once per SYMBOL ROW - i.e. it is a function of the grant.
    //   (2) SHAPE: the same 7344 useful threads as a single 1-D row - one partial threadgroup per
    //       dispatch instead of twelve - costs what?
    //   (3) SATURATION: how many threads does a dispatch need before it stops being launch-bound and
    //       starts being throughput-bound? That number decides whether ANY grant can fill the device
    //       with ONE dispatch, which is the premise the multi-lane direction rests on.
    {
      id<MTLComputePipelineState> p_eq_b  = from_lib(eq_lib, "equalize_mxn_batch");
      id<MTLComputePipelineState> p_eq_1d = from_lib(eq_lib, "equalize_mxn");
      id<MTLComputePipelineState> p_dm    = from_lib(dm_lib, "demod_soft");
      if ((p_eq_b == nil) || (p_eq_1d == nil) || (p_dm == nil)) {
        std::printf("\n(eq/demod parallelism arms skipped: a metallib or a kernel is missing)\n");
      }
      else {
        const NSUInteger big_p = 4u << 20;
        id<MTLBuffer>    b_hp  = [device newBufferWithLength:big_p options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_yp  = [device newBufferWithLength:big_p options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_eqp = [device newBufferWithLength:big_p options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_nvp = [device newBufferWithLength:big_p options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_llp = [device newBufferWithLength:big_p options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_tg  = [device newBufferWithLength:16u << 20 options:MTLResourceStorageModeShared];
        id<MTLBuffer>    b_cnt = [device newBufferWithLength:64 options:MTLResourceStorageModeShared];
        std::memset(b_hp.contents, 0, big_p);
        std::memset(b_yp.contents, 0, big_p);
        for (NSUInteger i = 0; i != big_p / sizeof(float); ++i) {
          static_cast<float*>(b_nvp.contents)[i] = 0.1F;
        }

        // ---- (1) TILING: what a (grid, threadgroup) pair ACTUALLY launches ----
        //
        // The rule "threadgroups = ceil(grid / tg) per dimension, the last one partial" is a MODEL of
        // dispatchThreads, and every derived number below rests on it, so it is measured rather than
        // quoted. This kernel records one record per lane THAT EXECUTES; a lane Metal never starts
        // writes nothing, so the record count is exactly the number of lanes the dispatch spends, and
        // `threads_per_threadgroup` says whether the partial threadgroup was shrunk (non-uniform) or
        // padded (uniform).
        NSString* probe_src = @R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void tg_probe(device uint* out [[buffer(0)]],
                     device atomic_uint* cnt [[buffer(1)]],
                     uint2 gid  [[thread_position_in_grid]],
                     uint2 tgid [[threadgroup_position_in_grid]],
                     uint2 lpos [[thread_position_in_threadgroup]],
                     uint2 tgsz [[threads_per_threadgroup]]) {
  const uint slot = atomic_fetch_add_explicit(cnt, 1u, memory_order_relaxed);
  out[slot * 6 + 0] = tgid.x;
  out[slot * 6 + 1] = tgid.y;
  out[slot * 6 + 2] = gid.x;
  out[slot * 6 + 3] = gid.y;
  out[slot * 6 + 4] = lpos.x;
  out[slot * 6 + 5] = tgsz.x;
}
)MSL";
        id<MTLComputePipelineState> p_probe = nil;
        {
          NSError*       perr = nil;
          id<MTLLibrary> plib = [device newLibraryWithSource:probe_src options:nil error:&perr];
          if (plib == nil) {
            std::printf("\n(tiling probe could not be compiled: %s)\n", perr.localizedDescription.UTF8String);
          }
          else {
            id<MTLFunction> pfn = [plib newFunctionWithName:@"tg_probe"];
            p_probe = (pfn != nil) ? [device newComputePipelineStateWithFunction:pfn error:&perr] : nil;
          }
        }

        const auto tile_of = [&](MTLSize grid, MTLSize tg, uint64_t* launched, uint64_t* groups,
                                 uint64_t* max_gid_x, uint64_t* min_tgsz, uint64_t* max_tgsz) {
          *launched = *groups = *max_gid_x = *max_tgsz = 0;
          *min_tgsz = 1u << 30;
          if (p_probe == nil) {
            return;
          }
          *static_cast<uint32_t*>(b_cnt.contents) = 0;
          id<MTLCommandBuffer>         cb  = [q commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_probe];
          [enc setBuffer:b_tg offset:0 atIndex:0];
          [enc setBuffer:b_cnt offset:0 atIndex:1];
          [enc dispatchThreads:grid threadsPerThreadgroup:tg];
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          const uint32_t n = *static_cast<uint32_t*>(b_cnt.contents);
          const uint32_t* r = static_cast<const uint32_t*>(b_tg.contents);
          std::vector<uint64_t> keys;
          keys.reserve(n);
          for (uint32_t i = 0; i != n; ++i) {
            keys.push_back((static_cast<uint64_t>(r[i * 6 + 1]) << 32) | r[i * 6 + 0]);
            *max_gid_x = std::max<uint64_t>(*max_gid_x, r[i * 6 + 2]);
            *min_tgsz  = std::min<uint64_t>(*min_tgsz, r[i * 6 + 5]);
            *max_tgsz  = std::max<uint64_t>(*max_tgsz, r[i * 6 + 5]);
          }
          std::sort(keys.begin(), keys.end());
          *launched = n;
          *groups   = static_cast<uint64_t>(std::unique(keys.begin(), keys.end()) - keys.begin());
        };

        std::printf("\n=== EQ/DEMAP PARALLELISM (the lane's own geometry; p150: 1 eq + 1 demap dispatch "
                    "per hop, 12 symbols each) ===\n");
        std::printf("threadgroup limits: max threads per threadgroup = %lu (per dimension %lu x %lu x %lu)\n",
                    static_cast<unsigned long>(p_eq_b.maxTotalThreadsPerThreadgroup),
                    static_cast<unsigned long>(device.maxThreadsPerThreadgroup.width),
                    static_cast<unsigned long>(device.maxThreadsPerThreadgroup.height),
                    static_cast<unsigned long>(device.maxThreadsPerThreadgroup.depth));
        if (p_probe != nil) {
          std::printf("\n[tiling] what dispatchThreads actually launches (grid, tg -> lanes spent vs "
                      "lanes doing work)\n");
          std::printf("%-18s %-14s %10s %10s %8s %12s %10s %7s %7s\n", "grid", "tg", "launched",
                      "useful", "ratio", "threadgroups", "max_gid.x", "tgsz.lo", "tgsz.hi");
          const std::pair<const char*, MTLSize> grids[] = {
              {"(612,12) 51PRB", MTLSizeMake(612, 12, 1)},
              {"(300,12) 25PRB", MTLSizeMake(300, 12, 1)},
              {"(24,12) 2PRB", MTLSizeMake(24, 12, 1)},
              {"(3300,14) 275PRB", MTLSizeMake(3300, 14, 1)},
              {"(7344,1) flat", MTLSizeMake(7344, 1, 1)},
              {"(3300,1) one row", MTLSizeMake(3300, 1, 1)},
          };
          for (const auto& gsel : grids) {
            const MTLSize tgs[] = {MTLSizeMake(256, 1, 1), MTLSizeMake(128, 1, 1), MTLSizeMake(64, 1, 1)};
            for (const MTLSize& tg : tgs) {
              uint64_t launched = 0, groups = 0, mgx = 0, lo = 0, hi = 0;
              tile_of(gsel.second, tg, &launched, &groups, &mgx, &lo, &hi);
              const uint64_t useful = static_cast<uint64_t>(gsel.second.width) * gsel.second.height;
              std::printf("%-18s (%3lu,1,1) %10llu %10llu %8.3f %12llu %10llu %7llu %7llu\n",
                          gsel.first,
                          static_cast<unsigned long>(tg.width),
                          static_cast<unsigned long long>(launched),
                          static_cast<unsigned long long>(useful),
                          static_cast<double>(launched) / static_cast<double>(useful),
                          static_cast<unsigned long long>(groups),
                          static_cast<unsigned long long>(mgx),
                          static_cast<unsigned long long>(lo),
                          static_cast<unsigned long long>(hi));
            }
          }
        }

        // ---- (2) THE LANE'S REAL SHAPE, and the same work as one 1-D row ----
        //
        // The 1-D arm is the SAME kernel body (L=1/P=1 => the single-layer path) over the SAME bytes: the
        // batch kernel's per-symbol strides make its addresses contiguous, so a 1-D arm with
        // nof_re = nof_re*n_sym reads h, y, eq and nv at exactly the offsets the 2-D arm does. The only
        // difference between the two rows is how the grid is cut into threadgroups.
        const auto eq_2d = [&](uint32_t re, uint32_t n_sym, MTLSize tg) -> double {
          equalize_params ep = make_eq_params(re, 1, 1);
          equalize_strides_h st{};
          st.nof_symbols = n_sym;
          st.h_stride    = re;
          st.eq_stride   = re;
          st.nv_stride   = re;
          for (uint32_t k = 0; k != n_sym; ++k) {
            st.h_starts[k] = k * re;
            st.y_starts[k] = k * re;
          }
          return time_it_grid(p_eq_b, MTLSizeMake(re, n_sym, 1), tg, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_hp offset:0 atIndex:0];
            [e setBuffer:b_yp offset:0 atIndex:1];
            [e setBuffer:b_eqp offset:0 atIndex:2];
            [e setBuffer:b_nvp offset:0 atIndex:3];
            [e setBytes:&ep length:sizeof(ep) atIndex:4];
            [e setBuffer:b_nvp offset:0 atIndex:5];
            [e setBytes:&st length:sizeof(st) atIndex:6];
          });
        };
        const auto dm_2d = [&](uint32_t re, uint32_t n_sym, MTLSize tg) -> double {
          demod_params dp{};
          dp.nof_symbols = n_sym;
          dp.nof_re      = re;
          dp.mod         = 0; // QPSK
          dp.sym_stride  = re;
          dp.nv_stride   = re;
          dp.llr_stride  = 2 * re;
          return time_it_grid(p_dm, MTLSizeMake(re, n_sym, 1), tg, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_eqp offset:0 atIndex:0];
            [e setBuffer:b_nvp offset:0 atIndex:1];
            [e setBuffer:b_llp offset:0 atIndex:2];
            [e setBytes:&dp length:sizeof(dp) atIndex:3];
          });
        };
        const auto eq_1d = [&](uint32_t threads) -> double {
          equalize_params ep = make_eq_params(threads, 1, 1);
          return time_it_threads(p_eq_1d, threads, 256, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_hp offset:0 atIndex:0];
            [e setBuffer:b_yp offset:0 atIndex:1];
            [e setBuffer:b_eqp offset:0 atIndex:2];
            [e setBuffer:b_nvp offset:0 atIndex:3];
            [e setBytes:&ep length:sizeof(ep) atIndex:4];
            [e setBuffer:b_nvp offset:0 atIndex:5];
          });
        };
        const auto dm_1d = [&](uint32_t threads) -> double {
          demod_params dp{};
          dp.nof_symbols = 1;
          dp.nof_re      = threads;
          dp.mod         = 0;
          dp.sym_stride  = 1;
          dp.nv_stride   = 1;
          dp.llr_stride  = 2 * threads;
          return time_it_threads(p_dm, threads, 256, ^(id<MTLComputeCommandEncoder> e) {
            [e setBuffer:b_eqp offset:0 atIndex:0];
            [e setBuffer:b_nvp offset:0 atIndex:1];
            [e setBuffer:b_llp offset:0 atIndex:2];
            [e setBytes:&dp length:sizeof(dp) atIndex:3];
          });
        };

        std::printf("\n[lane shape] one hop = 12 data OFDM symbols of one grant; grid (12*prb, 12) at "
                    "tg 256, vs the same work as one 1-D row\n");
        std::printf("%-16s %8s %10s %12s %12s %12s %12s %10s\n", "grant", "nof_re", "threads",
                    "eq 2-D us", "eq 1-D us", "dm 2-D us", "dm 1-D us", "us/hop");
        for (uint32_t prb : {2u, 25u, 51u, 100u, 275u}) {
          const uint32_t re  = 12u * prb;
          const uint32_t nth = re * 12u;
          const double   e2  = eq_2d(re, 12, MTLSizeMake(256, 1, 1));
          const double   e1  = eq_1d(nth);
          const double   d2  = dm_2d(re, 12, MTLSizeMake(256, 1, 1));
          const double   d1  = dm_1d(nth);
          std::printf("%-16u %8u %10u %12.3f %12.3f %12.3f %12.3f %10.3f\n", prb, re, nth, e2, e1, d2, d1,
                      e2 + d2);
        }

        std::printf("\n[threadgroup width] 51 PRB (612 x 12) at every tg width; the row is the same "
                    "dispatch and the same work\n");
        std::printf("%-16s %14s %14s %14s\n", "tg width", "eq_batch us", "demod us", "sum us");
        for (NSUInteger w : {32ul, 64ul, 128ul, 256ul, 512ul, 1024ul}) {
          const MTLSize tg = MTLSizeMake(w, 1, 1);
          const double  e  = eq_2d(612, 12, tg);
          const double  d  = dm_2d(612, 12, tg);
          std::printf("%-16lu %14.3f %14.3f %14.3f\n", static_cast<unsigned long>(w), e, d, e + d);
        }

        // ---- (3) SATURATION: where does a dispatch stop being launch-bound? ----
        //
        // The slope is the answer: a flat row means the device has spare lanes (the dispatch price is
        // the launch, not the work) and a linear row means the lanes are the limit. Read the knee - the
        // thread count where the slope turns - as "the device is full".
        std::printf("\n[saturation] 1-D grids of the real kernels, QPSK / 1 layer / 1 port; "
                    "ns/thread is the marginal price of one more lane\n");
        std::printf("%12s %12s %12s %14s %12s %14s\n", "threads", "eq us", "eq ns/thr", "dm us",
                    "dm ns/thr", "note");
        for (uint32_t nth : {1u, 32u, 612u, 2448u, 7344u, 12240u, 22032u, 39168u, 46200u}) {
          const double e  = eq_1d(nth);
          const double d  = dm_1d(nth);
          const char*  nt = "";
          if (nth == 612u) {
            nt = "51 PRB x 1 sym";
          }
          else if (nth == 7344u) {
            nt = "51 PRB x 12 sym";
          }
          else if (nth == 46200u) {
            nt = "275 PRB x 14 sym";
          }
          std::printf("%12u %12.3f %12.4f %14.3f %12.4f %14s\n", nth, e, e * 1000.0 / nth, d,
                      d * 1000.0 / nth, nt);
        }
      }
    }

    // ---- how the two block-sized kernels scale with the block -------------------------------------
    std::printf("\nblock-size sweep (same kernels, block_prb = 1 / 2 / 3, npt = %u):\n", g.npt);
    std::printf("%-34s %12s %14s\n", "kernel", "L / nout", "GPU us/dispatch");
    for (uint32_t prb : {1u, 2u, 3u}) {
      geometry        gs{};
      gs.block_prb = prb;
      corr_params     c2 = cp;
      c2.npf            = gs.npf();
      c2.nf             = gs.nf();
      c2.L              = gs.L();
      c2.Ls             = gs.L();
      c2.a_sys          = gs.L() * gs.L();
      c2.r_sys          = gs.nout() * gs.L();
      apply_params    a2{gs.nout(), gs.L(), gs.systems(), gs.blocks};
      const double    corr_a = time_it(p_corr_a, static_cast<NSUInteger>(gs.L()) * gs.L(), gs.systems(),
                                       ^(id<MTLComputeCommandEncoder> e) {
                                         [e setBuffer:b_a offset:0 atIndex:0];
                                         [e setBytes:&c2 length:sizeof(c2) atIndex:1];
                                         [e setBuffer:b_sc offset:0 atIndex:2];
                                       });
      const double corr_rhp = time_it(p_corr_rhp, static_cast<NSUInteger>(gs.nout()) * gs.L(), gs.systems(),
                                      ^(id<MTLComputeCommandEncoder> e) {
                                        [e setBuffer:b_rhp offset:0 atIndex:0];
                                        [e setBytes:&c2 length:sizeof(c2) atIndex:1];
                                      });
      const double apply = time_it(p_apply, gs.blocks * gs.systems(), gs.nout(),
                                   ^(id<MTLComputeCommandEncoder> e) {
                                     [e setBuffer:b_w offset:0 atIndex:0];
                                     [e setBuffer:b_y offset:0 atIndex:1];
                                     [e setBuffer:b_h offset:0 atIndex:2];
                                     [e setBytes:&a2 length:sizeof(a2) atIndex:3];
                                   });
      std::printf("  block_prb=%u  L=%-4u nout=%-5u   corr_a=%7.3f  corr_rhp=%7.3f  apply=%7.3f us\n",
                  prb, gs.L(), gs.nout(), corr_a, corr_rhp, apply);
    }

    // ==============================================================================================
    // A1 + A2 (dev doc 6.173): WHAT A DISPATCH COSTS ON THE GPU SIDE, and how much of it is the
    // DEPENDENCY between dispatches. The question the orchestration line turns on.
    //
    // The 1.4us "floor" this file has been quoting is measured from the CPU: `dispatchThreadgroups` only
    // writes a record into the command buffer, and the wall clock the CPU then sees is the whole round
    // trip. It cannot separate "the host wrote a record" from "the GPU started a grid, resolved its
    // dependencies and bound its resources" - and the front end's own numbers say the second part is
    // where the money is: 14 threadgroups in ONE dispatch cost 443us on air against 47us off-line, i.e.
    // +30.5us per threadgroup, while a dispatch with a dozen fewer threadgroups costs almost nothing.
    //
    // A1 - THE HOST'S OWN ENCODE. `time_encode` (below, same shape as the one this file already has)
    // encodes N dispatches and times ONLY the CPU, with no commit and no wait inside the timer. If this
    // is sub-microsecond per dispatch, then the 5-14us a leg saves per removed dispatch is NOT the
    // host's, and the whole of it is on the device side.
    //
    // A2 - THE DEVICE SIDE, SPLIT INTO "INDEPENDENT" AND "CHAINED". The same kernel (mmse_corr_a, whose
    // grid is L x L and whose cost is a few microseconds of real work) is dispatched N times in ONE
    // command buffer, in two layouts:
    //   * INDEPENDENT - each dispatch writes its own slice of the destination, so the GPU may run them
    //                   concurrently: the slope is what N dispatches cost when nothing orders them;
    //   * CHAINED     - every dispatch writes the SAME slice, so each one reads what the previous one
    //                   wrote: the slope is what N dispatches cost when each must wait for the last.
    // The DIFFERENCE of the two slopes is the price of one link in a dependency chain - the number the
    // merge (6 dispatches -> 1) is trying to buy, and the one nothing in this project has ever measured.
    // ==============================================================================================
    if (p_corr_a != nil) {
      const geometry   g2{};
      const NSUInteger a_elems = static_cast<NSUInteger>(g2.L()) * g2.L();
      corr_params      c3{};
      c3.nof_systems        = 1;
      c3.npt                = g2.npt;
      c3.npf                = g2.npf();
      c3.ncomb              = g2.ncomb;
      c3.nf                 = g2.nf();
      c3.L                  = g2.L();
      c3.Ls                 = g2.L();
      c3.a_sys              = g2.L() * g2.L();
      c3.r_sys              = g2.nout() * g2.L();
      c3.ts                 = 1.0F / (15e3F * 14.0F);
      c3.scs_hz             = 15e3F;
      c3.fd_hz              = 300.0F;
      c3.tau_rms_s          = 370e-9F;
      c3.sigma2             = 0.01F;
      c3.ridge              = 1e-6F;
      c3.sigma2_from_device = 0;
      c3.sigma2_slot        = 0;

      // A2 (dev doc 6.173): N dispatches of the SAME kernel in ONE command buffer, two layouts that
      // differ ONLY in where they write - which is what decides whether the hardware may overlap them.
      //
      // The first version of this arm got both layouts wrong and produced a reading that could not be
      // true (a flat 67us for N = 1..16, i.e. a "per dispatch" cost that did not scale with N at all,
      // and a chained figure BELOW it). The lesson is worth keeping: an arm whose two layouts differ in
      // more than the variable under test measures the difference of its own mistakes.
      //
      //   * TOGETHER  - `a_sys` steps each dispatch to its own slice, so nothing orders them and the
      //                 hardware may run them as concurrently as the device allows;
      //   * SAME SLOT - every dispatch writes the FIRST slice, so each one's store races the previous
      //                 one's: the serialization a dependency chain imposes, without needing a real
      //                 read-after-write to exist.
      //
      // The slopes (not the totals) are the reading: us per dispatch when the device is free to overlap,
      // against us per dispatch when it is not.
      // `dispatches` = how many dispatchThreadgroups calls carry the work; the threadgroup COUNT is n
      // either way. That is the axis this arm exists for: same work, same threadgroups, and the only
      // difference is how many DISPATCH BOUNDARIES sit between them.
      const auto run_layout = [&](uint32_t n, uint32_t dispatches, bool same_slot = false) -> double {
        corr_params c = c3;
        c.nof_systems = 1;
        c.a_sys       = same_slot ? 0u : static_cast<uint32_t>(a_elems);
        const uint32_t per = (n + dispatches - 1u) / dispatches;
        double         best = 1e30;
        for (unsigned round = 0; round != 7; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_corr_a];
          [enc setBuffer:b_a offset:0 atIndex:0];
          [enc setBytes:&c length:sizeof(c) atIndex:1];
          uint32_t done = 0;
          for (uint32_t d = 0; d != dispatches; ++d) {
            const uint32_t take = std::min(per, n - done);
            if (take == 0u) {
              break;
            }
            [enc dispatchThreadgroups:MTLSizeMake(1, take, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            done += take;
          }
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          best = std::min(best, (cb.GPUEndTime - cb.GPUStartTime) * 1e6);
        }
        return best;
      };

      // A3 (dev doc 6.174): the SAME work, the same threadgroups, only the number of DISPATCH
      // BOUNDARIES between them changes. The legs say a dispatch boundary is expensive (p72/p73/p74:
      // 468.7 -> 505.8 -> 703.4 us of merged_hop as the front end's 14 transforms went from 1 dispatch
      // to 2 to 7, i.e. ~33-39 us per added boundary, with the threadgroup count unchanged); this file
      // says a dispatch costs ~0.07 us to encode and ~3.6 us to run when it is alone. If the legs are
      // right about the boundary, this arm has to reproduce a slope that the dispatch COUNT, not the
      // work, explains.
      std::printf("\n[A3] same 16 threadgroups of mmse_corr_a (L=%u), split into 1 / 2 / 4 / 8 / 16 dispatches.\n"
                  "     'own' = each dispatch writes its own slice (free to overlap); 'shared' = every\n"
                  "     dispatch writes the FIRST slice, so their stores serialize on one address:\n",
                  g2.L());
      std::printf("     dispatches   own(us)   shared(us)   own/disp   shared/disp\n");
      double o1 = 0.0;
      double s1 = 0.0;
      double o16 = 0.0;
      double s16 = 0.0;
      for (uint32_t d : {1u, 2u, 4u, 8u, 16u}) {
        const double own    = run_layout(16u, d, false);
        const double shared = run_layout(16u, d, true);
        if (d == 1u) {
          o1 = own;
          s1 = shared;
        }
        o16 = own;
        s16 = shared;
        std::printf("  %8u   %8.3f   %9.3f     %8.3f    %8.3f\n",
                    d,
                    own,
                    shared,
                    own / static_cast<double>(d),
                    shared / static_cast<double>(d));
      }
      std::printf("[A3] slope over 1->16 dispatches: own=%.3f us per dispatch boundary, shared=%.3f us "
                  "-> the SERIALIZATION across boundaries costs %.3f us per boundary on the device\n",
                  (o16 - o1) / 15.0,
                  (s16 - s1) / 15.0,
                  ((s16 - s1) - (o16 - o1)) / 15.0);

      // A1: the host's own encode, with NO commit and NO wait inside the timer.
      const auto host_encode = [&](uint32_t n) -> double {
        double best = 1e30;
        for (unsigned round = 0; round != 7; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          const double t0 = CFAbsoluteTimeGetCurrent();
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_corr_a];
          [enc setBuffer:b_a offset:0 atIndex:0];
          [enc setBytes:&c3 length:sizeof(c3) atIndex:1];
          for (uint32_t r = 0; r != n; ++r) {
            [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
          }
          [enc endEncoding];
          const double t1 = CFAbsoluteTimeGetCurrent();
          [cb commit];
          [cb waitUntilCompleted];
          best = std::min(best, (t1 - t0) * 1e6);
        }
        return best;
      };
      const double enc_1  = host_encode(1u);
      const double enc_16 = host_encode(16u);
      std::printf("[A1] host encode only (no commit/wait inside the timer): 1=%.3f us, 16=%.3f us -> "
                  "%.3f us/dispatch on the HOST\n",
                  enc_1,
                  enc_16,
                  (enc_16 - enc_1) / 15.0);
    }

    // THE PRICE OF A DISPATCH, MEASURED SO THAT NOTHING ELSE MOVES (dev doc 6.177).
    //
    // WHY THE FIRST TWO ATTEMPTS FAILED, because the design they got wrong is easy to get wrong again:
    //   * version 1 compared mmse_corr_a_rhp against corr_a + corr_r_hp - TWO kernels, so the difference
    //     carried the merged kernel's own extra index arithmetic and (as it turned out) 190 threadgroups
    //     of padding that the grid handed to A's blocks because it sized every block by the WIDER matrix.
    //     It measured "my merged kernel is heavier", not "a boundary costs X".
    //   * version 2 split ONE kernel's elements into 1/2/4 dispatches but let every dispatch cover the
    //     FULL grid, so the 2- and 4-dispatch arms did two and four times the work. (k2-k1) was therefore
    //     "one boundary plus one whole extra copy of the work", and the 1.9us it reported agreeing with
    //     6.173's 1.6us was a coincidence.
    //
    // THE CLEAN DESIGN: the SAME kernel (mmse_corr_a), the SAME total work (every system built exactly
    // once), the SAME total threadgroup count - only HOW MANY DISPATCHES carry it changes. Each dispatch
    // gets its own slice of the systems, so `work x dispatches` is constant by construction:
    //     1 dispatch  -> grid (tgs, 2N, 1)
    //     2 dispatches -> grid (tgs, N, 1) each
    //     4 dispatches -> grid (tgs, N/2, 1) each
    // What is left in the difference is the boundary itself: the driver call, the command-buffer
    // bookkeeping, the scheduling decision and the threadgroup launch of one more dispatch - the part of
    // the cost that lives inside macOS and the GPU driver, where this project has no instrument at all.
    if (p_corr_a != nil) {
      const geometry   g6{};
      const NSUInteger a6 = static_cast<NSUInteger>(g6.L()) * g6.L();
      corr_params      c6{};
      c6.nof_systems        = 1; // set per arm
      c6.npt                = g6.npt;
      c6.npf                = g6.npf();
      c6.ncomb              = g6.ncomb;
      c6.nf                 = g6.nf();
      c6.L                  = g6.L();
      c6.Ls                 = g6.L();
      c6.a_sys              = g6.L() * g6.L();
      c6.r_sys              = g6.nout() * g6.L();
      c6.ts                 = 1.0F / (15e3F * 14.0F);
      c6.scs_hz             = 15e3F;
      c6.fd_hz              = 300.0F;
      c6.tau_rms_s          = 370e-9F;
      c6.sigma2             = 0.01F;
      c6.ridge              = 1e-6F;
      c6.sigma2_from_device = 0;
      c6.sigma2_slot        = 0;
      c6.dmrs_slots[0]      = 2;
      c6.dmrs_slots[1]      = 7;
      c6.dmrs_slots[2]      = 11;

      const uint32_t total_sys = 8u;             // constant work: 8 systems of A
      const NSUInteger tgs6    = (a6 + 255u) / 256u;

      const auto time_split = [&](uint32_t dispatches) -> double {
        const uint32_t per = total_sys / dispatches; // exact: 8/1, 8/2, 8/4, 8/8
        std::vector<double> v;
        for (unsigned round = 0; round != 21; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          for (uint32_t d = 0; d != dispatches; ++d) {
            c6.nof_systems = per;
            c6.a_sys       = static_cast<uint32_t>(a6);
            [enc setComputePipelineState:p_corr_a];
            // The slice starts at system d*per: the same buffer, offset so each dispatch writes its own
            // systems and NOTHING is written twice.
            [enc setBuffer:b_a offset:static_cast<NSUInteger>(d) * per * a6 * sizeof(float) atIndex:0];
            [enc setBytes:&c6 length:sizeof(c6) atIndex:1];
            [enc dispatchThreadgroups:MTLSizeMake(tgs6, per, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
          }
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          v.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
      };

      const double t1 = time_split(1u);
      const double t2 = time_split(2u);
      const double t4 = time_split(4u);
      const double t8 = time_split(8u);
      std::printf("\n[DISPATCH-PRICE] mmse_corr_a, the SAME 8 systems of A and the SAME %lu threadgroups\n"
                  "     per dispatch, split into 1/2/4/8 dispatches (median of 21 runs):\n",
                  static_cast<unsigned long>(tgs6 * total_sys));
      std::printf("     dispatches:   1       2       4       8\n");
      std::printf("     window us: %7.3f %7.3f %7.3f %7.3f\n", t1, t2, t4, t8);
      std::printf("     per extra dispatch: %.3f us (1->2), %.3f us (2->4), %.3f us (4->8)\n",
                  t2 - t1,
                  (t4 - t2) / 2.0,
                  (t8 - t4) / 4.0);
      std::printf("     => ONE DISPATCH BOUNDARY costs %.3f us at this geometry (linear fit over 1..8)\n",
                  (t8 - t1) / 7.0);
    }

    // O1, THE HONEST COMPARISON (dev doc 6.178): the merged kernel against the two it replaces, at the
    // SAME geometry and with the SAME buffers, so the only thing that differs is the kernel and how many
    // dispatches carry the work. Run AFTER the padding guard was added to the merged kernel: the first
    // version handed A's blocks 107 threadgroups each instead of 12, and that - not the merge - is what
    // made it look slower.
    if (p_corr_merged != nil && p_corr_a != nil && p_corr_rhp != nil) {
      const geometry   g7{};
      const NSUInteger a7 = static_cast<NSUInteger>(g7.L()) * g7.L();
      const NSUInteger r7 = static_cast<NSUInteger>(g7.nout()) * g7.L();
      corr_params      c7{};
      c7.nof_systems        = g7.systems();
      c7.npt                = g7.npt;
      c7.npf                = g7.npf();
      c7.ncomb              = g7.ncomb;
      c7.nf                 = g7.nf();
      c7.L                  = g7.L();
      c7.Ls                 = g7.L();
      c7.a_sys              = g7.L() * g7.L();
      c7.r_sys              = g7.nout() * g7.L();
      c7.ts                 = 1.0F / (15e3F * 14.0F);
      c7.scs_hz             = 15e3F;
      c7.fd_hz              = 300.0F;
      c7.tau_rms_s          = 370e-9F;
      c7.sigma2             = 0.01F;
      c7.ridge              = 1e-6F;
      c7.sigma2_from_device = 0;
      c7.sigma2_slot        = 0;
      c7.dmrs_slots[0]      = 2;
      c7.dmrs_slots[1]      = 7;
      c7.dmrs_slots[2]      = 11;

      const auto median_run = [&](void (^body)(id<MTLComputeCommandEncoder>)) -> double {
        std::vector<double> v;
        for (unsigned round = 0; round != 21; ++round) {
          id<MTLCommandBuffer> cb = [q commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          body(enc);
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          v.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
        }
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
      };

      // (a) the delivery route: two kernels, two dispatches, one boundary between them.
      const double two = median_run(^(id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:p_corr_a];
        [e setBuffer:b_a offset:0 atIndex:0];
        [e setBytes:&c7 length:sizeof(c7) atIndex:1];
        [e dispatchThreads:MTLSizeMake(a7, c7.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [e setComputePipelineState:p_corr_rhp];
        [e setBuffer:b_rhp offset:0 atIndex:0];
        [e setBytes:&c7 length:sizeof(c7) atIndex:1];
        [e dispatchThreads:MTLSizeMake(r7, c7.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      });
      // (b) the merged kernel: the same work, one dispatch.
      const NSUInteger wide7 = (a7 > r7) ? a7 : r7;
      const NSUInteger tgs7  = (wide7 + 255u) / 256u;
      const double     one   = median_run(^(id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:p_corr_merged];
        [e setBuffer:b_a offset:0 atIndex:0];
        [e setBuffer:b_rhp offset:0 atIndex:1];
        [e setBytes:&c7 length:sizeof(c7) atIndex:2];
        // ONE dimensional grid of threadgroups, as the engine dispatches it: with a 1-D threadgroup,
        // `dispatchThreads` would flatten gid.x over the WHOLE grid and each 256-thread threadgroup would
        // only ever work on the first 256 grid positions - measured: 27.5% of the launched threads did
        // any work, and that - not the merge - is why the merged kernel looked 4us slower.
        [e dispatchThreadgroups:MTLSizeMake(tgs7 * 2u * c7.nof_systems, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      });
      // (c) A alone and (d) R_hp alone, so the merged figure can be read against the parts.
      const double only_a = median_run(^(id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:p_corr_a];
        [e setBuffer:b_a offset:0 atIndex:0];
        [e setBytes:&c7 length:sizeof(c7) atIndex:1];
        [e dispatchThreads:MTLSizeMake(a7, c7.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      });
      const double only_r = median_run(^(id<MTLComputeCommandEncoder> e) {
        [e setComputePipelineState:p_corr_rhp];
        [e setBuffer:b_rhp offset:0 atIndex:0];
        [e setBytes:&c7 length:sizeof(c7) atIndex:1];
        [e dispatchThreads:MTLSizeMake(r7, c7.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      });
      std::printf("\n[O1-honest] same geometry, same buffers, median of 21 runs:\n");
      std::printf("     A alone                    : %7.3f us\n", only_a);
      std::printf("     R_hp alone                 : %7.3f us\n", only_r);
      std::printf("     A + R_hp (two dispatches)  : %7.3f us\n", two);
      std::printf("     merged (one dispatch)      : %7.3f us\n", one);
      std::printf("     => merged %s the split route by %.3f us (boundary measured at ~1.69 us)\n",
                  (one < two) ? "BEATS" : "LOSES TO",
                  (one < two) ? (two - one) : (one - two));
    }

    // WHY THE MERGED KERNEL IS SLOWER - the occupancy hypothesis (dev doc 6.178).
    //
    // After the padding guard and the threadgroup grid were fixed the merged kernel is still ~3.7us slower
    // than the two it replaces, while launching FEWER threadgroups of a slightly worse fill. That leaves
    // the kernel's own parallelism: two code paths and more index arithmetic in one kernel can raise the
    // register footprint, and a lower occupancy makes threadgroups overlap less - each one then costs more.
    //
    // THAT is measurable without Xcode: the cost PER THREADGROUP is the slope of the window against the
    // number of threadgroups launched. Two kernels doing the same per-threadgroup work, launched the same
    // way, with different slopes means different occupancy. `maxTotalThreadsPerThreadgroup` is printed too
    // (it is the pipeline's own limit, not the achieved occupancy, but it is a free reading).
    if (p_corr_a != nil && p_corr_merged != nil) {
      const geometry   g8{};
      const NSUInteger a8 = static_cast<NSUInteger>(g8.L()) * g8.L();
      const NSUInteger r8 = static_cast<NSUInteger>(g8.nout()) * g8.L();
      corr_params      c8{};
      c8.npf                = g8.npf();
      c8.ncomb              = g8.ncomb;
      c8.nf                 = g8.nf();
      c8.L                  = g8.L();
      c8.Ls                 = g8.L();
      c8.a_sys              = g8.L() * g8.L();
      c8.r_sys              = g8.nout() * g8.L();
      c8.ts                 = 1.0F / (15e3F * 14.0F);
      c8.scs_hz             = 15e3F;
      c8.fd_hz              = 300.0F;
      c8.tau_rms_s          = 370e-9F;
      c8.sigma2             = 0.01F;
      c8.ridge              = 1e-6F;
      c8.sigma2_from_device = 0;
      c8.sigma2_slot        = 0;
      c8.dmrs_slots[0]      = 2;
      c8.dmrs_slots[1]      = 7;
      c8.dmrs_slots[2]      = 11;
      const NSUInteger tgs_a8 = (a8 + 255u) / 256u;

      const auto slope_of = [&](bool merged) -> std::pair<double, double> {
        // (slope per threadgroup, intercept) from launches of 1x / 2x / 4x the same 8 systems.
        // CAP THE SYSTEMS. 32 systems of R_hp need 32 * 27216 * 4B = 3.48 MB, and the split arm writes
        // A into the same 4 MB buffer as well - the first version of this arm ran past the end and its
        // "base" reading (45 us) was the cost of the fault, not of the kernel. 4x is 1.4 MB of writes in
        // total, which fits with room to spare.
        double t[3];
        uint32_t mults[3] = {1u, 2u, 4u};
        for (unsigned k = 0; k != 3; ++k) {
          std::vector<double> v;
          for (unsigned round = 0; round != 15; ++round) {
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            if (merged) {
              c8.nof_systems = 1u; // set below
              [enc setComputePipelineState:p_corr_merged];
            } else {
              [enc setComputePipelineState:p_corr_a];
            }
            const uint32_t systems = 4u * mults[k]; // 4 / 8 / 16 systems: 1.74 MB of R_hp at most
            c8.nof_systems         = merged ? systems : 1u;
            if (merged) {
              [enc setBuffer:b_a offset:0 atIndex:0];
              [enc setBuffer:b_rhp offset:0 atIndex:1];
              [enc setBytes:&c8 length:sizeof(c8) atIndex:2];
              [enc dispatchThreadgroups:MTLSizeMake((r8 + 255u) / 256u * 2u * systems, 1, 1)
                  threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            } else {
              c8.nof_systems = systems;
              [enc setBuffer:b_a offset:0 atIndex:0];
              [enc setBytes:&c8 length:sizeof(c8) atIndex:1];
              [enc dispatchThreadgroups:MTLSizeMake(tgs_a8, systems, 1)
                  threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            v.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
          }
          std::sort(v.begin(), v.end());
          t[k] = v[v.size() / 2];
        }
        // slope from the 1x -> 4x endpoints, in us per launched threadgroup
        const double tg1 = static_cast<double>(merged ? ((r8 + 255u) / 256u * 2u * 4u) : (tgs_a8 * 4u));
        const double tg4 = static_cast<double>(merged ? ((r8 + 255u) / 256u * 2u * 16u) : (tgs_a8 * 16u));
        return {(t[2] - t[0]) / (tg4 - tg1), t[0]};
      };

      const auto [slope_a, base_a] = slope_of(false);
      const auto [slope_m, base_m] = slope_of(true);
      std::printf("\n[OCCUPANCY] cost per launched threadgroup (slope over 8/16/32 systems):\n");
      std::printf("     mmse_corr_a      : %.4f us/threadgroup   (base %.3f us)\n", slope_a, base_a);
      std::printf("     mmse_corr_a_rhp  : %.4f us/threadgroup   (base %.3f us)\n", slope_m, base_m);
      std::printf("     => the merged kernel's threadgroups cost %.2fx as much%s\n",
                  (slope_a > 0.0) ? (slope_m / slope_a) : 0.0,
                  (slope_m > slope_a * 1.3) ? "  <- OCCUPANCY HYPOTHESIS SUPPORTED"
                                            : "  <- occupancy is NOT the explanation");
      std::printf("     pipeline limits: corr_a maxThreads=%lu, corr_a_rhp maxThreads=%lu\n",
                  static_cast<unsigned long>(p_corr_a.maxTotalThreadsPerThreadgroup),
                  static_cast<unsigned long>(p_corr_merged.maxTotalThreadsPerThreadgroup));
    }

    // CAN TWO BUFFERS OF ONE QUEUE RUN AT THE SAME TIME? (dev doc 6.184)
    //
    // The question that decides whether "fewer commits" is worth anything. `merged_hop` is committed while
    // another buffer is running (Q9-F6, leg p150: idle-commit 0%), and its wait is ~210us. If the device
    // could start it on the cores the running buffer is NOT using, that wait would be spent doing this
    // hop's work instead - and collapsing submissions would buy nothing. If it cannot, the wait is a hard
    // serialization and the queue is a queue.
    //
    // HOW: the SAME kernel, the SAME total work, two layouts.
    //   * SERIAL  - commit A, wait for it, commit B, wait for it. The floor: two executions back to back.
    //   * TOGETHER- commit A and B, then wait for both. If the device overlaps them, this is FASTER than
    //               serial. If the second only starts when the first ends, it is the SAME - and that
    //               equality is the answer.
    // The work per buffer is deliberately much larger than the device can retire at once (thousands of
    // threadgroups), so "the first does not fill the machine" cannot be the reason they overlap.
    if (p_corr_a != nil) {
      const geometry   g9{};
      const NSUInteger a9 = static_cast<NSUInteger>(g9.L()) * g9.L();
      corr_params      c9{};
      c9.npt                = g9.npt;
      c9.npf                = g9.npf();
      c9.ncomb              = g9.ncomb;
      c9.nf                 = g9.nf();
      c9.L                  = g9.L();
      c9.Ls                 = g9.L();
      c9.a_sys              = g9.L() * g9.L();
      c9.r_sys              = g9.nout() * g9.L();
      c9.ts                 = 1.0F / (15e3F * 14.0F);
      c9.scs_hz             = 15e3F;
      c9.fd_hz              = 300.0F;
      c9.tau_rms_s          = 370e-9F;
      c9.sigma2             = 0.01F;
      c9.ridge              = 1e-6F;
      c9.sigma2_from_device = 0;
      c9.sigma2_slot        = 0;
      c9.dmrs_slots[0]      = 2;
      c9.dmrs_slots[1]      = 7;
      c9.dmrs_slots[2]      = 11;
      const uint32_t  sys9  = 64u; // 64 systems of A: thousands of threadgroups, far more than one pass
      const NSUInteger tgs9 = (a9 + 255u) / 256u;
      c9.nof_systems        = sys9;

      const auto issue = [&](id<MTLCommandBuffer> cb, NSUInteger offset) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p_corr_a];
        [enc setBuffer:b_a offset:offset atIndex:0];
        [enc setBytes:&c9 length:sizeof(c9) atIndex:1];
        [enc dispatchThreadgroups:MTLSizeMake(tgs9, sys9, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [enc endEncoding];
      };

      double best_serial = 1e30;
      double best_toget  = 1e30;
      for (unsigned round = 0; round != 11; ++round) {
        const double t0 = CFAbsoluteTimeGetCurrent();
        {
          id<MTLCommandBuffer> a = [q commandBuffer];
          issue(a, 0);
          [a commit];
          [a waitUntilCompleted];
          id<MTLCommandBuffer> b = [q commandBuffer];
          issue(b, static_cast<NSUInteger>(sys9) * a9 * sizeof(float));
          [b commit];
          [b waitUntilCompleted];
        }
        best_serial = std::min(best_serial, (CFAbsoluteTimeGetCurrent() - t0) * 1e6);

        const double t1 = CFAbsoluteTimeGetCurrent();
        {
          id<MTLCommandBuffer> a = [q commandBuffer];
          id<MTLCommandBuffer> b = [q commandBuffer];
          issue(a, 0);
          issue(b, static_cast<NSUInteger>(sys9) * a9 * sizeof(float));
          [a commit];
          [b commit];
          [a waitUntilCompleted];
          [b waitUntilCompleted];
        }
        best_toget = std::min(best_toget, (CFAbsoluteTimeGetCurrent() - t1) * 1e6);
      }
      std::printf("\n[TWO-QUEUED] the SAME work as two buffers, serial vs both committed then waited "
                  "(64 systems of \n     mmse_corr_a each = %lu threadgroups; median-of-11 minimum):\n",
                  static_cast<unsigned long>(tgs9 * sys9));
      std::printf("     serial   : %9.1f us\n", best_serial);
      std::printf("     together : %9.1f us\n", best_toget);
      std::printf("     => the device %s the two buffers (together/serial = %.3f)%s\n",
                  (best_toget < best_serial * 0.9) ? "OVERLAPS" : "SERIALIZES",
                  best_toget / best_serial,
                  (best_toget < best_serial * 0.9)
                      ? "  <- a queued buffer starts on the cores the running one is not using"
                      : "  <- a queued buffer does NOT start until the running one ends");
    }

    // HOW BIG MUST A GRID BE BEFORE THE DEVICE STOPS OVERLAPPING? (dev doc 6.185)
    //
    // The previous arm used 768 threadgroups per buffer and the device overlapped two such buffers at ratio
    // 0.526. But a hop's dispatches range from a dozen threadgroups (corr_a) to hundreds (the front end's
    // 14 transforms), and the sweep question is whether the overlap is a property of the DEVICE or of the
    // grid being small enough to leave room. It matters because "the two lane chains did not overlap" (p76
    // vs p72: same merged_hop exec at concurrency 1 and 2, but wait 54.4 vs 206.5) is only explained if a
    // large grid stops the second buffer from starting.
    //
    // Sweep the number of systems per buffer from 1 to 256 (12 to 3072 threadgroups) and report
    // together/serial. 0.5 = fully overlapped, 1.0 = fully serialized.
    if (p_corr_a != nil) {
      const geometry   gA{};
      const NSUInteger aA = static_cast<NSUInteger>(gA.L()) * gA.L();
      corr_params      cA{};
      cA.npt                = gA.npt;
      cA.npf                = gA.npf();
      cA.ncomb              = gA.ncomb;
      cA.nf                 = gA.nf();
      cA.L                  = gA.L();
      cA.Ls                 = gA.L();
      cA.a_sys              = gA.L() * gA.L();
      cA.r_sys              = gA.nout() * gA.L();
      cA.ts                 = 1.0F / (15e3F * 14.0F);
      cA.scs_hz             = 15e3F;
      cA.fd_hz              = 300.0F;
      cA.tau_rms_s          = 370e-9F;
      cA.sigma2             = 0.01F;
      cA.ridge              = 1e-6F;
      cA.sigma2_from_device = 0;
      cA.sigma2_slot        = 0;
      cA.dmrs_slots[0]      = 2;
      cA.dmrs_slots[1]      = 7;
      cA.dmrs_slots[2]      = 11;
      const NSUInteger tgsA = (aA + 255u) / 256u;

      std::printf("\n[OVERLAP-SWEEP] two buffers of mmse_corr_a, serial vs together, by grid size:\n");
      std::printf("     systems   threadgroups/buf   serial(us)   together(us)   ratio\n");
      for (uint32_t sys : {1u, 4u, 16u, 64u, 256u}) {
        cA.nof_systems        = sys;
        const auto issueA     = [&](id<MTLCommandBuffer> cb, NSUInteger off) {
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:p_corr_a];
          [enc setBuffer:b_a offset:off atIndex:0];
          [enc setBytes:&cA length:sizeof(cA) atIndex:1];
          [enc dispatchThreadgroups:MTLSizeMake(tgsA, sys, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
          [enc endEncoding];
        };
        double ser = 1e30;
        double tog = 1e30;
        for (unsigned round = 0; round != 9; ++round) {
          double t = CFAbsoluteTimeGetCurrent();
          {
            id<MTLCommandBuffer> x = [q commandBuffer];
            issueA(x, 0);
            [x commit];
            [x waitUntilCompleted];
            id<MTLCommandBuffer> y = [q commandBuffer];
            issueA(y, static_cast<NSUInteger>(sys) * aA * sizeof(float));
            [y commit];
            [y waitUntilCompleted];
          }
          ser = std::min(ser, (CFAbsoluteTimeGetCurrent() - t) * 1e6);

          t = CFAbsoluteTimeGetCurrent();
          {
            id<MTLCommandBuffer> x = [q commandBuffer];
            id<MTLCommandBuffer> y = [q commandBuffer];
            issueA(x, 0);
            issueA(y, static_cast<NSUInteger>(sys) * aA * sizeof(float));
            [x commit];
            [y commit];
            [x waitUntilCompleted];
            [y waitUntilCompleted];
          }
          tog = std::min(tog, (CFAbsoluteTimeGetCurrent() - t) * 1e6);
        }
        std::printf("  %8u   %14lu   %10.1f   %12.1f   %6.3f%s\n",
                    sys,
                    static_cast<unsigned long>(tgsA * sys),
                    ser,
                    tog,
                    tog / ser,
                    (tog / ser < 0.9) ? "  overlap" : "  serialized");
      }
    }

    // O1 (dev doc 6.174): the merged correlation kernel against the two it replaces, BYTE FOR BYTE.
    //
    // This is the arm that decides whether "one dispatch" changed anything it was not allowed to change.
    // Both destinations are read back from the device and compared element by element; a single differing
    // float means the merged kernel is not the same computation, and the knob must not be flown.
    if (p_corr_merged != nil && p_corr_a != nil && p_corr_rhp != nil) {
      const geometry   g3{};
      const NSUInteger a_elems3 = static_cast<NSUInteger>(g3.L()) * g3.L();
      const NSUInteger r_elems3 = static_cast<NSUInteger>(g3.nout()) * g3.L();
      // The buffers' OWN strides, which are what the comparison must walk with. The first version used
      // `a_elems + 64`, a made-up pad: it read the second system from the wrong offset and reported a
      // difference in the kernel that was in the comparison window (dev doc 6.174②).
      const NSUInteger a_sys3 = a_elems3;
      const NSUInteger r_sys3 = r_elems3;
      corr_params      c4{};
      c4.nof_systems        = g3.systems();
      c4.npt                = g3.npt;
      c4.npf                = g3.npf();
      c4.ncomb              = g3.ncomb;
      c4.nf                 = g3.nf();
      c4.L                  = g3.L();
      c4.Ls                 = g3.L();
      c4.a_sys              = g3.L() * g3.L();
      c4.r_sys              = g3.nout() * g3.L();
      c4.ts                 = 1.0F / (15e3F * 14.0F);
      c4.scs_hz             = 15e3F;
      c4.fd_hz              = 300.0F;
      c4.tau_rms_s          = 370e-9F;
      c4.sigma2             = 0.01F;
      c4.ridge              = 1e-6F;
      c4.sigma2_from_device = 0;
      c4.sigma2_slot        = 0;
      c4.dmrs_slots[0]      = 2;
      c4.dmrs_slots[1]      = 7;
      c4.dmrs_slots[2]      = 11;
      // pilot_re holds the pilot positions WITHIN one PRB - at most 12 - while npf (18) counts them
      // across the hop's DM-RS symbols. Filling npf of them wrote past the array and corrupted the stack
      // (the harness aborted before printing anything, with no diagnostic).
      for (uint32_t i = 0; i != 12u && i != g3.ncomb; ++i) {
        c4.pilot_re[i] = i * 2u; // comb 2, the air leg's pattern
      }
      // Fill the inputs with a deterministic pattern so a wrong index shows up as a difference.
      float* af = static_cast<float*>(b_a.contents);
      for (NSUInteger i = 0; i != big / sizeof(float); ++i) {
        af[i] = static_cast<float>((i * 2654435761u) % 1000u) / 1000.0F;
      }
      const auto run_two = [&]() {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p_corr_a];
        [enc setBuffer:b_a offset:0 atIndex:0];
        [enc setBytes:&c4 length:sizeof(c4) atIndex:1];
        [enc dispatchThreads:MTLSizeMake(a_elems3, c4.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [enc setComputePipelineState:p_corr_rhp];
        [enc setBuffer:b_rhp offset:0 atIndex:0];
        [enc setBytes:&c4 length:sizeof(c4) atIndex:1];
        [enc dispatchThreads:MTLSizeMake(r_elems3, c4.nof_systems, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      };
      const auto run_merged = [&]() {
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:p_corr_merged];
        [enc setBuffer:b_a offset:0 atIndex:0];
        [enc setBuffer:b_rhp offset:0 atIndex:1];
        [enc setBytes:&c4 length:sizeof(c4) atIndex:2];
        // The engine's own grid: one dimension of threadgroups, (matrix, system) decoded from the index.
        const NSUInteger wider = (a_elems3 > r_elems3) ? a_elems3 : r_elems3;
        const NSUInteger tgs_m = (wider + 255u) / 256u;
        [enc dispatchThreadgroups:MTLSizeMake(tgs_m * 2u * c4.nof_systems, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
      };
      run_two();
      std::vector<float> a_two(static_cast<size_t>(a_sys3 * c4.nof_systems));
      std::vector<float> r_two(static_cast<size_t>(r_sys3 * c4.nof_systems));
      std::memcpy(a_two.data(), b_a.contents, a_two.size() * sizeof(float));
      std::memcpy(r_two.data(), b_rhp.contents, r_two.size() * sizeof(float));
      std::memset(b_a.contents, 0, a_two.size() * sizeof(float));
      std::memset(b_rhp.contents, 0, r_two.size() * sizeof(float));
      run_merged();
      std::vector<float> a_m(static_cast<size_t>(a_sys3 * c4.nof_systems));
      std::vector<float> r_m(static_cast<size_t>(r_sys3 * c4.nof_systems));
      std::memcpy(a_m.data(), b_a.contents, a_m.size() * sizeof(float));
      std::memcpy(r_m.data(), b_rhp.contents, r_m.size() * sizeof(float));
      size_t diff_a = 0;
      size_t diff_r = 0;
      for (size_t i = 0; i != a_two.size(); ++i) {
        if (a_two[i] != a_m[i]) {
          ++diff_a;
        }
      }
      for (size_t i = 0; i != r_two.size(); ++i) {
        if (r_two[i] != r_m[i]) {
          ++diff_r;
        }
      }
      // WHERE the differences are, when there are any: an index inside Ls*Ls is an element of A and a
      // count of 128 (2 systems x 64) points at the PAD, which is the one region where the two routes
      // could legitimately disagree (the pad is written by the caller on the two-kernel route).
      {
        size_t shown = 0;
        for (size_t i = 0; (i != a_two.size()) && (shown != 4); ++i) {
          if (a_two[i] != a_m[i]) {
            const size_t sys = i / a_sys3;
            const size_t in  = i % a_sys3;
            std::printf("[O1]   A diff at i=%zu (system %zu, within-system %zu): two=%.9g merged=%.9g  "
                        "Ls*Ls=%lu L=%u\n",
                        i,
                        sys,
                        in,
                        static_cast<double>(a_two[i]),
                        static_cast<double>(a_m[i]),
                        static_cast<unsigned long>(g3.L() * g3.L()),
                        g3.L());
            ++shown;
          }
        }
      }
      std::printf("\n[O1] merged corr kernel vs the two it replaces (systems=%u, L=%u): "
                  "A differs in %zu of %zu, R_hp in %zu of %zu -> %s\n",
                  c4.nof_systems,
                  g3.L(),
                  diff_a,
                  a_two.size(),
                  diff_r,
                  r_two.size(),
                  ((diff_a == 0) && (diff_r == 0)) ? "BYTE-IDENTICAL" : "DIFFERENT (do not fly the knob)");
    }

    std::printf("\nRead it against the hop: the legs' merged_hop is ~534 us/lane, of which the CE's own\n"
                "dispatches are the ones counted here (5.91/hop without the scatter since 6.62). The\n"
                "BASELINE row is what a dispatch costs when it does nothing - the floor the elimination\n"
                "arms (5-14 us per removed dispatch) are moving.\n");
    return 0;
  }
}
