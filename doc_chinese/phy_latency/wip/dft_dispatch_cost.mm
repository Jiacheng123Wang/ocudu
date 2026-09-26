// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// WHAT DOES THE REAL FRONT-END DISPATCH COST, AT THE REAL GEOMETRY? (dev doc 6.81/6.82, offline)
//
// WHY IT EXISTS. Three numbers about the same kernel disagree, and the disagreement decides whether the
// front end is where a hop's ~450us goes:
//
//   * the kernel's own comment (ocudu_dft.metal, `pad`) records 12.18us for a single-threadgroup
//     dispatch of an n=768 transform, 14 of them in ONE dispatch for 13.75us, and ~171us per slot for
//     the old one-dispatch-per-symbol front end;
//   * the bare DFT route in the replay reads 36.4-39.9us per single-transform command buffer;
//   * the air front-end command buffer, which carries fourteen transforms, is resident 452.3us (p47).
//
// 452us cannot be the 13.75us dispatch, so either the air geometry (int16 radio input, a grid write of
// 612 subcarriers, the batching tables) is much more expensive than the shape the comment measured, or
// the window is not the dispatch at all. This harness runs the REAL kernel - compiled at run time from
// the same source file the engine builds - at both geometries, and reads each command buffer's own GPU
// window. It answers the question by measurement instead of by arithmetic.
//
// HOW TO RUN (offline; it touches the GPU, so NOT while a leg is being flown):
//   clang++ -fobjc-arc -std=c++17 -O2 -framework Metal -framework Foundation \
//       -o /tmp/dft_dispatch_cost doc_chinese/phy_latency/wip/dft_dispatch_cost.mm && /tmp/dft_dispatch_cost
//
// WHAT IT PRINTS, per arm: the command buffer's window (median of 15, ten warm-up runs) and, where the
// arm batches, the implied cost per transform.
//
// HOW TO READ IT. If the air-like arm is tens of microseconds, the dispatch is cheap at that geometry
// too and the air window is a WAIT - which sends the search back to what holds the buffer (and the
// fence instrument only accounts for 9-17us of it). If the air-like arm is hundreds of microseconds,
// the front end's dispatch is the cost and the lever is inside the kernel (its int16 path, its grid
// write, or its 32 KiB threadgroup array - see MAX_FFT_N).

#import <Metal/Metal.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <atomic>
#include <thread>
#include <vector>

