// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/generic_functions/dft_processor_grid_write.h"

namespace ocudu {

/// \brief Describes a Discrete Fourier Transform (DFT) processor.
/// \remark The DFT processor allocates the input and output buffers for optimization purposes.
/// \remark The input buffer is available using get_input().
/// \remark The output buffer is available using run().
class dft_processor
{
public:
  /// Indicates the DFT direction.
  enum class direction {
    /// \brief Converts a time domain signal into frequency domain.
    /// \remark For the FFTW based libraries, it is equivalent to \c FFTW_FORWARD sign.
    DIRECT = 0,
    /// \brief Converts a frequency domain signal into frequency (inverse DFT).
    /// \remark For the FFTW based libraries, it is equivalent to \c FFTW_BACKWARD sign.
    INVERSE
  };

  /// Describes the DFT parameters.
  struct configuration {
    /// Indicates the DFT size.
    unsigned size;
    /// Indicates if the DFT is direct or inverse.
    direction dir;
  };

  /// Converts a DFT direction to string.
  static std::string direction_to_string(direction dir) { return dir == direction::DIRECT ? "direct" : "inverse"; }

  /// Default destructor.
  virtual ~dft_processor() = default;

  /// \brief Gets the DFT direction.
  virtual direction get_direction() const = 0;

  /// \brief Gets the DFT number of points.
  virtual unsigned get_size() const = 0;

  /// \brief Gets a view of the internal input DFT buffer.
  ///
  /// \note Processors supporting batching (see get_max_batch()) expose the whole batch buffer,
  /// i.e. \c get_max_batch() * get_size() complex samples: the caller fills the inputs of the
  /// transforms it is about to run back to back. Callers that run one transform at a time use
  /// the first get_size() samples only.
  virtual span<cf_t> get_input() = 0;

  /// \brief Executes the DFT from the internal input data.
  /// \return A view of the internal output DFT buffer.
  virtual span<const cf_t> run() = 0;

  /// \brief Maximum number of transforms a single run_batch() call can execute.
  ///
  /// The default is 1 (one transform per call). Implementations backed by hardware that can
  /// process several independent transforms in one dispatch report a larger value.
  ///
  /// \note Batching applies to *independent transform streams*, of which there are exactly three
  /// kinds:
  ///  - multiple Rx ports of the same symbol: their transforms are independent and their samples
  ///    are already available together, so they can be batched with no added latency;
  ///  - multiple carriers / sectors (one symbol stream each): the same symbol index of several
  ///    carriers can share one dispatch (they must have the same transform size);
  ///  - several OFDM symbols of one stream, which is *not* used by the radio-paced RX
  ///    demodulation: the samples arrive symbol by symbol, so a slot-sized batch could only start
  ///    once the last symbol arrived - that moves the wait instead of removing it.
  /// Multiple PUSCH allocations or UEs of one cell share the very same per-symbol transform, so
  /// they add no transform and cannot benefit from batching.
  /// \todo Batch across Rx ports (immediately usable: no added latency) and across carriers /
  ///       sectors once several symbol streams are driven from one place.
  virtual unsigned get_max_batch() const { return 1; }

  /// \brief Executes \c nof_transforms transforms over the contiguous input buffer.
  ///
  /// The transforms are independent: transform \c i reads the input samples
  /// <tt>[i * get_size(), (i + 1) * get_size())</tt> and writes its output to the same range of
  /// the returned span.
  ///
  /// \param[in] nof_transforms Number of transforms, at most get_max_batch().
  /// \return A view of the internal output DFT buffer holding \c nof_transforms * get_size()
  ///         complex samples.
  /// \brief Executes the transform held in \c slot without waiting for the result.
  ///
  /// The transform is dispatched and the call returns immediately, so the CPU stays free while the
  /// GPU works. The internal buffers cover get_max_batch() transform slots: a pipelined caller
  /// fills slot after slot (a ring) and keeps several transforms in flight instead of stalling the
  /// CPU on each one. The output and the input slot are only valid once the work is synchronized:
  /// either by a consumer stage dispatching on the same command queue (queues execute command
  /// buffers in submission order) or explicitly through wait() before the data is read on the CPU.
  ///
  /// \note Intended for the CPU/GPU pipelined RX chain: the per-symbol DFTs are submitted without
  /// stalling the CPU and the consumer stage (channel estimator) synchronizes once.
  /// \warning The caller must not overwrite the input buffer (get_input()) nor read the output
  ///          until the submission is synchronized: an in-flight transform still reads its input
  ///          while the GPU executes it. Pipelined callers therefore need one input/output buffer
  ///          set per in-flight transform and must advance the ring only after synchronizing.
  /// \todo Provide the per-in-flight-transform ring (double/quad buffering) with the pipeline
  ///       decoupling work, so the RX chain can submit several symbols before the first wait.
  /// \todo Chain the RX stages onto one command queue and synchronize once per slot instead of
  ///       once per consumer stage, together with the pipeline decoupling work.
  virtual void run_async(unsigned slot)
  {
    // Default: the processor has no asynchronous path, execute synchronously.
    (void)slot;
    (void)run();
  }

  /// \brief Waits for every previously submitted asynchronous transform.
  ///
  /// No-op for processors without an asynchronous path. On a shared Metal queue the wait drains
  /// every earlier submission of every stage sharing the queue, not only this processor's.
  virtual void wait() {}

