// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// CAN THIS DEVICE TIME A DISPATCH? (dev doc 6.75, offline, no leg)
//
// WHY IT EXISTS. Every "is that window execution or residency" question in this workflow runs into the
// same wall: `GPUStartTime`/`GPUEndTime` are per COMMAND BUFFER, so a hop whose buffer is resident for
// ~551us while its dispatches cost ~60us cannot be split from the outside - the fence instrument (Q24)
// measures the waits the code encodes, and it says they account for ~17us of that hop. The remaining
// candidate explanations (the dispatches really are that slow / the GPU is executing somebody else's work
// in between) need timestamps INSIDE the buffer, and Metal has an API for exactly that: a counter sample
// buffer, sampled at the dispatch boundary. Whether an Apple GPU supports it is a device property, not a
// documented constant, so this probe asks the device instead of assuming.
//
// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
//   clang -fobjc-arc -framework Metal -framework Foundation -o /tmp/metal_counter_probe \
//       doc_chinese/phy_latency/wip/metal_counter_probe.mm && /tmp/metal_counter_probe
//
// WHAT THE ANSWER MEANS:
//   * timestamp counters + AtDispatchBoundary supported  -> a per-dispatch timeline of the hop's own
//     command buffer is buildable, and it settles the question directly (gaps between dispatches = time
//     the buffer was resident without executing; a dispatch that is itself hundreds of us = real compute);
//   * not supported -> the split is not measurable on this platform, and the next lever has to be chosen
//     by structure A/B (a change that moves the window, read with the instruments that already exist).

#import <Metal/Metal.h>

#include <cstdio>

namespace {

const char* yes_no(bool v)
{
  return v ? "YES" : "no";
}

void report_sampling_points(id<MTLDevice> device)
{
  std::printf("counter sampling points:\n");
  std::printf("  AtDispatchBoundary   : %s   <- the one a per-dispatch timeline needs\n",
              yes_no([device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary]));
  std::printf("  AtStageBoundary      : %s\n",
              yes_no([device supportsCounterSampling:MTLCounterSamplingPointAtStageBoundary]));
  std::printf("  AtDrawBoundary       : %s\n",
              yes_no([device supportsCounterSampling:MTLCounterSamplingPointAtDrawBoundary]));
  std::printf("  AtTileDispatchBoundary: %s\n",
              yes_no([device supportsCounterSampling:MTLCounterSamplingPointAtTileDispatchBoundary]));
  std::printf("  AtBlitBoundary       : %s\n",
              yes_no([device supportsCounterSampling:MTLCounterSamplingPointAtBlitBoundary]));
}

void report_counter_sets(id<MTLDevice> device)
{
  std::printf("counter sets:\n");
  for (NSString* name in @[ MTLCommonCounterSetTimestamp, MTLCommonCounterSetStatistic ]) {
    id<MTLCounterSet> set = nil;
    for (id<MTLCounterSet> candidate in device.counterSets) {
      if ([candidate.name isEqualToString:name]) {
        set = candidate;
        break;
      }
    }
    if (set == nil) {
      std::printf("  %-24s : absent\n", name.UTF8String);
      continue;
    }
    std::printf("  %-24s : present, counters =", name.UTF8String);
    for (id<MTLCounter> counter in set.counters) {
      std::printf(" %s", counter.name.UTF8String);
    }
    std::printf("\n");
  }
}

/// One command buffer with two dispatches and a timestamp sample between them: if the whole thing can be
/// created and read back, the timeline is not just advertised but usable in the shape this workflow needs.
bool try_timestamped_dispatch(id<MTLDevice> device)
{
  if (![device supportsCounterSampling:MTLCounterSamplingPointAtDispatchBoundary]) {
    return false;
  }
  id<MTLCounterSet> timestamp_set = nil;
  for (id<MTLCounterSet> candidate in device.counterSets) {
    if ([candidate.name isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestamp_set = candidate;
      break;
    }
  }
  if (timestamp_set == nil) {
    return false;
  }
  MTLCounterSampleBufferDescriptor* desc = [MTLCounterSampleBufferDescriptor new];
  desc.counterSet                = timestamp_set;
  desc.storageMode               = MTLStorageModeShared;
  desc.sampleCount               = 3;
  NSError*                   error = nil;
  id<MTLCounterSampleBuffer> buffer = [device newCounterSampleBufferWithDescriptor:desc error:&error];
  if (buffer == nil) {
    std::printf("  sample buffer creation failed: %s\n", error.localizedDescription.UTF8String);
    return false;
  }
  // The smallest pipeline there is: the probe only needs the ENCODER calls to be accepted.
  NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\nkernel void noop(device uint* out [[buffer(0)]],"
                     " uint i [[thread_position_in_grid]]) { out[i] = i; }\n";
  NSError*        lib_error = nil;
  id<MTLLibrary>  library   = [device newLibraryWithSource:source options:nil error:&lib_error];
  if (library == nil) {
    std::printf("  library build failed: %s\n", lib_error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"noop"]
                                                                              error:&lib_error];
  if (pipeline == nil) {
    std::printf("  pipeline build failed: %s\n", lib_error.localizedDescription.UTF8String);
    return false;
  }
  id<MTLBuffer>              out      = [device newBufferWithLength:256 options:MTLResourceStorageModeShared];
  id<MTLCommandQueue>        queue    = [device newCommandQueue];
  id<MTLCommandBuffer>       cb       = [queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [cb computeCommandEncoder];
  [encoder sampleCountersInBuffer:buffer atSampleIndex:0 withBarrier:YES];
  [encoder setComputePipelineState:pipeline];
  [encoder setBuffer:out offset:0 atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
  [encoder sampleCountersInBuffer:buffer atSampleIndex:1 withBarrier:YES];
  [encoder dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
  [encoder sampleCountersInBuffer:buffer atSampleIndex:2 withBarrier:YES];
  [encoder endEncoding];
  [cb commit];
  [cb waitUntilCompleted];
  if (cb.status != MTLCommandBufferStatusCompleted) {
    std::printf("  the timestamped command buffer did not complete (%s)\n",
                cb.error.localizedDescription.UTF8String);
    return false;
  }
  NSData* data = [buffer resolveCounterRange:NSMakeRange(0, 3)];
  if (data == nil || data.length < 3 * sizeof(MTLCounterResultTimestamp)) {
    std::printf("  resolveCounterRange returned nothing usable\n");
    return false;
  }
  const MTLCounterResultTimestamp* samples = static_cast<const MTLCounterResultTimestamp*>(data.bytes);
  std::printf("  three samples read back (ns): %llu %llu %llu  => the first dispatch took %llu ns\n",
              samples[0].timestamp,
              samples[1].timestamp,
              samples[2].timestamp,
              (samples[1].timestamp > samples[0].timestamp) ? (samples[1].timestamp - samples[0].timestamp) : 0ULL);
  return true;
}

} // namespace

int main()
{
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    std::printf("no Metal device\n");
    return 1;
  }
  std::printf("device: %s (unified memory: %s)\n", device.name.UTF8String, yes_no(device.hasUnifiedMemory));
  report_sampling_points(device);
  report_counter_sets(device);
  std::printf("usable per-dispatch timeline: %s\n", yes_no(try_timestamped_dispatch(device)));
  return 0;
}
