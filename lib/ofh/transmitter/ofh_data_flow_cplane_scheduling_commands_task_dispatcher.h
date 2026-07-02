// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ofh_data_flow_cplane_scheduling_commands.h"
#include "ocudu/support/executors/strand_executor.h"
#include "ocudu/support/executors/task_executor.h"
#include "ocudu/support/ocudu_assert.h"
#include "ocudu/support/rtsan.h"
#include "ocudu/support/synchronization/stop_event.h"
#include <memory>
#include <vector>

namespace ocudu {
namespace ofh {

/// Open Fronthaul Control-Plane scheduling and beamforming commands data flow task dispatcher implementation.
///
/// The dispatcher owns one serialization strand per eAxC. All messages of a given eAxC are processed in enqueue order,
/// so that the eCPRI sequence identifiers are generated in transmission order, while different eAxCs are processed
/// concurrently in the underlying pool executor.
class data_flow_cplane_downlink_task_dispatcher : public data_flow_cplane_scheduling_commands,
                                                  public operation_controller
{
  /// Size of each per-eAxC strand task queue. Limit it to 8 slots.
  static constexpr unsigned strand_queue_size = 8u;

public:
  data_flow_cplane_downlink_task_dispatcher(ocudulog::basic_logger&                               logger_,
                                            std::unique_ptr<data_flow_cplane_scheduling_commands> data_flow_cplane_,
                                            task_executor&                                        executor,
                                            unsigned                                              nof_eaxc,
                                            unsigned                                              sector_id_) :
    logger(logger_), data_flow_cplane(std::move(data_flow_cplane_)), sector_id(sector_id_)
  {
    ocudu_assert(data_flow_cplane, "Invalid data flow");
    ocudu_assert(nof_eaxc != 0, "At least one eAxC must be configured");

    strands.reserve(nof_eaxc);
    for (unsigned i = 0; i != nof_eaxc; ++i) {
      strands.push_back(make_task_strand_ptr<concurrent_queue_policy::lockfree_mpmc>(executor, strand_queue_size));
    }
  }

  // See interface for documentation.
  operation_controller& get_operation_controller() override { return *this; }

  // See interface for documentation.
  void start() override { stop_manager.reset(); }

  // See interface for documentation.
  void stop() override { stop_manager.stop(); }

  // See interface for documentation.
  void enqueue_section_type_1_message(const data_flow_cplane_type_1_context& context) override
  {
    // Do not process Control Plane if the stop was requested.
    auto token = stop_manager.get_token();
    if (OCUDU_UNLIKELY(token.is_stop_requested())) {
      return;
    }

    ocudu_assert(context.port < strands.size(),
                 "Invalid port index '{}'. Number of configured eAxCs is '{}'",
                 context.port,
                 strands.size());

    if (!strands[context.port]->defer([this, context, tk = std::move(token)]() noexcept OCUDU_RTSAN_NONBLOCKING {
          data_flow_cplane->enqueue_section_type_1_message(context);
        })) {
      logger.warning(
          "Sector#{}: failed to dispatch Control-Plane type 1 message for slot '{}'", sector_id, context.slot);
    }
  }

  // See interface for documentation.
  void enqueue_section_type_3_prach_message(const data_flow_cplane_scheduling_prach_context& context) override
  {
    // Do not process Control Plane if the stop was requested.
    auto token = stop_manager.get_token();
    if (OCUDU_UNLIKELY(token.is_stop_requested())) {
      return;
    }

    // PRACH Control-Plane is not part of the downlink flow; it is handled on the uplink request path. Serialize any
    // stray request through the first strand to keep the sequence identifier generation ordered.
    if (!strands.front()->defer([this, context, tk = std::move(token)]() noexcept OCUDU_RTSAN_NONBLOCKING {
          data_flow_cplane->enqueue_section_type_3_prach_message(context);
        })) {
      logger.warning(
          "Sector#{}: failed to dispatch Control-Plane type 3 message for slot '{}'", sector_id, context.slot);
    }
  }

  // See interface for documentation.
  data_flow_message_encoding_metrics_collector* get_metrics_collector() override
  {
    return data_flow_cplane->get_metrics_collector();
  }

private:
  ocudulog::basic_logger&                               logger;
  std::unique_ptr<data_flow_cplane_scheduling_commands> data_flow_cplane;
  const unsigned                                        sector_id;
  std::vector<std::unique_ptr<task_executor>>           strands;
  rt_stop_event_source                                  stop_manager;
};

} // namespace ofh
} // namespace ocudu
