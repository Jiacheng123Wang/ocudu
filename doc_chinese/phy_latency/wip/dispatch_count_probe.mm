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
  }
  return 0;
}
