// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#pragma once

#include "ocudu/phy/support/interpolator.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator.h"
#include "ocudu/phy/upper/signal_processors/channel_estimator/port_channel_estimator_parameters.h"
#include <optional>

namespace ocudu {

/// \brief Port channel estimator that averages all OFDM symbols containing DM-RS.
///
/// This estimator considers the channel constant in time over the entire slot. As for the frequency domain, the channel
/// coefficients corresponding to subcarriers containing DM-RS pilots are LSE-estimated, while the remaining
/// coefficients are estimated by interpolating the previous ones.
class port_channel_estimator_average_impl : public port_channel_estimator, private port_channel_estimator_results
{
public:
  /// Maximum supported number of layers.
  static constexpr unsigned MAX_LAYERS = 4;

  /// \brief Maximum SINR in decibels.
  ///
  /// The SINR is bounded above to avoid a zero noise variance.
  static constexpr float MAX_SINR_DB = 100;

  /// Maximum number of virtual pilots used for estimation at the edges.
  static constexpr unsigned MAX_V_PILOTS = 12;

  /// Maximum number of pilots per OFDM symbol.
  static constexpr unsigned MAX_NOF_PILOTS_SYMBOL = MAX_NOF_SUBCARRIERS + 2 * MAX_V_PILOTS;

  /// Maximum number of OFDM symbols that contain DM-RS in a transmission.
  static constexpr unsigned MAX_NOF_DMRS_SYMBOLS = pusch_constants::MAX_NOF_DMRS_SYMBOLS;

  /// Constructor - Sets the internal interpolator and inverse DFT processor of size \c DFT_SIZE.
  port_channel_estimator_average_impl(std::unique_ptr<interpolator>                    interp,
                                      std::unique_ptr<time_alignment_estimator>        ta_estimator_,
                                      port_channel_estimator_fd_smoothing_strategy     fd_smoothing_strategy_,
                                      port_channel_estimator_td_interpolation_strategy td_interpolation_strategy_,
                                      bool                                             compensate_cfo_ = true) :
    fd_smoothing_strategy(fd_smoothing_strategy_),
    td_interpolation_strategy(td_interpolation_strategy_),
    compensate_cfo(compensate_cfo_),
    freq_interpolator(std::move(interp)),
    ta_estimator(std::move(ta_estimator_))
  {
    ocudu_assert(freq_interpolator, "Invalid interpolator.");
    ocudu_assert(ta_estimator, "Invalid TA estimator.");
  }

  // See the port_channel_estimator interface for documentation.
  const port_channel_estimator_results& compute(const resource_grid_reader& grid,
                                                unsigned                    port,
                                                const dmrs_symbol_list&     pilots,
                                                const configuration&        cfg) override
  {
    cfg_local = cfg;
    // compute() completes every hop it submits, so no stage is ever left running: a device backend
    // must not hand its dispatches to a command buffer that someone else commits, because there is
    // no gap for that commit to happen in (see fd_td_estimation_stage_args::deferred).
    do_submit(grid, port, pilots, /*deferred=*/false);
    do_finish(pilots);
    return *this;
  }

  // See the port_channel_estimator interface for documentation.
  const port_channel_estimator_results& submit(const resource_grid_reader& grid,
                                              unsigned                    port,
                                              const dmrs_symbol_list&     pilots,
                                              const configuration&        cfg) override
  {
    cfg_local = cfg;
    // The caller will collect the last hop through finish(), and the receiving chain runs in
    // between: that gap is what the deferred stage of a device backend may use.
    do_submit(grid, port, pilots, /*deferred=*/true);
    return *this;
  }

  // See the port_channel_estimator interface for documentation.
  bool finish(const dmrs_symbol_list& pilots) override { return do_finish(pilots); }

protected:
  // See the port_channel_estimator_results interface for documentation.
  // (protected so derived estimators can delegate to the classical fallback path).
  void get_symbol_ch_estimate(span<cbf16_t> symbol, unsigned i_symbol, unsigned tx_layer) const override;

