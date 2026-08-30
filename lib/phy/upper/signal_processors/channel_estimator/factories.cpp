// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "ocudu/phy/upper/signal_processors/channel_estimator/factories.h"
#include "port_channel_estimator_average_impl.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/support/support_factories.h"

#if defined(OCUDU_METAL_CHEST)
#include "metal/channel_statistics_estimator.h"
#include "metal/port_channel_estimator_metal_mmse_impl.h"
#include "metal/port_channel_estimator_helena_impl.h"
#endif // OCUDU_METAL_CHEST

using namespace ocudu;

namespace {

class port_channel_estimator_factory_sw : public port_channel_estimator_factory
{
public:
  explicit port_channel_estimator_factory_sw(std::shared_ptr<time_alignment_estimator_factory> ta_estimator_factory_,
                                             port_channel_estimator_algorithm algo_,
                                             float                            mmse_tau_rms_s_,
                                             float                            mmse_fd_hz_,
                                             unsigned                         mmse_block_prb_,
                                             std::string                      helena_model_path_,
                                             std::string                      helena_model_path_52_,
                                             std::string                      helena_model_path_106_) :
    ta_estimator_factory(std::move(ta_estimator_factory_)),
    algo(algo_),
    mmse_tau_rms_s(mmse_tau_rms_s_),
    mmse_fd_hz(mmse_fd_hz_),
    mmse_block_prb(mmse_block_prb_),
    helena_model_path(std::move(helena_model_path_)),
    helena_model_path_52(std::move(helena_model_path_52_)),
    helena_model_path_106(std::move(helena_model_path_106_))
  {
    ocudu_assert(ta_estimator_factory, "Invalid TA estimator factory.");
#if !defined(OCUDU_METAL_CHEST)
    if (algo == port_channel_estimator_algorithm::metal_mmse || algo == port_channel_estimator_algorithm::helena) {
      report_error("The 'metal_mmse'/'helena' channel estimators are only available on Apple Silicon macOS builds.");
    }
#endif
  }

  std::unique_ptr<port_channel_estimator>
  create(port_channel_estimator_fd_smoothing_strategy     fd_smoothing_strategy,
         port_channel_estimator_td_interpolation_strategy td_interpolation_strategy,
         bool                                             compensate_cfo) override
  {
    std::unique_ptr<interpolator> interp = create_interpolator();

    if (algo == port_channel_estimator_algorithm::metal_mmse) {
#if defined(OCUDU_METAL_CHEST)
      return std::make_unique<port_channel_estimator_metal_mmse_impl>(
          std::move(interp),
          ta_estimator_factory->create(),
          std::make_shared<channel_statistics_estimator_fixed>(mmse_tau_rms_s, mmse_fd_hz),
          mmse_block_prb,
          compensate_cfo);
#else
      return nullptr;
#endif
    }

    if (algo == port_channel_estimator_algorithm::helena) {
#if defined(OCUDU_METAL_CHEST)
      const std::string& path    = helena_model_path.empty() ? OCUDU_HELENA_MODEL_PATH : helena_model_path;
      const std::string& path52  = helena_model_path_52.empty() ? OCUDU_HELENA_MODEL_PATH_52 : helena_model_path_52;
      const std::string& path106 = helena_model_path_106.empty() ? OCUDU_HELENA_MODEL_PATH_106 : helena_model_path_106;
      return std::make_unique<port_channel_estimator_helena_impl>(
          std::move(interp), ta_estimator_factory->create(), path, path52, path106, compensate_cfo);
#else
      return nullptr;
#endif
    }

    return std::make_unique<port_channel_estimator_average_impl>(std::move(interp),
                                                                 ta_estimator_factory->create(),
                                                                 fd_smoothing_strategy,
                                                                 td_interpolation_strategy,
                                                                 compensate_cfo);
  }

private:
  std::shared_ptr<time_alignment_estimator_factory> ta_estimator_factory;
  port_channel_estimator_algorithm                  algo;
  float                                             mmse_tau_rms_s;
  float                                             mmse_fd_hz;
  unsigned                                          mmse_block_prb;
  std::string                                       helena_model_path;
  std::string                                       helena_model_path_52;
  std::string                                       helena_model_path_106;
};

} // namespace

std::shared_ptr<port_channel_estimator_factory>
ocudu::create_port_channel_estimator_factory_sw(std::shared_ptr<time_alignment_estimator_factory> ta_estimator_factory,
                                                port_channel_estimator_algorithm algo,
                                                float                            mmse_tau_rms_s,
                                                float                            mmse_fd_hz,
                                                unsigned                         mmse_block_prb,
                                                const std::string&               helena_model_path,
                                                const std::string&               helena_model_path_52,
                                                const std::string&               helena_model_path_106)
{
  return std::make_shared<port_channel_estimator_factory_sw>(std::move(ta_estimator_factory),
                                                             algo,
                                                             mmse_tau_rms_s,
                                                             mmse_fd_hz,
                                                             mmse_block_prb,
                                                             helena_model_path,
                                                             helena_model_path_52,
                                                             helena_model_path_106);
}
