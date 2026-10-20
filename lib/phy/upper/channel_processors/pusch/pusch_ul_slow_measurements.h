// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/span.h"
#include <cstdint>
#include <limits>

namespace ocudu {

/// \brief One pilot sample, as every slow measurement consumes it: the least-squares estimate of the channel at a
/// DM-RS resource element, in GRID units, tagged with the symbol it came from.
///
/// WHY IT IS ITS OWN TYPE (design section 19.244). These measurements happen BEFORE a network is called and must
/// be shared by every network, including ones that do not have the fourteen-symbol plane contract this tree's
/// depth-3 arm assumes. A plane-shaped input would tie them to that contract; pilot samples do not, and a caller
/// that has the grid can produce them for any symbol count.
struct ul_pilot_sample {
  /// Symbol index within the slot the pilot was received in.
  unsigned symbol = 0;
  /// Least-squares channel estimate, real part, in the grid's units.
  float ls_re = 0.0F;
  /// Least-squares channel estimate, imaginary part, in the grid's units.
  float ls_im = 0.0F;
};

/// \brief The slow measurements a reception yields before any network sees it.
///
/// Every quantity is in the GRID's units, which is the convention the classical estimator publishes in, so the two
/// can be compared without a conversion: \ref epre_lin is the received power at the DM-RS resource elements,
/// \ref noise_lin is the noise variance estimated from the pilot least-squares values alone, and \ref snr_lin is
/// the data-RE power over that noise, i.e. the estimator's own definition.
struct ul_slow_measurements {
  /// Average received power at the DM-RS resource elements, linear.
  float epre_lin = 0.0F;
  /// Noise variance, linear, in the grid's units.
  float noise_lin = 0.0F;
  /// Signal to noise ratio at the data resource elements, linear.
  float snr_lin = 0.0F;
  /// Carrier frequency offset in cycles per symbol, or NaN when the samples carry fewer than two symbols.
  float cfo_cycles_per_symbol = std::numeric_limits<float>::quiet_NaN();
  /// Pilots the averages were taken over. Zero means nothing was measured and every value above is zero.
  unsigned nof_pilot_re = 0;
  /// Residuals the noise estimate was taken over. Zero means the noise is not defined.
  unsigned nof_residuals = 0;
};

/// \brief Measures EPRE, noise, SNR and CFO from pilot samples.
///
/// \param[in] pilots              Pilot samples ordered by SYMBOL and, within a symbol, by subcarrier. The ordering
///                                is part of the contract: the noise estimate is a second difference along the
///                                sample sequence and must never span two symbols, and the CFO pairs the k-th sample
///                                of the first two symbols.
/// \param[in] scaling             DM-RS to data amplitude ratio, i.e. the factor by which the DM-RS resource
///                                elements are stronger than the data ones.
/// \param[in] nof_layers          Number of transmit layers the pilots belong to.
/// \return The measurements, with \ref ul_slow_measurements::nof_pilot_re zero when the input was empty.
ul_slow_measurements measure_ul_slow(span<const ul_pilot_sample> pilots, float scaling, float nof_layers);

} // namespace ocudu
