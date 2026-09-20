// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Q: does `memoryBarrierWithScope:MTLBarrierScopeBuffers` order a WRITE made through one MTLBuffer
//    against a READ of the same memory made through ANOTHER MTLBuffer that covers it?
//
// Why it matters here: the MMSE engine's zero-copy cache is keyed by POINTER, so the merged batch's
// edge group - whose A slot starts 11664 bytes into gpu_a (correlation_stage(): sys_offset * stride^2)
// - gets a SECOND MTLBuffer object over memory the batch's own mapping already covers. The fused
// route (OCUDU_CE_EDGE_FUSE=1) is the first route that WRITES through one of those objects and READS
// through the other INSIDE one command buffer, with one `memoryBarrierWithScope` between them, and it
// computes NaN there while the standalone route (same kernels, same slots, but the writer in its own
// command buffer, host-waited) is byte-identical to the host build.
//
// The experiment is the mechanism in isolation: a writer kernel and a reader kernel in ONE command
// buffer with one barrier between them, four ways:
//
//   A same-object   - writer and reader both bind the WHOLE buffer            (control: must pass)
//   B slice-write   - writer binds the PAGE-ALIGNED slice, reader the whole    (the engine's shape)
//   C interior      - writer binds base + 11664 (NOT page-aligned), reader the whole
//   D slice-read    - writer binds the whole, reader the page-aligned slice    (the other direction)
//   E interior-read - writer binds the whole, reader binds base + 11664
//
// and, because S13-P2 asks the second question - "may the two stages be two ENCODERS of one command
// buffer, or must they share one encoder?":
//
//   F two encoders  - same object, writer in the FIRST encoder, reader in the SECOND encoder of one
//                     command buffer (the shape a "hold the extraction's buffer open for the weights"
//                     would produce)
//   G two enc., alias - same, but the writer binds the page-aligned slice (an aliased pair)
//
// Each case runs `repeat` times in one process. "PASS" = the reader read the value the writer wrote
// (1.0) in every repetition; "FAIL" = it read the pre-write content (0.0).
//
// Build/run:  clang++ -std=c++17 -fobjc-arc -O2 -framework Metal -framework Foundation \
//                     metal_alias_order.mm -o /tmp/metal_alias_order && /tmp/metal_alias_order
#include <Foundation/Foundation.h>
#include <Metal/Metal.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

static const char* kSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void probe_write(device float* dst [[buffer(0)]], constant uint& idx [[buffer(1)]])
{
  dst[idx] = 1.0F;
}

kernel void probe_read(device const float* src [[buffer(0)]], device float* out [[buffer(1)]], constant uint& idx [[buffer(2)]])
{
  out[0] = src[idx];
}
)MSL";

struct case_result {
  const char* name;
  bool        pass;
  float       observed;
};

