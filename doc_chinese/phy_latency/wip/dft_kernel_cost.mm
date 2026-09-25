// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief WHAT THE FRONT END'S 941 us ARE: an offline micro-benchmark of the production `dft_dit` kernel.
///
/// WHY IT EXISTS (dev doc 6.25 (2)). The split arm answered Q1 - the hop's device execution is 87% the front-end
/// per-symbol transforms (941 us of ~1080 us) - and that makes the DFT the V1 lever. But a GPU executing a
/// 768-point FFT should not need ~67 us per transform (which is what 941 us over ~14 transforms per slot means),
/// so the next question is WHERE those microseconds are: the butterflies, the grid write, the digit-reversed
/// load, or the dispatch shape (one threadgroup per transform, repeated per symbol).
///
/// WHAT IT MEASURES. The kernel is loaded from the PRODUCTION artifact (`ocudu_dft.metallib`) with the same
/// tables the engine builds (N/2 twiddles, mixed-radix digit reversal), and each arm dispatches the production
/// shape: T threadgroups per dispatch, one per transform, for T = 1/2/7/14 - the last being one n78 slot's worth
/// of symbols. GPU time comes from the command buffer's own timestamps, so the number is the device's.
///
/// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
///
///   clang++ -std=c++20 -fobjc-arc -framework Metal -framework Foundation -O2 \\
///           dft_kernel_cost.mm -o dft_kernel_cost
///   ./dft_kernel_cost [metallib-path]      # default: the checkout's ocudu_dft.metallib

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

struct tables {
  std::vector<float>    twiddle; // N/2 float2
  std::vector<uint32_t> perm;    // N entries, digit-reversed
  uint32_t              radix2 = 0;
  uint32_t              radix3 = 0;
};

/// Same construction as dft_metal_engine's (the mixed-radix digit reversal, radix-2 digits first).
tables build_tables(uint32_t n)
{
  tables t;
  uint32_t r2 = 0;
  uint32_t r3 = 0;
  uint32_t rem = n;
  while ((rem % 2) == 0) {
    rem /= 2;
    ++r2;
  }
  while ((rem % 3) == 0) {
    rem /= 3;
    ++r3;
  }
  t.radix2 = r2;
  t.radix3 = r3;
  t.twiddle.resize(static_cast<size_t>(n / 2) * 2);
  for (uint32_t k = 0; k != n / 2; ++k) {
    const double ang = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
    t.twiddle[2 * k]     = static_cast<float>(std::cos(ang));
    t.twiddle[2 * k + 1] = static_cast<float>(std::sin(ang));
  }
  t.perm.resize(n);
  for (uint32_t i = 0; i != n; ++i) {
    uint32_t x         = i;
    uint32_t rev       = 0;
    uint32_t remaining = n;
    for (uint32_t q = 0; q != r2; ++q) {
      remaining /= 2;
      rev += (x % 2) * remaining;
      x /= 2;
    }
    for (uint32_t q = 0; q != r3; ++q) {
      remaining /= 3;
      rev += (x % 3) * remaining;
      x /= 3;
    }
    t.perm[i] = rev;
  }
  return t;
}

/// A grid-write parameter block with the same layout the kernel declares (grid_write_params).
struct grid_write_params {
  uint32_t active;
  uint32_t dst_offset;
  uint32_t nof_subc;
  uint32_t map_offset;
  float    phase_re;
  float    phase_im;
  uint32_t pad0;
  uint32_t pad1;
};

struct input_params {
  uint32_t is_ci16;
  uint32_t offset;
  float    gain;
  uint32_t pad;
};

struct arm_result {
  const char* label;
  uint32_t    transforms;
  double      gpu_us_per_dispatch;
  double      us_per_transform;
};

} // namespace

