// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// WHAT DOES ONE MORE DISPATCH COST WHEN THE DISPATCH HAS NOTHING TO DO? (dev doc 6.161, offline, no leg)
//
// WHY IT EXISTS. The air hop's command buffer carries ~5 dispatches (front end batched ~1.3 + the lane's
// burst 4.00, counted from a leg's own counters) and is resident ~469us. The offline scan of the REAL
// `dft_dit` kernel (wip/dft_dispatch_cost.mm) reads 47.5 / 84.8 / 161.3 / 278.2 / 539.3us for 1 / 2 / 4 /
// 7 / 14 dispatches - a straight line of ~38us per dispatch, with the threadgroups inside a dispatch fully
// overlapped (14 threadgroups = 1 threadgroup = 46.6us). If that per-dispatch price is content-independent,
// then the air window is a DISPATCH-COUNT account and the only lever is issuing fewer dispatches per hop -
// which would also kill the "one command buffer for N hops" (C-A) idea, because merging hops does not
// reduce the dispatch count.
//
// WHAT IT MEASURES. The same scan with a kernel whose body does nothing (compiled here, from source, so no
// metallib is needed), against the recorded real-kernel numbers: dispatch counts 1 / 2 / 4 / 7 / 14, one
// threadgroup each, plus (a) an EMPTY command buffer (no dispatch at all - is there a per-buffer floor?),
// (b) the same real-shaped arm but with 14 threadgroups in ONE dispatch, (c) a command buffer that encodes a
// shared-event signal, and (d) one that also waits on a cross-queue event. GPU time is the command buffer's
// own `GPUEndTime - GPUStartTime`, median of N after warm-up; host time is commit -> waitUntilCompleted.
//
// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
//   clang++ -fobjc-arc -std=c++17 -O2 -framework Metal -framework Foundation \
//       -o /tmp/dispatch_count_probe doc_chinese/phy_latency/wip/dispatch_count_probe.mm && /tmp/dispatch_count_probe
//
// HOW TO READ IT. If the no-op slope ~= 38us/dispatch, the price is per dispatch and content-independent:
// the treatment is fewer dispatches per hop, and C-A is dead. If the no-op slope is much smaller, then the
// real kernel's geometry (int16 input, grid write, 14 threadgroups) is what the ~38us comes from and the
// question moves back to the front end's own shape.

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

namespace {

constexpr unsigned nof_runs   = 15;
constexpr unsigned nof_warmup = 10;

/// A kernel that does nothing but cannot be optimised away: the store is behind a condition that is never
/// true, so the compiler keeps the memory argument alive and the dispatch has real arguments to bind.
NSString* const noop_source = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void noop_kernel(device float* out [[buffer(0)]],
                        constant uint&  n   [[buffer(1)]],
                        uint            i   [[thread_position_in_grid]])
{
  if (i > n) { out[0] = 1.0f; }
}
)";

/// A memory-bound kernel with the air chain's shape at the air chain's sizes: each dispatch reads the whole
/// input (46 KB, the slot's IQ) and sweeps the whole grid (64 KB, 612 subcarriers x 14 symbols x 8 bytes).
/// Chained on the SAME buffers, five of them are the hop's data-dependency chain (front end writes the grid,
/// the estimator/equalizer/demapper read and write it); on SEPARATE buffers they are independent dispatches
/// of the same size. The difference is what the dependency costs - the candidate the per-dispatch prices
/// cannot see.
NSString* const sweep_source = @R"(
#include <metal_stdlib>
using namespace metal;
kernel void sweep_kernel(device const float* in   [[buffer(0)]],
                         device float*       grid [[buffer(1)]],
                         constant uint&      n_in [[buffer(2)]],
                         constant uint&      n_grid [[buffer(3)]],
                         constant uint&      mode [[buffer(4)]],
                         uint                i    [[thread_position_in_grid]])
{
  if (i >= n_grid) { return; }
  float acc = 0.0f;
  if (mode == 0u) {
    // PRODUCER: reads the input, writes the grid (the front end's shape).
    const uint base = (i * 7u) % n_in;
    acc += in[base] + in[(base + 1u) % n_in] + in[(base + 2u) % n_in] + in[(base + 3u) % n_in];
    grid[i] = acc * 0.25f;
  } else {
    // CONSUMER: reads the grid the previous dispatch wrote, writes it back (the estimator/equalizer/
    // demapper shape). This is the read-after-write edge the earlier arm was missing: without it, five
    // dispatches on the same buffers were still independent.
    const uint base = (i * 3u) % n_grid;
    acc += grid[base] + grid[(base + 1u) % n_grid] + grid[(base + 2u) % n_grid];
    grid[i] = acc * 0.3333f;
  }
}
)";

