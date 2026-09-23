// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief D1 step 1's mechanism test: the DFT hands its open block over UNCOMMITTED, the lane adopts it
///        (shared_burst::adopt()), and the stage that follows reads the grid the block wrote - all inside
///        ONE command buffer, which is the shape D1 is built on (design document 5.9.4 / 5.9.5).
///
/// The experiment is one loop over repetitions with ONE free variable: the MTLBuffer OBJECT the reader
/// binds for the grid.
///
///  * \c shared  - the mapping the grid's consumers use (shared_queue::wrap_no_copy, the same one the
///                 released block wrote through): the reader must read the DFT's values in EVERY
///                 repetition. This is the mechanism D1 step 2 needs;
///  * \c private - a no-copy object of this test's own over the same memory, which is the shape the DFT's
///                 engine-private cache produces: the reader must read the PRE-WRITE content instead.
///
/// The second arm is what makes the first one mean something. Metal relates memory accesses through the
/// \c MTLBuffer OBJECT, not through the address: two objects over one allocation are unordered, and no
/// barrier and no encoder boundary fixes that (wip/metal_alias_order.mm, 200/200 on both). Without this
/// arm, "the reader saw the data" in the first one could just be the driver happening to serialize two
/// unrelated objects - the hollow assertion this project has been burned by before.
///
/// A third arm checks the CONTROL and the DEFAULT, which are two different statements since 5.9.49 moved
/// the knob's default to ON: with OCUDU_DFT_RELEASE_BLOCK=0 release_block() refuses and the ordinary
/// begin_block()/commit_open()/wait_slot() path is untouched, and with the variable UNSET the hand-over is
/// ARMED (the flip's own criterion, asserted in the same arm so that a default cannot be flipped back by
/// accident). The gate around both is value_net 47/0 + ctest -R metal, of which this test is one; the arm is
/// the same statement inside the binary.
///
/// The grid is bound at a NON-ZERO offset in the shared arm: the process-wide cache hands out the mapping
/// of the whole allocation, so the grid's byte offset travels as the buffer binding's offset. That path is
/// a silent wrong-address defect when it is dropped, so it is exercised here rather than assumed.

#include "dft_processor_metal.h"
#include "ocudu_dft_metal_engine.h"
#include "ocudu_metal_burst.h"
#include "ocudu_metal_queue.h"

#include "ocudu/phy/phy_pipeline_grid_ready.h"
#include "ocudu/phy/phy_pipeline_mode.h"
#include "ocudu/phy/phy_pipeline_contract.h"

#include "ocudu/support/macos_compat.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <optional>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <thread>

using namespace ocudu;

namespace {

/// The reader kernel, compiled at RUNTIME on purpose: the production metallib (ocudu_dft.metallib) is what
/// the DFT engine loads, and adding a probe kernel to it would change a production artifact to test a
/// mechanism - with the .air staleness trap that comes with editing a .metal file. This is the same route
/// wip/metal_alias_order.mm takes.
const char* kReaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

kernel void probe_grid_read(device const ushort* grid [[buffer(0)]],
                            device uint*         out  [[buffer(1)]],
                            constant uint&       dst  [[buffer(2)]],
                            constant uint&       n    [[buffer(3)]],
                            uint                 gid  [[thread_position_in_grid]])
{
  if (gid >= n) {
    return;
  }
  out[gid] = uint(grid[2u * (dst + gid)]) | (uint(grid[2u * (dst + gid) + 1u]) << 16);
}
)MSL";

/// Receiving slot the test's blocks belong to. The registry is keyed by (grid storage, slot) because the
/// grid pool reuses the storage from one slot to the next (5.9.15), so the test names it exactly as the
/// receiving chain does: through the engine (set_lane_slot()) and through the consumer's ask.
constexpr uint64_t test_slot = 4242;

constexpr unsigned transform_size = 512; // 5 MHz cell
constexpr unsigned nof_subc       = 300; // 25 PRB
constexpr unsigned dst_offset     = 64;  // the grid element the (port 0, symbol 0) symbol starts at
constexpr unsigned repetitions    = 20;
constexpr uint16_t poison         = 0xDEAD;

/// The two halves of grid element \c k, packed the way the reader kernel packs them.
uint32_t host_word(const uint16_t* grid, unsigned k)
{
  return static_cast<uint32_t>(grid[2 * k]) | (static_cast<uint32_t>(grid[2 * k + 1]) << 16);
}

constexpr uint32_t poison_word = static_cast<uint32_t>(poison) | (static_cast<uint32_t>(poison) << 16);

id<MTLComputePipelineState> make_reader_pipeline(id<MTLDevice> device)
{
  NSError*       error   = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:@(kReaderSource) options:nil error:&error];
  if (library == nil) {
    std::fprintf(stderr,
                 "FAIL: the reader kernel did not compile: %s\n",
                 error != nil ? error.localizedDescription.UTF8String : "nil error");
    return nil;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"probe_grid_read"];
  if (function == nil) {
    std::fprintf(stderr, "FAIL: probe_grid_read is not in the library\n");
    return nil;
  }
  id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&error];
  if (pipeline == nil) {
    std::fprintf(stderr,
                 "FAIL: the reader pipeline did not build: %s\n",
                 error != nil ? error.localizedDescription.UTF8String : "nil error");
  }
  return pipeline;
}

/// One repetition's outcome.
struct outcome {
  unsigned mismatches    = 0; ///< reader words that differ from the grid's own content
  unsigned reader_poison = 0; ///< reader words that are still the pre-write pattern
  unsigned host_poison   = 0; ///< grid elements the DFT did NOT write (must be 0, or nothing is tested)
};

/// The keep-alive a hop attaches to a block so the input it reads outlives the dispatches (D1 step 3).
///
/// It exists because the release would otherwise be a use-after-free with no failure of its own: the
/// receiving chain recycles a symbol's samples as soon as finish_symbol() returns, and a handed-over block
/// runs later (measured on air: crc=KO 942 / OK 46 at sinr=37.6 dB, design document 5.9.7).
struct keep_alive_probe {
  std::atomic<unsigned> releases{0};
  /// Whether the release ran while the adopted command buffer was still incomplete: the one thing that would
  /// make the token useless (it has to outlive the DISPATCHES, not just the hand-over).
  std::atomic<bool> released_before_completion{false};
  id<MTLCommandBuffer> watched = nil;