  // See the port_channel_estimator_results interface for documentation.
  void get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                              unsigned                                   i_symbol,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override;

  // See the port_channel_estimator_results interface for documentation.
  // (protected so the HELENA impl can gate the NN on the estimated SNR: beyond the
  // training envelope the classical grid is near-optimal and the NN would corrupt it).
  float get_noise_variance() const override { return noise_var; }

  // See the port_channel_estimator_results interface for documentation.
  float get_snr() const override { return snr_linear; }

private:

  // See the port_channel_estimator_results interface for documentation.
  float get_epre() const override { return epre; }

  // See the port_channel_estimator_results interface for documentation.
  float get_rsrp(unsigned tx_layer) const override
  {
    ocudu_assert(tx_layer < cfg_local.dmrs_pattern.size(),
                 "Layer index {} is larger than the maximum supported index {}.",
                 tx_layer,
                 cfg_local.dmrs_pattern.size() - 1);
    return rsrp[tx_layer];
  }

  // See the port_channel_estimator_results interface for documentation.
  std::optional<float> get_cfo_Hz() const override { return cfo_Hz; }

  // See the port_channel_estimator_results interface for documentation.
  phy_time_unit get_time_alignment() const override { return phy_time_unit::from_seconds(time_alignment_s); }

  /// Actual implementation of the \c compute public method.
  void do_compute(const resource_grid_reader& grid, unsigned port, const dmrs_symbol_list& pilots);

  /// \brief First phase of do_compute(): per-hop pilot processing and the estimation stage.
  ///
  /// The last hop is left for do_finish() to complete: its estimation stage may return before its
  /// results exist (see port_channel_estimator::submit()), and holding it back is what lets the rest
  /// of the receiving chain overlap the device work. Every other hop is completed here, so nothing
  /// of the shared staging outlives its own hop.
  /// \param[in] deferred Whether the caller promises to complete the last hop later, through
  ///             finish() (port_channel_estimator::submit()) instead of inside this call
  ///             (port_channel_estimator::compute()). It reaches the estimation stage through
  ///             fd_td_estimation_stage_args::deferred, and it is FALSE for every hop this method
  ///             completes itself - including hop 0 of a hopping slot, which the second hop forces
  ///             to complete here because they share the staging.
  void do_submit(const resource_grid_reader& grid, unsigned port, const dmrs_symbol_list& pilots, bool deferred);

