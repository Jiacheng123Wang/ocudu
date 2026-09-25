// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief THE 5 SECONDS OF THE UL STALLS: a Metal command buffer whose device-side event wait is not satisfied
///        when the queue reaches it is released anyway after EXACTLY 5.00 s (dev doc 6.20).
///
/// WHY IT EXISTS. Seven air legs (`p07`-`p14`) stalled the whole gNB for 5.000 +- 0.005 s at a time, and four
/// independent instruments measured that same 5 s from four sides: `input hold max = 5004.7 ms`,
/// `pop_blocking max = 4997.6 ms`, `deposit -> completion = 5004669 us` and - decisive - the lane probe's
/// `commit->start = 5002977.3 us` with `start->end = 1244.1 us`, i.e. the GPU did not START a committed command
/// buffer for 5 s and then ran it in 1.2 ms. No fence order explained it (Q9-D: every wait named a signaller
/// that had been handed out first), no host lag explained it (Q9-E: the completion handler ran 2 ms after the
/// GPU finished), and no timeout in this repository is 5 s.
///
/// WHAT IT SHOWS (measured 2026-09-25, Apple M4 Pro, macOS 26.6.2). Six arms, each on its OWN queue and its own
/// event so nothing leaks between them; the waiter (a buffer that waits for the event to reach 1) is committed
/// first in every arm but the last:
///
///   arm                                  waiter completes   commit->start   signaller
///   A1 signaller committed at +0 ms          5.001 s          5001187 us     never
///   A2 signaller committed at +200 ms        5.001 s          5000668 us     5.008 s
///   A3 signaller committed at +2000 ms       5.002 s          5001350 us     5.008 s
///   A4 signaller committed at +8000 ms       5.001 s          5001325 us     8.005 s
///   A5 NO signaller at all (event = 0)       5.000 s          5000260 us     -
///   B  signaller committed FIRST             0.206 s           484 us        0.007 s
///
/// ⇒ The wait is BOUNDED at 5.00 s by the driver and then simply LET GO (status = Completed, no error): in A5
/// the event was never signalled at all and the buffer still ran. ⇒ And until that bound expires the whole
/// queue is held: in A1-A4 the SIGNALLER could not run either, even when it had been committed 200 ms after the
/// waiter, so the producer was stuck behind the consumer for the full 5 s.
///
/// TWO CONSEQUENCES, both of which the air legs show:
///  * LATENCY: a consumer that is COMMITTED before the command buffer carrying its signal costs 5.00 s - and
///    every command buffer behind it, including the hand-over blocks whose completion releases the receive
///    buffers, waits too. That is the observed stall: the receive pool drains, the receive thread parks in
///    `pop_blocking()`, the radio's samples stop being consumed (`Real-time failure in RF: late` for every
///    slot, UHD ring overflow), no slot indication is produced, and the WHOLE slot loop freezes - downlink
///    included - until the driver's 5 s expire, after which everything catches up in a burst.
///  * CORRECTNESS: after the bound the wait is dropped, so a fence that relies on it is best-effort. In A4 the
///    consumer ran at 5.001 s while its producer only ran at 8.005 s: the ordering the fence exists for was
///    violated. A device-side wait must therefore only ever be encoded for a signal that is ALREADY COMMITTED.
///
/// HOW TO RUN (no gNB, no radio - it touches the GPU only, so it must not run while a leg is being flown):
///
///   cd doc_chinese/phy_latency/wip
///   clang++ -std=c++20 -fobjc-arc -framework Metal -framework Foundation -O1 \
///           metal_wait_timeout_probe.mm -o metal_wait_timeout_probe
///   ./metal_wait_timeout_probe          # ~50 s: each arm waits out the 5 s bound
///
/// This is a MEASUREMENT, not a fix - the fix is the commit handshake in `shared_burst` (dev doc 6.20 (3)).

#include <Foundation/Foundation.h>
#include <Metal/Metal.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <thread>

static const char* kSpin = R"MSL(
#include <metal_stdlib>
using namespace metal;
kernel void spin(device uint* out [[buffer(0)]], constant uint& iters [[buffer(1)]], uint gid [[thread_position_in_grid]])
{
  uint x = gid;
  for (uint i = 0; i < iters; ++i) { x = x * 1664525u + 1013904223u; }
  out[gid % 1024u] = x;
}
)MSL";

static double host_seconds()
{
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static id<MTLComputePipelineState> make_spin(id<MTLDevice> dev)
{
  NSError*       err = nil;
  id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSpin) options:nil error:&err];
  if (lib == nil) {
    std::printf("FAIL: spin kernel did not compile: %s\n", err.localizedDescription.UTF8String);
    return nil;
  }
  return [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"spin"] error:&err];
}