namespace {

constexpr unsigned nof_runs   = 15;
constexpr unsigned nof_warmup = 10;
constexpr unsigned fft_n      = 768;  ///< 23.04 MHz at 30 kHz, the delivered cell's transform
constexpr unsigned nof_subc   = 612;  ///< 51 PRB: what the grid write covers on air
constexpr unsigned max_batch  = 14;   ///< one slot's symbols

/// The engine's blocks, field for field (see ocudu_dft_metal_engine.mm's dft_grid_write_block).
struct grid_write_params {
  uint32_t active;
  uint32_t nof_subc;
  uint32_t dst_offset;
  uint32_t map_offset;
  float    phase_re;
  float    phase_im;
  uint32_t apply_window;
  uint32_t pad;
};
static_assert(sizeof(grid_write_params) == 8 * sizeof(uint32_t), "must match the kernel's struct");

struct input_params {
  uint32_t is_ci16;
  uint32_t offset;
  float    gain;
  uint32_t pad;
};
static_assert(sizeof(input_params) == 4 * sizeof(uint32_t), "must match the kernel's struct");

std::string read_file(const std::string& path)
{
  std::ifstream in(path);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

/// The kernel source, with its header include inlined (newLibraryWithSource has no include path).
bool load_kernel_source(const std::string& dir, std::string& out)
{
  const std::string header = read_file(dir + "/ocudu_dft_butterflies.h");
  std::string       source = read_file(dir + "/ocudu_dft.metal");
  if (header.empty() || source.empty()) {
    return false;
  }
  const std::string include = "#include \"ocudu_dft_butterflies.h\"";
  const auto        at      = source.find(include);
  if (at == std::string::npos) {
    return false;
  }
  source.replace(at, include.size(), header);
  // A second kernel, only for the contention arm: it occupies the device for a controlled while on
  // ANOTHER queue, which is the one shape the harness has not yet reproduced (the air GPU is shared).
  source +=
      "\nkernel void busy(device float* out [[buffer(0)]], constant uint& iters [[buffer(1)]],\n"
      "                 uint gid [[thread_position_in_grid]]) {\n"
      "  float acc = float(gid);\n"
      "  for (uint i = 0; i < iters; ++i) { acc = fma(acc, 1.000001f, 0.5f); }\n"
      "  out[gid] = acc;\n"
      "}\n";
  out = source;
  return true;
}

id<MTLComputePipelineState> make_pipeline(id<MTLDevice> device, NSString* source, NSError** error)
{
  id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:error];
  if (library == nil) {
    return nil;
  }
  return [device newComputePipelineStateWithFunction:[library newFunctionWithName:@"dft_dit"] error:error];
}

/// One arm: \p nof_dispatches command buffers, each carrying \p groups_per_dispatch threadgroups.
struct arm_result {
  double window_us = 0.0;
  double per_transform_us = 0.0;
};

arm_result run_arm(id<MTLCommandQueue>          queue,
                   id<MTLComputePipelineState>  pipeline,
                   id<MTLBuffer>                in,
                   id<MTLBuffer>                out,
                   id<MTLBuffer>                twiddle,
                   id<MTLBuffer>                perm,
                   id<MTLBuffer>                grid,
                   id<MTLBuffer>                window,
                   id<MTLBuffer>                in16,
                   id<MTLBuffer>                gw,
                   id<MTLBuffer>                ip,
                   unsigned                     groups_per_dispatch,
                   unsigned                     nof_dispatches)
{
  std::vector<double> windows;
  windows.reserve(nof_runs);
  for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
    id<MTLCommandBuffer>         cb  = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    [enc setComputePipelineState:pipeline];
    [enc setBuffer:in offset:0 atIndex:0];
    [enc setBuffer:out offset:0 atIndex:1];
    [enc setBuffer:twiddle offset:0 atIndex:2];
    [enc setBuffer:perm offset:0 atIndex:3];
    // n = 2^8 * 3^1 = 768: the mixed-radix decomposition the engine hands this kernel for the cell.
    const uint32_t radix2  = 8;
    const uint32_t radix3  = 1;
    const uint32_t inverse = 0;
    const uint32_t base    = 0;
    [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
    [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
    [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
    [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
    [enc setBuffer:grid offset:0 atIndex:8];
    [enc setBuffer:window offset:0 atIndex:9];
    [enc setBuffer:gw offset:0 atIndex:10];
    [enc setBuffer:in16 offset:0 atIndex:11];
    [enc setBuffer:ip offset:0 atIndex:12];
    for (unsigned d = 0; d != nof_dispatches; ++d) {
      [enc dispatchThreadgroups:MTLSizeMake(groups_per_dispatch, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
    }
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
  arm_result r;
  r.window_us         = windows.empty() ? 0.0 : windows[windows.size() / 2];
  const unsigned total = groups_per_dispatch * nof_dispatches;
  r.per_transform_us   = (total != 0) ? (r.window_us / static_cast<double>(total)) : 0.0;
  return r;
}

} // namespace

int main()
{
  id<MTLDevice> device = MTLCreateSystemDefaultDevice();
  if (device == nil) {
    std::printf("no Metal device\n");
    return 1;
  }
  std::string source;
  const std::string dir = "lib/phy/generic_functions/metal";
  if (!load_kernel_source(dir, source)) {
    std::printf("cannot read %s/ocudu_dft.metal (+ its header) - run from the repository root\n", dir.c_str());
    return 1;
  }
  NSError*                    error    = nil;
  id<MTLComputePipelineState> pipeline = make_pipeline(device, [NSString stringWithUTF8String:source.c_str()], &error);
  if (pipeline == nil) {
    std::printf("pipeline failed: %s\n", error.localizedDescription.UTF8String);
    return 1;
  }
  id<MTLCommandQueue> queue = [device newCommandQueue];
  id<MTLCommandQueue> other = [device newCommandQueue];

  // Buffers. The VALUES do not matter for a timing probe (twiddles are filled with the real roots, the
  // permutation with the identity: the kernel's memory traffic is the same either way).
  const size_t n = fft_n;
  id<MTLBuffer> in = [device newBufferWithLength:max_batch * n * sizeof(float) * 2
                                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> out = [device newBufferWithLength:max_batch * n * sizeof(float) * 2
                                          options:MTLResourceStorageModeShared];
  id<MTLBuffer> twiddle = [device newBufferWithLength:(n / 2) * sizeof(float) * 2
                                              options:MTLResourceStorageModeShared];
  id<MTLBuffer> perm = [device newBufferWithLength:n * sizeof(uint32_t) options:MTLResourceStorageModeShared];
  id<MTLBuffer> grid = [device newBufferWithLength:(max_batch * nof_subc) * 2 * sizeof(uint16_t)
                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> window = [device newBufferWithLength:n * sizeof(float) * 2 options:MTLResourceStorageModeShared];
  id<MTLBuffer> in16 = [device newBufferWithLength:max_batch * n * 2 * sizeof(int16_t)
                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> gw = [device newBufferWithLength:max_batch * sizeof(grid_write_params)
                                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> ip = [device newBufferWithLength:max_batch * sizeof(input_params)
                                         options:MTLResourceStorageModeShared];

  {
    auto* tw = static_cast<float*>(twiddle.contents);
    for (size_t k = 0; k != n / 2; ++k) {
      const double ang = -2.0 * M_PI * static_cast<double>(k) / static_cast<double>(n);
      tw[2 * k]        = static_cast<float>(std::cos(ang));
      tw[2 * k + 1]    = static_cast<float>(std::sin(ang));
    }
    auto* pm = static_cast<uint32_t*>(perm.contents);
    for (size_t k = 0; k != n; ++k) {
      pm[k] = static_cast<uint32_t>(k);
    }
    auto* samples = static_cast<int16_t*>(in16.contents);
    for (size_t k = 0; k != max_batch * n * 2; ++k) {
      samples[k] = static_cast<int16_t>((k % 251) - 125);
    }
  }

  /// Fills the two parameter tables for a batch of \p groups transforms.
  const auto fill_tables = [&](unsigned groups, bool ci16, bool grid_write) {
    auto* g = static_cast<grid_write_params*>(gw.contents);
    auto* i = static_cast<input_params*>(ip.contents);
    for (unsigned k = 0; k != groups; ++k) {
      g[k] = {grid_write ? 1u : 0u,
              grid_write ? nof_subc : 0u,
              static_cast<uint32_t>(k) * nof_subc,
              0u,
              1.0F,
              0.0F,
              0u,
              groups};
      i[k] = {ci16 ? 1u : 0u, static_cast<uint32_t>(k * n), 1.0F / 32767.0F, 0u};
    }
  };

  id<MTLComputePipelineState> busy_pipeline =
      [device newComputePipelineStateWithFunction:[[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                                         options:nil
                                                                           error:&error] newFunctionWithName:@"busy"]
                                            error:&error];
  if (busy_pipeline == nil) {
    std::printf("busy pipeline failed: %s\n", error.localizedDescription.UTF8String);
    return 1;
  }
  id<MTLBuffer> busy_out = [device newBufferWithLength:4096 * sizeof(float) options:MTLResourceStorageModeShared];
  const uint32_t busy_iters = 200000;

  /// Runs \p nof_cbs long dispatches on the OTHER queue, so the next measured arm is submitted while the
  /// device is already busy with somebody else's work.
  const auto start_background_load = [&](unsigned nof_cbs) {
    for (unsigned c = 0; c != nof_cbs; ++c) {
      id<MTLCommandBuffer>         cb  = [other commandBuffer];
      id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
      [enc setComputePipelineState:busy_pipeline];
      [enc setBuffer:busy_out offset:0 atIndex:0];
      [enc setBytes:&busy_iters length:sizeof(uint32_t) atIndex:1];
      [enc dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [enc endEncoding];
      [cb commit];
    }
  };

  const auto report = [&](const char* label, unsigned groups, unsigned dispatches, bool ci16, bool grid_write, unsigned background_cbs = 0) {
    fill_tables(groups, ci16, grid_write);
    if (background_cbs != 0) {
      start_background_load(background_cbs);
    }
    const arm_result r =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, groups, dispatches);
    std::printf("%-52s window=%9.1fus  per-transform=%7.1fus\n", label, r.window_us, r.per_transform_us);
  };

  std::printf("device: %s   n=%u   subcarriers=%u   median of %u runs (%u warm-up)\n",
              device.name.UTF8String,
              fft_n,
              nof_subc,
              nof_runs,
              nof_warmup);
  std::printf("%-52s %14s %16s\n", "arm", "cb window", "per transform");
  std::printf("%-52s %14s %16s\n", "---------------------------------------------------", "--------------", "--------------");
  report("1 transform,  1 dispatch  (float2, no grid)", 1, 1, false, false);
  report("14 transforms, 1 dispatch  (float2, no grid)", 14, 1, false, false);
  report("14 transforms, 14 dispatches (the old front end)", 1, 14, false, false);
  report("1 transform,  1 dispatch  (int16 + grid write)", 1, 1, true, true);
  report("14 transforms, 1 dispatch  (int16 + grid write)", 14, 1, true, true);
  report("14 transforms, 14 dispatches (int16 + grid)", 1, 14, true, true);
  std::printf("\n--- dispatch count, int16 + grid (the shape a command buffer's window follows) ---\n");
  for (unsigned d : { 2u, 4u, 7u }) {
    char label[64];
    std::snprintf(label, sizeof(label), "%u transforms, %u dispatches", d, d);
    report(label, 1, d, true, true);
  }
  std::printf("\n--- contention: the same batched arm while ANOTHER queue is busy ---\n");
  report("14 in 1 dispatch, 8 background cbs", 14, 1, true, true, 8);
  report("14 in 1 dispatch, 32 background cbs", 14, 1, true, true, 32);

  // --- the last difference between this harness and air: the input is a LIVE page mapping ---------
  //
  // On air the transform reads the radio's samples through a zero-copy mapping of memory the USB DMA
  // is still writing (the radio streams the NEXT slots into the same allocation while this one is
  // transformed). Here the same shape is reproduced: a page-aligned region wrapped with
  // newBufferWithBytesNoCopy, read as int16 by the kernel, with a host thread writing into it in a
  // loop for as long as the arm runs. If the device's reads of a mapping somebody else is writing
  // stall, the window grows; if it does not, the air number comes from somewhere else.
  std::printf("\n--- live mapping: the input read zero-copy while a writer keeps touching it ---\n");
  {
    const size_t page = 16384;
    void*        raw  = nullptr;
    if (posix_memalign(&raw, page, 1u << 20) == 0) {
      auto* samples = static_cast<int16_t*>(raw);
      for (size_t k = 0; k != (1u << 19); ++k) {
        samples[k] = static_cast<int16_t>((k % 251) - 125);
      }
      id<MTLBuffer> live = [device newBufferWithBytesNoCopy:raw
                                                     length:(1u << 20)
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
      if (live != nil) {
        std::atomic<bool> stop{false};
        std::thread       writer([&]() {
          size_t at = 0;
          while (!stop.load(std::memory_order_relaxed)) {
            // ~4 KiB every iteration at the tail of the allocation: the same pages the transform reads,
            // which is what a streaming radio does to the allocation the engine mapped.
            for (size_t k = 0; k != 2048; ++k) {
              samples[((at + k) & ((1u << 19) - 1))] = static_cast<int16_t>(k & 0x7ff);
            }
            at += 2048;
          }
        });
        fill_tables(14, true, true);
        const arm_result r =
            run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, live, gw, ip, 14, 1);
        stop.store(true, std::memory_order_relaxed);
        writer.join();
        std::printf("%-52s window=%9.1fus  per-transform=%7.1fus\n",
                    "14 in 1 dispatch, input = LIVE mapped while written",
                    r.window_us,
                    r.per_transform_us);
      } else {
        std::printf("could not wrap the live region (posix_memalign page alignment)\n");
      }
    }
  }
  return 0;
}