  /// \brief Second phase of do_compute(): completes the last hop and derives the metrics.
  /// \return False when the deferred stage of the last hop failed (the metrics are then meaningless).
  bool do_finish(const dmrs_symbol_list& pilots);

protected:
  /// \brief Arguments passed to the FD+TD estimation stage virtual hook.
  ///
  /// The stage is responsible for producing, from the LSE pilots, the filtered pilot estimates
  /// (stored in \c filtered_pilots_lse_view, used downstream for RSrp, noise variance and time
  /// alignment) and the frequency-domain channel response (\c freq_response, consumed by the
  /// time-domain strategy of the default implementation). Implementations may also produce and
  /// store their own full time-frequency grid and override \c get_symbol_ch_estimate instead of
  /// relying on the time-domain strategy.
  struct fd_td_estimation_stage_args {
    /// Resource grid the hop's pilots were extracted from, and the receive port they belong to.
    ///
    /// The host stage does not need them (it is handed the extracted pilots), but a DEVICE backend
    /// does: it runs the extraction itself, reading the grid in place through
    /// resource_grid_reader::get_device_view() - the grid is already device-resident (S-7b), so
    /// nothing has to be brought over. See ocudu_mmse_pilots.metal (K0-a).
    const resource_grid_reader& grid;
    unsigned                    port;
    /// Transmitted pilots (per layer, per DM-RS symbol).
    const dmrs_symbol_list& pilots;
    /// Received pilots (per CDM group, per DM-RS symbol).
    const dmrs_symbol_list& rx_pilots;
    /// DM-RS patterns, one per transmission layer.
    span<const layer_dmrs_pattern> dmrs_patterns;
    /// Boolean mask of the OFDM symbols carrying DM-RS in the slot.
    const bounded_bitset<MAX_NSYMB_PER_SLOT>& pattern_symbols;
    /// Index of the first OFDM symbol of the current hop, within the slot.
    unsigned first_symbol;
    /// Index of the last OFDM symbol of the current hop (not included), within the slot.
    unsigned last_symbol;
    /// Number of OFDM symbols carrying DM-RS in the current hop.
    unsigned nof_dmrs_symbols;
    /// Number of pilots in a single OFDM symbol carrying DM-RS.
    unsigned nof_symbol_pilots;
    /// Current hop index (0 or 1).
    unsigned hop;
    /// Number of OFDM symbols carrying DM-RS in the previous hop.
    unsigned hop_offset;
    /// DM-RS to data amplitude scaling.
    float beta_scaling;
    /// Estimated CFO of the current hop (empty when unavailable).
    std::optional<float> cfo_hop;
    /// Starting time of the symbols inside the slot, in units of OFDM symbol duration.
    span<const float> symbol_start_epochs;
    /// Whether CFO compensation is active.
    bool compensate_cfo_flag;
    /// Subcarrier spacing of the current transmission.
    subcarrier_spacing scs;
    /// DC subcarrier of the cell, in absolute subcarriers within the BWP, when the allocation
    /// contains it (see port_channel_estimator::configuration).
    std::optional<unsigned> dc_position;
    /// View over the LSE pilots buffer (per symbol per layer).
    modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS>& pilots_lse_view;
    /// View over the filtered-pilots buffer — the stage MUST fill it with the filtered
    /// channel estimates at the pilot REs, scaled by the same factor the FD processing
    /// applies (see \ref apply_fd_td_estimation_stage); used for RSrp, noise variance and TA.
    modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS>& filtered_pilots_lse_view;
    /// Enlarged filtered-pilots buffer (per LSE symbol per layer; includes virtual-pilot
    /// headroom, see the default stage implementation).
    static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS>&
        enlarged_filtered_pilots_lse;
    /// Frequency-response output buffer (per LSE symbol per layer).
    re_measurement<cf_t>& freq_response;
    /// \brief CPU time spent in this hop before the stage (pilot extraction from the grid, EPRE, LSE, CFO and the
    /// argument assembly), in microseconds. Measured only by builds with the channel-estimation phase probe
    /// (OCUDU_CE_TIME); zero otherwise. Reported by [mmse_time]/[mmse_time_sum] as the pre= field.
    double pre_stage_us = 0.0;
    /// \brief Whether the caller collects this hop later, through finish(), instead of completing it
    /// inside the call that submits it (see port_channel_estimator::submit() and compute()).
    ///
    /// A device backend whose completion is what publishes the hop's results - the Metal MMSE
    /// estimator reads two scalars back from the command buffer that produces them - may use this to
    /// encode its dispatches into the command buffer the rest of the receiving chain shares, and let
    /// the chain's own synchronization cover them (see ocudu_metal_burst.h). It MUST NOT do that when
    /// this is false: nothing would commit those dispatches before the completion reads them, and the
    /// hop would be estimated from stale memory - silently.
    bool deferred = false;
  };

  /// \brief FD+TD estimation stage of one hop.
  ///
  /// The default implementation performs the existing frequency-domain smoothing and
  /// interpolation (producing \c freq_response and \c filtered_pilots_lse_view); the time-domain
  /// strategy is then applied on demand by \c get_symbol_ch_estimate. Derived estimators
  /// (e.g., the Metal 2D MMSE estimator) may override this stage to produce the complete
  /// time-frequency grid in one shot and override \c get_symbol_ch_estimate accordingly.
  virtual void apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args)
  {
    apply_fd_td_estimation_stage_classical(args);
  }