int main(int argc, char** argv)
{
  @autoreleasepool {
    NSString* lib_path = (argc > 1) ? @(argv[1]) : @"lib/phy/generic_functions/metal/ocudu_dft.metallib";
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
    id<MTLFunction>              fn   = [lib newFunctionWithName:@"dft_dit"];
    id<MTLComputePipelineState>  pipe = (fn != nil) ? [device newComputePipelineStateWithFunction:fn error:&err] : nil;
    if (pipe == nil) {
      std::printf("FAIL: no dft_dit pipeline in the library\n");
      return 1;
    }
    std::printf("device=%s  kernel=dft_dit  library=%s\n", device.name.UTF8String, lib_path.UTF8String);

    // The two sizes the two cells of this workflow use: n1 (5 MHz, 15 kHz) and n78 (20 MHz, 30 kHz).
    const uint32_t sizes[] = {512u, 768u};
    const uint32_t arms[]  = {1u, 2u, 7u, 14u}; // transforms per dispatch: one symbol up to one slot's worth

    for (uint32_t n : sizes) {
      const tables t = build_tables(n);
      std::printf("\n=== n=%u  (radix2=%u radix3=%u, threads=%u, threadgroup memory=%u B) ===\n",
                  n,
                  t.radix2,
                  t.radix3,
                  std::min(n, 1024u),
                  n * 8u);

      const uint32_t max_in_flight = 16;
      id<MTLBuffer>  b_in  = [device newBufferWithLength:static_cast<NSUInteger>(n) * max_in_flight * 8
                                                options:MTLResourceStorageModeShared];
      id<MTLBuffer>  b_out = [device newBufferWithLength:static_cast<NSUInteger>(n) * max_in_flight * 8
                                                 options:MTLResourceStorageModeShared];
      id<MTLBuffer>  b_tw  = [device newBufferWithBytes:t.twiddle.data()
                                                 length:t.twiddle.size() * sizeof(float)
                                                options:MTLResourceStorageModeShared];
      id<MTLBuffer>  b_perm = [device newBufferWithBytes:t.perm.data()
                                                  length:t.perm.size() * sizeof(uint32_t)
                                                 options:MTLResourceStorageModeShared];
      // A grid the size of the cell's subcarriers (n78: 51 PRB = 612; n1: 25 PRB = 300) - the write path is
      // only interesting when it is ON, so both settings are measured.
      const uint32_t nof_subc = (n == 768u) ? 612u : 300u;
      id<MTLBuffer>  b_grid   = [device newBufferWithLength:static_cast<NSUInteger>(nof_subc) * 4u + 4096u
                                                    options:MTLResourceStorageModeShared];

      id<MTLCommandQueue> q = [device newCommandQueue];

      const auto run = [&](const char* label, uint32_t transforms, bool grid_write) -> arm_result {
        grid_write_params gw{};
        gw.active     = grid_write ? 1u : 0u;
        gw.dst_offset = 64u;
        gw.nof_subc   = grid_write ? nof_subc : 0u;
        gw.map_offset = n - nof_subc / 2u;
        gw.phase_re   = 1.0F;
        input_params ip{0u, 0u, 1.0F, 0u};
        const uint32_t base = 0u;

        // 200 dispatches in ONE command buffer: the shape the lane commits, and the only way the GPU timestamps
        // describe the dispatch rather than the queue.
        constexpr uint32_t reps = 200;
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        [enc setBuffer:b_in offset:0 atIndex:0];
        [enc setBuffer:b_out offset:0 atIndex:1];
        [enc setBuffer:b_tw offset:0 atIndex:2];
        [enc setBuffer:b_perm offset:0 atIndex:3];
        [enc setBytes:&t.radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&t.radix3 length:sizeof(uint32_t) atIndex:5];
        const uint32_t inverse = 0u;
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:b_grid offset:0 atIndex:8];
        [enc setBuffer:b_tw offset:0 atIndex:9]; // any table: window is unused when gw.active = 0
        [enc setBytes:&gw length:sizeof(gw) atIndex:10];
        [enc setBuffer:b_in offset:0 atIndex:11];
        [enc setBytes:&ip length:sizeof(input_params) atIndex:12];
        for (uint32_t r = 0; r != reps; ++r) {
          [enc dispatchThreadgroups:MTLSizeMake(transforms, 1, 1)
              threadsPerThreadgroup:MTLSizeMake(std::min(n, 1024u), 1, 1)];
        }
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        const double us = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
        return {label, transforms, us / reps, us / reps / transforms};
      };

      std::printf("%-28s %10s %14s %16s\n", "arm", "xforms", "GPU us/dispatch", "GPU us/transform");
      for (uint32_t x : arms) {
        const arm_result r = run("butterflies + grid write", x, true);
        std::printf("%-28s %10u %14.2f %16.2f\n", "butterflies + grid write", r.transforms, r.gpu_us_per_dispatch, r.us_per_transform);
      }
      for (uint32_t x : arms) {
        const arm_result r = run("butterflies only", x, false);
        std::printf("%-28s %10u %14.2f %16.2f\n", "butterflies only (no write)", r.transforms, r.gpu_us_per_dispatch, r.us_per_transform);
      }
      (void)q;
    }
    // ---- Does a SHARED Metal object serialize consecutive command buffers? ---------------------------------
    //
    // The air legs read the front-end block (14 transforms, ~14 us of work by the arm above) as a 941 us GPU
    // WINDOW. The pool hands the SAME grid storage address back from one slot to the next, and the process-wide
    // no-copy wrap (shared_queue::wrap_no_copy) deliberately maps one address to ONE MTLBuffer object - so two
    // consecutive hops bind the same object, and Metal's hazard tracking relates them. This arm asks whether
    // that relation makes a SHORT buffer report a LONG window when the buffer before it is long.
    {
      const uint32_t n = 768u;
      const tables   t = build_tables(n);
      id<MTLCommandQueue> q = [device newCommandQueue];

      // A long-running buffer: a dispatch that keeps the device busy for ~1 ms (the lane's merged hop).
      id<MTLBuffer>  shared = [device newBufferWithLength:static_cast<NSUInteger>(n) * 16u * 8
                                                  options:MTLResourceStorageModeShared];
      id<MTLBuffer>  other  = [device newBufferWithLength:static_cast<NSUInteger>(n) * 16u * 8
                                                   options:MTLResourceStorageModeShared];
      id<MTLBuffer>  b_tw   = [device newBufferWithBytes:t.twiddle.data()
                                                   length:t.twiddle.size() * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
      id<MTLBuffer>  b_perm = [device newBufferWithBytes:t.perm.data()
                                                   length:t.perm.size() * sizeof(uint32_t)
                                                  options:MTLResourceStorageModeShared];
      grid_write_params gw{};
      input_params      ip{0u, 0u, 1.0F, 0u};
      const uint32_t    base = 0u;
      const uint32_t    inverse = 0u;

      // One "previous hop": 64 dispatches of 14 transforms (~64 x 14 us ~ 900 us), binding \c prev_buf.
      const auto long_hop = [&](id<MTLCommandBuffer> cb, id<MTLBuffer> prev_buf) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pipe];
        [e setBuffer:prev_buf offset:0 atIndex:0];
        [e setBuffer:prev_buf offset:0 atIndex:1];
        [e setBuffer:b_tw offset:0 atIndex:2];
        [e setBuffer:b_perm offset:0 atIndex:3];
        [e setBytes:&t.radix2 length:sizeof(uint32_t) atIndex:4];
        [e setBytes:&t.radix3 length:sizeof(uint32_t) atIndex:5];
        [e setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [e setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [e setBuffer:prev_buf offset:0 atIndex:8];
        [e setBuffer:b_tw offset:0 atIndex:9];
        [e setBytes:&gw length:sizeof(gw) atIndex:10];
        [e setBuffer:prev_buf offset:0 atIndex:11];
        [e setBytes:&ip length:sizeof(input_params) atIndex:12];
        for (uint32_t r = 0; r != 60; ++r) {
          [e dispatchThreadgroups:MTLSizeMake(14, 1, 1) threadsPerThreadgroup:MTLSizeMake(768, 1, 1)];
        }
        [e endEncoding];
      };
      // The "deposit": one dispatch of 14 transforms (~14 us), binding \c buf.
      const auto short_deposit = [&](id<MTLCommandBuffer> cb, id<MTLBuffer> buf) {
        id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
        [e setComputePipelineState:pipe];
        [e setBuffer:buf offset:0 atIndex:0];
        [e setBuffer:buf offset:0 atIndex:1];
        [e setBuffer:b_tw offset:0 atIndex:2];
        [e setBuffer:b_perm offset:0 atIndex:3];
        [e setBytes:&t.radix2 length:sizeof(uint32_t) atIndex:4];
        [e setBytes:&t.radix3 length:sizeof(uint32_t) atIndex:5];
        [e setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [e setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [e setBuffer:buf offset:0 atIndex:8];
        [e setBuffer:b_tw offset:0 atIndex:9];
        [e setBytes:&gw length:sizeof(grid_write_params) atIndex:10];
        [e setBuffer:buf offset:0 atIndex:11];
        [e setBytes:&ip length:sizeof(input_params) atIndex:12];
        [e dispatchThreadgroups:MTLSizeMake(14, 1, 1) threadsPerThreadgroup:MTLSizeMake(768, 1, 1)];
        [e endEncoding];
      };

      const auto measure = [&](const char* label, bool share_objects) {
        id<MTLBuffer> hop_buf     = share_objects ? shared : other;
        id<MTLBuffer> deposit_buf = shared; // the deposit always writes the shared object (the reused grid)
        id<MTLCommandBuffer> cb_hop = [q commandBuffer];
        long_hop(cb_hop, hop_buf);
        id<MTLCommandBuffer> cb_dep = [q commandBuffer];
        short_deposit(cb_dep, deposit_buf);
        [cb_hop commit];
        [cb_dep commit];
        [cb_hop waitUntilCompleted];
        [cb_dep waitUntilCompleted];
        const double hop_us = (cb_hop.GPUEndTime - cb_hop.GPUStartTime) * 1e6;
        const double dep_us = (cb_dep.GPUEndTime - cb_dep.GPUStartTime) * 1e6;
        const double dep_start_delay = (cb_dep.GPUStartTime - cb_hop.GPUEndTime) * 1e6;
        std::printf("%-46s hop=%8.1f us  deposit=%8.1f us  start-delay-after-hop=%8.1f us\n",
                    label,
                    hop_us,
                    dep_us,
                    dep_start_delay);
      };
      measure("previous hop binds a DIFFERENT object", false);
      measure("previous hop binds the SAME object", true);
      measure("previous hop binds the SAME object (repeat)", true);
      (void)q;
    }

    std::printf("\n(200 dispatches per command buffer; GPU time is the command buffer's own window)\n");
  }
  return 0;
}
