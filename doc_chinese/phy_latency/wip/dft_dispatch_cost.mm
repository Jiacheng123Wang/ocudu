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
constexpr unsigned max_batch  = 14;   ///< one slot's symbols (what the engine batches by default)
/// The buffers are sized for MORE than one slot so the parallelism sweep can push the grid past the
/// device's core count and find where the transforms stop fitting (each transform is one threadgroup of
/// 32 KiB of threadgroup memory, so the cliff is a hardware number, not a software one).
constexpr unsigned alloc_batch = 32;

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
      "}\n"
      // Cache thrash: sweeps a buffer far larger than any cache, so the NEXT measured command buffer
      // meets the same cold state every hop meets on air (where the working set is evicted by whatever
      // else the device ran in between, and the harness's tight loop keeps everything hot instead).
      "\nkernel void thrash(device uint* buf [[buffer(0)]], constant uint& n [[buffer(1)]],\n"
      "                   uint gid [[thread_position_in_grid]], uint stride [[threads_per_grid]]) {\n"
      "  uint acc = gid;\n"
      "  for (uint i = gid; i < n; i += stride) { acc = acc * 1664525u + buf[i]; buf[i] = acc; }\n"
      "}\n"
      // --- the THREADGROUP-MEMORY competitor (2026-09-27) ---------------------------------------------
      // WHY IT EXISTS. The air front end's packed dispatch (14 threadgroups, one transform each) is
      // resident 452us on air and 46.9us here, and the difference is NOT the dispatch count: it is that
      // the 14 groups run one after another on air and all at once here (air: 452/14 = 32.3us per
      // group, which is exactly the single group's own work; here: 46.9us for all fourteen). The DFT
      // kernel declares `threadgroup float2 buf[MAX_FFT_N]` = 32 KiB PER THREADGROUP, i.e. the device's
      // whole per-core threadgroup budget, so a dispatch of 14 groups needs 14 cores' worth of it at
      // once. Every contention arm so far used a kernel that declares NO threadgroup memory (the spin
      // `busy` above), i.e. it competed for cores but not for the resource the DFT actually needs - and
      // §6.101(1)'s "200 concurrent cbs cost +0.5..+3.5us" was measured with that kernel, or with the
      // DFT itself. This kernel declares the SAME 32 KiB per group and does the same spin, so a
      // background cb of 16 such groups covers the whole device's threadgroup memory.
      //
      // HOW TO READ IT. If the packed arm inflates towards 14 x 32us while this streams, threadgroup
      // memory (not cores, not clock) is the currency the air path is short of, and the search moves to
      // WHO holds it on air. If it stays ~47us, the resource is not threadgroup memory and the air
      // mechanism is somewhere else entirely.
      "\nkernel void busy_tg(device float* out [[buffer(0)]], constant uint& iters [[buffer(1)]],\n"
      "                    uint gid [[thread_position_in_grid]], uint tid [[thread_position_in_threadgroup]]) {\n"
      "  threadgroup float2 pad[4096];\n"
      "  pad[tid] = float2(float(tid), 0.5f);\n"
      "  threadgroup_barrier(mem_flags::mem_threadgroup);\n"
      "  float acc = pad[tid & 4095u].x;\n"
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