  /// \brief The hop's device-side rsrp sum, when a device backend produced one for this hop.
  ///
  /// The hop's rsrp is the mean power of the estimated grid over the pilot resource elements. A
  /// backend that already reduces that sum where the estimates live (the Metal MMSE estimator does,
  /// see ocudu_mmse_rsrp.metal) hands it over here so that \c compute_hop_finish() does not have to
  /// read the whole estimated grid back from the device for a REPORTING value. That read-back is the
  /// last device -> host crossing a fused lane makes, and the sum it produces is the same quantity
  /// the host accumulates below - over the same REs, up to floating-point summation order.
  ///
  /// \param[in] i_layer Layer index.
  /// \return SUM of |h|^2 over the hop's DM-RS resource elements of that layer, or nullopt when the
  ///         host must accumulate the value itself (no device reduction for this hop, or the device
  ///         statistics are switched off).
  virtual std::optional<float> get_device_rsrp_sum(unsigned i_layer) const
  {
    (void) i_layer;
    return std::nullopt;
  }

  /// \brief The hop's device-side time alignment, in SECONDS, when a device backend produced one.
  ///
  /// The hop's timing advance is a REPORTING value, like the rsrp above and for the same reason: it
  /// reaches the timing-advance report and the debug dump, never the LLR path. A backend that
  /// transforms the pilot estimates where they already live and reduces the power delay profile there
  /// (the Metal MMSE estimator does, see ocudu_mmse_ta.metal) hands the answer over here, so that
  /// compute_hop_finish() does not have to read the estimated grid back from the device to derive it
  /// with estimate_time_alignment() - the LAST device -> host crossing a fused lane makes for a
  /// reporting value.
  ///
  /// The device computes the same quantity the host does (an inverse transform per DM-RS symbol and
  /// layer, the sum of their |.|^2, the half-cyclic-prefix peak search and the parabolic refinement),
  /// but the two do NOT agree bit for bit: the transform is a different implementation and the
  /// refinement is applied to it rather than to the host's. They agree to the resolution the sampling
  /// rate sets, which is what the offline A/B compares them at.
  ///
  /// \return The hop's time alignment in seconds, or nullopt when the host must estimate it itself
  ///         (no device stage for this hop, or the device statistics are switched off).
  virtual std::optional<float> get_device_ta_seconds() const { return std::nullopt; }

  /// \brief The time alignment estimator this instance resolves hops with.
  ///
  /// Exposed to backends because the estimator's own parameters are part of the answer: a device that
  /// reproduces the hop's alignment elsewhere must transform the SAME number of points, which is the
  /// size the estimator would have picked (see time_alignment_estimator::get_idft_size()).
  const time_alignment_estimator& get_ta_estimator() const
  {
    ocudu_assert(ta_estimator, "Invalid TA estimator.");
    return *ta_estimator;
  }

  /// The same estimator, mutable: an A/B probe runs the HOST's own estimate on the hop a device
  /// backend just produced one for, and estimate() is a mutating call (it drives its own transforms).
  time_alignment_estimator& get_ta_estimator()
  {
    ocudu_assert(ta_estimator, "Invalid TA estimator.");
    return *ta_estimator;
  }

  /// \brief Whether the estimation stage produces the hop's least-squares pilots itself.
  ///
  /// A device backend recomputes them from the resource grid inside the stage and the result is what
  /// the rest of the estimator consumes (see port_channel_estimator_metal_mmse_impl, where K0-a
  /// overwrites \c pilots_lse_view): the host pre-stage would then fill a buffer that is replaced
  /// before any reader, i.e. a CPU step in the middle of the chain with no consumer. Such a backend
  /// answers true here and the pre-stage is skipped. It answers BEFORE the stage runs, because the
  /// pre-stage is exactly what is being skipped, so the answer must not depend on the stage's own
  /// result: a backend that says true and then cannot build the pilots calls run_ls_pre_stage() from
  /// the stage (the cold path) and keeps the host result.
  ///
  /// The default is false: the classical estimator and every backend that estimates on the host keep
  /// the pre-stage, which is also the fallback the caller selects with the estimator's own knobs.
  virtual bool stage_produces_ls_pilots(const fd_td_estimation_stage_args& args) const { return false; }

