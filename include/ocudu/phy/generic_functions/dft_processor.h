// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"

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
  /// \note Batching is *not* used by the radio-paced RX OFDM demodulation: the baseband samples
  /// of a slot arrive symbol by symbol in real time, so a slot-sized batch could not start
  /// before the last symbol arrives - it would only move the wait from the per-symbol dispatches
  /// to the end of the slot, without shortening the elapsed time of the time-frequency phase.
  /// The batched path is kept for the planned batched/multi-PUSCH processing (see the TODO in
  /// ofdm_demodulator.h), where several allocations are processed together and the samples are
  /// already available when the batch is issued.
  /// \todo Feed the batched path from a multi-allocation scheduler (multi-PUSCH / multi-slot
  ///       batches) once the samples of several allocations can be gathered before dispatching.
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
  /// \brief Executes the DFT without waiting for the result.
  ///
  /// The transform is dispatched and the call returns immediately, so the CPU stays free while the
  /// GPU works. The output returned by a later run()/run_batch() and the internal input buffer are
  /// only valid once the work is synchronized: either by a consumer stage dispatching on the same
  /// command queue (queues execute command buffers in submission order) or explicitly through the
  /// Metal engine's wait_all_committed() before the data is read on the CPU.
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
  virtual void run_async()
  {
    // Default: the processor has no asynchronous path, execute synchronously.
    (void)run();
  }

  virtual span<const cf_t> run_batch(unsigned nof_transforms)
  {
    ocudu_assert(nof_transforms == 1, "Batched DFT is not supported by this processor (requested {} transforms).",
                 nof_transforms);
    return run();
  }
};

} // namespace ocudu
