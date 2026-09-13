// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Channel estimator declaration.

#pragma once

#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/phy/upper/dmrs_mapping.h"
#include "ocudu/phy/upper/re_measurement.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/ran/subcarrier_spacing.h"
#include <optional>

namespace ocudu {

class resource_grid_reader;

/// Container for DM-RS symbols.
using dmrs_symbol_list = static_re_measurement<cf_t,
                                               pusch_constants::MAX_NOF_DMRS_SUBC,
                                               pusch_constants::MAX_NOF_DMRS_SYMBOLS,
                                               pusch_constants::MAX_NOF_LAYERS>;

class port_channel_estimator_results;

/// \brief Device-resident (GPU) channel estimates of one OFDM symbol: the optional fast path that
/// lets a consumer read the estimates where the estimator produced them instead of gathering them
/// out of a host grid, RE by RE.
///
/// The buffer belongs to the estimator and stays valid until its next estimation. A consumer MUST
/// check \c nof_re against the number of REs of the mask it is about to apply: the estimator
/// derives the RE layout of the allocation itself, so a mismatch means the two disagree (a
/// non-contiguous allocation, for example) and the host path is the correct source.
struct ch_est_device_view {
  /// Base of the layer-major buffer: layer \c l starts at <tt>data + l * total_re</tt>.
  const cbf16_t* data = nullptr;
  /// First RE of the requested symbol within each layer.
  unsigned offset = 0;
  /// Number of REs of the requested symbol.
  unsigned nof_re = 0;
  /// REs per layer (the layer stride).
  unsigned total_re = 0;
  /// Number of layers the buffer holds.
  unsigned nof_layers = 0;

  /// Estimates of one transmission layer, or an empty span when the layer is out of range.
  span<const cbf16_t> get_layer(unsigned tx_layer) const
  {
    if ((data == nullptr) || (tx_layer >= nof_layers)) {
      return {};
    }
    return span<const cbf16_t>(data + static_cast<std::size_t>(tx_layer) * total_re + offset, nof_re);
  }
};

/// DM-RS-based channel estimator for one receive antenna port.
class port_channel_estimator
{
public:
  /// \brief DM-RS pattern to specify the position of DM-RS symbols in the resource grid.
  ///
  /// DM-RS symbols are transmitted with a specific subcarrier pattern that repeats at specific OFDM symbols within the
  /// slot. Therefore, the position of the DM-RS symbols can be univocally characterized by a time mask and a frequency
  /// mask: Resource element \f$(k, l)\f$ contains a DM-RS symbol only if element \f$k\f$ of the frequency mask and
  /// element \f$l\f$ of the time mask are set to true.
  struct layer_dmrs_pattern {
    /// Boolean mask to specify the OFDM symbols carrying DM-RS.
    bounded_bitset<MAX_NSYMB_PER_SLOT> symbols;
    /// Boolean mask to specify the resource blocks carrying DM-RS.
    crb_bitmap rb_mask;
    /// Boolean mask to specify the resource blocks carrying DM-RS after the frequency hop.
    crb_bitmap rb_mask2;
    /// Symbol index within the slot in which the first hop occurs if it has a value.
    std::optional<unsigned> hopping_symbol_index;
    /// Boolean mask to specify the resource elements within the resource blocks carrying DM-RS symbols.
    bounded_bitset<NOF_SUBCARRIERS_PER_RB> re_pattern;
  };

  /// Estimator configuration parameters.
  struct configuration {
    /// Subcarrier spacing of the estimated channel.
    subcarrier_spacing scs = subcarrier_spacing::kHz15;
    /// Cyclic prefix.
    cyclic_prefix cp;
    /// First OFDM symbol within the slot for which the channel should be estimated.
    unsigned first_symbol;
    /// Number of OFDM symbols for which the channel should be estimated.
    unsigned nof_symbols;
    /// DM-RS pattern for each layer. The number of elements, indicates the number of layers.
    static_vector<layer_dmrs_pattern, pusch_constants::MAX_NOF_LAYERS> dmrs_pattern;
    /// List of receive ports.
    static_vector<uint8_t, DMRS_MAX_NPORTS> rx_ports;
    /// \brief DM-RS scaling factor with respect to data amplitude.
    ///
    /// Should be equal to one for PUCCH and equal to parameter \f$\beta_{\textup{PUSCH}}^{\textup{DMRS}}\f$ (see
    /// TS38.214 Section 6.2.2) for PUSCH.
    float scaling = 1;
    /// \brief DC subcarrier of the cell, in absolute subcarriers within the BWP, when the
    /// allocation contains it.
    ///
    /// The DC subcarrier carries no data (TS38.211 Section 6.3.1.7): a consumer of the estimates
    /// erases that resource element, and an estimator that builds the consumer's input on the
    /// device must write it as zero. Estimators that let the consumer gather the estimates itself
    /// may ignore it.
    std::optional<unsigned> dc_position;
  };

  /// Default destructor.
  virtual ~port_channel_estimator() = default;

  /// \brief Computes the channel estimate.
  ///
  /// Estimates the channel for all transmit layers, as seen by the selected receive antenna port. The estimated channel
  /// and the associated metrics (e.g., EPRE, SINR) are stored internally and accessible via the \c get methods of the
  /// \c port_channel_estimator_results.
  /// \param[in]  grid     The frequency&ndash;time resource grid.
  /// \param[in]  port     Receive antenna port the estimation refers to.
  /// \param[in]  pilots   DM-RS symbols (a.k.a. pilots). For each layer, symbols are listed by RE first and then by
  ///                      OFDM symbol.
  /// \param[in]  cfg      Estimator configuration.
  /// \return A reader interface for accessing the estimated channel coefficients and metrics.
  virtual const port_channel_estimator_results& compute(const resource_grid_reader& grid,
                                                        unsigned                    port,
                                                        const dmrs_symbol_list&     pilots,
                                                        const configuration&        cfg) = 0;

