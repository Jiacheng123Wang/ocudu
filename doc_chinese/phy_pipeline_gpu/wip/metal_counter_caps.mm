// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Why the counter instrument cannot exist on this machine (2026-09-20, session 14).
//
// ---- What this answers ----
// The design document's 5.8.14 recorded a "per-dispatch GPU timestamp" instrument (MTLCounterSampleBuffer +
// sampleCountersInBuffer:atSampleIndex:withBarrier:) that was encoded at the right places, that built its
// sample buffer, and that printed nothing on the deferred route - so the first thing the next session was
// told to do was to find out WHERE the deferred submission is waited for and hang the parse hook there.
//
// It turns out the hook's position was never the (only) problem. On this device a COMPUTE encoder cannot
// sample the timestamp counter at all:
//
//     -[AGXG16XFamilyComputeContext sampleCountersInBuffer:atSampleIndex:withBarrier:]:1018:
//         failed assertion `MTLComputeCommandEncoder:sampleCountersInBuffer:atSampleIndex:withBarrier
//         not supported on this device'
//
// and the process ABORTS on that assertion (SIGABRT, exit 134) - the instrument does not merely stay
// silent, it kills the leg. The query below is the reason, and it is a device property, not a coding
// mistake: sampleCountersInBuffer requires the sampling point the call sits at, and Apple M4 Pro supports
// exactly ONE of the four - AtStageBoundary, which is about RENDER encoders between stages. There is no
// dispatch boundary and no blit boundary, so a pure compute chain has nowhere to put a sample.
//
// ---- Consequence for the design document ----
// 5.8.14's closing advice ("switch instruments: MTLCounterSampleBuffer per-dispatch GPU timestamps") is
// NOT available on this hardware and must not be re-attempted as written. The other half of that sentence -
// "or encode the measured segment into a command buffer of its own and measure it in isolation" - is the
// one that works here, because [ul_gpu_lane]'s busy split is built from MTLCommandBuffer's own
// GPUStartTime/GPUEndTime pair, which needs no counter sample buffer at all (see the session memo for the
// repeatability measurement that makes it usable).
//
// ---- Build and run ----
//     xcrun clang++ -std=c++17 -fobjc-arc -framework Metal -framework Foundation \
//         -o /tmp/metal_counter_caps wip/metal_counter_caps.mm && /tmp/metal_counter_caps
//
// Measured on Apple M4 Pro (macOS, 2026-09-20):
//     device: Apple M4 Pro
//       supportsCounterSampling:AtStageBoundary    = YES
//       supportsCounterSampling:AtDrawBoundary     = no
//       supportsCounterSampling:AtBlitBoundary     = no
//       supportsCounterSampling:AtDispatchBoundary = no
//     counterSets:
//       timestamp (1 counters) -> GPUTimestamp
//     sample buffer: built          <- a counter SET being present says nothing about a sampling POINT

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <Metal/MTLCounters.h>

#include <cstdio>

int main()
{
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) {
      std::printf("no Metal device\n");
      return 1;
    }
    std::printf("device: %s (registryID=%llu)\n",
                dev.name.UTF8String,
                static_cast<unsigned long long>(dev.registryID));

    const struct {
      const char*             name;
      MTLCounterSamplingPoint point;
    } points[] = {
        {"AtStageBoundary", MTLCounterSamplingPointAtStageBoundary},
        {"AtDrawBoundary", MTLCounterSamplingPointAtDrawBoundary},
        {"AtBlitBoundary", MTLCounterSamplingPointAtBlitBoundary},
        {"AtDispatchBoundary", MTLCounterSamplingPointAtDispatchBoundary},
    };
    for (const auto& p : points) {
      std::printf("  supportsCounterSampling:%-18s = %s\n", p.name, [dev supportsCounterSampling:p.point] ? "YES" : "no");
    }

    std::printf("counterSets:\n");
    id<MTLCounterSet> timestamp = nil;
    for (id<MTLCounterSet> set in [dev counterSets]) {
      std::printf("  %s (%lu counters)\n", set.name.UTF8String, static_cast<unsigned long>(set.counters.count));
      for (id<MTLCounter> c in set.counters) {
        std::printf("      %s\n", c.name.UTF8String);
      }
      if ([set.name isEqualToString:MTLCommonCounterSetTimestamp]) {
        timestamp = set;
      }
    }

    // The two questions are INDEPENDENT, and the failed instrument answered only the easy one: a sample
    // buffer is allocatable from a counter set the device has, whether or not any encoder may sample it.
    if (timestamp == nil) {
      std::printf("no %s counter set: even the easy question is a no\n", MTLCommonCounterSetTimestamp.UTF8String);
      return 0;
    }
    MTLCounterSampleBufferDescriptor* desc = [[MTLCounterSampleBufferDescriptor alloc] init];
    desc.counterSet                         = timestamp;
    desc.storageMode                        = MTLStorageModeShared;
    desc.sampleCount                        = 8;
    NSError*                   err          = nil;
    id<MTLCounterSampleBuffer> sb           = [dev newCounterSampleBufferWithDescriptor:desc error:&err];
    std::printf("sample buffer: %s\n", (sb != nil) ? "built" : "NIL");
  }
  return 0;
}
