// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "port_channel_estimator_helena_impl.h"
#include "../port_channel_estimator_helpers.h"
#include "ocudu/ocudulog/ocudulog.h"

#include <chrono>
#include <cmath>
#include <cstring>

using namespace ocudu;

port_channel_estimator_helena_impl::port_channel_estimator_helena_impl(
    std::unique_ptr<interpolator>                        interp,
    std::unique_ptr<time_alignment_estimator>            ta_estimator,
    std::string                                         modelc_path_,
    std::string                                         modelc_path_52_,
    std::string                                         modelc_path_106_,
    bool                                                compensate_cfo_) :
  port_channel_estimator_average_impl(std::move(interp),
                                      std::move(ta_estimator),
                                      port_channel_estimator_fd_smoothing_strategy::none,
                                      // The classical pre-stage keeps the per-DMRS-symbol frequency
                                      // responses; the TD interpolation fills the NN input grid.
                                      port_channel_estimator_td_interpolation_strategy::interpolate,
                                      compensate_cfo_),
  modelc_path(std::move(modelc_path_)),
  modelc_path_52(std::move(modelc_path_52_)),
  modelc_path_106(std::move(modelc_path_106_))
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
  if (!modelc_path_106.empty()) {
    engine_106 = std::make_unique<metal::coreml_nn_engine>();
    if (!engine_106->init(modelc_path_106.c_str())) {
      ocudulog::fetch_basic_logger("PHY").warning("AI-CE: 106-PRB engine init failed - classical fallback for 106 PRB");
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
  if (engine_106 != nullptr) {
    (void)engine_106->predict(nn_in.data(), nn_out.data(), 1272);
  }
}

port_channel_estimator_helena_impl::~port_channel_estimator_helena_impl() = default;

bool port_channel_estimator_helena_impl::reload(const std::string& modelc_path_, unsigned nof_subc)
{
  auto next = std::make_unique<metal::coreml_nn_engine>();
  if (!next->init(modelc_path_.c_str())) {
    ocudulog::fetch_basic_logger("PHY").warning("AI-CE: reload failed - incumbent model kept ({})",
                                                modelc_path_);
    return false;
  }
  // Atomic swap into the width-matching bucket; the new engine serves from the next slot.
  unsigned width = 612;
  if (nof_subc <= 612) {
    engine      = std::move(next);
    modelc_path = modelc_path_;
  } else if (nof_subc <= 624) {
    engine_52      = std::move(next);
    modelc_path_52 = modelc_path_;
    width          = 624;
  } else {
    engine_106      = std::move(next);
    modelc_path_106 = modelc_path_;
    width           = 1272;
  }
  // Warm up the new engine too (the first prediction compiles the ANE program;
  // the worker-thread keep-alive covers the idle-eviction tax afterwards).
  if (width == 612) {
    (void)engine->predict(nn_in.data(), nn_out.data(), 612);
  } else if (width == 624) {
    (void)engine_52->predict(nn_in.data(), nn_out.data(), 624);
  } else {
    (void)engine_106->predict(nn_in.data(), nn_out.data(), 1272);
  }
  ocudulog::fetch_basic_logger("PHY").info("AI-CE: model reloaded from {} (width {})", modelc_path_, width);
  return true;
}

void port_channel_estimator_helena_impl::apply_fd_td_estimation_stage(fd_td_estimation_stage_args& args)
{
  const bool time_en = std::getenv("OCUDU_MMSE_TIME") != nullptr;
  const auto t_begin = std::chrono::steady_clock::now();
  // Classical pre-stage: fills freq_response and the filtered pilots (RSrp / noise / TA).
  apply_fd_td_estimation_stage_classical(args);
  const auto t_classical = std::chrono::steady_clock::now();

  const unsigned nof_layers = args.dmrs_patterns.size();
  const unsigned nof_prb    = args.dmrs_patterns.front().rb_mask.count();
  const unsigned nof_subc   = nof_prb * NOF_SUBCARRIERS_PER_RB;
  nn_grid_valid             = false;
  last_predict_us_          = 0.0;

  // High-SNR bypass (2026-08-30 E2E root cause): the network was trained at
  // SNR -5..25 dB. On much cleaner channels (the ZMQ E2E runs at ~45 dB) the
  // interpolated-LS input is already near-perfect (measured -45 dB NMSE) and the
  // NN's denoising bias actively CORRUPTS it (-22 dB at 6 PRB, -33 dB at 52 PRB),
  // breaking 64QAM/256QAM demod and the attach (SRB1 PUSCH/PUCCH CRC KO at
  // 40+ dB SINR). Beyond the training envelope the classical grid is near-optimal,
  // so serve it unchanged (the NN's value zone is low/mid SNR).
  constexpr float kHelenaMaxSnrDb = 25.0F;
  // OCUDU_HELENA_FORCE_NN disables the gate (the head-to-head harness needs it:
  // its synthetic noise estimation saturates at the 100 dB floor, which would
  // bypass the NN for the whole test set).
  const bool force_nn = std::getenv("OCUDU_HELENA_FORCE_NN") != nullptr;
  if (!force_nn && 10.0F * std::log10(get_snr()) > kHelenaMaxSnrDb) {
    if (time_en) {
      ocudulog::fetch_basic_logger("PHY").debug(
          "[helena_gate] prb={} snr={:.1f}dB > {:.0f}dB -> classical bypass",
          nof_prb,
          10.0F * std::log10(get_snr()),
          kHelenaMaxSnrDb);
    }
    return;
  }

  // Bucket dispatch (v1): <=52 PRB -> 52-model, 53..106 PRB -> 106-model,
  // below kHelenaMinPrb PRB or frequency hopping -> classical. The NN grid is
  // allocation-local (zero-padded to the bucket width) so any CRB works.
  constexpr unsigned kHelenaMinPrb = 6;
  active_engine     = nullptr;
  active_engine_nsc = 0;
  if (args.hop != 0 || nof_prb < kHelenaMinPrb) {
    return;
  }
  if (nof_subc <= 624 && engine_52 != nullptr) {
    active_engine     = engine_52.get();
    active_engine_nsc = 624;
  } else if (nof_subc <= 1272 && engine_106 != nullptr) {
    active_engine     = engine_106.get();
    active_engine_nsc = 1272;
  } else if (nof_subc == 612 && engine != nullptr) {
    active_engine     = engine.get();
    active_engine_nsc = 612;
  }
  if (active_engine == nullptr) {
    return;
  }
  const unsigned engine_nsc = active_engine_nsc;

  grid_est.resize(nof_layers * MAX_NSYMB_PER_SLOT, nof_subc);
  const auto& pattern = args.dmrs_patterns.front();
  modular_re_measurement<const cf_t, MAX_NOF_DMRS_SYMBOLS, MAX_LAYERS> freq_view =
      static_cast<const re_measurement<cf_t>&>(args.freq_response);

  for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
    // Build the NN input grid: the classical TD-interpolated LS estimates, laid out
    // subcarrier-major [nsc, 14, 2] exactly like the training set (no extra scaling),
    // zero-padded to the bucket width beyond the allocation.
    std::fill(nn_in.begin() + static_cast<size_t>(nof_subc) * MAX_NSYMB_PER_SLOT * 2, nn_in.end(), 0.0F);
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

    const auto t_nn_begin = std::chrono::steady_clock::now();
    if (!active_engine->predict(nn_in.data(), nn_out.data(), engine_nsc)) {
      return; // classical fallback for this slot
    }
    last_predict_us_ = active_engine->last_predict_us();
    if (time_en) {
      const auto us = [](auto d) { return std::chrono::duration<double, std::micro>(d).count(); };
      ocudulog::fetch_basic_logger("PHY").debug(
          "[helena_time] prb={} subc={} engine={} layer={} | classical={:.1f}us predict={:.1f}us (worker engine last={:.1f}us)",
          nof_prb,
          nof_subc,
          engine_nsc,
          i_layer,
          us(t_classical - t_begin),
          us(std::chrono::steady_clock::now() - t_nn_begin),
          last_predict_us_);
    }

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