  /// \brief The classical FD smoothing + TD interpolation stage (the default behavior,
  /// factored out of the virtual hook). Derived estimators that need the classical
  /// per-symbol estimates as their input (e.g. the AI channel estimator) call this
  /// explicitly and then refine the result.
  void apply_fd_td_estimation_stage_classical(fd_td_estimation_stage_args& args);

  /// \brief Runs the host pre-stage of one hop: the least-squares pilots and the CFO estimate.
  ///
  /// This is the part of compute_hop_submit() that a device backend skips when
  /// stage_produces_ls_pilots() says it will build the pilots itself - and that it calls from the
  /// stage when the build fails anyway.
  /// \param[in] args  Hop arguments; \c pilots and the pattern describe the hop.
  /// \return The hop's CFO estimate, when the pilots carry one.
  std::optional<float> run_ls_pre_stage(const fd_td_estimation_stage_args& args);

  /// \brief Records the CFO the hop's pilots were rotated with, for the statistics and the report.
  ///
  /// The host pre-stage estimates it, but a device backend that builds the pilots returns its own
  /// with them: the filtered pilots the statistics read come from that same rotation, so the CFO
  /// that reaches estimate_noise() and get_cfo_Hz() has to be the matching one.
  void account_hop_cfo(std::optional<float> cfo);

  /// \brief Completes the estimation stage of a hop that apply_fd_td_estimation_stage() started.
  ///
  /// The default implementation has nothing to do: a stage that computes inline has already filled
  /// its outputs when it returns. A backend that dispatches the stage to a device overrides it with
  /// the wait and the unpack, so that the caller can run the rest of the chain in between (see
  /// port_channel_estimator::submit()).
  /// \return False when a batch submitted by the stage failed, in which case the results of the hop
  ///         are not valid.
  virtual bool complete_fd_td_estimation_stage() { return true; }

  /// \brief Applies the time domain interpolation strategy for a given OFDM symbol within the hop transmission.
  /// (protected: derived estimators build their input grid from the classical per-symbol estimates).
  /// \param[out] estimated_rg        Estimated resource grid OFDM symbol for a single channel.
  /// \param[in]  dmrs_mask           Time-domain DM-RS mask.
  /// \param[in]  freq_response_dmrs  Frequency-domain channel estimates for the given channel for each of the OFDM
  ///                                 symbols containing DM-RS.
  /// \param[in]  hop_first_symbol    Start symbol index for the hop within the slot.
  /// \param[in]  hop_last_symbol     Last symbol index for the hop within the slot.
  /// \param[in]  i_symbol            OFDM symbol index within the slot to calculate.
  /// \param[in]  i_layer             Transmission layer.
  void apply_td_domain_strategy(span<cbf16_t>                     estimated_rg,
                                const symbol_slot_mask&           dmrs_mask,
                                const re_measurement<const cf_t>& freq_response_dmrs,
                                unsigned                          hop_first_symbol,
                                unsigned                          hop_last_symbol,
                                unsigned                          i_symbol,
                                unsigned                          i_layer) const;
private:

  /// Specializes \ref compute for one hop.
  void compute_hop(const resource_grid_reader& grid, unsigned port, const dmrs_symbol_list& pilots, unsigned hop);

  /// \brief First phase of compute_hop(): everything up to the estimation stage of the hop.
  ///
  /// The hop is left pending (see \c pending_hop): its estimation stage may not have produced its
  /// outputs yet, and compute_hop_finish() completes it.
  /// \param[in] deferred Whether this hop is the one the caller completes later (see do_submit()); it
  ///             reaches the stage as fd_td_estimation_stage_args::deferred.
  void compute_hop_submit(const resource_grid_reader& grid,
                          unsigned                    port,
                          const dmrs_symbol_list&     pilots,
                          unsigned                    hop,
                          bool                        deferred);