/// \param fresh_wrap When set, a NEW MTLBuffer object is created over \p wrap_region before every
///        measured run and dropped after it - the shape of a per-hop re-wrap of the radio's samples,
///        which the legs show happening ~1.7 times per hop (wrap creates=244k, purges=265k). Creating
///        the object once and reusing it, as every other arm here does, is the shape the engine has when
///        its cache HITS.
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
                   unsigned                     nof_dispatches,
                   void*                        wrap_region   = nullptr,
                   size_t                       wrap_len      = 0,
                   bool                         fresh_wrap    = false,
                   unsigned                     open_hold_us  = 0,
                   bool                         commit_off_thread = false,
                   id<MTLComputePipelineState>  thrash_pipeline = nil,
                   id<MTLBuffer>                thrash_buf = nil,
                   uint32_t                     thrash_n = 0,
                   id<MTLSharedEvent>           signal_event = nil,
                   unsigned                     close_hold_us = 0)
{
  std::vector<double> windows;
  windows.reserve(nof_runs);
  for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
    if (thrash_pipeline != nil) {
      // Cold the caches: sweep a buffer much larger than L2 and wait for it, so the measured buffer below
      // cannot be found warm. Nothing else about the measured run changes.
      id<MTLCommandBuffer>         tcb  = [queue commandBuffer];
      id<MTLComputeCommandEncoder> tenc = [tcb computeCommandEncoder];
      [tenc setComputePipelineState:thrash_pipeline];
      [tenc setBuffer:thrash_buf offset:0 atIndex:0];
      [tenc setBytes:&thrash_n length:sizeof(uint32_t) atIndex:1];
      [tenc dispatchThreads:MTLSizeMake(1u << 16, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [tenc endEncoding];
      [tcb commit];
      [tcb waitUntilCompleted];
    }
    id<MTLBuffer> bound_in16 = in16;
    if (fresh_wrap && (wrap_region != nullptr)) {
      // A fresh OBJECT over the SAME pages: what a re-wrap does (newBufferWithBytesNoCopy, no copy).
      bound_in16 = [queue.device newBufferWithBytesNoCopy:wrap_region
                                             length:wrap_len
                                            options:MTLResourceStorageModeShared
                                        deallocator:nil];
      if (bound_in16 == nil) {
        std::printf("fresh wrap refused\n");
        break;
      }
    }
    id<MTLCommandBuffer>         cb  = [queue commandBuffer];
    id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
    if (open_hold_us != 0) {
      // The front end's block is opened at the slot's FIRST symbol and released at its last one, so the
      // encoder is open for about a slot before the commit (see ofdm_demodulator_impl::finish_symbol).
      std::this_thread::sleep_for(std::chrono::microseconds(open_hold_us));
    }
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
    [enc setBuffer:bound_in16 offset:0 atIndex:11];
    [enc setBuffer:ip offset:0 atIndex:12];
    for (unsigned d = 0; d != nof_dispatches; ++d) {
      [enc dispatchThreadgroups:MTLSizeMake(groups_per_dispatch, 1, 1)
          threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
    }
    [enc endEncoding];
    if (close_hold_us != 0) {
      // THE HAND-OVER'S OTHER ORDER (2026-09-27, dev doc 6.127): the front end's block is created at the
      // slot's FIRST symbol, the transforms are dispatched when the batch fills (the slot's LAST symbol),
      // and the buffer is then handed over UNCOMMITTED - the lane commits it only when the hop reaches the
      // estimator. So on air the dispatch is encoded FIRST and the commit comes ~hundreds of microseconds
      // later, while every arm here encoded and committed back to back. `open_hold_us` above holds the
      // encoder open BEFORE the dispatch is encoded; this holds the CLOSED buffer before the commit, which
      // is the air order. If a buffer that sits encoded and uncommitted costs the device-side wait the legs
      // show (~390us once per hop, independent of dispatches and threadgroups, absent on the plain route),
      // this arm is where it appears - and if it does not, the air constant is not a submission-order cost.
      std::this_thread::sleep_for(std::chrono::microseconds(close_hold_us));
    }
    if (signal_event != nil) {
      // The D1 hand-over signals a shared event on the block it releases (the replacement waits on it),
      // which every plain-route buffer - the PRACH ones that read 47.8us on air - does not do.
      static std::atomic<uint64_t> gen{0};
      [cb encodeSignalEvent:signal_event value:gen.fetch_add(1, std::memory_order_relaxed) + 1];
    }
    if (commit_off_thread) {
      // The hand-over commits the block on the LANE's thread - the encoder ran on the receiving one.
      std::thread committer([&]() {
        [cb commit];
        [cb waitUntilCompleted];
      });
      committer.join();
    } else {
      [cb commit];
      [cb waitUntilCompleted];
    }
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
  id<MTLBuffer> in = [device newBufferWithLength:alloc_batch * n * sizeof(float) * 2
                                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> out = [device newBufferWithLength:alloc_batch * n * sizeof(float) * 2
                                          options:MTLResourceStorageModeShared];
  id<MTLBuffer> twiddle = [device newBufferWithLength:(n / 2) * sizeof(float) * 2
                                              options:MTLResourceStorageModeShared];
  id<MTLBuffer> perm = [device newBufferWithLength:n * sizeof(uint32_t) options:MTLResourceStorageModeShared];
  id<MTLBuffer> grid = [device newBufferWithLength:(alloc_batch * nof_subc) * 2 * sizeof(uint16_t)
                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> window = [device newBufferWithLength:n * sizeof(float) * 2 options:MTLResourceStorageModeShared];
  id<MTLBuffer> in16 = [device newBufferWithLength:alloc_batch * n * 2 * sizeof(int16_t)
                                           options:MTLResourceStorageModeShared];
  id<MTLBuffer> gw = [device newBufferWithLength:alloc_batch * sizeof(grid_write_params)
                                         options:MTLResourceStorageModeShared];
  id<MTLBuffer> ip = [device newBufferWithLength:alloc_batch * sizeof(input_params)
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
    for (size_t k = 0; k != alloc_batch * n * 2; ++k) {
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
  // --- PARALLELISM: how many transforms does the device run AT ONCE? --------------------------------
  //
  // The front end's whole point is that a slot's transforms are independent, so the engine encodes them as
  // ONE dispatch of N threadgroups (one transform each) and the batch cap is the slot's symbol count. What
  // that buys depends on how many of those threadgroups the device actually holds at once: the window stays
  // FLAT while they all fit and steps up once they do not. Each threadgroup needs 32 KiB of threadgroup
  // memory (the kernel's `threadgroup float2 buf[4096]`), so the cliff is where the cores run out.
  std::printf("\n--- how many transforms run CONCURRENTLY (one dispatch, one threadgroup each) ---\n");
  for (unsigned g : { 1u, 2u, 4u, 7u, 10u, 14u, 16u, 20u, 24u, 28u, 32u }) {
    char label[80];
    std::snprintf(label, sizeof(label), "%2u transforms in ONE dispatch (int16 + grid)", g);
    report(label, g, 1, true, true);
  }

  std::printf("\n--- dispatch count, int16 + grid (the shape a command buffer's window follows) ---\n");
  for (unsigned d : { 2u, 4u, 7u }) {
    char label[64];
    std::snprintf(label, sizeof(label), "%u transforms, %u dispatches", d, d);
    report(label, 1, d, true, true);
  }
  std::printf("\n--- contention: the same batched arm while ANOTHER queue is busy ---\n");
  report("14 in 1 dispatch, 8 background cbs", 14, 1, true, true, 8);
  report("14 in 1 dispatch, 32 background cbs", 14, 1, true, true, 32);

  // --- the one thing the PRACH buffers (47.8us on air, like the harness) do NOT do: signal an event --
  // --- how long the device sat IDLE before the buffer: the air steady state, or a clock state? --------
  //
  // Every offline arm here keeps the device busy (fifteen runs back to back after ten warm-up runs) and
  // reads 13-58us. Air reads 452us for the same work while its device is mostly idle (busy(union)/window
  // is 27-29% on the probed queues). If a command buffer's cost depends on how long the device has been
  // idle, that whole family of air numbers is a CLOCK state rather than a structure.
  std::printf("\n--- idle before the dispatch: 0 / 1ms / 50ms / 500ms / 2s ---\n");
  fill_tables(14, true, true);
  for (unsigned idle_us : { 0u, 1000u, 50000u, 500000u, 2000000u }) {
    const arm_result r = run_arm(
        queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, idle_us, false);
    char label[64];
    std::snprintf(label, sizeof(label), "idle %u us before the commit", idle_us);
    std::printf("%-52s window=%9.1fus\n", label, r.window_us);
  }

  std::printf("\n--- the hand-over's signal on the released block ---\n");
  {
    id<MTLSharedEvent> ev = [device newSharedEvent];
    fill_tables(14, true, true);
    const arm_result plain = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
    const arm_result signalled = run_arm(
        queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 0, false, nil, nil, 0, ev);
    std::printf("%-52s window=%9.1fus\n", "14 in 1 dispatch, no signal (harness default)", plain.window_us);
    std::printf("%-52s window=%9.1fus\n", "14 in 1 dispatch, signals a shared event (D1 shape)", signalled.window_us);
  }

  // --- cold caches: the shape air has and a tight benchmark loop does not --------------------------
  std::printf("\n--- cold caches: a cache-sweeping command buffer before every measured run ---\n");
  {
    id<MTLComputePipelineState> thrash_pipeline =
        [device newComputePipelineStateWithFunction:[[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                                           options:nil
                                                                             error:&error] newFunctionWithName:@"thrash"]
                                              error:&error];
    if (thrash_pipeline == nil) {
      std::printf("thrash pipeline failed: %s\n", error.localizedDescription.UTF8String);
    } else {
      const uint32_t sweep_words = 64u << 20; // 256 MiB, far beyond any cache
      id<MTLBuffer>  thrash_buf  = [device newBufferWithLength:sweep_words * sizeof(uint32_t)
                                                      options:MTLResourceStorageModePrivate];
      fill_tables(14, true, true);
      const arm_result warm = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
      const arm_result cold = run_arm(
          queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 0, false, thrash_pipeline, thrash_buf, sweep_words);
      std::printf("%-52s window=%9.1fus\n", "14 in 1 dispatch, warm caches (harness default)", warm.window_us);
      std::printf("%-52s window=%9.1fus\n", "14 in 1 dispatch, caches swept before every run", cold.window_us);
    }
  }

  // --- two code-shape differences that only air has (both need no new mechanism, only an order) ----
  std::printf("\n--- shape: the front end's encoder is open for a slot, and the lane commits it ---\n");
  fill_tables(14, true, true);
  {
    const arm_result plain = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
    const arm_result held =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 500, false);
    const arm_result other =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 0, true);
    const arm_result both =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 500, true);
    std::printf("%-52s window=%9.1fus\n", "encode+commit immediately (the harness default)", plain.window_us);
    std::printf("%-52s window=%9.1fus\n", "encoder open 500us before the commit", held.window_us);
    std::printf("%-52s window=%9.1fus\n", "committed from ANOTHER thread (the lane)", other.window_us);
    std::printf("%-52s window=%9.1fus\n", "both (open a slot, committed by the lane)", both.window_us);
    // The air order (see run_arm's close_hold_us): encoded first, committed hundreds of us later.
    const arm_result closed500 =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 0, false, nil, nil, 0, nil, 500);
    const arm_result closed2000 =
        run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1, nullptr, 0, false, 0, false, nil, nil, 0, nil, 2000);
    std::printf("%-52s window=%9.1fus\n", "encoded, then 500us before the commit", closed500.window_us);
    std::printf("%-52s window=%9.1fus\n", "encoded, then 2000us before the commit", closed2000.window_us);
  }

  // --- fresh wrap per run: a NEW MTLBuffer object over the SAME radio pages -----------------------
  //
  // The legs show the engine re-wrapping the radio's samples ~1.7 times per hop (244k creates against
  // 3.8M hits, 265k purges), i.e. a new MTLBuffer object over pages the device has already seen. Every
  // other arm here creates its buffers ONCE and reuses them. If the device charges for meeting a new
  // object over old pages, this arm is where the air front end's ~452us comes from - and it would also
  // explain why 6.79's staging arm (fourteen FRESH buffers a slot) came out at 1005us.
  std::printf("\n--- fresh wrap per run: new MTLBuffer object over the same pages ---\n");
  {
    const size_t page = 16384;
    void*        raw  = nullptr;
    if (posix_memalign(&raw, page, 1u << 20) == 0) {
      auto* samples = static_cast<int16_t*>(raw);
      for (size_t k = 0; k != (1u << 19); ++k) {
        samples[k] = static_cast<int16_t>((k % 251) - 125);
      }
      id<MTLBuffer> once = [device newBufferWithBytesNoCopy:raw
                                                     length:(1u << 20)
                                                    options:MTLResourceStorageModeShared
                                                deallocator:nil];
      fill_tables(14, true, true);
      const arm_result reuse =
          run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, once, gw, ip, 14, 1);
      const arm_result fresh = run_arm(
          queue, pipeline, in, out, twiddle, perm, grid, window, once, gw, ip, 14, 1, raw, 1u << 20, true);
      std::printf("%-52s window=%9.1fus  per-transform=%7.1fus\n",
                  "14 in 1 dispatch, wrap created ONCE and reused",
                  reuse.window_us,
                  reuse.per_transform_us);
      std::printf("%-52s window=%9.1fus  per-transform=%7.1fus\n",
                  "14 in 1 dispatch, FRESH wrap object per run",
                  fresh.window_us,
                  fresh.per_transform_us);
    }
  }

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
  // --- CONCURRENCY: does a command buffer's window stretch because the device is shared? ----------
  //
  // The air reading this arm exists for: a hop's command buffer shows ~500us of window while the same
  // shape runs in ~50-150us here, and the lane probe's own numbers say the windows OVERLAP - the sum of
  // them per slot (47 + 548 + the front end's) exceeds the 500us slot, while the union of the probed
  // queues is busy only 27-30% of the wall time (Q9-F3). A window is wall time, not work: if the device
  // time-slices several buffers, every one of them reads longer than it executes. Every arm above ran
  // ALONE (the "contention" arms used a 4096-thread busy kernel, which is not what the air mixes in),
  // so this arm supplies the missing shape: the SAME kernel on the other queue, submitted continuously
  // for as long as the measured arm runs, one 14-transform dispatch per command buffer - i.e. the front
  // end of another slot, over and over.
  std::printf("\n--- concurrency: the same kernel streaming on the other queue while the arm runs ---\n");
  {
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> background_cbs{0};
    const auto stream_background = [&](unsigned transforms_per_cb, unsigned dispatches_per_cb) {
      stop.store(false, std::memory_order_relaxed);
      background_cbs.store(0, std::memory_order_relaxed);
      std::thread worker([&]() {
        while (!stop.load(std::memory_order_relaxed)) {
          id<MTLCommandBuffer>         cb  = [other commandBuffer];
          id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
          [enc setComputePipelineState:pipeline];
          [enc setBuffer:in offset:0 atIndex:0];
          [enc setBuffer:out offset:0 atIndex:1];
          [enc setBuffer:twiddle offset:0 atIndex:2];
          [enc setBuffer:perm offset:0 atIndex:3];
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
          for (unsigned d = 0; d != dispatches_per_cb; ++d) {
            [enc dispatchThreadgroups:MTLSizeMake(transforms_per_cb, 1, 1)
                threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
          }
          [enc endEncoding];
          [cb commit];
          [cb waitUntilCompleted];
          background_cbs.fetch_add(1, std::memory_order_relaxed);
        }
      });
      return worker;
    };

    fill_tables(14, true, true);
    const arm_result alone = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
    std::printf("%-52s window=%9.1fus\n", "14 in 1 dispatch, device otherwise IDLE", alone.window_us);

    for (unsigned transforms : { 14u, 56u }) {
      std::thread worker = stream_background(transforms, 1);
      // Let the stream saturate the device before the measured arm starts.
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      fill_tables(14, true, true);
      const arm_result shared = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
      stop.store(true, std::memory_order_relaxed);
      worker.join();
      char label[96];
      std::snprintf(label,
                    sizeof(label),
                    "14 in 1 dispatch, %u transforms/cb streaming elsewhere",
                    transforms);
      std::printf("%-52s window=%9.1fus  (background cbs=%llu)\n",
                  label,
                  shared.window_us,
                  static_cast<unsigned long long>(background_cbs.load(std::memory_order_relaxed)));
    }
  }

  // --- THE AIR MIX (2026-09-27): every shape the device really shares the packed dispatch with ------
  //
  // The air reading this section exists for (dev doc 6.123/§6.124, legs p57-p70): the front end's packed
  // dispatch is resident ~452-482us on air while a single-transform command buffer of the SAME kernel is
  // 46.85us there and the packed shape is 46.9us here. 452/14 = 32.3us per threadgroup is the single
  // group's own work, i.e. on air the fourteen groups run ONE AFTER ANOTHER, and the offline arms above
  // say why the previous explanations do not hold: the same-kernel background stream (transforms = 14/56,
  // further up) leaves the measured arm at 12.9-15.9us.
  //
  // What is left is the resource the DFT kernel actually needs: `threadgroup float2 buf[4096]` = 32 KiB
  // PER GROUP, the device's whole per-core threadgroup budget. The arms below therefore mix in, one at a
  // time, (a) the plain route's own shape - single-transform DFT cbs on the other queue, which is what
  // the front end queue actually streams on air - and (b) a spin kernel that declares the SAME 32 KiB per
  // group, sixteen groups per command buffer, i.e. a full device's worth of threadgroup memory.
  std::printf("\n--- the air mix: the packed arm against what the device really shares it with ---\n");
  {
    id<MTLComputePipelineState> busy_tg_pipeline =
        [device newComputePipelineStateWithFunction:[[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                                           options:nil
                                                                             error:&error] newFunctionWithName:@"busy_tg"]
                                             error:&error];
    id<MTLComputePipelineState> busy_pipeline =
        [device newComputePipelineStateWithFunction:[[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                                           options:nil
                                                                             error:&error] newFunctionWithName:@"busy"]
                                             error:&error];
    if ((busy_tg_pipeline == nil) || (busy_pipeline == nil)) {
      std::printf("air-mix pipelines failed: %s\n", error.localizedDescription.UTF8String);
    } else {
      id<MTLBuffer> busy_out = [device newBufferWithLength:(1u << 16) * sizeof(float)
                                                   options:MTLResourceStorageModeShared];
      const uint32_t busy_iters = 200000;
      std::atomic<bool>     stop{false};
      std::atomic<uint64_t> background_cbs{0};

      /// Streams \p kind on the other queue until \p stop: 0 = the DFT itself, 1 = the no-threadgroup
      /// spin, 2 = the 32 KiB-per-group spin.
      const auto stream = [&](unsigned kind, unsigned groups) {
        stop.store(false, std::memory_order_relaxed);
        background_cbs.store(0, std::memory_order_relaxed);
        return std::thread([&, kind, groups]() {
          while (!stop.load(std::memory_order_relaxed)) {
            id<MTLCommandBuffer>         cb  = [other commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            if (kind == 0) {
              [enc setComputePipelineState:pipeline];
              [enc setBuffer:in offset:0 atIndex:0];
              [enc setBuffer:out offset:0 atIndex:1];
              [enc setBuffer:twiddle offset:0 atIndex:2];
              [enc setBuffer:perm offset:0 atIndex:3];
              const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
              [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
              [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
              [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
              [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
              [enc setBuffer:grid offset:0 atIndex:8];
              [enc setBuffer:window offset:0 atIndex:9];
              [enc setBuffer:gw offset:0 atIndex:10];
              [enc setBuffer:in16 offset:0 atIndex:11];
              [enc setBuffer:ip offset:0 atIndex:12];
              [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1)
                  threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
            } else {
              [enc setComputePipelineState:(kind == 1) ? busy_pipeline : busy_tg_pipeline];
              [enc setBuffer:busy_out offset:0 atIndex:0];
              [enc setBytes:&busy_iters length:sizeof(uint32_t) atIndex:1];
              if (kind == 1) {
                [enc dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
              } else {
                [enc dispatchThreadgroups:MTLSizeMake(groups, 1, 1) threadsPerThreadgroup:MTLSizeMake(768, 1, 1)];
              }
            }
            [enc endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            background_cbs.fetch_add(1, std::memory_order_relaxed);
          }
        });
      };

      const auto measure_with = [&](const char* label, unsigned kind, unsigned groups, bool run_it) {
        fill_tables(14, true, true);
        std::thread worker;
        if (run_it) {
          worker = stream(kind, groups);
          std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        fill_tables(14, true, true);
        const arm_result r = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
        if (run_it) {
          stop.store(true, std::memory_order_relaxed);
          worker.join();
        }
        std::printf("%-52s window=%9.1fus  per-transform=%7.1fus  (background cbs=%llu)\n",
                    label,
                    r.window_us,
                    r.per_transform_us,
                    static_cast<unsigned long long>(background_cbs.load(std::memory_order_relaxed)));
      };

      measure_with("14 in 1 dispatch, device otherwise IDLE", 0, 0, false);
      measure_with("14 in 1 dispatch, OTHER queue: 1-transform DFT cbs", 0, 1, true);
      measure_with("14 in 1 dispatch, OTHER queue: 14-transform DFT cbs", 0, 14, true);
      measure_with("14 in 1 dispatch, OTHER queue: spin, 0 KiB tgmem", 1, 0, true);
      measure_with("14 in 1 dispatch, OTHER queue: spin, 32 KiB x 16 groups", 2, 16, true);
      measure_with("14 in 1 dispatch, OTHER queue: spin, 32 KiB x 4 groups", 2, 4, true);
    }
  }

  // --- MEMORY FOOTPRINT: the air process holds ~hundreds of MB of mapped buffers, the harness ~10MB ---
  //
  // WHY (dev doc 6.131). With the wait family closed (a device-side event wait is NOT inside the window:
  // 11.8us waited vs 11.8us control, host elapsed 752us) the remaining explanations have to be about
  // execution. The one difference between this process and the air one that no arm has ever reproduced is
  // the SIZE of the mapped working set: the air pipeline holds the receive pool, the transmit rings, the
  // zero-copy grids, the exported tensors and thousands of wraps - hundreds of MB of shared
  // (host-visible) buffers - while every arm here runs with ~10MB. A dispatch's bindings then cost page
  // walks and TLB fills that a small-footprint process never pays. 6.79's staging arm is consistent with
  // this reading (it CREATEd fourteen buffers a slot and got +464us ~ 14 x 33us).
  //
  // WHAT IT DOES. Allocates and touches a large set of shared buffers (the size is printed), keeps them
  // alive, and re-measures the packed arm that every other arm reports at 12-47us.
  std::printf("\n--- memory footprint: the packed arm with a large mapped working set ---\n");
  {
    const auto median_of = [](std::vector<double> v) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    fill_tables(14, true, true);
    const arm_result before = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);

    std::vector<id<MTLBuffer>> balloon;
    size_t                     bytes = 0;
    for (unsigned i = 0; i != 64; ++i) {
      id<MTLBuffer> b = [device newBufferWithLength:(8u << 20) options:MTLResourceStorageModeShared];
      if (b == nil) {
        break;
      }
      std::memset(b.contents, 0xA5, 8u << 20); // touch every page, like a live pool
      balloon.push_back(b);
      bytes += (8u << 20);
    }
    const arm_result during = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);
    std::printf("%-52s window=%9.1fus\n", "packed arm, small footprint (the harness default)", before.window_us);
    std::printf("%-52s window=%9.1fus  (%.0f MB mapped and touched)\n",
                "packed arm, large footprint",
                during.window_us,
                static_cast<double>(bytes) / 1e6);
    (void)median_of;
    balloon.clear();
  }

  // --- BOUND-BUFFER SIZE: does the device charge for what a dispatch BINDS, not what it touches? -----
  //
  // WHY (dev doc 6.131). Everything else about "execution" has been closed: the window is this buffer's
  // own last dispatch (a 2673us buffer behind it changes nothing), an event wait is NOT inside it, the
  // mapped footprint does not matter (537MB: 11.8 -> 11.8us), and the same content costs 47us or 517us
  // depending only on WHICH buffer carries it. The one property of a buffer that no arm has varied is the
  // SIZE of the buffers it binds: the air hop binds the receive pool, the resource grid, the exported
  // tensors and the LLR target - tens to hundreds of KB each - while the arms here bind 3-90KB. If the
  // device validates or maps the whole bound range, execution would scale with what is bound rather than
  // with what is touched, which is exactly a content-independent, placement-dependent cost.
  //
  // WHAT IT DOES. Re-measures the packed arm with the same kernel and the same touched bytes, but with
  // `in`, `out`, `grid` and `window` replaced by 64MB buffers (the kernel touches only their first bytes).
  std::printf("\n--- bound-buffer size: same work, same touched bytes, bigger bindings ---\n");
  {
    fill_tables(14, true, true);
    const arm_result small = run_arm(queue, pipeline, in, out, twiddle, perm, grid, window, in16, gw, ip, 14, 1);

    const size_t   big_len = 64u << 20;
    id<MTLBuffer>  in_big = [device newBufferWithLength:big_len options:MTLResourceStorageModeShared];
    id<MTLBuffer>  out_big = [device newBufferWithLength:big_len options:MTLResourceStorageModeShared];
    id<MTLBuffer>  grid_big = [device newBufferWithLength:big_len options:MTLResourceStorageModeShared];
    id<MTLBuffer>  win_big = [device newBufferWithLength:big_len options:MTLResourceStorageModeShared];
    std::memset(in_big.contents, 0, big_len);
    std::memset(out_big.contents, 0, big_len);
    std::memset(grid_big.contents, 0, big_len);
    std::memset(win_big.contents, 0, big_len);
    const arm_result big = run_arm(queue, pipeline, in_big, out_big, twiddle, perm, grid_big, win_big, in16, gw, ip, 14, 1);
    std::printf("%-52s window=%9.1fus\n", "packed arm, bindings of a few KB (harness default)", small.window_us);
    std::printf("%-52s window=%9.1fus  (4 x 64 MB bound, same bytes touched)\n",
                "packed arm, bindings of 64 MB",
                big.window_us);
  }

  // --- IS GPUEndTime STAMPED AT OUR LAST DISPATCH, OR WHEN THE QUEUE DRAINS? ------------------------
  //
  // WHY (dev doc 6.131). If Metal stamped a buffer's GPUEndTime when the QUEUE drains rather than when
  // that buffer's own last dispatch retires, then every window in this line would include whatever runs
  // next on the same queue - and the air lane's buffers are always followed by the next hop's (or the
  // peer lane's) work, which is exactly where a content-independent ~450us could come from.
  //
  // WHAT IT DOES. Measures the packed arm alone (control), then measures it again with a LONG buffer
  // committed back to back behind it on the same queue. If the first buffer's window grows to the second
  // one's duration, the window is queue-drain, not execution, and every window reading has to be re-read.
  std::printf("\n--- is GPUEndTime our last dispatch, or the queue draining? ---\n");
  {
    id<MTLComputePipelineState> busy_pipeline =
        [device newComputePipelineStateWithFunction:[[device newLibraryWithSource:[NSString stringWithUTF8String:source.c_str()]
                                                                           options:nil
                                                                             error:&error] newFunctionWithName:@"busy"]
                                             error:&error];
    id<MTLBuffer> busy_out = [device newBufferWithLength:(1u << 16) * sizeof(float)
                                                 options:MTLResourceStorageModeShared];
    const uint32_t busy_iters = 200000;
    const auto     commit_busy = [&](id<MTLCommandQueue> q) {
      id<MTLCommandBuffer>         cb  = [q commandBuffer];
      id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
      [enc setComputePipelineState:busy_pipeline];
      [enc setBuffer:busy_out offset:0 atIndex:0];
      [enc setBytes:&busy_iters length:sizeof(uint32_t) atIndex:1];
      [enc dispatchThreads:MTLSizeMake(4096, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
      [enc endEncoding];
      [cb commit];
      return cb;
    };
    const auto median_of = [](std::vector<double> v) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    fill_tables(14, true, true);
    std::vector<double> alone, followed, busy_len;
    for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
      { // control: measured alone
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (run >= nof_warmup) {
          alone.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
        }
      }
      { // measured, with a long buffer behind it on the SAME queue
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
        [cb commit];
        id<MTLCommandBuffer> behind = commit_busy(queue);
        [cb waitUntilCompleted];
        [behind waitUntilCompleted];
        if (run >= nof_warmup) {
          followed.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
          busy_len.push_back((behind.GPUEndTime - behind.GPUStartTime) * 1e6);
        }
      }
    }
    std::printf("%-52s window=%9.1fus\n", "packed arm alone (control)", median_of(alone));
    std::printf("%-52s window=%9.1fus  (the buffer behind it ran %.1fus)\n",
                "packed arm with a long buffer queued behind it",
                median_of(followed),
                median_of(busy_len));
  }

  // --- IS A DEVICE-SIDE WAIT INSIDE THE WINDOW? -----------------------------------------------------
  //
  // WHY (dev doc 6.129/6.130). Every reading in this line assumes `GPUEndTime - GPUStartTime` is
  // EXECUTION. Five legs say one buffer per hop carries ~450us and that WHICH buffer moves with the
  // structure while the same content costs 47us elsewhere - which is what a *wait* looks like, not what
  // execution looks like. The waits that could do it are the grid-production fence and the front-end
  // fence, and Q24 has never seen them (`per kind: stage=..., corr=0 grid=0`). If Metal stamps
  // GPUStartTime when the buffer is dispatched - before an `encodeWaitForEvent:` resolves - then such a
  // wait is inside the window and the whole attribution changes.
  //
  // WHAT IT DOES. A buffer that waits on an event which a LATER commit signals, then runs the usual
  // packed dispatch. The host sleeps so the wait is unambiguous (500us, an order of magnitude above the
  // dispatch itself), and the arm prints the waited buffer's own window next to a control that does not
  // wait.
  std::printf("\n--- is a device-side wait inside GPUStart->GPUEnd? ---\n");
  {
    id<MTLSharedEvent> wait_event = [device newSharedEvent];
    fill_tables(14, true, true);

    std::vector<double> waited_window;
    std::vector<double> control_window;
    for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
      // (a) the control: no wait at all.
      {
        id<MTLCommandBuffer>         cb  = [queue commandBuffer];
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
        [cb commit];
        [cb waitUntilCompleted];
        if (run >= nof_warmup) {
          control_window.push_back((cb.GPUEndTime - cb.GPUStartTime) * 1e6);
        }
        if (run == nof_warmup) {
          std::printf("DEBUG control run: start=%.6f end=%.6f diff_us=%.3f status=%ld\n",
                      cb.GPUStartTime, cb.GPUEndTime, (cb.GPUEndTime - cb.GPUStartTime) * 1e6,
                      (long)cb.status);
        }
      }
      // (b) the waited buffer: it waits on generation run+1, which only the NEXT commit signals, and the
      // host sleeps in between so the wait is unambiguous.
      const uint64_t gen = run + 1;
      id<MTLCommandBuffer> waited = [queue commandBuffer];
      [waited encodeWaitForEvent:wait_event value:gen];
      {
        id<MTLComputeCommandEncoder> enc = [waited computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
      }
      const auto t0 = std::chrono::steady_clock::now();
      [waited commit];
      std::this_thread::sleep_for(std::chrono::microseconds(500));
      id<MTLCommandBuffer> signaller = [other commandBuffer];
      [signaller encodeSignalEvent:wait_event value:gen];
      [signaller commit];
      [waited waitUntilCompleted];
      [signaller waitUntilCompleted];
      const double host_us =
          std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      if (run >= nof_warmup) {
        if ((waited.GPUEndTime > waited.GPUStartTime) && (waited.GPUStartTime > 0.0)) {
          waited_window.push_back((waited.GPUEndTime - waited.GPUStartTime) * 1e6);
        }
      }
      if (run == nof_warmup) {
        std::printf("DEBUG waited run: start=%.6f end=%.6f diff_us=%.3f host_us=%.1f status=%ld\n",
                    waited.GPUStartTime, waited.GPUEndTime,
                    (waited.GPUEndTime - waited.GPUStartTime) * 1e6, host_us, (long)waited.status);
      }
    }
    const auto median_of = [](std::vector<double> v) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    std::printf("%-52s window=%9.1fus  (n=%zu)\n",
                "control: packed dispatch, no wait",
                median_of(control_window),
                control_window.size());
    std::printf("%-52s window=%9.1fus  (n=%zu)\n",
                "packed dispatch AFTER a ~500us event wait",
                median_of(waited_window),
                waited_window.size());
    std::printf("READ: ~47us means the wait is OUTSIDE the window (commit -> start); ~550us means a\n"
                "      device-side wait IS charged to GPUStart->GPUEnd and every 'window = execution'\n"
                "      reading in this line has to be re-read.\n");
  }

  // --- cb-INTERNAL timeline: WHERE INSIDE a command buffer does its window go? ---------------------
  //
  // WHY (dev doc 6.129(5)). There are three rulers today - buffer level (GPUStartTime -> GPUEndTime,
  // the Q9-F3 per-label table), queue level (commit -> start) and fence level (Q24) - and none of them
  // looks INSIDE a buffer, while "which segment of that one buffer spends the ~450us" is the only
  // question left in the air line (p77: one buffer per hop eats ~450us and which one moves with the
  // structure, while the same four dispatches split over two buffers cost 70us).
  //
  // Metal has no per-dispatch timestamps on this device (6.75), but `encodeSignalEvent:` is a
  // COMMAND-BUFFER-level call that must run with NO encoder open (the DFT engine's own comment says so)
  // - which is exactly the shape the air path already uses for its stage fences (Q9-C counts two
  // signals a hop). So a signal between dispatches plus a host poller of `signaledValue` gives the
  // completion instants INSIDE the buffer.
  //
  // WHAT THIS ARM PROVES BEFORE ANY LEG IS FLOWN. (a) that the shape (one encoder per dispatch, a signal
  // between them) does not itself change the window; (b) that the poller's per-segment deltas match the
  // dispatch price the arms above measure (~41us a dispatch offline); (c) the poller's own resolution -
  // if it is coarser than the segments, the reading must say so instead of inventing a timeline.
  std::printf("\n--- cb-INTERNAL timeline: one signal per dispatch, timestamped by a host poller ---\n");
  {
    id<MTLSharedEvent> timeline_event = [device newSharedEvent];
    std::atomic<bool>  poll_stop{false};
    std::atomic<uint64_t> poll_seen{0};
    std::vector<double>   stamps;
    const auto now_s = []() {
      return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    };
    std::thread poller([&]() {
      uint64_t seen = 0;
      while (!poll_stop.load(std::memory_order_relaxed)) {
        const uint64_t v = timeline_event.signaledValue;
        while (seen != v) {
          stamps.push_back(now_s());
          ++seen;
        }
        std::this_thread::yield();
      }
      poll_seen.store(seen, std::memory_order_relaxed);
    });

    constexpr unsigned nof_seg = 5;
    std::vector<double> seg_us[nof_seg];      // per-segment deltas, one entry per measured run
    std::vector<double> window_us;
    uint64_t            signals_expected = 0;
    uint64_t            signals_missed   = 0;

    fill_tables(14, true, true);
    uint64_t generation = 0; // MONOTONE across runs: a shared event's value never goes down, so a run that
                             // re-signalled 1..5 would be invisible to the poller after the first one
                             // (measured: generations observed in total = 5 for 25 runs, i.e. every later
                             // run collapsed to nothing and the segments came out empty).
    for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
      stamps.clear();
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      for (unsigned d = 0; d != nof_seg; ++d) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
        [cb encodeSignalEvent:timeline_event value:++generation];
      }
      [cb commit];
      [cb waitUntilCompleted];
      const double start = cb.GPUStartTime;
      const double end   = cb.GPUEndTime;
      if (run >= nof_warmup) {
        window_us.push_back((end - start) * 1e6);
        signals_expected += nof_seg;
        // The poller may observe several bumps at once: those segments collapse into one sample and are
        // counted as missed rather than silently attributed to the wrong dispatch.
        size_t at = 0;
        double prev = start;
        for (unsigned d = 0; d != nof_seg; ++d) {
          const double target = ((d + 1) == nof_seg) ? end : 0.0;
          (void)target;
          if (at < stamps.size()) {
            seg_us[d].push_back((stamps[at] - prev) * 1e6);
            prev = stamps[at];
            ++at;
          }
        }
        if (stamps.size() < nof_seg) {
          signals_missed += nof_seg - stamps.size();
        }
      }
    }
    poll_stop.store(true, std::memory_order_relaxed);
    poller.join();

    const auto median_of = [](std::vector<double> v) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    std::printf("%-52s window=%9.1fus\n", "5 dispatches, 1 cb, a signal after each", median_of(window_us));
    for (unsigned d = 0; d != nof_seg; ++d) {
      char label[64];
      std::snprintf(label, sizeof(label), "   segment %u (previous signal -> this one)", d + 1);
      std::printf("%-52s %9.1fus  (n=%zu)\n", label, median_of(seg_us[d]), seg_us[d].size());
    }
    std::printf("poller: signals expected=%llu missed=%llu (a count above 0 means the poller collapsed "
                "segments - the deltas above are then an UPPER bound on each segment)\n",
                static_cast<unsigned long long>(signals_expected),
                static_cast<unsigned long long>(signals_missed));
    std::printf("poller: generations observed in total=%llu\n",
                static_cast<unsigned long long>(poll_seen.load(std::memory_order_relaxed)));
  }

  // --- the same timeline, but stamped ON THE DEVICE by marker command buffers ----------------------
  //
  // The host poller above is not enough, and this arm is where that was found (before any leg was flown):
  // it observed the five signals of a 51.6us buffer within ~2us of EACH OTHER, ~30us after the buffer's
  // own GPUEndTime - i.e. a shared event's value reaches the CPU in one batch at the buffer's completion,
  // so a host-polled timeline cannot see inside a buffer at all (segment 1 read 83.1us for a whole 51.6us
  // buffer, and segments 2-5 read 1.9/0.4/0.3/0.4us).
  //
  // WHAT DOES WORK: a marker command buffer on ANOTHER queue that only WAITS on the event, and whose own
  // GPUStartTime/GPUEndTime (device clock) then says WHEN the signal fired. The markers are committed
  // BEFORE the measured buffer (they wait on future generations), so nothing is coalesced. This is the
  // ruler the air leg needs: buffer level, queue level, fence level, and now the inside of a buffer.
  //
  // HOW TO READ IT. The marker's start deltas should resolve the five dispatches (offline: a few us each
  // in the warm state, ~41us each in the cold one) and the sum should land inside the buffer's own
  // window; markers whose deltas collapse again mean this platform batches even device-side wakes.
  std::printf("\n--- cb-INTERNAL timeline, device-stamped: a marker buffer per signal ---\n");
  {
    id<MTLSharedEvent> timeline_event = [device newSharedEvent];
    std::atomic<bool>  poll_stop{false};
    std::thread        poller([&]() {
      uint64_t seen = 0;
      while (!poll_stop.load(std::memory_order_relaxed)) {
        const uint64_t v = timeline_event.signaledValue;
        while (seen != v) {
          ++seen;
        }
        std::this_thread::yield();
      }
    });

    constexpr unsigned nof_seg = 5;
    std::vector<double> marker_start_us[nof_seg];
    std::vector<double> window_us;
    fill_tables(14, true, true);
    uint64_t generation = 0;
    for (unsigned run = 0; run != nof_runs + nof_warmup; ++run) {
      const uint64_t base = generation;
      // (a) the markers first: each waits for one of the generations the measured buffer will signal.
      id<MTLCommandBuffer> markers[nof_seg];
      for (unsigned d = 0; d != nof_seg; ++d) {
        markers[d] = [other commandBuffer];
        [markers[d] encodeWaitForEvent:timeline_event value:base + d + 1];
        [markers[d] commit];
      }
      // (b) the measured buffer: five dispatches, a signal between them.
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      for (unsigned d = 0; d != nof_seg; ++d) {
        id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
        [enc setComputePipelineState:pipeline];
        [enc setBuffer:in offset:0 atIndex:0];
        [enc setBuffer:out offset:0 atIndex:1];
        [enc setBuffer:twiddle offset:0 atIndex:2];
        [enc setBuffer:perm offset:0 atIndex:3];
        const uint32_t radix2 = 8, radix3 = 1, inverse = 0, base_param = 0;
        [enc setBytes:&radix2 length:sizeof(uint32_t) atIndex:4];
        [enc setBytes:&radix3 length:sizeof(uint32_t) atIndex:5];
        [enc setBytes:&inverse length:sizeof(uint32_t) atIndex:6];
        [enc setBytes:&base_param length:sizeof(uint32_t) atIndex:7];
        [enc setBuffer:grid offset:0 atIndex:8];
        [enc setBuffer:window offset:0 atIndex:9];
        [enc setBuffer:gw offset:0 atIndex:10];
        [enc setBuffer:in16 offset:0 atIndex:11];
        [enc setBuffer:ip offset:0 atIndex:12];
        [enc dispatchThreadgroups:MTLSizeMake(14, 1, 1)
            threadsPerThreadgroup:MTLSizeMake(std::min<unsigned>(fft_n, 1024u), 1, 1)];
        [enc endEncoding];
        [cb encodeSignalEvent:timeline_event value:++generation];
      }
      [cb commit];
      [cb waitUntilCompleted];
      for (unsigned d = 0; d != nof_seg; ++d) {
        [markers[d] waitUntilCompleted];
      }
      if (run >= nof_warmup) {
        const double start = cb.GPUStartTime;
        window_us.push_back((cb.GPUEndTime - start) * 1e6);
        for (unsigned d = 0; d != nof_seg; ++d) {
          const double ms = markers[d].GPUStartTime;
          // The FIRST marker is measured from the buffer's own start; the rest from the previous marker.
          const double from = (d == 0) ? start : markers[d - 1].GPUStartTime;
          if (ms > 0.0) {
            marker_start_us[d].push_back((ms - from) * 1e6);
          }
        }
      }
    }
    poll_stop.store(true, std::memory_order_relaxed);
    poller.join();

    const auto median_of = [](std::vector<double> v) {
      if (v.empty()) {
        return 0.0;
      }
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    std::printf("%-52s window=%9.1fus\n", "5 dispatches, 1 cb, device-stamped markers", median_of(window_us));
    double sum = 0.0;
    for (unsigned d = 0; d != nof_seg; ++d) {
      char label[64];
      std::snprintf(label, sizeof(label), "   marker %u (buffer start / previous marker -> here)", d + 1);
      const double m = median_of(marker_start_us[d]);
      sum += m;
      std::printf("%-52s %9.1fus  (n=%zu)\n", label, m, marker_start_us[d].size());
    }
    std::printf("%-52s %9.1fus\n", "sum of the five marker deltas", sum);
  }

  return 0;
}