  /// \brief Starts the channel estimation, without waiting for it.
  ///
  /// Same inputs as compute(). A backend that dispatches the work to a device may return from this
  /// call before the results exist, which lets the rest of the receiving chain run while the device
  /// works; the caller must then complete it with finish() before reading any host-visible result.
  ///
  /// The default implementation calls compute(), so the estimation is already complete when it
  /// returns and an estimator that runs on the host is unaffected.
  /// \return The results of this estimation - the same reader compute() returns, whose host-visible
  ///         values are only complete after finish().
  virtual const port_channel_estimator_results& submit(const resource_grid_reader& grid,
                                                       unsigned                    port,
                                                       const dmrs_symbol_list&     pilots,
                                                       const configuration&        cfg)
  {
    return compute(grid, port, pilots, cfg);
  }

  /// \brief Completes an estimation started by submit().
  ///
  /// \param[in] pilots The same DM-RS symbols submit() was called with. They must outlive the
  ///                   estimation: the stages that complete it read them.
  /// \return True when the results are available. False when a deferred stage failed, in which case
  ///         the results must not be used.
  /// \note The default implementation has nothing to do (submit() completed the estimation).
  virtual bool finish(const dmrs_symbol_list& /*pilots*/) { return true; }
};

/// \brief Port channel estimator reader.
///
/// Provides access to the estimated channel coefficients and metrics.
class port_channel_estimator_results
{
public:
  /// Default destructor.
  virtual ~port_channel_estimator_results() = default;

  /// \brief Gets the estimated channel coefficients for one OFDM symbol.
  /// \param[out] symbol    Storage for the channel coefficients.
  /// \param[in]  i_symbol  Index of the OFDM symbol within the slot.
  /// \param[in]  tx_layer  Transmission layer the estimated channel refers to.
  /// \warning The method raises an assertion if the size of \c symbol does not match the size of the estimated channel
  /// in the frequency domain.
  virtual void get_symbol_ch_estimate(span<cbf16_t> symbol, unsigned i_symbol, unsigned tx_layer) const = 0;

  /// \brief Gets the estimated channel coefficients for one OFDM symbol, at the REs specified by the boolean mask.
  /// \param[out] symbol    Storage for the channel coefficients.
  /// \param[in]  i_symbol  Index of the OFDM symbol within the slot.
  /// \param[in]  tx_layer  Transmission layer the estimated channel refers to.
  /// \param[in]  re_mask   Boolean mask of requested REs (only relative to the allocated RBs).
  /// \warning The method raises an assertion if the size of \c re_mask does not match the grid size stored in the
  /// channel estimator or if the size of \c symbol does not match the number of active REs in the mask.
  virtual void get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                                      unsigned                                   i_symbol,
                                      unsigned                                   tx_layer,
                                      const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const = 0;

  /// \brief Gets the device-resident channel estimates of one OFDM symbol, when the estimator
  /// produces them (see \ref ch_est_device_view).
  ///
  /// The default implementation reports "not available": an estimator that runs on the host, or a
  /// consumer that does not want the device path, keeps working unchanged.
  virtual std::optional<ch_est_device_view> get_device_ch_estimates(unsigned i_symbol, unsigned tx_layer) const
  {
    return std::nullopt;
  }

  /// \brief Whether the device-resident results of the last estimation cover everything a consumer
  /// of that estimate needs: the estimates of every symbol and layer, and the noise variance.
  ///
  /// A consumer that reads them where they were produced needs no synchronization: the device queue
  /// orders the estimation before its own dispatch. One that does not must complete the estimation
  /// first (see dmrs_pusch_estimator_results::sync_device_estimates()).
  ///
  /// \note The default reports "no", which is also the answer for an estimator that runs on the
  /// host.
  virtual bool device_results_cover_last_estimate() const { return false; }

  /// \brief Gets the device-resident noise variance, when the estimator produces it there.
  ///
  /// The value lives in the same device buffer the estimates do, so a consumer that binds them
  /// binds this too and needs no host copy of either (see \ref ch_est_device_view).
  ///
  /// \return The device address of the noise variance, or nullptr when there is none (the default).
  virtual const float* get_device_noise_variance() const { return nullptr; }

  /// Gets the estimated EPRE.
  virtual float get_epre() const = 0;

  /// Gets the estimated noise variance.
  virtual float get_noise_variance() const = 0;

  /// Gets the estimated signal-to-noise ratio (linear scale).
  virtual float get_snr() const = 0;

  /// Gets the estimated RSRP for the given transmission layer.
  virtual float get_rsrp(unsigned tx_layer) const = 0;

  /// \brief Gets the estimated carrier frequency offset in hertz.
  ///
  /// The CFO can only be computed if the PUSCH PDU has two or more OFDM symbols carrying DM-RS in each hop.
  virtual std::optional<float> get_cfo_Hz() const = 0;

  /// Gets the estimated time alignment in PHY time units.
  virtual phy_time_unit get_time_alignment() const = 0;
};

} // namespace ocudu
