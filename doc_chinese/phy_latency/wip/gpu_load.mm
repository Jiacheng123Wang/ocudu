// An EXTERNAL GPU load, for the experiment in dev doc 6.150 (5) (a): does a Metal user that has nothing to do with
// the gNB reproduce the multi-millisecond transport stalls that a gNB leg sees only when its own PHY has a Metal
// path in it (cpu 0.002% of receive calls >1 ms, cpu_gpu 0.118%, gpu 0.058-0.171%)?
//
// TWO MODES, because the archive already narrowed the mechanism once and they tell different stories:
//   empty - dispatch a kernel whose body does nothing, in the tightest possible commit -> waitUntilCompleted loop.
//           This exercises the Metal RUNTIME (command buffer creation, submission, completion, the driver's
//           bookkeeping) with ~no GPU execution and ~no memory traffic. The 1-in-8 ablation legs of 6.141, whose
//           kernels are nearly empty but which submit the same command buffers, read the same stall rate as the
//           full ones - so if a load like this reproduces the stalls, the mechanism is the runtime being in the
//           loop, not the GPU computing.
//   heavy - a kernel that burns ALU and writes memory, dispatched over many thread groups, in the same loop. If
//           only THIS mode reproduces them, the mechanism is contention for the memory subsystem / the GPU itself.
//
// It is deliberately a standalone tool with no repo dependencies: the point is that the gNB knows nothing about it.
//
// usage: gpu_load <seconds> [empty|heavy]        (prints its dispatch rate once a second)
// build: xcrun -sdk macosx clang++ -fobjc-arc -O2 -framework Metal -framework Foundation -o gpu_load gpu_load.mm

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

static NSString* const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;

// empty: never taken, so the dispatch costs submission and nothing else.
kernel void noop_kernel(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]])
{
  if (i == 0xffffffffu) {
    out[0] = 1u;
  }
}

// heavy: ALU + shared memory + a scattered store per thread, enough to keep the cores and the memory system busy.
kernel void heavy_kernel(device uint* out [[buffer(0)]], uint i [[thread_position_in_grid]])
{
  threadgroup float buf[256];
  const uint       lane = i % 256u;
  float            acc  = float(lane) * 1.0009765625f;
  buf[lane]             = acc;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  for (uint k = 0; k != 4096u; ++k) {
    acc = fma(acc, 1.00000011920928955078125f, buf[(lane + k) % 256u]);
  }
  buf[lane] = acc;
  threadgroup_barrier(mem_flags::mem_threadgroup);
  out[i % 1048576u] = as_type<uint>(buf[(lane + 1u) % 256u]);
}
)";

int main(int argc, char** argv)
{
  const double seconds = (argc > 1) ? std::atof(argv[1]) : 60.0;
  const std::string mode = (argc > 2) ? argv[2] : "empty";
  const bool        heavy = (mode == "heavy");
  if (!heavy && mode != "empty") {
    std::fprintf(stderr, "usage: %s <seconds> [empty|heavy]\n", argv[0]);
    return 2;
  }

  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) {
      std::fprintf(stderr, "gpu_load: no Metal device\n");
      return 1;
    }
    NSError*        err = nil;
    id<MTLLibrary>  lib = [dev newLibraryWithSource:kSrc options:nil error:&err];
    if (lib == nil) {
      std::fprintf(stderr, "gpu_load: kernel compile failed: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLFunction>             fn   = [lib newFunctionWithName:(heavy ? @"heavy_kernel" : @"noop_kernel")];
    id<MTLComputePipelineState> pipe = [dev newComputePipelineStateWithFunction:fn error:&err];
    if (pipe == nil) {
      std::fprintf(stderr, "gpu_load: pipeline failed: %s\n", err.localizedDescription.UTF8String);
      return 1;
    }
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLBuffer>       out   = [dev newBufferWithLength:(1u << 22) options:MTLResourceStorageModePrivate];

    // empty: one tiny thread group; heavy: enough groups to fill the machine (20 GPU cores on this M4 Pro, see
    // dev doc 6.143 (8)), 256 threads each - the same shape the real kernels use.
    const NSUInteger groups = heavy ? 1024 : 1;
    const NSUInteger width  = heavy ? 256 : 64;

    std::printf("gpu_load: mode=%s seconds=%.0f device=%s groups=%lu width=%lu\n",
                mode.c_str(),
                seconds,
                dev.name.UTF8String,
                (unsigned long)groups,
                (unsigned long)width);

    const auto t0      = std::chrono::steady_clock::now();
    auto       t_print = t0;
    uint64_t   iters   = 0;
    uint64_t   since   = 0;
    while (true) {
      @autoreleasepool {
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipe];
        [enc setBuffer:out offset:0 atIndex:0];
        [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(width, 1, 1)];
        [enc endEncoding];
        [cb commit];
        // Waited for on purpose: the experiment is about the runtime path being active in the process, and this is
        // what keeps the submission rate bounded and observable (an un-waited loop would just queue up and die).
        [cb waitUntilCompleted];
      }
      ++iters;
      ++since;
      const auto now = std::chrono::steady_clock::now();
      if (std::chrono::duration<double>(now - t_print).count() >= 1.0) {
        std::printf("gpu_load: %.0fs  %llu dispatch(es)/s (total %llu)\n",
                    std::chrono::duration<double>(now - t0).count(),
                    (unsigned long long)since,
                    (unsigned long long)iters);
        std::fflush(stdout);
        since   = 0;
        t_print = now;
      }
      if (std::chrono::duration<double>(now - t0).count() >= seconds) {
        break;
      }
    }
    std::printf("gpu_load: done, %llu dispatch(es) in %.1fs\n",
                (unsigned long long)iters,
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  return 0;
}