static void add_spin(id<MTLCommandBuffer> cb, id<MTLComputePipelineState> pipe, id<MTLBuffer> out)
{
  id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
  [e setComputePipelineState:pipe];
  [e setBuffer:out offset:0 atIndex:0];
  const uint32_t iters = 2000;
  [e setBytes:&iters length:sizeof(iters) atIndex:1];
  [e dispatchThreadgroups:MTLSizeMake(32, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
  [e endEncoding];
}

static void arm(id<MTLDevice>                              dev,
                id<MTLComputePipelineState>                pipe,
                id<MTLBuffer>                              out,
                const char*                                name,
                int                                        signaller_delay_ms, // 0 = never
                int                                        observe_ms,
                bool                                       signaller_first)
{
  // Own queue, own event: nothing an earlier arm did can reach this one.
  id<MTLCommandQueue> q  = [dev newCommandQueue];
  id<MTLSharedEvent>  ev = [dev newSharedEvent];

  const double t0          = host_seconds();
  auto         waiter_done = std::make_shared<std::atomic<double>>(-1.0);
  auto         sig_done    = std::make_shared<std::atomic<double>>(-1.0);
  auto         waiter_win  = std::make_shared<std::atomic<double>>(-1.0); // GPUStartTime - commit
  auto         waiter_run  = std::make_shared<std::atomic<double>>(-1.0); // GPUEndTime - GPUStartTime
  auto         waiter_st   = std::make_shared<std::atomic<long>>(-1);
  auto         sig_st      = std::make_shared<std::atomic<long>>(-1);

  const auto make_signaller = [&]() {
    id<MTLCommandBuffer> cb = [q commandBuffer];
    [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
      sig_done->store(host_seconds() - t0);
      sig_st->store((long)c.status);
    }];
    add_spin(cb, pipe, out);
    [cb encodeSignalEvent:ev value:1];
    [cb commit];
  };

  if (signaller_first) {
    make_signaller();
    std::this_thread::sleep_for(std::chrono::milliseconds(signaller_delay_ms));
  }

  id<MTLCommandBuffer> waiter = [q commandBuffer];
  const double         commit_s = host_seconds();
  [waiter addCompletedHandler:^(id<MTLCommandBuffer> c) {
    waiter_done->store(host_seconds() - t0);
    waiter_st->store((long)c.status);
    if (c.GPUStartTime > 0.0) {
      waiter_win->store((c.GPUStartTime - commit_s) * 1e6); // us, commit -> GPU start
      waiter_run->store((c.GPUEndTime - c.GPUStartTime) * 1e6);
    }
  }];
  [waiter encodeWaitForEvent:ev value:1];
  add_spin(waiter, pipe, out);
  [waiter commit];

  if (!signaller_first && (signaller_delay_ms != 0)) {
    std::thread([make_signaller, signaller_delay_ms]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(signaller_delay_ms));
      make_signaller();
    }).detach();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(observe_ms));
  // NOTE the units: the *_done values are SECONDS (host_seconds() - t0); commit->start and run are us.
  std::printf("%-30s waiter:%7.3f s (st=%ld, commit->start=%9.1f us, run=%7.1f us)   signaller:%7.3f s (st=%ld)   event=%llu\n",
              name,
              waiter_done->load(),
              waiter_st->load(),
              waiter_win->load(),
              waiter_run->load(),
              sig_done->load(),
              sig_st->load(),
              (unsigned long long)ev.signaledValue);
  std::fflush(stdout);
  (void)q;
  (void)ev;
}

int main()
{
  id<MTLDevice>               dev  = MTLCreateSystemDefaultDevice();
  id<MTLBuffer>               out  = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
  id<MTLComputePipelineState> pipe = make_spin(dev);
  if ((dev == nil) || (out == nil) || (pipe == nil)) {
    std::printf("FAIL: could not build the Metal objects\n");
    return 1;
  }
  std::printf("device=%s\n", dev.name.UTF8String);
  std::printf("(waiter is committed FIRST in every arm except the last; 'signaller: -1' = never committed)\n");

  arm(dev, pipe, out, "A1 signaller at +0 ms (same instant)", 0, 7000, false);
  arm(dev, pipe, out, "A2 signaller at +200 ms", 200, 7000, false);
  arm(dev, pipe, out, "A3 signaller at +2000 ms", 2000, 8000, false);
  arm(dev, pipe, out, "A4 signaller at +8000 ms", 8000, 14000, false);
  arm(dev, pipe, out, "A5 NO signaller", 0, 8000, false);
  arm(dev, pipe, out, "B  signaller FIRST", 200, 3000, true);
  return 0;
}
