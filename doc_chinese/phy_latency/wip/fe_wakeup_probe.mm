// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// DOES A DISPATCH PAY A WAKE-UP WHEN IT FOLLOWS AN IDLE GAP? (dev doc 6.80, offline, no leg)
//
// WHY IT EXISTS. The front end's command buffer is resident ~452us on air (dev doc 6.77) while the
// same batched dispatch EXECUTES in 10.6us a slot in the isolated benchmark (6.65) - a 43x gap that no
// candidate so far explains: not the compute (the window does not scale with the allocation), not the
// fences (9-17us), not DL load, not peer sharing (~70us), not threadgroup serialisation (the isolated
// bill again), and not the zero-copy input (replacing it made things WORSE, 6.79).
//
// What separates the two measurements is not the kernel but the NEIGHBOURHOOD: the benchmark runs two
// hundred dispatches back to back and takes the minimum of three, while the real chain commits one
// command buffer per slot, i.e. one dispatch after ~1-2ms in which the GPU had nothing to do. If the
// device charges a wake-up (clock, threadgroup launch, page tables) per dispatch - or per threadgroup
// of a dispatch - then the isolated number is a steady-state floor and the air number is the cold one,
// and the two are not in contradiction at all.
//
// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
//   clang++ -fobjc-arc -std=c++17 -framework Metal -framework Foundation -O2 \
//       -o /tmp/fe_wakeup_probe doc_chinese/phy_latency/wip/fe_wakeup_probe.mm && /tmp/fe_wakeup_probe
//
// WHAT IT PRINTS. For each threadgroup count (1, 2, 7, 14) and each of two modes - BACK TO BACK (the
// benchmark's shape) and GAP (a 1ms sleep before every commit, the air chain's shape) - the command
// buffer's own GPU window (`GPUEndTime - GPUStartTime`), median of N runs, after a warm-up.
//
// HOW TO READ IT. If GAP grows with the threadgroup count while BACK TO BACK does not, the wake-up is
// per THREADGROUP and the front end's ~450us is 14 of them: the lever is then to cover a slot with
// FEWER threadgroups (or fewer dispatches), not to shorten the transform. If GAP is flat, the wake-up
// (if any) is per command buffer and something else holds the front end.

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr unsigned nof_runs      = 15;
constexpr unsigned nof_warmup    = 10;
constexpr unsigned threads_per_tg = 256;
constexpr unsigned work_iters    = 4096; ///< tuned so one threadgroup is a few microseconds of work

NSString* kernel_source()
{
  return @"#include <metal_stdlib>\n"
          "using namespace metal;\n"
          "kernel void spin(device float* out [[buffer(0)]], constant uint& iters [[buffer(1)]],\n"
          "                 uint gid [[thread_position_in_grid]], uint tid [[thread_position_in_threadgroup]],\n"
          "                 uint tg [[threadgroup_position_in_grid]]) {\n"
          "  float acc = float(tid + tg);\n"
          "  for (uint i = 0; i < iters; ++i) { acc = fma(acc, 1.000001f, 0.5f); }\n"
          "  out[gid] = acc;\n"
          "}\n";
}

double window_us(id<MTLCommandQueue> queue,
                 id<MTLComputePipelineState> pipeline,
                 id<MTLBuffer> out,
                 id<MTLBuffer> iters,
                 unsigned nof_threadgroups,
                 bool gap)
{
  std::vector<double> windows;
  windows.reserve(nof_runs);
  for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
    if (gap) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    id<MTLCommandBuffer>         cb  = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pipeline];
    [enc setBuffer:out offset:0 atIndex:0];
    [enc setBuffer:iters offset:0 atIndex:1];
    [enc dispatchThreadgroups:MTLSizeMake(nof_threadgroups, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(threads_per_tg, 1, 1)];
    [enc endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    const double start = cb.GPUStartTime;
    const double end   = cb.GPUEndTime;
    if ((run >= nof_warmup) && (end > start)) {
      windows.push_back((end - start) * 1e6);
    }
  }
  std::sort(windows.begin(), windows.end());
  return windows.empty() ? 0.0 : windows[windows.size() / 2];
}

} // namespace

int main(int argc, char** argv)
{
  // The work per threadgroup is the knob that separates "the floor" from "the compute": a dispatch with
  // a few iterations per thread measures what a command buffer costs by itself, a big one measures the
  // kernel. The default is the tiny end, because the floor is what the front end's window is suspected of.
  const unsigned work = (argc > 1) ? static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10)) : 8u;
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    std::printf("no Metal device\n");
    return 1;
  }
  NSError*        error   = nil;
  id<MTLLibrary>  library = [device newLibraryWithSource:kernel_source() options:nil error:&error];
  if (library == nil) {
    std::printf("library failed: %s\n", error.localizedDescription.UTF8String);
    return 1;
  }
  id<MTLComputePipelineState> pipeline =
      [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"spin"] error:&error];
  if (pipeline == nil) {
    std::printf("pipeline failed: %s\n", error.localizedDescription.UTF8String);
    return 1;
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLBuffer>       out   = [device newBufferWithLength:14 * threads_per_tg * sizeof(float)
                                                  options:MTLResourceStorageModeShared];
  uint32_t            iters = work;
  id<MTLBuffer>       params = [device newBufferWithBytes:&iters length:sizeof(iters) options:MTLResourceStorageModeShared];

  std::printf("device: %s   runs=%u (median), warmup=%u, threads/threadgroup=%u, work=%u iters\n",
              device.name.UTF8String,
              nof_runs,
              nof_warmup,
              threads_per_tg,
              work);
  std::printf("%-16s %14s %14s %14s\n", "threadgroups", "back-to-back", "gap 1ms", "gap - bt");
  std::printf("%-16s %14s %14s %14s\n", "----------------", "--------------", "--------------", "--------------");
  for (unsigned tg : { 1u, 2u, 7u, 14u }) {
    const double bt = window_us(queue, pipeline, out, params, tg, /*gap=*/false);
    const double gp = window_us(queue, pipeline, out, params, tg, /*gap=*/true);
    std::printf("%-16u %11.1fus %11.1fus %11.1fus\n", tg, bt, gp, gp - bt);
  }
  return 0;
}
