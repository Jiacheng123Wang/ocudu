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
  virtual span<const cf_t> run_batch(unsigned nof_transforms)
  {
    ocudu_assert(nof_transforms == 1, "Batched DFT is not supported by this processor (requested {} transforms).",
                 nof_transforms);
    return run();
  }
};

} // namespace ocudu
