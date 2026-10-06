// DISPATCH / BARRIER COST PROBE — the number the metal_kernel_fusion workstream rests on.
//
// WHY THIS EXISTS (2026-10-06, plan doc §3 H1). The whole workstream assumes that turning the fused lane's
// 4 dispatches per hop into 1 is worth something. The leg probes cannot answer that: `OCUDU_LANE_ABLATE*`
// replaces a kernel's BODY but leaves the submission and barrier structure intact, so it measures WORK, never
// dispatch cost. And the previous workstream's 300-400us per commit was measured on an IDLE queue (the driver's
// submission window), which is a different quantity from the GPU-side launch/barrier cost of a dispatch inside an
// already-running command buffer.
//
// So this is a standalone Metal program with no radio, no repo build system and no flight in it: N trivial
// dispatches in one command buffer versus ONE dispatch doing N times the work, with the same grid, the same queue
// and the same buffers, measured BOTH on the device's own clock (GPUStartTime/GPUEndTime, read in a completion
// handler installed before commit - the same reading the leg's [ul_gpu_lane] busy is built from) and on the host
// around commit->wait.
//
// HOW TO READ IT. `slope` is the answer to H1: the device-side cost of one more dispatch. If it is single-digit
// microseconds against a hop whose lane busy is ~480us (the M0 baseline), then dispatch count is a few percent and
// M1's gains must come from removing intermediate buffers and reusing data inside one kernel - not from the count.
//
// usage:
//   xcrun clang++ -std=c++17 -fobjc-arc -O2 -framework Metal -framework Foundation \
//       dispatch_cost_probe.mm -o /tmp/dispatch_cost_probe && /tmp/dispatch_cost_probe
//
// OUTPUT: one line per arm (device time per command buffer, and the host time around commit+wait).

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static NSString* const kSource = @R"MSL(
#include <metal_stdlib>
using namespace metal;

// Essentially free: one store per thread.
kernel void trivial(device float* out [[buffer(0)]],
                    constant uint& n   [[buffer(1)]],
                    uint           tid [[thread_position_in_grid]])
{
  if (tid < n) { out[tid] = 1.0f; }
}

// A fixed amount of arithmetic per thread, so "N times the work" can be expressed as iters=N.
kernel void work(device float* out [[buffer(0)]],
                 constant uint& n   [[buffer(1)]],
                 constant uint& iters [[buffer(2)]],
                 uint           tid [[thread_position_in_grid]])
{
  if (tid >= n) { return; }
  float x = out[tid] + 1.0f;
  for (uint i = 0; i < iters; ++i) {
    x = fma(x, 1.0000001f, 0.5f);
  }
  out[tid] = x;
}

// The realistic stage pattern: reads what the previous dispatch wrote, so the two are ORDERED by the data.
kernel void work_dep(device float* out [[buffer(0)]],
                     constant uint& n   [[buffer(1)]],
                     constant uint& iters [[buffer(2)]],
                     uint           tid [[thread_position_in_grid]])
{
  if (tid >= n) { return; }
  float x = out[tid];            // the dependency
  for (uint i = 0; i < iters; ++i) {
    x = fma(x, 1.0000001f, 0.5f);
  }
  out[tid] = x;
}
)MSL";

struct reading {
  double device_us = 0.0; // GPUStartTime -> GPUEndTime of the command buffer
  double host_us   = 0.0; // steady_clock around commit -> waitUntilCompleted
};

struct stats {
  double median = 0.0, p10 = 0.0, p90 = 0.0, mn = 0.0;
};

static stats summarize(std::vector<double> v)
{
  stats s{};
  if (v.empty()) {
    return s;
  }
  std::sort(v.begin(), v.end());
  s.median = v[v.size() / 2];
  s.p10    = v[v.size() / 10];
  s.p90    = v[(v.size() * 9) / 10];
  s.mn     = v.front();
  return s;
}