  /// \brief Second phase of compute_hop(): the hop statistics derived from the filtered pilots.
  ///
  /// Completes the estimation stage first (a no-op for a stage that computed inline) and then
  /// accumulates the RSRP, the noise variance and the time alignment of the hop.
  /// \return False when the completion of the stage failed.
  bool compute_hop_finish(const dmrs_symbol_list& pilots);

  /// \brief State of the hop between compute_hop_submit() and compute_hop_finish().
  ///
  /// The statistics of a hop are derived from the filtered pilot estimates the estimation stage
  /// writes, so - when the stage completes the hop later than it was submitted - the buffer holding
  /// them and the values the stage was called with have to outlive the call. Everything else the
  /// statistics need is derived again from \c cfg_local and the pilots.
  struct pending_hop_state {
    /// True while a hop is waiting for compute_hop_finish().
    bool                 valid = false;
    /// Index of the pending hop (0 or 1).
    unsigned             hop = 0;
    /// Number of LSE symbols of the pending hop.
    unsigned             nof_lse_symbols = 0;
    /// Offset of the hop within the DM-RS symbols of the slot (see fd_td_estimation_stage_args).
    unsigned             stage_hop_offset = 0;
    /// DM-RS to data amplitude scaling of the pending hop.
    float                beta_scaling = 0.0F;
    /// Estimated CFO of the pending hop (empty when unavailable).
    std::optional<float> cfo_hop;
    /// Filtered pilot estimates of the pending hop: written by the estimation stage, read by the
    /// statistics. \c filtered_pilots_lse is a view over it and is rebuilt in compute_hop_finish().
    static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> enlarged_filtered_pilots_lse;
  };
  pending_hop_state pending_hop;

  /// \brief Preprocesses the pilots and computes the CFO.
  ///
  /// For the current hop, the function does the following:
  /// - matches the received pilots with the expected ones (element-wise multiplication with complex conjugate);
  /// - estimates the CFO (if the number of OFDM symbols with pilots is at least 2).
  /// \param[in]  pilots            Transmitted pilots.
  /// \param[in]  dmrs_mask         Boolean mask identifying the OFDM symbols carrying DM-RS within the slot.
  /// \param[in]  first_hop_symbol  Index of the first OFDM symbol of the current hop, within the slot.
  /// \param[in]  last_hop_symbol   Index of the last OFDM symbol of the current hop (not included), within the slot.
  /// \param[in]  hop_offset        Number of OFDM symbols carrying DM-RS in the previous hop.
  /// \param[in]  start_layer       Index of the first transmission layer to be preprocessed.
  /// \param[in]  stop_layer        Index of the last transmission layer (not included) to be preprocessed.
  /// \return A contribution to the CFO estimate. CFO is empty if the hop has only one OFDM symbol carrying DM-RS.
  ///
  /// \warning This method updates the content of the buffers \c pilots_lse and \c pilot_products.
  std::optional<float> preprocess_pilots_and_estimate_cfo(const dmrs_symbol_list&                   pilots,
                                                          const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_mask,
                                                          unsigned                                  first_hop_symbol,
                                                          unsigned                                  last_hop_symbol,
                                                          unsigned                                  hop_offset,
                                                          unsigned                                  start_layer,
                                                          unsigned                                  stop_layer);

  /// \brief Compensates the CFO.
  ///
  /// For the current hop:
  /// - compensates the CFO for all pilots;
  /// - accumulates all the matched, CFO-compensated received pilots from all OFDM symbols carrying DM-RS.
  /// \param[in]  pilots            Transmitted pilots.
  /// \param[in]  dmrs_mask         Boolean mask identifying the OFDM symbols carrying DM-RS within the slot.
  /// \param[in]  first_hop_symbol  Index of the first OFDM symbol of the current hop, within the slot.
  /// \param[in]  last_hop_symbol   Index of the last OFDM symbol of the current hop (not included), within the slot.
  /// \param[in]  cfo               Estimated CFO.
  ///
  /// \warning This method updates the content of the buffers \c pilots_lse and \c pilot_products.
  void compensate_cfo_and_accumulate(const dmrs_symbol_list&                   pilots,
                                     const bounded_bitset<MAX_NSYMB_PER_SLOT>& dmrs_mask,
                                     unsigned                                  first_hop_symbol,
                                     unsigned                                  last_hop_symbol,
                                     std::optional<float>                      cfo);

