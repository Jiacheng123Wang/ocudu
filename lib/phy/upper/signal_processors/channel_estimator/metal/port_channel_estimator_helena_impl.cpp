// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "port_channel_estimator_helena_impl.h"
#include "../port_channel_estimator_helpers.h"
#include "ocudu/ocudulog/ocudulog.h"

#include <cstring>

using namespace ocudu;

port_channel_estimator_helena_impl::port_channel_estimator_helena_impl(
    std::unique_ptr<interpolator>                        interp,
    std::unique_ptr<time_alignment_estimator>            ta_estimator,
    std::string                                         modelc_path_,
    std::string                                         modelc_path_52_,
    bool                                                compensate_cfo_) :
  port_channel_estimator_average_impl(std::move(interp),
                                      std::move(ta_estimator),
                                      port_channel_estimator_fd_smoothing_strategy::none,
                                      // The classical pre-stage keeps the per-DMRS-symbol frequency
                                      // responses; the TD interpolation fills the NN input grid.
                                      port_channel_estimator_td_interpolation_strategy::interpolate,
                                      compensate_cfo_),
  modelc_path(std::move(modelc_path_)),
  modelc_path_52(std::move(modelc_path_52_))
{
  engine = std::make_unique<metal::coreml_nn_engine>();
  if (!engine->init(modelc_path.c_str())) {
    ocudulog::fetch_basic_logger("PHY").warning("AI-CE: 51-PRB engine init failed - classical fallback for 51 PRB");
  }
  if (!modelc_path_52.empty()) {
    engine_52 = std::make_unique<metal::coreml_nn_engine>();
    if (!engine_52->init(modelc_path_52.c_str())) {
      ocudulog::fetch_basic_logger("PHY").warning("AI-CE: 52-PRB engine init failed - classical fallback for 52 PRB");
    }
  }

  // Warm-up predictions: the FIRST Core ML prediction compiles the ANE program
  // (~15 ms, observed in the E2E first full-bandwidth slot - the attach-phase
  // dec_t spike). Running it here at construction moves the cost off the slot
  // critical path (the '粮草先行' principle, Core ML edition).
  std::fill(nn_in.begin(), nn_in.end(), 0.0F);
  if (engine != nullptr) {
    (void)engine->predict(nn_in.data(), nn_out.data(), 612);
  }
  if (engine_52 != nullptr) {
    (void)engine_52->predict(nn_in.data(), nn_out.data(), 624);
  }
}

port_channel_estimator_helena_impl::~port_channel_estimator_helena_impl() = default;

bool port_channel_estimator_helena_impl::reload(const std::string& modelc_path_)
{
  auto next = std::make_unique<metal::coreml_nn_engine>();
  if (!next->init(modelc_path_.c_str())) {
    ocudulog::fetch_basic_logger("PHY").warning("AI-CE: reload failed - incumbent model kept ({})",
                                                modelc_path_);
    return false;
  }
  // Atomic swap: the new engine serves from the next slot.
  engine      = std::move(next);
  modelc_path = modelc_path_;
  // Warm up the new engine too (the first prediction compiles the ANE program).
  (void)engine->predict(nn_in.data(), nn_out.data(), 612);
  ocudulog::fetch_basic_logger("PHY").info("AI-CE: model reloaded from {}", modelc_path);
  return true;
}

void port_channel_estimator_helena_impl::apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args)
{
  // Classical pre-stage: fills freq_response and the filtered pilots (RSrp / noise / TA).
  apply_fd_td_estimation_stage_classical(args);

  const unsigned nof_layers = args.dmrs_patterns.size();
  const unsigned nof_prb    = args.dmrs_patterns.front().rb_mask.count();
  const unsigned nof_subc   = nof_prb * NOF_SUBCARRIERS_PER_RB;
  nn_grid_valid             = false;
  last_predict_us_          = 0.0;

  // v1 envelope: single hop, 51/52 PRB starting at CRB 0 (the trained grid shapes);
  // everything else is served by the classical path unchanged.
  active_engine = nullptr;
  if (args.hop != 0 || args.dmrs_patterns.front().rb_mask.find_lowest() != 0) {
    return;
  }
  if (nof_subc == 612 && engine != nullptr) {
    active_engine = engine.get();
  } else if (nof_subc == 624 && engine_52 != nullptr) {
    active_engine = engine_52.get();
  }
  if (active_engine == nullptr) {
    return;
  }

  grid_est.resize(nof_layers * MAX_NSYMB_PER_SLOT, nof_subc);
  const auto& pattern = args.dmrs_patterns.front();
  modular_re_measurement<const cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> freq_view =
      static_cast<const re_measurement<cf_t>&>(args.freq_response);

  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    // Build the NN input grid: the classical TD-interpolated LS estimates, laid out
    // subcarrier-major [612, 14, 2] exactly like the training set (no extra scaling).
    for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
      span<cbf16_t> scratch(td_scratch.data(), nof_subc);
      apply_td_domain_strategy(scratch,
                               pattern.symbols,
                               freq_view,
                               args.first_symbol,
                               args.last_symbol,
                               sym,
                               i_layer);
      for (unsigned k = 0; k != nof_subc; ++k) {
        const cf_t v                          = to_cf(scratch[k]);
        nn_in[2 * (k * MAX_NSYMB_PER_SLOT + sym)]     = v.real();
        nn_in[2 * (k * MAX_NSYMB_PER_SLOT + sym) + 1] = v.imag();
      }
    }

    if (!active_engine->predict(nn_in.data(), nn_out.data(), nof_subc)) {
      return; // classical fallback for this slot
    }
    last_predict_us_ = active_engine->last_predict_us();

    for (unsigned sym = 0; sym != MAX_NSYMB_PER_SLOT; ++sym) {
      span<cf_t> dst = grid_est.get_slice(i_layer * MAX_NSYMB_PER_SLOT + sym);
      for (unsigned k = 0; k != nof_subc; ++k) {
        dst[k] = {nn_out[2 * (k * MAX_NSYMB_PER_SLOT + sym)],
                  nn_out[2 * (k * MAX_NSYMB_PER_SLOT + sym) + 1]};
      }
    }
  }
  nn_grid_valid = true;
}

void port_channel_estimator_helena_impl::get_symbol_ch_estimate(span<cbf16_t> symbol,
                                                                unsigned      i_symbol,
                                                                unsigned      tx_layer) const
{
  if (!nn_grid_valid) {
    port_channel_estimator_average_impl::get_symbol_ch_estimate(symbol, i_symbol, tx_layer);
    return;
  }
  span<const cf_t> src = grid_est.get_slice(tx_layer * MAX_NSYMB_PER_SLOT + i_symbol);
  ocudu_assert(symbol.size() == src.size(), "Invalid symbol buffer size.");
  for (unsigned i = 0; i != src.size(); ++i) {
    symbol[i] = to_cbf16(src[i]);
  }
}

void port_channel_estimator_helena_impl::get_symbol_ch_estimate(span<cbf16_t>                              symbol,
                                                                unsigned                                   i_symbol,
                                                                unsigned                                   tx_layer,
                                                                const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const
{
  if (!nn_grid_valid) {
    port_channel_estimator_average_impl::get_symbol_ch_estimate(symbol, i_symbol, tx_layer, re_mask);
    return;
  }
  span<const cf_t> src = grid_est.get_slice(tx_layer * MAX_NSYMB_PER_SLOT + i_symbol);
  unsigned         j   = 0;
  re_mask.for_each(0, re_mask.size(), [&](unsigned i_re) { symbol[j++] = to_cbf16(src[i_re]); });
}