int main(int argc, char** argv)
{
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::fprintf(stderr, "no Metal device\n");
      return 2;
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    NSError*            err   = nil;
    id<MTLLibrary>      lib   = [device newLibraryWithSource:kSource options:nil error:&err];
    if (lib == nil) {
      std::fprintf(stderr, "shader compile failed: %s\n", err.localizedDescription.UTF8String);
      return 2;
    }
    id<MTLFunction>              fn_trivial = [lib newFunctionWithName:@"trivial"];
    id<MTLFunction>              fn_work    = [lib newFunctionWithName:@"work"];
    id<MTLFunction>              fn_dep     = [lib newFunctionWithName:@"work_dep"];
    id<MTLComputePipelineState>  p_trivial  = [device newComputePipelineStateWithFunction:fn_trivial error:&err];
    id<MTLComputePipelineState>  p_work     = [device newComputePipelineStateWithFunction:fn_work error:&err];
    id<MTLComputePipelineState>  p_dep      = [device newComputePipelineStateWithFunction:fn_dep error:&err];

    // ONE HOP'S GEOMETRY (the M0 baseline): 51 RB x 12 subcarriers x 14 symbols = 8568 REs, 256 per threadgroup -
    // the same shape the equalizer and the demapper dispatch with.
    const uint hop_threads = 51 * 12 * 14;
    const uint tg          = 256;

    const size_t bytes = 1u << 20;
    id<MTLBuffer> buf  = [device newBufferWithLength:bytes options:MTLResourceStorageModeShared];
    if (buf == nil || p_trivial == nil || p_work == nil || p_dep == nil) {
      std::fprintf(stderr, "setup failed (%s)\n", err.localizedDescription.UTF8String);
      return 2;
    }

    const uint thread_counts[] = {hop_threads, hop_threads * 64, 1u << 20}; // one hop, a batch, and a big grid
    const uint dispatch_counts[] = {1, 2, 4, 8, 16};
    const int  reps              = 200;
    const int  warmup            = 20;

    auto run_arm = [&](id<MTLComputePipelineState> pipe, bool dependent, uint threads, uint nof_dispatches,
                       uint iters, int repetitions, bool one_cb) -> std::vector<reading> {
      std::vector<reading> out;
      for (int r = 0; r < repetitions + warmup; ++r) {
        const uint n = std::min<uint>(threads, static_cast<uint>(bytes / sizeof(float)));
        auto t0 = std::chrono::steady_clock::now();
        // __block: the completion handler runs later, on a Metal thread, and writes these.
        __block uint64_t start_ns = 0;
        __block uint64_t end_ns   = 0;
        if (one_cb) {
          id<MTLCommandBuffer>         cb  = [queue commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          for (uint d = 0; d < nof_dispatches; ++d) {
            [enc setComputePipelineState:pipe];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&n length:sizeof(n) atIndex:1];
            [enc setBytes:&iters length:sizeof(iters) atIndex:2];
            // A memory barrier between dispatches of DIFFERENT kernels is implicit; within one encoder Metal
            // inserts the dependency, which is exactly the stage-to-stage pattern being measured.
            [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
          }
          [enc endEncoding];
          [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            start_ns = static_cast<uint64_t>(c.GPUStartTime * 1e9);
            end_ns   = static_cast<uint64_t>(c.GPUEndTime * 1e9);
          }];
          [cb commit];
          [cb waitUntilCompleted];
        } else {
          // The same total work, one command buffer per dispatch: the "no command-buffer fusion" shape.
          std::vector<id<MTLCommandBuffer>> cbs;
          for (uint d = 0; d < nof_dispatches; ++d) {
            id<MTLCommandBuffer>         cb  = [queue commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pipe];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&n length:sizeof(n) atIndex:1];
            [enc setBytes:&iters length:sizeof(iters) atIndex:2];
            [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
            [enc endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
              start_ns = static_cast<uint64_t>(c.GPUStartTime * 1e9);
              end_ns   = static_cast<uint64_t>(c.GPUEndTime * 1e9);
            }];
            [cb commit];
            cbs.push_back(cb);
          }
          for (id<MTLCommandBuffer> cb : cbs) {
            [cb waitUntilCompleted];
          }
        }
        auto t1 = std::chrono::steady_clock::now();
        if (r >= warmup) {
          reading rd;
          rd.device_us = (end_ns > start_ns) ? static_cast<double>(end_ns - start_ns) / 1e3 : 0.0;
          rd.host_us   = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1e3;
          out.push_back(rd);
        }
      }
      return out;
    };

    auto device_series = [](const std::vector<reading>& v) {
      std::vector<double> d;
      for (const reading& r : v) {
        d.push_back(r.device_us);
      }
      return d;
    };
    auto host_series = [](const std::vector<reading>& v) {
      std::vector<double> d;
      for (const reading& r : v) {
        d.push_back(r.host_us);
      }
      return d;
    };

    std::printf("device: %s\n", device.name.UTF8String);
    std::printf("grid: one hop = %u threads (51x12x14), threadgroup %u; %d timed reps after %d warmup\n\n",
                hop_threads, tg, reps, warmup);

    // ---- ARM 1: N INDEPENDENT trivial dispatches in ONE command buffer -> the per-dispatch device cost --------
    std::printf("ARM 1  trivial kernel, N dispatches in ONE command buffer (device us per command buffer)\n");
    std::printf("  %-8s %10s %10s %10s   %s\n", "N", "median", "p10", "p90", "device us per dispatch");
    std::vector<std::pair<uint, double>> arm1;
    for (uint nd : dispatch_counts) {
      stats s = summarize(device_series(run_arm(p_trivial, false, hop_threads, nd, 1, reps, true)));
      arm1.emplace_back(nd, s.median);
      std::printf("  %-8u %10.1f %10.1f %10.1f   %s\n", nd, s.median, s.p10, s.p90,
                  nd > 1 ? std::to_string(s.median / nd).c_str() : "-");
    }

    // ---- ARM 2: N DEPENDENT dispatches (each reads what the previous wrote) ---------------------------------
    std::printf("\nARM 2  dependent dispatches (stage-to-stage pattern), ONE command buffer\n");
    std::printf("  %-8s %10s %10s %10s   %s\n", "N", "median", "p10", "p90", "device us per stage");
    for (uint nd : dispatch_counts) {
      stats s = summarize(device_series(run_arm(p_dep, true, hop_threads, nd, 8, reps, true)));
      std::printf("  %-8u %10.1f %10.1f %10.1f   %s\n", nd, s.median, s.p10, s.p90,
                  nd > 1 ? std::to_string(s.median / nd).c_str() : "-");
    }

    // ---- ARM 3: the FUSED reference: ONE dispatch, N times the work per thread ------------------------------
    std::printf("\nARM 3  ONE dispatch with N x the work per thread (the fused shape)\n");
    std::printf("  %-8s %10s %10s %10s   %s\n", "N", "median", "p10", "p90", "device us per unit of work");
    for (uint nd : dispatch_counts) {
      stats s = summarize(device_series(run_arm(p_work, false, hop_threads, 1, nd * 8, reps, true)));
      std::printf("  %-8u %10.1f %10.1f %10.1f   %.2f\n", nd, s.median, s.p10, s.p90, s.median / nd);
    }

    // ---- ARM 4: command buffer per dispatch (no CB fusion) vs one CB, same dispatch count -------------------
    std::printf("\nARM 4  host side: one CB with N dispatches vs N CBs with 1 dispatch each\n");
    std::printf("  %-8s %22s %22s\n", "N", "1 CB: device/host us", "N CBs: device/host us");
    for (uint nd : dispatch_counts) {
      stats d1 = summarize(device_series(run_arm(p_trivial, false, hop_threads, nd, 1, reps, true)));
      stats h1 = summarize(host_series(run_arm(p_trivial, false, hop_threads, nd, 1, reps, true)));
      stats dN = summarize(device_series(run_arm(p_trivial, false, hop_threads, nd, 1, reps, false)));
      stats hN = summarize(host_series(run_arm(p_trivial, false, hop_threads, nd, 1, reps, false)));
      std::printf("  %-8u %10.1f / %10.1f %10.1f / %10.1f\n", nd, d1.median, h1.median, dN.median, hN.median);
    }

    // ---- ARM 5: grid size dependence (is the dispatch cost per THREAD or per DISPATCH?) --------------------
    std::printf("\nARM 5  trivial kernel, grid size sweep, ONE dispatch per command buffer\n");
    std::printf("  %-10s %10s %10s\n", "threads", "device us", "host us");
    for (uint tcount : thread_counts) {
      std::vector<reading> v = run_arm(p_trivial, false, tcount, 1, 1, reps, true);
      stats                d = summarize(device_series(v));
      stats                h = summarize(host_series(v));
      std::printf("  %-10u %10.1f %10.1f\n", tcount, d.median, h.median);
    }

    // ---- correctness: the kernels must actually have run -----------------------------------------------
    {
      float* p = static_cast<float*>(buf.contents);
      double sum = 0.0;
      for (int i = 0; i < 64; ++i) {
        sum += p[i];
      }
      std::printf("\nsanity: first 64 floats of the output buffer sum to %.3f (the kernels ran)\n", sum);
    }

    // ---- the H1 verdict, printed rather than left to the reader -----------------------------------------
    if (arm1.size() >= 2) {
      const double t1 = arm1.front().second;
      const double t2 = arm1.back().second;
      const double n1 = arm1.front().first;
      const double n2 = arm1.back().first;
      const double slope = (t2 - t1) / (n2 - n1);
      std::printf("\nH1: marginal device cost of ONE more trivial dispatch = %.2f us "
                  "(from N=%u: %.1f us to N=%u: %.1f us)\n",
                  slope, static_cast<unsigned>(n1), t1, static_cast<unsigned>(n2), t2);
      std::printf("    against a lane hop of ~480 us (M0 baseline), that is %.2f%% per dispatch; "
                  "4 -> 1 dispatches would be worth ~%.1f us\n",
                  100.0 * slope / 480.0, 3.0 * slope);
    }
  }
  return 0;
}
