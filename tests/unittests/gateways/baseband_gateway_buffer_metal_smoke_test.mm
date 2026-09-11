// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Metal zero-copy smoke test of baseband_gateway_buffer_dynamic_aligned: wraps the
/// page-aligned storage with MTLDevice::newBufferWithBytesNoCopy (Shared) and verifies a
/// trivial GPU kernel reads exactly the bytes the CPU wrote (no copy, no conversion).
/// Apple platforms only; the target is not built elsewhere.

#include <gtest/gtest.h>

#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "ocudu/gateways/baseband/buffer/baseband_gateway_buffer_dynamic.h"
#include "ocudu/support/macos_compat.h"
#include <cstdint>

using namespace ocudu;

namespace {

// ci16_t is std::complex<int16_t>: 2 x int16, byte-identical to the Metal short2 type.
static const char* kSumKernel = R"METAL(
#include <metal_stdlib>
using namespace metal;
kernel void smoke_sum(device const short2* in  [[buffer(0)]],
                      device long2*        out [[buffer(1)]],
                      constant uint&       n   [[buffer(2)]])
{
    long2 s = 0;
    for (uint i = 0; i < n; ++i) {
        s.x += in[i].x;
        s.y += in[i].y;
    }
    out[0] = s;
}
)METAL";

} // namespace

TEST(baseband_gateway_buffer_metal_smoke_test, zero_copy_wrap_and_gpu_read)
{
  // 2 channels x 512 samples.
  baseband_gateway_buffer_dynamic_aligned buffer(2, 512);

  // Deterministic pattern + CPU reference sums.
  int64_t re_sum_ref = 0;
  int64_t im_sum_ref = 0;
  for (unsigned i_channel = 0; i_channel != buffer.get_nof_channels(); ++i_channel) {
    span<ci16_t> channel = buffer[i_channel];
    for (unsigned i = 0; i != channel.size(); ++i) {
      const int16_t re = static_cast<int16_t>(((i_channel + 1) * (i + 1) * 7) % 30000);
      const int16_t im = static_cast<int16_t>(-(((i_channel + 1) * (i + 3) * 11) % 30000));
      channel[i]        = ci16_t(re, im);
      re_sum_ref += re;
      im_sum_ref += im;
    }
  }

  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  ASSERT_NE(device, nil);

  // The zero-copy wrap under test: page-aligned base + page-multiple length.
  id<MTLBuffer> mtl_buffer = [device newBufferWithBytesNoCopy:buffer.data()
                                                      length:buffer.capacity_bytes()
                                                     options:MTLResourceStorageModeShared
                                                 deallocator:nil];
  ASSERT_NE(mtl_buffer, nil);

  // Runtime-compiled trivial kernel (tests only; the hot path uses precompiled metallibs).
  NSError*  error   = nil;
  NSString* src_str = [NSString stringWithUTF8String:kSumKernel];
  id<MTLLibrary> library = [device newLibraryWithSource:src_str options:nil error:&error];
  ASSERT_NE(library, nil);
  id<MTLFunction> fn = [library newFunctionWithName:@"smoke_sum"];
  ASSERT_NE(fn, nil);
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:fn error:&error];
  ASSERT_NE(pipeline, nil);

  id<MTLBuffer> out_buffer = [device newBufferWithLength:sizeof(int64_t) * 2
                                                 options:MTLResourceStorageModeShared];
  ASSERT_NE(out_buffer, nil);

  id<MTLCommandQueue>        queue   = [device newCommandQueue];
  id<MTLCommandBuffer>       cmd_buf = [queue commandBuffer];
  id<MTLComputeCommandEncoder> enc   = [cmd_buf computeCommandEncoder];
  const uint32_t n = buffer.get_nof_samples() * buffer.get_nof_channels();
  [enc setComputePipelineState:pipeline];
  [enc setBuffer:mtl_buffer offset:0 atIndex:0];
  [enc setBuffer:out_buffer offset:0 atIndex:1];
  [enc setBytes:&n length:sizeof(n) atIndex:2];
  [enc dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
  [enc endEncoding];
  [cmd_buf commit];
  [cmd_buf waitUntilCompleted];
  ASSERT_EQ(cmd_buf.status, MTLCommandBufferStatusCompleted);

  // The GPU must have read exactly the bytes the CPU wrote: no copy, no conversion.
  const int64_t* gpu_sum = static_cast<const int64_t*>(out_buffer.contents);
  EXPECT_EQ(gpu_sum[0], re_sum_ref);
  EXPECT_EQ(gpu_sum[1], im_sum_ref);

  // Sanity: the storage was not modified by the GPU read.
  int64_t re_sum_after = 0;
  for (unsigned i_channel = 0; i_channel != buffer.get_nof_channels(); ++i_channel) {
    span<const ci16_t> channel = buffer[i_channel];
    for (unsigned i = 0; i != channel.size(); ++i) {
      re_sum_after += channel[i].real();
    }
  }
  EXPECT_EQ(re_sum_after, re_sum_ref);
}

#endif // defined(__APPLE__)
