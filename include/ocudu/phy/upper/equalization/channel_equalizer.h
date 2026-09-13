// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

/// \file
/// \brief Channel equalizer interface.

#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/re_buffer.h"

#include <cstddef>
#include <optional>

namespace ocudu {

/// \brief Channel equalizer interface.
///
/// The channel equalizer mitigates the radio propagation channel effects. Also, it reverts the layer mapping described
/// by TS38.211 Section 6.3.1.3.
class channel_equalizer
{
public:
  /// \brief Interface for the list of channel estimates.
  class ch_est_list
  {
  public:
    /// Default destructor.
    virtual ~ch_est_list() = default;

    /// Gets a read-only channel given a receive port and layer indices.
    virtual span<const cbf16_t> get_channel(unsigned i_rx_port, unsigned i_layer) const = 0;

    /// Gets the number of resource elements.
    virtual unsigned get_nof_re() const = 0;

    /// Gets the number of receive ports.
    virtual unsigned get_nof_rx_ports() const = 0;

    /// Gets the number of transmit layers.
    virtual unsigned get_nof_tx_layers() const = 0;

    /// \brief Device-resident storage of the estimates of one receive port.
    ///
    /// The coefficients of every transmission layer of one OFDM symbol live in a single backend
    /// buffer - typically the one the channel estimator produced them in - starting at \c offset
    /// elements from \c base, \c layer_stride elements apart. A GPU backend binds that buffer
    /// instead of gathering the coefficients into its own staging, so the estimates are read where
    /// they were produced and never travel through host memory.
    struct device_slice {
      /// Buffer base, as wrapped by the stage that owns it.
      const void* base = nullptr;
      /// First coefficient of the first layer, in \c cbf16_t elements from \c base.
      std::size_t offset = 0;
      /// Number of coefficients between two consecutive layers.
      unsigned layer_stride = 0;
      /// Number of layers the slice holds.
      unsigned nof_layers = 0;
    };

    /// \brief Gets the device-resident storage of one receive port, if there is one.
    ///
    /// \note The default implementation reports "not available": a list that gathers the
    /// coefficients into host memory, or a backend that reads them from there, is unaffected.
    virtual std::optional<device_slice> get_device_slice(unsigned /*i_rx_port*/) const
    {
      return std::nullopt;
    }
  };

  /// Default destructor.
  virtual ~channel_equalizer() = default;

  /// Determines if the dimensions and algorithm are valid.
  virtual bool is_supported(unsigned nof_ports, unsigned nof_layers) = 0;

  /// \brief Equalizes the MIMO channel and combines Tx&ndash;Rx paths.
  ///
  /// For each transmit layer, the contributions of all receive ports are combined with weights obtained from the
  /// estimated channel coefficients. The variance of the point-to-point equivalent noise perturbing each modulated
  /// symbol is also estimated.
  ///
  /// Also, it reverts the layer mapping described by TS38.211 Section 6.3.1.3.
  ///
  /// \param[out] eq_symbols          Equalized modulation symbols.
  /// \param[out] eq_noise_vars       Post-equalization noise variances.
  /// \param[in]  ch_symbols          Channel symbols, i.e., complex samples organized by receive port.
  /// \param[in]  ch_estimates        Channel estimation coefficients, indexed by receive port and transmission layer.
  /// \param[in]  noise_var_estimates Noise variance estimation for each receive port.
  /// \param[in]  tx_scaling          Transmission gain scaling factor.
  /// \note The sizes of \c eq_symbols, \c eq_noise_vars, \c ch_symbols and \c noise_var_estimates must be consistent
  /// with the \c ch_estimates channel dimensions.
  /// \warning If, for a given transmitted RE, the combined channel across all paths results in a non-normal value
  /// (zero, infinity or NaN), the corresponding equalized modulation RE and equalized noise variance will be set to
  /// zero and infinity, respectively.
  /// \warning If the \c noise_var_estimates noise variances have ill-formed values (zero, negative, infinity or NaN),
  /// the corresponding equalized modulation REs and equalized noise variances will be set to zero and infinity,
  /// respectively.
  virtual void equalize(span<cf_t>                       eq_symbols,
                        span<float>                      eq_noise_vars,
                        const re_buffer_reader<cbf16_t>& ch_symbols,
                        const ch_est_list&               ch_estimates,
                        span<const float>                noise_var_estimates,
                        float                            tx_scaling) = 0;