int main()
{
  @autoreleasepool {
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    if (dev == nil) {
      std::fprintf(stderr, "no Metal device\n");
      return 2;
    }
    NSError* err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:@(kSource) options:nil error:&err];
    if (lib == nil) {
      std::fprintf(stderr, "library: %s\n", err.localizedDescription.UTF8String);
      return 2;
    }
    id<MTLFunction> wfn = [lib newFunctionWithName:@"probe_write"];
    id<MTLFunction> rfn = [lib newFunctionWithName:@"probe_read"];
    id<MTLComputePipelineState> wpipe = [dev newComputePipelineStateWithFunction:wfn error:&err];
    id<MTLComputePipelineState> rpipe = [dev newComputePipelineStateWithFunction:rfn error:&err];
    id<MTLCommandQueue>         queue = [dev newCommandQueue];

    // One page-aligned allocation, exactly like the engine's staging buffers (alloc_aligned).
    const NSUInteger page = 4096;
    const NSUInteger whole_bytes = 8 * page;
    void*            raw         = nullptr;
    if (posix_memalign(&raw, page, whole_bytes) != 0) {
      return 2;
    }
    std::memset(raw, 0, whole_bytes);

    // The two views the engine creates for one allocation: the whole thing (the batch) and an
    // interior slice that starts at a NON-page-aligned offset (the merged batch's edge group).
    const NSUInteger interior_off = 11664; // 54 * 54 * sizeof(float): one A slot, exactly the engine's
    id<MTLBuffer> buf_whole = [dev newBufferWithBytesNoCopy:raw length:whole_bytes options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> buf_page  = [dev newBufferWithBytesNoCopy:(char*)raw + page length:whole_bytes - page options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> buf_inner = [dev newBufferWithBytesNoCopy:(char*)raw + interior_off length:whole_bytes - interior_off options:MTLResourceStorageModeShared deallocator:nil];
    id<MTLBuffer> buf_out   = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
    if (buf_whole == nil || buf_page == nil || buf_inner == nil) {
      std::fprintf(stderr, "wrap failed: whole=%p page=%p inner=%p\n", buf_whole, buf_page, buf_inner);
      return 2;
    }
    std::fprintf(stderr,
                 "[alias] whole=%p page=%p inner=%p | whole.length=%llu page.length=%llu inner.length=%llu\n",
                 buf_whole.contents,
                 buf_page.contents,
                 buf_inner.contents,
                 (unsigned long long)buf_whole.length,
                 (unsigned long long)buf_page.length,
                 (unsigned long long)buf_inner.length);

    const uint32_t idx = 64; // a float inside the page-aligned slice AND inside the interior slice
    const unsigned repeat = 200;
    std::vector<case_result> results;

    const auto run_case = [&](const char* name, id<MTLBuffer> writer_view, id<MTLBuffer> reader_view) {
      unsigned fails = 0;
      float    last  = -1.0F;
      for (unsigned it = 0; it != repeat; ++it) {
        std::memset(raw, 0, whole_bytes);
        float* out = (float*)buf_out.contents;
        out[0]     = -1.0F;
        @autoreleasepool {
          id<MTLCommandBuffer>         cb  = [queue commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:wpipe];
          [enc setBuffer:writer_view offset:0 atIndex:0];
          [enc setBytes:&idx length:sizeof(idx) atIndex:1];
          [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
          // THE ONE THING UNDER TEST: exactly the barrier the engine encodes between a correlation
          // prefix and the inversion (encode_run: `if (!st.burst) { [enc memoryBarrierWithScope:
          // MTLBarrierScopeBuffers]; }`).
          [enc memoryBarrierWithScope:MTLBarrierScopeBuffers];
          [enc setComputePipelineState:rpipe];
          [enc setBuffer:reader_view offset:0 atIndex:0];
          [enc setBuffer:buf_out offset:0 atIndex:1];
          [enc setBytes:&idx length:sizeof(idx) atIndex:2];
          [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
        }
        last = out[0];
        if (last != 1.0F) {
          ++fails;
        }
      }
      results.push_back({name, fails == 0, last});
    };

    run_case("A same-object (write whole, read whole)", buf_whole, buf_whole);
    run_case("B page-aligned slice write, whole read", buf_page, buf_whole);
    run_case("C interior (base+11664) write, whole read", buf_inner, buf_whole);
    run_case("D whole write, page-aligned slice read", buf_whole, buf_page);
    run_case("E whole write, interior (base+11664) read", buf_whole, buf_inner);

    // ---- F/G: two ENCODERS of one command buffer, with no barrier between them -------------------
    // The extraction / weights split (S13-P2) would put the two stages in one command buffer; whether
    // they may be two encoders or must share one is a property of Metal, not of the engine.
    const auto run_two_encoder_case = [&](const char* name, id<MTLBuffer> writer_view, id<MTLBuffer> reader_view) {
      unsigned fails = 0;
      float    last  = -1.0F;
      for (unsigned it = 0; it != repeat; ++it) {
        std::memset(raw, 0, whole_bytes);
        float* out = (float*)buf_out.contents;
        out[0]     = -1.0F;
        @autoreleasepool {
          id<MTLCommandBuffer> cb = [queue commandBuffer];
          {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:wpipe];
            [enc setBuffer:writer_view offset:0 atIndex:0];
            [enc setBytes:&idx length:sizeof(idx) atIndex:1];
            [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
          }
          {
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:rpipe];
            [enc setBuffer:reader_view offset:0 atIndex:0];
            [enc setBuffer:buf_out offset:0 atIndex:1];
            [enc setBytes:&idx length:sizeof(idx) atIndex:2];
            [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [enc endEncoding];
          }
          [cb commit];
          [cb waitUntilCompleted];
        }
        last = out[0];
        if (last != 1.0F) {
          ++fails;
        }
      }
      results.push_back({name, fails == 0, last});
    };
    run_two_encoder_case("F two encoders, one object", buf_whole, buf_whole);
    run_two_encoder_case("G two encoders, aliased pair", buf_page, buf_whole);

    std::fprintf(stderr, "[alias] %u repetitions per case, barrier = memoryBarrierWithScope(Buffers)\n", repeat);
    for (const case_result& r : results) {
      std::fprintf(stderr, "[alias] %-46s %s (last read = %.1f)\n", r.name, r.pass ? "PASS" : "FAIL", r.observed);
    }
    return 0;
  }
}