  static void release(void* context)
  {
    auto* probe = static_cast<keep_alive_probe*>(context);
    probe->releases.fetch_add(1, std::memory_order_acq_rel);
    if ((probe->watched != nil) && (probe->watched.status != MTLCommandBufferStatusCompleted)) {
      probe->released_before_completion.store(true, std::memory_order_relaxed);
    }
  }

  metal::dft_metal_engine::keep_alive token() { return {&keep_alive_probe::release, this}; }
};

} // namespace

int main()
{
  @autoreleasepool {
    // The knob is armed BEFORE the engine exists: it also selects the queue the block is created on, and a
    // released block has to live on the queue the lane commits it on (it is the lane that commits it).
    ::setenv("OCUDU_DFT_RELEASE_BLOCK", "1", 1);

    metal::dft_metal_engine engine;
    if (!engine.init(transform_size, false)) {
      std::fprintf(stderr, "FAIL: the Metal DFT engine did not initialize\n");
      return 1;
    }
    engine.set_lane_slot(test_slot);

    id<MTLDevice> device = metal::shared_queue::device();
    if (device == nil) {
      std::fprintf(stderr, "FAIL: no Metal device\n");
      return 1;
    }
    id<MTLComputePipelineState> reader = make_reader_pipeline(device);
    if (reader == nil) {
      return 1;
    }

    const size_t page = compat::page_size();

    // The transform input ring and output ring: the engine's batch buffers (dft_processor_metal::max_batch
    // slots of `transform_size` complex floats each - the size the engine wraps them with).
    const size_t ring_bytes =
        static_cast<size_t>(transform_size) * dft_processor_metal::max_batch * 2 * sizeof(float);
    void* in_mem  = compat::aligned_alloc(page, ring_bytes);
    void* out_mem = compat::aligned_alloc(page, ring_bytes);
    // The grid storage: one page-aligned allocation, like the resource grid's (rg_buffer). The mapping the
    // consumers make covers the WHOLE allocation, so the grid is placed one page in on purpose - that is
    // what puts a non-zero offset into the buffer binding of the shared arm.
    const size_t alloc_bytes = 2 * page;
    void*        grid_alloc  = compat::aligned_alloc(page, alloc_bytes);
    if ((in_mem == nullptr) || (out_mem == nullptr) || (grid_alloc == nullptr)) {
      std::fprintf(stderr, "FAIL: allocation of the test buffers failed\n");
      return 1;
    }
    const void* grid_base = static_cast<const char*>(grid_alloc) + page;
    const size_t grid_bytes = page;
    auto*        grid_u16   = reinterpret_cast<uint16_t*>(static_cast<char*>(grid_alloc) + page);

    // ---- The mapping invariant, asserted directly -------------------------------------------------
    // The consumers' view of the container (this is what the later stages of the lane do before they bind
    // a slice), created FIRST: the block's own wrap of the grid is then served by it instead of creating a
    // second object over the same memory.
    size_t        container_off  = 0;
    id<MTLBuffer> container_view = metal::shared_queue::wrap_no_copy(device, grid_alloc, alloc_bytes, &container_off);
    size_t        grid_off       = 0;
    id<MTLBuffer> grid_view      = metal::shared_queue::wrap_no_copy(device, grid_base, grid_bytes, &grid_off);
    if ((container_view == nil) || (grid_view == nil) || (container_off != 0)) {
      std::fprintf(stderr, "FAIL: the process-wide wrap did not produce a mapping (container=%p grid=%p)\n",
                   container_view,
                   grid_view);
      return 1;
    }
    if ((grid_view != container_view) || (grid_off != page)) {
      std::fprintf(stderr,
                   "FAIL: the process-wide cache handed out a DIFFERENT object for one address "
                   "(container=%p grid=%p offset=%zu, expected the same object and offset=%zu) - the whole "
                   "point of the release path is that the DFT and its consumers bind one and the same "
                   "MTLBuffer\n",
                   container_view,
                   grid_view,
                   grid_off,
                   page);
      return 1;
    }

    // The trap arm's object: a no-copy mapping of this test's own over the very same memory, which is the
    // shape the engine's private cache would produce for the grid.
    id<MTLBuffer> private_view = [device newBufferWithBytesNoCopy:(void*)grid_base
                                                          length:grid_bytes
                                                         options:MTLResourceStorageModeShared
                                                     deallocator:nil];
    if (private_view == nil) {
      std::fprintf(stderr, "FAIL: the private no-copy wrap failed\n");
      return 1;
    }

    id<MTLBuffer> reader_out = [device newBufferWithLength:nof_subc * sizeof(uint32_t)
                                                   options:MTLResourceStorageModeShared];
    if (reader_out == nil) {
      std::fprintf(stderr, "FAIL: the reader output buffer was not created\n");
      return 1;
    }

    // ---- Arm 0: the CONTROL, asked for explicitly (knob = 0) --------------------------------------
    // D1 step 1's criterion, stated inside the binary: with the knob set to 0, release_block() refuses - and
    // the ordinary block path still commits and waits as it always did.
    //
    // \note Since 5.9.49 the knob's DEFAULT is armed, so the control has to say so: leaving it UNSET is no
    //       longer "off". The default's own criterion is asserted right below, after this block.
    {
      ::setenv("OCUDU_DFT_RELEASE_BLOCK", "0", 1);
      if (metal::dft_metal_engine::block_release_enabled()) {
        std::fprintf(stderr, "FAIL: the release path reports itself armed with the knob set to 0\n");
        return 1;
      }
      for (size_t i = 0; i != alloc_bytes / sizeof(uint16_t); ++i) {
        reinterpret_cast<uint16_t*>(grid_alloc)[i] = poison;
      }
      if (!engine.begin_block()) {
        std::fprintf(stderr, "FAIL: begin_block() refused with the release path off\n");
        return 1;
      }
      metal::dft_metal_engine::grid_write write;
      write.grid_base  = grid_base;
      write.grid_bytes = grid_bytes;
      write.dst_offset = dst_offset;
      write.nof_subc   = nof_subc;
      write.map_offset = transform_size - nof_subc / 2;
      write.phase_re   = 1.0F;
      if (!engine.submit_slot_grid_write(in_mem, out_mem, 0, write)) {
        std::fprintf(stderr, "FAIL: submit_slot_grid_write() refused with the release path off\n");
        return 1;
      }
      if (engine.release_block(grid_base) != nullptr) {
        std::fprintf(stderr, "FAIL: release_block() handed a block over with OCUDU_DFT_RELEASE_BLOCK=0\n");
        return 1;
      }
      if (metal::shared_burst::take_released(grid_base, test_slot) != nil) {
        std::fprintf(stderr, "FAIL: a block was deposited with OCUDU_DFT_RELEASE_BLOCK=0\n");
        return 1;
      }
      if (!engine.commit_open()) {
        std::fprintf(stderr, "FAIL: commit_open() failed with the release path off\n");
        return 1;
      }
      if (!engine.wait_slot(0)) {
        std::fprintf(stderr, "FAIL: wait_slot() failed on the ordinary (unreleased) path\n");
        return 1;
      }
      unsigned unwritten = 0;
      for (unsigned k = 0; k != nof_subc; ++k) {
        if (host_word(grid_u16, dst_offset + k) == poison_word) {
          ++unwritten;
        }
      }
      if (unwritten != 0) {
        std::fprintf(stderr, "FAIL: the ordinary block path left %u of %u grid elements unwritten\n", unwritten, nof_subc);
        return 1;
      }
      std::fprintf(stderr, "[dft-release] arm 0 (knob=0, the control): release refused, the ordinary commit path wrote all %u elements\n",
                   nof_subc);

      // ---- The DEFAULT, also stated inside the binary (5.9.49) ------------------------------------
      // The knob was flipped from OFF to ON after the s46 controlled leg pair, and the flip's own criterion
      // is that an UNSET variable now arms the hand-over: a default nobody asserts is a default that can be
      // flipped back by accident, which is exactly what this arm would have caught before.
      ::unsetenv("OCUDU_DFT_RELEASE_BLOCK");
      if (!metal::dft_metal_engine::block_release_enabled()) {
        std::fprintf(stderr,
                     "FAIL: the release path reports itself DISARMED with OCUDU_DFT_RELEASE_BLOCK unset - "
                     "the default is armed since 5.9.49\n");
        return 1;
      }
      // A typo must not silently pick a path either: it warns once and keeps the default.
      ::setenv("OCUDU_DFT_RELEASE_BLOCK", "yes", 1);
      if (!metal::dft_metal_engine::block_release_enabled()) {
        std::fprintf(stderr, "FAIL: a non-numeric knob value did not fall back to the default (armed)\n");
        return 1;
      }
      std::fprintf(stderr, "[dft-release] arm 0b (unset and non-numeric): the default is ARMED\n");

      ::setenv("OCUDU_DFT_RELEASE_BLOCK", "1", 1);
      if (!metal::dft_metal_engine::block_release_enabled()) {
        std::fprintf(stderr, "FAIL: the release path does not report itself armed after the knob was set\n");
        return 1;
      }
    }

    // ---- Arms 1/2: the handover, one free variable ------------------------------------------------
    unsigned shared_ok    = 0; // repetitions where the reader read what the block wrote
    unsigned private_trap = 0; // repetitions where the private object still showed the pre-write content
    unsigned private_correct = 0;
    bool     hard_failure    = false;

    for (unsigned rep = 0; (rep != repetitions) && !hard_failure; ++rep) {
      for (unsigned mode = 0; mode != 2; ++mode) {
        const bool shared_mapping = (mode == 0);

        // Pre-write content: a pattern no transform output can produce (every element is 0xDEADDEAD).
        for (size_t i = 0; i != alloc_bytes / sizeof(uint16_t); ++i) {
          reinterpret_cast<uint16_t*>(grid_alloc)[i] = poison;
        }
        // A deterministic, non-trivial input in slot 0 of the ring: 300 subcarriers of distinct values, so
        // an index or offset mistake shows up as a mismatch instead of passing on a constant.
        auto* in_f = static_cast<float*>(in_mem);
        for (unsigned i = 0; i != transform_size; ++i) {
          in_f[2 * i]     = std::cos(0.017F * static_cast<float>(i));
          in_f[2 * i + 1] = std::sin(0.011F * static_cast<float>(i));
        }

        if (!engine.begin_block()) {
          std::fprintf(stderr, "FAIL: begin_block() refused on the release path (rep %u)\n", rep);
          return 1;
        }
        metal::dft_metal_engine::grid_write write;
        write.grid_base  = grid_base;
        write.grid_bytes = grid_bytes;
        write.dst_offset = dst_offset;
        write.nof_subc   = nof_subc;
        write.map_offset = transform_size - nof_subc / 2;
        write.phase_re   = 1.0F;
        if (!engine.submit_slot_grid_write(in_mem, out_mem, 0, write)) {
          std::fprintf(stderr,
                       "FAIL: submit_slot_grid_write() refused on the release path (rep %u, shared=%d)\n",
                       rep,
                       static_cast<int>(shared_mapping));
          return 1;
        }

        // D1 step 3: the input's lifetime travels with the block. The receiving chain attaches the handle of
        // the samples this block reads, and it must come back when the ADOPTED buffer completes - not
        // earlier (the dispatches have not read it yet) and not later (the radio would starve).
        keep_alive_probe probe;
        if (!engine.retain_for_block(probe.token())) {
          std::fprintf(stderr, "FAIL: retain_for_block() refused while a block was open (rep %u)\n", rep);
          return 1;
        }

        // THE HANDOVER, through the entry points the receiving chain and the lane actually use:
        // release_block() deposits the buffer under the grid it wrote, and the consumer takes it back by
        // that same address (shared_burst::take_released()). Nothing is committed by the engine here.
        void* handle = engine.release_block(grid_base);
        if (handle == nullptr) {
          std::fprintf(stderr, "FAIL: release_block() refused an open block (rep %u)\n", rep);
          return 1;
        }
        id<MTLCommandBuffer> cb = (__bridge id<MTLCommandBuffer>)handle;
        probe.watched           = cb;
        if (probe.releases.load() != 0) {
          std::fprintf(stderr,
                       "FAIL: the input token was released by the hand-over itself (rep %u) - the block has "
                       "not run yet\n",
                       rep);
          return 1;
        }
        if (metal::shared_burst::take_released(grid_base, test_slot) != cb) {
          std::fprintf(stderr,
                       "FAIL: the deposit did not come back for the grid it was keyed by (rep %u) - the "
                       "handover is not addressable by the consumer\n",
                       rep);
          return 1;
        }
        if (metal::shared_burst::take_released(grid_base, test_slot) != nil) {
          std::fprintf(stderr, "FAIL: a deposit was handed out twice (rep %u)\n", rep);
          return 1;
        }
        if (metal::shared_burst::take_released(static_cast<const char*>(grid_alloc), test_slot) != nil) {
          std::fprintf(stderr, "FAIL: a deposit was handed out for an address nothing was deposited for\n");
          return 1;
        }
        if (cb.status != MTLCommandBufferStatusNotEnqueued) {
          std::fprintf(stderr,
                       "FAIL: the released buffer is already %lu - release_block() committed it\n",
                       static_cast<unsigned long>(cb.status));
          return 1;
        }
        if (engine.has_open()) {
          std::fprintf(stderr, "FAIL: the engine still reports an open block after the release\n");
          return 1;
        }
        if (!metal::shared_burst::adopt(cb)) {
          std::fprintf(stderr, "FAIL: shared_burst::adopt() refused the released buffer\n");
          return 1;
        }

        // The lane's stage that follows: a reader that binds the grid the way the real consumers do.
        // The deposit was TAKEN above (that is what the estimator's extraction does with it) and adopted
        // here (that is what the lane does with it).
        id<MTLComputeCommandEncoder> encoder = metal::shared_burst::encoder(reader);
        if (encoder == nil) {
          std::fprintf(stderr, "FAIL: the adopted burst did not give an encoder\n");
          return 1;
        }
        const unsigned dst  = dst_offset;
        const unsigned nsub = nof_subc;
        [encoder setBuffer:(shared_mapping ? grid_view : private_view)
                    offset:(shared_mapping ? grid_off : 0)
                   atIndex:0];
        [encoder setBuffer:reader_out offset:0 atIndex:1];
        [encoder setBytes:&dst length:sizeof(dst) atIndex:2];
        [encoder setBytes:&nsub length:sizeof(nsub) atIndex:3];
        [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(nof_subc, 1, 1)];
        metal::shared_burst::count_dispatch();
        if (!metal::shared_burst::commit() || !metal::shared_burst::wait_committed()) {
          std::fprintf(stderr, "FAIL: the adopted buffer did not complete (rep %u)\n", rep);
          return 1;
        }
        if (cb.status != MTLCommandBufferStatusCompleted) {
          std::fprintf(stderr, "FAIL: the adopted buffer completed with status %lu\n",
                       static_cast<unsigned long>(cb.status));
          return 1;
        }
        // The token came back, exactly once, and NOT before the buffer completed: that is the whole
        // contract - the input has to outlive the dispatches, and only the dispatches.
        {
          const unsigned releases = probe.releases.load();
          if (releases != 1) {
            std::fprintf(stderr,
                         "FAIL: the input token was released %u times for one completed block (rep %u)\n",
                         releases,
                         rep);
            return 1;
          }
          if (probe.released_before_completion.load()) {
            std::fprintf(stderr,
                         "FAIL: the input token was released while the adopted buffer was still running "
                         "(rep %u) - the transforms had not read the input yet\n",
                         rep);
            return 1;
          }
        }

        outcome result;
        const auto* got = static_cast<const uint32_t*>(reader_out.contents);
        for (unsigned k = 0; k != nof_subc; ++k) {
          const uint32_t want = host_word(grid_u16, dst_offset + k);
          if (got[k] != want) {
            ++result.mismatches;
          }
          if (got[k] == poison_word) {
            ++result.reader_poison;
          }
          if (want == poison_word) {
            ++result.host_poison;
          }
        }

        // The judgement must be able to trigger: if the DFT wrote nothing, "the reader read the grid" would
        // be vacuously true on an all-poison grid, which is how a hollow assertion passes for the wrong
        // reason. Every element has to have been written by the block.
        if (result.host_poison != 0) {
          std::fprintf(stderr,
                       "FAIL: the released block left %u of %u grid elements unwritten (rep %u, shared=%d)\n",
                       result.host_poison,
                       nof_subc,
                       rep,
                       static_cast<int>(shared_mapping));
          hard_failure = true;
          break;
        }

        if (shared_mapping) {
          if (result.mismatches == 0) {
            ++shared_ok;
          } else if (rep == 0) {
            std::fprintf(stderr,
                         "FAIL: the reader read %u of %u elements differently from the grid the released "
                         "block wrote, through the mapping its consumers use\n",
                         result.mismatches,
                         nof_subc);
            hard_failure = true;
            break;
          }
        } else if (result.reader_poison == nof_subc) {
          ++private_trap;
        } else if (result.mismatches == 0) {
          // The driver happened to order two unrelated objects. Recorded, not asserted: it is exactly the
          // case the shared arm must not be read as ("it passed" would then mean nothing).
          ++private_correct;
        }
      }
    }
    if (hard_failure) {
      return 1;
    }

    std::fprintf(stderr,
                 "[dft-release] %u repetitions per arm | shared mapping: %u/%u read the block's grid | "
                 "private object: %u/%u read the pre-write content, %u read the block's grid anyway\n",
                 repetitions,
                 shared_ok,
                 repetitions,
                 private_trap,
                 repetitions,
                 private_correct);

    if (shared_ok != repetitions) {
      std::fprintf(stderr,
                   "FAIL: the consumer mapping did not read the released block's grid in %u of %u repetitions\n",
                   repetitions - shared_ok,
                   repetitions);
      return 1;
    }
    if (private_trap * 2 < repetitions) {
      std::fprintf(stderr,
                   "FAIL: the trap did NOT reproduce (%u of %u repetitions read the block's grid through a "
                   "second object over the same memory). The shared arm's result is then not evidence of "
                   "anything - re-read wip/metal_alias_order.mm before trusting it\n",
                   private_correct,
                   repetitions);
      return 1;
    }

    // The release also takes the host wait away, and says so instead of returning as a satisfied wait: the
    // caller that released the block promised the host would not read its output.
    if (engine.wait_slot(0)) {
      std::fprintf(stderr, "FAIL: wait_slot() reported success for a slot whose block was handed over\n");
      return 1;
    }

    // ---- Arm 3: a hand-over NOBODY CLAIMS must still give the input back ---------------------------
    // This is not a corner case: the air leg had ~2 blocks per hop, so most blocks have no consumer at all.
    // Such a block is never committed, so its completion handler never runs - if the token were only
    // released there, the receiving chain would lose a radio buffer per unconsumed slot and stall. This is
    // what the registry's on_drop hook is for, and it is asserted here rather than reasoned about.
    {
      keep_alive_probe dropped_probe;
      if (!engine.begin_block()) {
        std::fprintf(stderr, "FAIL: begin_block() refused on the unconsumed-block arm\n");
        return 1;
      }
      metal::dft_metal_engine::grid_write write;
      write.grid_base  = grid_base;
      write.grid_bytes = grid_bytes;
      write.dst_offset = dst_offset;
      write.nof_subc   = nof_subc;
      write.map_offset = transform_size - nof_subc / 2;
      write.phase_re   = 1.0F;
      if (!engine.submit_slot_grid_write(in_mem, out_mem, 0, write) ||
          !engine.retain_for_block(dropped_probe.token())) {
        std::fprintf(stderr, "FAIL: the unconsumed-block arm could not stage its submission\n");
        return 1;
      }
      if (engine.release_block(grid_base) == nullptr) {
        std::fprintf(stderr, "FAIL: release_block() refused on the unconsumed-block arm\n");
        return 1;
      }
      // A SECOND block for the same grid, also unconsumed: the first deposit is superseded, and the block it
      // held can never be committed by anyone - so its input has to come back NOW.
      if (!engine.begin_block() || !engine.submit_slot_grid_write(in_mem, out_mem, 0, write) ||
          (engine.release_block(grid_base) == nullptr)) {
        std::fprintf(stderr, "FAIL: the unconsumed-block arm could not stage its second submission\n");
        return 1;
      }
      if (dropped_probe.releases.load() != 1) {
        std::fprintf(stderr,
                     "FAIL: the input of an UNCONSUMED hand-over was released %u times (expected exactly 1 "
                     "- the deposit it belonged to is gone and its command buffer will never be committed)\n",
                     dropped_probe.releases.load());
        return 1;
      }
      // The deposit that replaced it is still live, and claims back cleanly.
      if (metal::shared_burst::take_released(grid_base, test_slot) == nil) {
        std::fprintf(stderr, "FAIL: the replacing deposit was not there to claim\n");
        return 1;
      }
      std::fprintf(stderr,
                   "[dft-release] arm 3: an unconsumed hand-over gave its input back exactly once "
                   "(superseded by the next block for the same grid)\n");
    }

    // ---- Arm 4: a token with no block open stays the caller's --------------------------------------
    // The engine only holds what it was handed while a block was open; without one it must refuse, or a
    // factory path that never batches would silently lose its input.
    {
      keep_alive_probe stray;
      if (engine.retain_for_block(stray.token())) {
        std::fprintf(stderr, "FAIL: retain_for_block() accepted a token with no block open\n");
        return 1;
      }
      if (stray.releases.load() != 0) {
        std::fprintf(stderr, "FAIL: a refused token was released by the engine\n");
        return 1;
      }
    }

    // ---- Arm 5: a block that is COMMITTED (not handed over) gives the input back too ----------------
    // This is the path a CONTROL run takes, and it now arms tokens as well: a block that is committed rather
    // than released must hold its input for exactly as long as its own dispatches, which is the behaviour
    // the receiving chain has always depended on.
    //
    // \note The control is asked for explicitly: since 5.9.49 the knob's default is armed, so an UNSET
    //       variable would put this arm on the HANDED-OVER path and it would silently stop testing the
    //       committed one.
    {
      ::setenv("OCUDU_DFT_RELEASE_BLOCK", "0", 1);
      keep_alive_probe committed_probe;
      if (!engine.begin_block() ||
          !engine.retain_for_block(committed_probe.token())) {
        std::fprintf(stderr, "FAIL: the committed-block arm could not stage its block\n");
        return 1;
      }
      metal::dft_metal_engine::grid_write write;
      write.grid_base  = grid_base;
      write.grid_bytes = grid_bytes;
      write.dst_offset = dst_offset;
      write.nof_subc   = nof_subc;
      write.map_offset = transform_size - nof_subc / 2;
      write.phase_re   = 1.0F;
      if (!engine.submit_slot_grid_write(in_mem, out_mem, 0, write) || !engine.commit_open()) {
        std::fprintf(stderr, "FAIL: the committed-block arm could not commit its block\n");
        return 1;
      }
      if (committed_probe.releases.load() != 0) {
        std::fprintf(stderr, "FAIL: a committed block's input was released before the commit completed\n");
        return 1;
      }
      if (!engine.wait_slot(0)) {
        std::fprintf(stderr, "FAIL: the committed-block arm's wait failed\n");
        return 1;
      }
      if (committed_probe.releases.load() != 1) {
        std::fprintf(stderr,
                     "FAIL: a committed block released its input %u times (expected exactly 1, on the "
                     "commit's completion)\n",
                     committed_probe.releases.load());
        return 1;
      }
      std::fprintf(stderr, "[dft-release] arm 5: a committed block gave its input back exactly once, on its completion\n");
      ::setenv("OCUDU_DFT_RELEASE_BLOCK", "1", 1);
    }

    // ---- Arm 6: a HOST reader is served (D1-A, 5.9.13) --------------------------------------------
    // The upper PHY's host readers (the PUCCH, the SRS) reach the registry through a hook, so that a build
    // without Metal links without it. The metal side is what installs it: assert that here, or the whole
    // wiring would be a function nobody calls.
    if (!grid_ready_hook::installed()) {
      std::fprintf(stderr, "FAIL: the grid-ready hook was never installed by the Metal side\n");
      return 1;
    }

    // Two shapes, and both must end with the grid WRITTEN and the input given back:
    //  * nobody claims the block (a slot no hop runs for - a PUCCH-only slot in the receiving chain):
    //    ensure_grid_produced() has to commit it, or the grid is never written at all;
    //  * a hop claims it, so the producer is the lane's commit and the reader waits for that.
    {
      for (unsigned claimed_by_hop = 0; claimed_by_hop != 2; ++claimed_by_hop) {
        // Poison, so "the grid was produced" is a real reading and not leftover data.
        for (size_t i = 0; i != alloc_bytes / sizeof(uint16_t); ++i) {
          reinterpret_cast<uint16_t*>(grid_alloc)[i] = poison;
        }
        keep_alive_probe host_probe;
        if (!engine.begin_block()) {
          std::fprintf(stderr, "FAIL: begin_block() refused on the host-reader arm\n");
          return 1;
        }
        metal::dft_metal_engine::grid_write write;
        write.grid_base  = grid_base;
        write.grid_bytes = grid_bytes;
        write.dst_offset = dst_offset;
        write.nof_subc   = nof_subc;
        write.map_offset = transform_size - nof_subc / 2;
        write.phase_re   = 1.0F;
        if (!engine.submit_slot_grid_write(in_mem, out_mem, 0, write) ||
            !engine.retain_for_block(host_probe.token()) || (engine.release_block(grid_base) == nullptr)) {
          std::fprintf(stderr, "FAIL: the host-reader arm could not stage its block\n");
          return 1;
        }

        if (claimed_by_hop != 0) {
          // A hop does what the lane does: take it and commit it in a burst.
          id<MTLCommandBuffer> cb = metal::shared_burst::take_released(grid_base, test_slot);
          if (cb == nil) {
            std::fprintf(stderr, "FAIL: the host-reader arm's deposit was not there to take\n");
            return 1;
          }
          if (!metal::shared_burst::adopt(cb) || !metal::shared_burst::commit() ||
              !metal::shared_burst::wait_committed()) {
            std::fprintf(stderr, "FAIL: the host-reader arm's hop did not complete\n");
            return 1;
          }
        }

        // The host reader: it must not have to know which of the two shapes it is looking at.
        if (!metal::shared_burst::ensure_grid_produced(grid_base, test_slot)) {
          std::fprintf(stderr,
                       "FAIL: ensure_grid_produced() timed out (claimed_by_hop=%u) - a host reader would "
                       "have read a grid nobody wrote\n",
                       claimed_by_hop);
          return 1;
        }
        unsigned unwritten = 0;
        for (unsigned k = 0; k != nof_subc; ++k) {
          if (host_word(grid_u16, dst_offset + k) == poison_word) {
            ++unwritten;
          }
        }
        if (unwritten != 0) {
          std::fprintf(stderr,
                       "FAIL: %u of %u grid elements are unwritten after ensure_grid_produced() "
                       "(claimed_by_hop=%u)\n",
                       unwritten,
                       nof_subc,
                       claimed_by_hop);
          return 1;
        }
        // And the input comes back, whichever shape it was: the fallback commit owes the same release the
        // lane's does. WAITED for, not read at once: the fence says the GPU is done with the samples, while
        // the token is released by a COMPLETION HANDLER, and Metal does not order the two - which is exactly
        // why the release must not be something a caller can assume the instant the data is readable.
        for (unsigned spin = 0; (spin != 2000) && (host_probe.releases.load() == 0); ++spin) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (host_probe.releases.load() != 1) {
          std::fprintf(stderr,
                       "FAIL: the host-reader arm released its input %u times (claimed_by_hop=%u)\n",
                       host_probe.releases.load(),
                       claimed_by_hop);
          return 1;
        }
        // A second call finds nothing to do - the record is gone with the completion.
        if (!metal::shared_burst::ensure_grid_produced(grid_base, test_slot)) {
          std::fprintf(stderr, "FAIL: ensure_grid_produced() failed on an already produced grid\n");
          return 1;
        }
      }
      std::fprintf(stderr,
                   "[dft-release] arm 6: a host reader is served both ways - the unclaimed block is "
                   "committed for it, the claimed one is waited for, and the grid is written either way\n");
    }

    // ---- Arm 7: the KEY is (storage, slot), not storage alone ------------------------------------
    // This is the defect the s34 leg found (5.9.15): the grid pool hands a storage address back as soon as
    // the next slot's grid arrives, so a consumer asking by ADDRESS is served the NEXT slot's block, commits
    // and waits for that one, and reads a grid nobody ever wrote - 72% of the PUCCH reports were unusable.
    // Two blocks for the SAME storage in two different slots must stay two different answers.
    {
      constexpr uint64_t slot_a = test_slot + 1;
      constexpr uint64_t slot_b = test_slot + 2;
      id<MTLCommandBuffer> cb_a  = nil;
      id<MTLCommandBuffer> cb_b  = nil;
      metal::dft_metal_engine::grid_write write;
      write.grid_base  = grid_base;
      write.grid_bytes = grid_bytes;
      write.dst_offset = dst_offset;
      write.nof_subc   = nof_subc;
      write.map_offset = transform_size - nof_subc / 2;
      write.phase_re   = 1.0F;
      for (unsigned which = 0; which != 2; ++which) {
        engine.set_lane_slot(which == 0 ? slot_a : slot_b);
        if (!engine.begin_block() || !engine.submit_slot_grid_write(in_mem, out_mem, 0, write)) {
          std::fprintf(stderr, "FAIL: the key arm could not stage its block\n");
          return 1;
        }
        void* handle = engine.release_block(grid_base);
        if (handle == nullptr) {
          std::fprintf(stderr, "FAIL: the key arm's release was refused\n");
          return 1;
        }
        (which == 0 ? cb_a : cb_b) = (__bridge id<MTLCommandBuffer>)handle;
      }
      if (cb_a == cb_b) {
        std::fprintf(stderr, "FAIL: the key arm produced one buffer for two slots\n");
        return 1;
      }
      // Each slot's consumer must be given ITS OWN block - the whole point of the key.
      if (metal::shared_burst::take_released(grid_base, slot_a) != cb_a) {
        std::fprintf(stderr,
                     "FAIL: the consumer of slot %llu was not given its own block - the storage address was "
                     "reused by a later slot and the key did not tell them apart\n",
                     static_cast<unsigned long long>(slot_a));
        return 1;
      }
      if (metal::shared_burst::take_released(grid_base, slot_b) != cb_b) {
        std::fprintf(stderr, "FAIL: the consumer of slot %llu was not given its own block\n",
                     static_cast<unsigned long long>(slot_b));
        return 1;
      }
      // A slot nothing was handed over for gets nothing (and is counted): a reader must not be served
      // somebody else's grid.
      if (metal::shared_burst::take_released(grid_base, slot_b + 1000) != nil) {
        std::fprintf(stderr, "FAIL: a slot with no hand-over was given a block\n");
        return 1;
      }
      // Both blocks stay uncommitted on purpose: that is the state a deposit is in, and the registry's
      // records are what the next ask must resolve.
      std::fprintf(stderr,
                   "[dft-release] arm 7: two slots, one storage - each consumer is served its own block\n");
    }

    // ---- Arm 8: a slot NOBODY ever asks about must be swept --------------------------------------
    // This is the defect the s35 leg found (5.9.17), and it is a consequence of the key being right: with
    // (storage, slot) a deposit is NEVER superseded, so a block no hop and no host reader ever asked for
    // would sit in the registry holding its input for good - handed=64 taken=39 fallback=21, keepalives
    // 840/896, four receive buffers gone and the pool at zero. The sweep commits it once the receiving chain
    // has moved past its slot, which writes its grid and gives the input back.
    {
      keep_alive_probe orphan_probe;
      constexpr uint64_t orphan_slot = test_slot + 100;
      engine.set_lane_slot(orphan_slot);
      metal::dft_metal_engine::grid_write write;
      write.grid_base  = grid_base;
      write.grid_bytes = grid_bytes;
      write.dst_offset = dst_offset;
      write.nof_subc   = nof_subc;
      write.map_offset = transform_size - nof_subc / 2;
      write.phase_re   = 1.0F;
      for (size_t i = 0; i != alloc_bytes / sizeof(uint16_t); ++i) {
        reinterpret_cast<uint16_t*>(grid_alloc)[i] = poison;
      }
      if (!engine.begin_block() || !engine.submit_slot_grid_write(in_mem, out_mem, 0, write) ||
          !engine.retain_for_block(orphan_probe.token()) || (engine.release_block(grid_base) == nullptr)) {
        std::fprintf(stderr, "FAIL: the sweep arm could not stage its block\n");
        return 1;
      }
      if (orphan_probe.releases.load() != 0) {
        std::fprintf(stderr, "FAIL: the sweep arm released its input before anything swept it\n");
        return 1;
      }
      // The chain moves on: two more slots, which is what the sweep waits for. Nobody ever asks about the
      // orphan - that is the whole point.
      for (uint64_t step = 1; step <= 3; ++step) {
        engine.set_lane_slot(orphan_slot + step);
        if (!engine.begin_block() || !engine.submit_slot_grid_write(in_mem, out_mem, 0, write) ||
            (engine.release_block(grid_base) == nullptr)) {
          std::fprintf(stderr, "FAIL: the sweep arm could not advance the chain\n");
          return 1;
        }
      }
      // The orphan's grid is written (the sweep committed it) and its input is back.
      for (unsigned spin = 0; (spin != 2000) && (orphan_probe.releases.load() == 0); ++spin) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (orphan_probe.releases.load() != 1) {
        std::fprintf(stderr,
                     "FAIL: the input of a slot NOBODY asked about was released %u times (expected 1: the "
                     "sweep commits it). It is a receive buffer the radio can never have back\n",
                     orphan_probe.releases.load());
        return 1;
      }
      unsigned unwritten = 0;
      for (unsigned k = 0; k != nof_subc; ++k) {
        if (host_word(grid_u16, dst_offset + k) == poison_word) {
          ++unwritten;
        }
      }
      std::fprintf(stderr,
                   "[dft-release] arm 8: a slot nobody asked about is swept - its grid is written and its "
                   "input comes back (%u of %u elements left unwritten by a LATER slot's block)\n",
                   unwritten,
                   nof_subc);
    }

    // ---- Arm 10: "no record" must never mean "the write is still in flight" (5.9.62) ----------------
    // The registry's eviction loop erases entries, and a reader that finds NOTHING cannot wait - so an entry
    // erased before its block COMPLETED is the one way a hop can read a grid nobody has written. Until
    // 5.9.62 the loop could do exactly that: with no produced entry to prefer it took the oldest unproduced
    // one, handed it to the late-commit path and erased it in the same breath.
    //
    // The bound is what makes that branch reachable, and at its production value (256) it is NOT reachable
    // from a test: each entry needs a real command buffer and Metal blocks at about sixty uncommitted ones
    // (measured). OCUDU_D1_HANDED_BOUND is the diagnostic override that closes that gap, and this arm is why
    // it exists. What is asserted is the invariant, not the number: nothing vanishes while in flight, and
    // the pressure is REPORTED (over_bound) instead of being paid for with the hole.
    {
      ::setenv("OCUDU_D1_HANDED_BOUND", "4", 1);
      id<MTLCommandQueue> queue = metal::shared_queue::backend_queue();
      if (queue == nil) {
        std::fprintf(stderr, "FAIL: arm 10 has no back-end queue\n");
        return 1;
      }
      // 6 > the bound of 4, and every buffer is real and NEVER committed, so nothing can be produced and
      // nothing is safe to reclaim.
      constexpr size_t over    = 6;
      constexpr uint64_t arm_slot = test_slot + 300;
      void*            pool    = compat::aligned_alloc(page, page * over);
      if (pool == nullptr) {
        std::fprintf(stderr, "FAIL: arm 10 could not allocate its storage pool\n");
        return 1;
      }
      const metal::shared_burst::handed_counters before = metal::shared_burst::handed_stats();
      for (size_t i = 0; i != over; ++i) {
        metal::shared_burst::deposit_released(static_cast<const char*>(pool) + i * page, arm_slot, [queue commandBuffer]);
      }
      const metal::shared_burst::handed_counters after = metal::shared_burst::handed_stats();

      // 1) Nothing was erased while in flight: the OLDEST entry - the first victim the old loop would have
      //    taken - is still there to be found.
      id<MTLCommandBuffer> oldest = metal::shared_burst::take_released(pool, arm_slot);
      // 2) The counter that says a reader was left with nothing did not move.
      // 3) The bound is soft, and its pressure is reported.
      // 4) The invariant counter stayed at zero.
      if ((oldest == nil) || (after.grid_not_found != before.grid_not_found) ||
          (after.evicted_unproduced != before.evicted_unproduced) || (after.over_bound <= before.over_bound)) {
        std::fprintf(stderr,
                     "FAIL: arm 10 oldest=%p not_found %llu->%llu evicted_unproduced %llu->%llu "
                     "over_bound %llu->%llu\n",
                     (__bridge const void*)oldest,
                     static_cast<unsigned long long>(before.grid_not_found),
                     static_cast<unsigned long long>(after.grid_not_found),
                     static_cast<unsigned long long>(before.evicted_unproduced),
                     static_cast<unsigned long long>(after.evicted_unproduced),
                     static_cast<unsigned long long>(before.over_bound),
                     static_cast<unsigned long long>(after.over_bound));
        return 1;
      }
      ::unsetenv("OCUDU_D1_HANDED_BOUND");
      std::fprintf(stderr,
                   "[dft-release] arm 10 (soft bound): %zu deposits over a bound of 4 kept every record "
                   "(oldest still found), evicted_unproduced stayed 0, over_bound=%llu\n",
                   over,
                   static_cast<unsigned long long>(after.over_bound - before.over_bound));
    }

    // ---- NOT tested here: the registry's bound, and the two halves of `evicted` (5.9.54 item 14) ------
    // Recorded because the attempt was made and its cost is the finding: the bound cannot be reached from
    // this test cheaply. `deposit_released` refuses a nil command buffer, so every entry needs a REAL one,
    // and Metal blocks at about sixty uncommitted command buffers per queue (measured: the run hangs inside
    // -[AGXG16XFamilyCommandQueue commandBuffer], on _dispatch_semaphore_wait) while the bound is 256.
    // Handing the same buffer to several entries instead is not an option either: the eviction path commits
    // the victim, so a shared buffer would be committed more than once.
    //
    // On air the bound IS reached - every armed leg carries handed - evicted == shared_burst::handed_capacity
    // exactly - because consumers commit along the way and free those slots. So the split is judged THERE,
    // by the relation the leg report prints: `evicted` alone is handed - capacity and says nothing, while
    // `evicted_unproduced` must stay at or below `late` (each of its entries owes a late commit) and well
    // below `evicted` (the registry prefers to evict an entry that was already produced). A mis-wired split
    // shows up as `evicted_unproduced == evicted` on a leg that is otherwise healthy.

    // ---- the "zero-copy wraps" contract arm (5.9.114) ---------------------------------------------
    //
    // Pass 3 of the audit (5.9.102) recorded that six of the eight contract checks had never been driven
    // red. This is one of them, and it lives HERE because its counter is reached through an
    // Objective-C++-only header (ocudu_metal_queue.h): the check claims the wrap cache never replaces, fails
    // or misaligns a mapping, and the cache has a public reporter for the misaligned case. What is verified
    // is the VERDICT the contract prints, not a message: the control reads it with the counters clean (this
    // test wraps buffers, so the check is applicable rather than "not applicable"), and the report must
    // turn it red.
    {
      phy_pipeline_mode_registry::set(phy_pipeline_mode::gpu);
      const phy_pipeline_check* wraps_check = nullptr;
      for (const phy_pipeline_check& candidate : phy_pipeline_checks()) {
        if (std::strcmp(candidate.name, "zero-copy wraps") == 0) {
          wraps_check = &candidate;
        }
      }
      if (wraps_check == nullptr) {
        std::fprintf(stderr, "[dft-release] FAIL: the 'zero-copy wraps' check is not registered\n");
        return 1;
      }
      const std::optional<bool> control = wraps_check->evaluate();
      metal::shared_queue::notify_wrap_misaligned();
      const std::optional<bool> armed = wraps_check->evaluate();
      if (!control.has_value() || !*control || !armed.has_value() || *armed) {
        std::fprintf(stderr,
                     "[dft-release] FAIL: 'zero-copy wraps' did not go red on a misaligned wrap "
                     "(control=%s, armed=%s)\n",
                     control.has_value() ? (*control ? "true" : "false") : "n/a",
                     armed.has_value() ? (*armed ? "true" : "false") : "n/a");
        return 1;
      }
      std::fprintf(stderr,
                   "[dft-release] PASS: 'zero-copy wraps' reads true on a clean cache and false after a "
                   "misaligned wrap\n");
    }

    std::fprintf(stderr,
                 "[dft-release] PASS: the block was handed over uncommitted, the lane adopted it, the "
                 "consumer read the grid it wrote, and the input's lifetime stayed with the block - one "
                 "command buffer, one commit\n");
    return 0;
  }
}