  /// \brief Computes the starting time of the symbols inside a slot for the given subcarrier spacing.
  ///
  /// The symbol starting time is computed from the start of the slot and is expressed in units of OFDM symbol duration.
  void initialize_symbol_start_epochs(cyclic_prefix cp, subcarrier_spacing scs);



private:

  /// Frequency domain smoothing strategy.
  port_channel_estimator_fd_smoothing_strategy fd_smoothing_strategy;

  /// Time domain smoothing strategy.
  port_channel_estimator_td_interpolation_strategy td_interpolation_strategy;

  /// Boolean flag for activating CFO compensation (active when true).
  bool compensate_cfo;

  /// \brief Interpolator.
  ///
  /// When DM-RS pilots do not occupy all REs in an OFDM symbol, the interpolator is used to estimate the channel of the
  /// REs without pilots.
  std::unique_ptr<interpolator> freq_interpolator;

  /// Time alignment estimator.
  std::unique_ptr<time_alignment_estimator> ta_estimator;

  /// Buffer of received signal samples corresponding to pilots.
  dmrs_symbol_list rx_pilots;

  /// Auxiliary buffer for processing the pilots.
  static_re_buffer<MAX_LAYERS, MAX_NOF_SUBCARRIERS> pilot_products;

  /// \brief View of the filtered pilot estimates the hop statistics are computed from.
  ///
  /// It has to be the same object in both phases of a hop: setup_auxiliary_buffers() assigns it as a
  /// window of the enlarged buffer (offset MAX_V_PILOTS with the filter smoothing strategy), so
  /// rebuilding it from that buffer instead would silently drop the offset and shift every derived
  /// measurement - RSRP, noise variance and time alignment - by that many subcarriers.
  modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> filtered_pilots_lse;

  /// Second auxiliary buffer for processing the pilots.
  static_re_measurement<cf_t, MAX_NOF_PILOTS_SYMBOL, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> enlarged_pilots_lse;
  modular_re_measurement<cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS>                       pilots_lse;

  /// \brief Buffer of frequency response coefficients for each hop.
  ///
  /// The storage for the first hop needs to be bigger since it covers the cases with no frequency hopping.
  /// @{
  static_re_measurement<cf_t, MAX_NOF_SUBCARRIERS, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS>     freq_response_hop0;
  static_re_measurement<cf_t, MAX_NOF_SUBCARRIERS, MAX_NOF_DMRS_SYMBOLS / 2, MAX_LAYERS> freq_response_hop1;
  /// @}

  /// Buffer for the OFDM symbols starting epochs within the current slot.
  std::array<float, MAX_NSYMB_PER_SLOT> aux_symbol_start_epochs;
  /// View on the used part of the symbols starting epochs buffer \c aux_symbol_start_epochs.
  span<float> symbol_start_epochs;

  /// Channel estimator configuration.
  configuration cfg_local = {};

  /// Estimated RSRP value per layer.
  std::array<float, MAX_LAYERS> rsrp;

  /// \brief Observed average DM-RS EPRE.
  ///
  /// \remark The EPRE is defined as the average received power (including noise) across all REs carrying DM-RS.
  float epre = 0;

  /// Estimated noise variance (single layer).
  float noise_var = 0;

  /// Estimated SNR (linear scale).
  float snr_linear = 0;

  /// Estimated time alignment in seconds.
  float time_alignment_s = 0;

  /// Estimated CFO, normalized with respect to the subcarrier spacing.
  std::optional<float> cfo_normalized = std::nullopt;

  /// Estimated CFO in hertz.
  std::optional<float> cfo_Hz = std::nullopt;
};

} // namespace ocudu