  /// \brief Opens one command buffer for the transforms of the caller's current block of samples.
  ///
  /// A caller that holds a whole block of samples (the receiving chain under the whole-slot policy holds
  /// a whole slot) can encode its transforms into one command buffer instead of one each: what that
  /// saves is the per-command-buffer cost of the GPU timeline, which is real (a command buffer costs the
  /// same whether it carries one transform or fourteen, see the DFT unit test) - but only samples that
  /// have ALREADY ARRIVED may be batched, or the batch trades a command buffer for a stall. Both calls
  /// are no-ops for processors without such a path.
  /// \return Whether the block is open (false: every transform keeps its own command buffer).
  virtual bool begin_block() { return false; }

  /// \brief Closes the block opened by begin_block(): commits the command buffer its transforms went into.
  virtual bool end_block() { return false; }

  /// \brief Closes the block opened by begin_block() and HANDS ITS COMMAND BUFFER OVER, uncommitted (D1).
  ///
  /// The counterpart of end_block() for the fused lane: instead of committing the slot's transforms, the
  /// engine deposits the command buffer under the resource grid that block wrote, and the hop that reads
  /// that grid adopts it - so the transforms, the extraction, the weights, the equalization and the
  /// demapping are ONE submission that the lane commits once (see dft_metal_engine::release_block()).
  ///
  /// The caller owes what a commit owed: nothing else may read those transforms, and whoever adopts the
  /// buffer commits it. It is therefore called only where a consumer of that grid is guaranteed - the
  /// receiving chain's device-consumed path.
  ///
  /// \param[in] grid_base Storage base of the resource grid the block wrote
  ///            (resource_grid_device_view::base), i.e. the key its consumer takes it by.
  /// \return True when a block was handed over. False for every processor without such a path, and for
  ///         one whose release path is not armed.
  virtual bool release_block(const void* /*grid_base*/) { return false; }

  /// \brief Whether a transform this processor accepted may be EXECUTED after the caller's wait returned.
  ///
  /// The receiving chain's samples live in the radio's receive buffer, and what tells the radio that they
  /// may be recycled is `finish_symbol()` returning (puxch_processor_impl::finish_oldest_symbol() retires
  /// the buffer's handle right after it). A backend that answers true here defers its dispatches past that
  /// point - the fused lane's single submission does - and the caller must then hand the input over with
  /// retain_input() instead of relying on the wait. False (the default, and every backend that waits) keeps
  /// the historical lifetime exactly as it was.
  virtual bool defers_transform_execution() const { return false; }

  /// \brief Hands the input a submitted transform reads over to the block that will run it (D1, 5.9.7).
  ///
  /// Attach one per piece of input (the receiving chain attaches the samples a symbol was demodulated
  /// from), after the corresponding submission and only while defers_transform_execution() is true.
  ///
  /// \param[in] release Called EXACTLY ONCE with \p context - when the block that carries the transform
  ///            completes on the device, or when that block is definitively dropped. It may run on a
  ///            completion thread: the caller's release must be thread-safe, or must only hand the work
  ///            back to the thread that owns the state.
  /// \param[in] context Opaque; owned by this processor once this returns true.
  /// \return True when the input is now this processor's to release; false when it did not take it (the
  ///         caller then keeps its own lifetime rule, which is what every non-deferring path does).
  virtual bool retain_input(void (* /*release*/)(void* /*context*/), void* /*context*/) { return false; }

  /// \brief Tells the processor which receiving slot the transforms it is about to submit belong to.
  ///
  /// Instrumentation only: the device probe accounts the GPU time of a slot's transforms, and they are
  /// submitted by the radio thread while the rest of the chain runs on another one, so the slot is the
  /// identity that lets the two timelines be compared. A processor without a device probe ignores it.
  virtual void set_lane_slot(uint64_t /*slot_index*/) {}

  /// \brief Tells the processor how many OFDM symbols ONE receiving slot of this cell carries.
  ///
  /// 14 with a normal cyclic prefix and 12 with an extended one (the caller that knows the slot - see
  /// set_lane_slot() - is the one that knows this too). It is the unit the fused lane's batched front end
  /// works in: the transforms of one slot are handed over as one block, and the front end dispatches them in
  /// ONE dispatch per slot, so the number it batches by must be the CELL's, not a constant. A processor that
  /// does not batch ignores this, and one that is never told does not batch at all (rather than guessing).
  virtual void set_slot_symbols(unsigned /*nof_symbols_per_slot*/) {}

  /// \brief Waits for the transform submitted in \c slot.
  ///
  /// Releases the caller's input slot for reuse without stalling on newer submissions, which is
  /// what a ring pipeline needs (wait() would also cover the newest one).
  virtual void wait_slot(unsigned slot) { (void)slot; }

  /// \brief View of the ring outputs: get_max_batch() * get_size() complex samples.
  ///
  /// Empty for processors without an asynchronous path. Valid after the corresponding
  /// wait_slot()/wait().
  virtual span<const cf_t> get_output_batch() { return {}; }

  virtual span<const cf_t> run_batch(unsigned nof_transforms)
  {
    ocudu_assert(nof_transforms == 1, "Batched DFT is not supported by this processor (requested {} transforms).",
                 nof_transforms);
    return run();
  }

  /// \brief Grid-write capability of this processor, when it has one.
  ///
  /// Lets a demodulator fill the resource grid from the device without knowing which implementation it holds (the
  /// build has no RTTI, and not every processor can do it). A processor that returns nullptr is asked for nothing:
  /// its caller writes the grid from the host, as it always did.
  ///
  /// \return The capability, or nullptr.
  virtual dft_processor_grid_write* get_grid_write() { return nullptr; }
};

} // namespace ocudu