  /// \name Deferred (fused) chain hooks.
  ///
  /// A GPU backend can keep the equalization of one symbol and the demodulation that consumes its
  /// output in a single command buffer, which removes the per-stage synchronization wait (each
  /// wait costs ~0.1 ms, and the PUSCH chain performs ~22 of them per slot). These hooks let the
  /// caller use that path. Every default implementation keeps the classic synchronous behaviour,
  /// so existing (CPU) backends are unaffected.
  ///@{

  /// \brief Submits the equalization of one symbol without waiting for the result.
  ///
  /// \param[in,out] eq_symbols      Equalized symbols, layout [RE][layer], written by the call.
  /// \param[out]    eq_noise_vars   Post-equalization noise variances, layout [RE][layer].
  /// \param[in]     ch_symbols      Received symbols per port.
  /// \param[in]     ch_estimates    Channel estimates per port and layer.
  /// \param[in]     noise_var_estimates Noise variance estimates per receive port.
  /// \param[in]     tx_scaling      Transmission gain scaling factor.
  /// \note The outputs are only valid after wait(). Backends whose supports_deferred_chain()
  ///       returns false execute synchronously, i.e. exactly like equalize().
  virtual void submit(span<cf_t>                       eq_symbols,
                      span<float>                      eq_noise_vars,
                      const re_buffer_reader<cbf16_t>& ch_symbols,
                      const ch_est_list&               ch_estimates,
                      span<const float>                noise_var_estimates,
                      float                            tx_scaling)
  {
    equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling);
  }

  /// \brief One symbol of a group submitted through submit_group() (same arguments as submit()).
  struct group_symbol {
    span<cf_t>                       eq_symbols;
    span<float>                      eq_noise_vars;
    const re_buffer_reader<cbf16_t>* ch_symbols;
    const ch_est_list*               ch_estimates;
    span<const float>                noise_var_estimates;
    float                            tx_scaling;
  };

  /// \brief Submits the equalization of a whole group of symbols without waiting.
  ///
  /// A GPU backend uses one call to encode the group as a few dispatches instead of one per symbol
  /// (the symbols of a group do not all share a geometry: a symbol carrying DM-RS has fewer active
  /// RE than a data-only one, so a backend may split the group accordingly). The default
  /// implementation is exactly the sequence of submit() calls that it replaces, so a backend that
  /// does not override it keeps its per-symbol behaviour.
  ///
  /// \note As for submit(), the outputs are only valid after wait().
  virtual void submit_group(span<const group_symbol> group)
  {
    for (const group_symbol& symbol : group) {
      submit(symbol.eq_symbols,
             symbol.eq_noise_vars,
             *symbol.ch_symbols,
             *symbol.ch_estimates,
             symbol.noise_var_estimates,
             symbol.tx_scaling);
    }
  }

  /// \brief Waits for the work submitted by submit().
  virtual void wait() {}

  /// \brief Post-equalization SINR reduction of the symbol submitted by the last submit().
  ///
  /// Computes over the equalized noise variances of that symbol: \c out[0] receives the sum of
  /// the finite values (infinities and NaN are skipped) and \c out[1] their count, matching the
  /// CPU reduction performed by the PUSCH demodulator.
  ///
  /// \return True when the reduction is available, false when the caller must compute it from the
  ///         equalized noise variances itself (default).
  virtual bool get_post_eq_sinr(span<float> out)
  {
    (void)out;
    return false;
  }

  /// \brief True when submit() defers the work, wait() synchronizes it and get_post_eq_sinr()
  /// provides the SINR reduction without a CPU pass over the equalized noise variances.
  virtual bool supports_deferred_chain() const { return false; }
  ///@}
};

} // namespace ocudu