struct sample {
  double gpu_us  = 0.0;
  double host_us = 0.0;
};

double median(std::vector<double> v)
{
  if (v.empty()) {
    return 0.0;
  }
  std::sort(v.begin(), v.end());
  return v[v.size() / 2];
}

} // namespace

int main()
{
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::fprintf(stderr, "no Metal device\n");
      return 1;
    }
    std::printf("device: %s\n", [[device name] UTF8String]);

    NSError*          err  = nil;
    id<MTLLibrary>    lib  = [device newLibraryWithSource:noop_source options:nil error:&err];
    if (lib == nil) {
      std::fprintf(stderr, "library: %s\n", [[err localizedDescription] UTF8String]);
      return 1;
    }
    id<MTLFunction>             fn   = [lib newFunctionWithName:@"noop_kernel"];
    id<MTLComputePipelineState> pipe = [device newComputePipelineStateWithFunction:fn error:&err];
    if (pipe == nil) {
      std::fprintf(stderr, "pipeline: %s\n", [[err localizedDescription] UTF8String]);
      return 1;
    }
    id<MTLCommandQueue>    queue = [device newCommandQueue];
    id<MTLCommandQueue>    other = [device newCommandQueue];
    id<MTLBuffer>          buf   = [device newBufferWithLength:4096 options:MTLResourceStorageModeShared];
    id<MTLSharedEvent>     ev    = [device newSharedEvent];

    const unsigned threads_per_tg = 256;
    const unsigned grid_threads   = 256 * 64; // the front end's order of magnitude per threadgroup

    // One arm = (nof dispatches, threadgroups per dispatch, option). Option 0 = plain, 1 = encode a signal on
    // `ev`, 2 = wait on `ev` before the dispatches (signalled by a producer committed on the other queue).
    auto run_arm = [&](unsigned nof_disp, unsigned tgs_per_disp, int option, const char* what) {
      std::vector<double> gpu, host;
      for (unsigned i = 0; i != nof_warmup + nof_runs; ++i) {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        if (option == 2) {
          // A producer on the other queue that signals the event; the consumer waits for it, so the wait is a
          // real cross-queue dependency rather than a self-signal.
          id<MTLCommandBuffer> prod = [other commandBuffer];
          [prod encodeSignalEvent:ev value:i + 1];
          [prod commit];
          [cb encodeWaitForEvent:ev value:i + 1];
        }
        if (nof_disp != 0) {
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:pipe];
          [enc setBuffer:buf offset:0 atIndex:0];
          unsigned n = grid_threads;
          [enc setBytes:&n length:sizeof(n) atIndex:1];
          for (unsigned d = 0; d != nof_disp; ++d) {
            [enc dispatchThreadgroups:MTLSizeMake(tgs_per_disp, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads_per_tg, 1, 1)];
          }
          [enc endEncoding];
        }
        if (option == 1) {
          [cb encodeSignalEvent:ev value:i + 1];
        }
        const auto t0 = std::chrono::steady_clock::now();
        [cb commit];
        [cb waitUntilCompleted];
        const auto t1 = std::chrono::steady_clock::now();
        if (i >= nof_warmup) {
          const double g = (cb.GPUEndTime - cb.GPUStartTime) * 1e6;
          gpu.push_back(g);
          host.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
        }
      }
      std::printf("%-42s gpu median %8.1f us   host median %8.1f us\n", what, median(gpu), median(host));
      std::fflush(stdout);
    };

    std::printf("\n-- dispatch count scan, ONE threadgroup per dispatch (no-op kernel) --\n");
    for (unsigned k : {0u, 1u, 2u, 4u, 7u, 14u}) {
      char label[96];
      std::snprintf(label, sizeof(label), "no-op: %u dispatch(es), 1 tg each", k);
      run_arm(k, 1, 0, label);
    }

    std::printf("\n-- threadgroups INSIDE one dispatch (no-op kernel) --\n");
    for (unsigned k : {1u, 7u, 14u}) {
      char label[96];
      std::snprintf(label, sizeof(label), "no-op: 1 dispatch, %u threadgroups", k);
      run_arm(1, k, 0, label);
    }

    std::printf("\n-- events and cross-queue waits on a 1-dispatch buffer --\n");
    run_arm(1, 1, 1, "no-op: 1 dispatch + signal event");
    run_arm(1, 1, 2, "no-op: 1 dispatch + wait on other queue");

    std::printf("\n-- the air hop's dispatch count, no-op content --\n");
    run_arm(5, 1, 0, "no-op: 5 dispatches (one hop's appx count)");
    run_arm(5, 4, 0, "no-op: 5 dispatches x 4 threadgroups");

    // ---- the memory-bound chain: 5 dispatches at the air chain's sizes -------------------------------
    // Same kernel, same sizes; only the buffers differ. CHAINED = all five read/write ONE input and ONE grid
    // (the hop's data dependency), SEPARATE = five private buffer pairs (no dependency). If CHAINED is much
    // more expensive, the ~470us is memory-dependency serialisation, and the treatment is fewer passes over
    // the grid - not fewer dispatches.
    id<MTLLibrary>            slib = [device newLibraryWithSource:sweep_source options:nil error:&err];
    id<MTLFunction>           sfn  = (slib != nil) ? [slib newFunctionWithName:@"sweep_kernel"] : nil;
    id<MTLComputePipelineState> spipe = (sfn != nil) ? [device newComputePipelineStateWithFunction:sfn error:&err] : nil;
    if (spipe == nil) {
      std::printf("sweep kernel unavailable: %s\n", [[err localizedDescription] UTF8String]);
    } else {
      constexpr unsigned n_in   = 11520 * 4;  // the slot's samples as floats: 46 KB
      constexpr unsigned n_grid = 612 * 14 * 8; // the grid as floats: 64 KB
      id<MTLBuffer> in_shared   = [device newBufferWithLength:n_in * sizeof(float) options:MTLResourceStorageModePrivate];
      id<MTLBuffer> grid_shared = [device newBufferWithLength:n_grid * sizeof(float) options:MTLResourceStorageModePrivate];
      std::vector<id<MTLBuffer>> in_priv, grid_priv;
      for (unsigned d = 0; d != 5; ++d) {
        in_priv.push_back([device newBufferWithLength:n_in * sizeof(float) options:MTLResourceStorageModePrivate]);
        grid_priv.push_back([device newBufferWithLength:n_grid * sizeof(float) options:MTLResourceStorageModePrivate]);
      }
      const unsigned tg   = (n_grid + threads_per_tg - 1) / threads_per_tg;
      auto run_sweep = [&](bool chained, const char* what) {
        std::vector<double> gpu, host;
        for (unsigned i = 0; i != nof_warmup + nof_runs; ++i) {
          id<MTLCommandBuffer>          cb  = [queue commandBuffer];
          id<MTLComputeCommandEncoder>  enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:spipe];
          unsigned ni = n_in, ng = n_grid;
          for (unsigned d = 0; d != 5; ++d) {
            [enc setBuffer:(chained ? in_shared : in_priv[d]) offset:0 atIndex:0];
            [enc setBuffer:(chained ? grid_shared : grid_priv[d]) offset:0 atIndex:1];
            [enc setBytes:&ni length:sizeof(ni) atIndex:2];
            [enc setBytes:&ng length:sizeof(ng) atIndex:3];
            [enc dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads_per_tg, 1, 1)];
          }
          [enc endEncoding];
          const auto t0 = std::chrono::steady_clock::now();
          [cb commit];
          [cb waitUntilCompleted];
          const auto t1 = std::chrono::steady_clock::now();
          if (i >= nof_warmup) {
            gpu.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
            host.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
          }
        }
        std::printf("%-42s gpu median %8.1f us   host median %8.1f us\n", what, median(gpu), median(host));
        std::fflush(stdout);
      };
      std::printf("\n-- 5 memory-bound dispatches at the air chain's sizes (46 KB in / 64 KB grid) --\n");
      run_sweep(true, "5 chained (same in+grid, NO raw edge)");
      run_sweep(false, "5 separate (own in+grid: no dep)");

      // ---- the TRUE read-after-write chain ---------------------------------------------------------
      // Dispatch 0 produces the grid; dispatches 1..4 consume it (read the grid the PREVIOUS dispatch
      // wrote, write it back). That is the hop's real dependency: front end -> estimator -> equalizer ->
      // demapper, all on one grid. A window much longer than the sum of the isolated prices would then be
      // the price of the raw edges (cache visibility / barriers), not of the work.
      auto run_raw = [&](bool with_competitor, const char* what) {
        std::vector<double> gpu, host;
        std::atomic<bool>   stop{false};
        std::thread         comp;
        if (with_competitor) {
          comp = std::thread([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
              id<MTLCommandBuffer>         ccb = [other commandBuffer];
              id<MTLComputeCommandEncoder> cen = [ccb computeCommandEncoder];
              [cen setComputePipelineState:spipe];
              unsigned ni = n_in, ng = n_grid, m0 = 0, m1 = 1;
              for (unsigned d = 0; d != 5; ++d) {
                [cen setBuffer:in_shared offset:0 atIndex:0];
                [cen setBuffer:grid_shared offset:0 atIndex:1];
                [cen setBytes:&ni length:sizeof(ni) atIndex:2];
                [cen setBytes:&ng length:sizeof(ng) atIndex:3];
                [cen setBytes:(d == 0 ? &m0 : &m1) length:sizeof(unsigned) atIndex:4];
                [cen dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads_per_tg, 1, 1)];
              }
              [cen endEncoding];
              [ccb commit];
            }
          });
        }
        for (unsigned i = 0; i != nof_warmup + nof_runs; ++i) {
          id<MTLCommandBuffer>         cb  = [queue commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:spipe];
          unsigned ni = n_in, ng = n_grid, m0 = 0, m1 = 1;
          for (unsigned d = 0; d != 5; ++d) {
            [enc setBuffer:in_shared offset:0 atIndex:0];
            [enc setBuffer:grid_shared offset:0 atIndex:1];
            [enc setBytes:&ni length:sizeof(ni) atIndex:2];
            [enc setBytes:&ng length:sizeof(ng) atIndex:3];
            [enc setBytes:(d == 0 ? &m0 : &m1) length:sizeof(unsigned) atIndex:4];
            [enc dispatchThreadgroups:MTLSizeMake(tg, 1, 1) threadsPerThreadgroup:MTLSizeMake(threads_per_tg, 1, 1)];
          }
          [enc endEncoding];
          const auto t0 = std::chrono::steady_clock::now();
          [cb commit];
          [cb waitUntilCompleted];
          const auto t1 = std::chrono::steady_clock::now();
          if (i >= nof_warmup) {
            gpu.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
            host.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
          }
        }
        stop.store(true, std::memory_order_relaxed);
        if (comp.joinable()) {
          comp.join();
        }
        std::printf("%-42s gpu median %8.1f us   host median %8.1f us\n", what, median(gpu), median(host));
        std::fflush(stdout);
      };
      std::printf("\n-- TRUE read-after-write chain: 1 producer + 4 consumers on ONE grid --\n");
      run_raw(false, "RAW chain, GPU otherwise idle");
      run_raw(true, "RAW chain + a competing lane streaming");
      std::printf("\n-- the same RAW chain with NO-OP content (the ablation question) --\n");
      run_arm(5, 1, 0, "(no-op 5 dispatches, for comparison: no raw edge)");
    }
  }
  return 0;
}
