// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "o_du_impl.h"
#include "ocudu/du/du_high/o_du_high.h"
#include "ocudu/du/du_low/o_du_low_metrics_collector.h"
#include "ocudu/du/o_du_metrics.h"
#include "ocudu/du/o_du_metrics_notifier.h"
#include "ocudu/fapi_adaptor/mac/mac_fapi_fastpath_adaptor.h"
#include "ocudu/ocudulog/ocudulog.h"
#include <chrono>
#include <functional>

using namespace ocudu;
using namespace odu;

namespace {

/// O-DU metrics notifier dummy implementation.
class o_du_metrics_notifier_dummy : public o_du_metrics_notifier
{
public:
  // See interface for documentation.
  void on_new_metrics(const o_du_metrics& metrics) override {}
};

} // namespace

/// Dummy O-DU metrics notifier.
static o_du_metrics_notifier_dummy dummy_notifier;

namespace {

/// \brief Times one step of the DU shutdown and names it.
///
/// WHY IT EXISTS. The gNB's own shutdown is timed step by step (gnb.cpp), which is how "DU stop" was found to
/// be the step that overruns the 5-second termination alarm and gets the process SIGKILLed. But "DU stop" is
/// itself three waits in a row - the MAC-FAPI adaptor, the lower PHY and the DU high - and the log could not
/// say which of them spends the budget. A run that is killed names nothing, which is the failure this whole
/// instrumentation line exists to remove.
///
/// Logged BEFORE the step as well as after: a step that never returns is the answer, and without the opening
/// line the log ends on the previous step's timing and points at the wrong one.
void shutdown_step(const char* what, const std::function<void()>& step)
{
  ocudulog::basic_logger& logger = ocudulog::fetch_basic_logger("DU");
  logger.info("o_du stop: {} ...", what);
  const auto t0 = std::chrono::steady_clock::now();
  step();
  logger.info("o_du stop: {} took {} ms",
              what,
              std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

} // namespace

o_du_impl::o_du_impl(o_du_dependencies&& dependencies) :
  metrics_notifier(dependencies.metrics_notifier ? *dependencies.metrics_notifier : dummy_notifier),
  odu_hi(std::move(dependencies.odu_hi)),
  odu_lo(std::move(dependencies.odu_lo))
{
  ocudu_assert(odu_lo, "Invalid DU low");
  ocudu_assert(odu_hi, "Invalid DU high");

  // Register the O-DU in the O-DU high to listen to O-DU high metrics.
  odu_hi->set_o_du_high_metrics_notifier(*this);
}

void o_du_impl::on_new_metrics(const o_du_high_metrics& metrics)
{
  o_du_metrics du_metrics;

  // Get O-DU low metrics.
  if (auto* odu_low_collector = odu_lo->get_metrics_collector()) {
    auto& odu_low_metrics = du_metrics.low.emplace();
    odu_low_collector->collect_metrics(odu_low_metrics);
  }

  // Notify the metrics.
  metrics_notifier.on_new_metrics(du_metrics);
}

void o_du_impl::start()
{
  odu_hi->get_operation_controller().start();
  odu_lo->get_operation_controller().start();
}

void o_du_impl::stop()
{
  // Stop the MAC-FAPI adaptor first.
  //
  // Timed in three steps rather than one, because these three are independent waits and only one of them was
  // ever the number the operator saw (see shutdown_step above). The order is the behavioural part and is
  // unchanged: the adaptor stops feeding tasks before either side is taken down.
  shutdown_step("mac_fapi_adaptor", [&]() { odu_hi->get_mac_fapi_fastpath_adaptor().stop(); });
  shutdown_step("lower_phy", [&]() { odu_lo->get_operation_controller().stop(); });
  shutdown_step("du_high", [&]() { odu_hi->get_operation_controller().stop(); });
}

o_du_high& o_du_impl::get_o_du_high()
{
  return *odu_hi;
}

o_du_low& o_du_impl::get_o_du_low()
{
  return *odu_lo;
}
