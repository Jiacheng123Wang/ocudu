// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "lib/nrppa/nrppa_impl.h"
#include "tests/test_doubles/nrppa/nrppa_test_message_validators.h"
#include "tests/test_doubles/nrppa/nrppa_test_messages.h"
#include "ocudu/adt/format.h"
#include "ocudu/support/async/async_no_op_task.h"
#include "ocudu/support/async/async_test_utils.h"
#include "ocudu/support/async/fifo_async_task_scheduler.h"
#include "ocudu/support/executors/manual_task_worker.h"
#include <gtest/gtest.h>

using namespace ocudu;
using namespace ocucp;

/// \brief Fake CU-CP notifier whose on_new_nrppa_ue() can be told to return nullptr, simulating a CU-CP UE that no
/// longer exists when a DL NRPPA message arrives for it.
class test_nrppa_cu_cp_notifier : public nrppa_cu_cp_notifier
{
public:
  nrppa_cu_cp_ue_notifier* on_new_nrppa_ue(cu_cp_ue_index_t ue_index) override { return ue_notifier; }

  void on_ul_nrppa_pdu(const byte_buffer&                                nrppa_pdu,
                       std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t> ue_or_amf_index) override
  {
    ul_pdu_sent       = true;
    last_ul_nrppa_pdu = nrppa_pdu.copy();
  }

  async_task<trp_information_cu_cp_response_t>
  on_trp_information_request(const trp_information_request_t& request) override
  {
    return launch_no_op_task(trp_information_cu_cp_response_t{});
  }

  nrppa_cu_cp_ue_notifier* ue_notifier = nullptr;
  bool                     ul_pdu_sent = false;
  byte_buffer              last_ul_nrppa_pdu;
};

/// Fake F1AP notifier that records which positioning request reached the DU.
class test_nrppa_f1ap_notifier : public nrppa_f1ap_notifier
{
public:
  async_task<expected<positioning_information_response_t, positioning_information_failure_t>>
  on_positioning_information_request(const positioning_information_request_t& request) override
  {
    positioning_information_requested = true;
    return launch_no_op_task(expected<positioning_information_response_t, positioning_information_failure_t>{
        positioning_information_response_t{}});
  }

  async_task<expected<positioning_activation_response_t, positioning_activation_failure_t>>
  on_positioning_activation_request(const positioning_activation_request_t& request) override
  {
    positioning_activation_requested = true;
    return launch_no_op_task(expected<positioning_activation_response_t, positioning_activation_failure_t>{
        positioning_activation_response_t{}});
  }

  async_task<expected<measurement_response_t, measurement_failure_t>>
  on_measurement_information_request(const measurement_request_t& request) override
  {
    return launch_no_op_task(expected<measurement_response_t, measurement_failure_t>{measurement_response_t{}});
  }

  bool positioning_information_requested = false;
  bool positioning_activation_requested  = false;
};

/// Fake CU-CP UE notifier that runs the scheduled task in line, so that the UE-associated procedures execute within
/// the test.
class test_nrppa_cu_cp_ue_notifier : public nrppa_cu_cp_ue_notifier
{
public:
  cu_cp_ue_index_t get_ue_index() const override { return ue_index; }
  cu_cp_du_index_t get_du_index() const override { return du_index; }

  std::optional<cell_measurement_positioning_info>& on_measurement_results_required() override { return meas_results; }

  bool schedule_async_task(async_task<void> task) override
  {
    pending_task = std::move(task);
    launcher.emplace(pending_task);
    return true;
  }

  cu_cp_ue_index_t                                 ue_index = uint_to_ue_index(0);
  cu_cp_du_index_t                                 du_index = uint_to_cu_cp_du_index(0);
  std::optional<cell_measurement_positioning_info> meas_results;
  async_task<void>                                 pending_task;
  std::optional<lazy_task_launcher<void>>          launcher;
};

static asn1::nrppa::nr_ppa_pdu_c unpack_nrppa_pdu(const byte_buffer& pdu)
{
  asn1::nrppa::nr_ppa_pdu_c nrppa_pdu;
  asn1::cbit_ref            bref(pdu);
  report_fatal_error_if_not(nrppa_pdu.unpack(bref) == asn1::OCUDUASN_SUCCESS, "Failed to unpack NRPPa-PDU");
  return nrppa_pdu;
}

/// Fixture class for the NRPPA implementation.
class nrppa_impl_test : public ::testing::Test
{
protected:
  nrppa_impl_test()
  {
    logger.set_level(ocudulog::basic_levels::debug);
    ocudulog::init();
  }

  ~nrppa_impl_test()
  {
    // Flush logger after each test.
    ocudulog::flush();
  }

  ocudulog::basic_logger&      logger = ocudulog::fetch_basic_logger("NRPPA");
  timer_manager                timer_mng;
  manual_task_worker           ctrl_worker{128};
  fifo_async_task_scheduler    task_sched{32};
  test_nrppa_cu_cp_notifier    cu_cp_notifier;
  nrppa_impl                   nrppa{{}, cu_cp_notifier, task_sched, timer_mng, ctrl_worker};
  cu_cp_ue_index_t             ue_index = uint_to_ue_index(0);
  test_nrppa_cu_cp_ue_notifier ue_notifier;
  test_nrppa_f1ap_notifier     f1ap_notifier;
};

TEST_F(nrppa_impl_test, when_malformed_pdu_is_received_then_it_is_dropped)
{
  byte_buffer garbage_pdu = make_byte_buffer("ffffffffff").value();

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(garbage_pdu,
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_FALSE(cu_cp_notifier.ul_pdu_sent);
}

TEST_F(nrppa_impl_test, when_positioning_information_request_for_unknown_ue_is_received_then_failure_is_sent)
{
  cu_cp_notifier.ue_notifier = nullptr;

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_information_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(cu_cp_notifier.ul_pdu_sent);
  ASSERT_TRUE(
      test_helpers::is_valid_nrppa_positioning_information_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}

TEST_F(nrppa_impl_test, when_positioning_activation_request_for_unknown_ue_is_received_then_failure_is_sent)
{
  cu_cp_notifier.ue_notifier = nullptr;

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_activation_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(cu_cp_notifier.ul_pdu_sent);
  ASSERT_TRUE(
      test_helpers::is_valid_nrppa_positioning_activation_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}

TEST_F(nrppa_impl_test, when_the_du_serving_the_ue_has_no_context_then_positioning_information_request_is_rejected)
{
  cu_cp_notifier.ue_notifier = &ue_notifier;

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_information_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(cu_cp_notifier.ul_pdu_sent);
  ASSERT_TRUE(
      test_helpers::is_valid_nrppa_positioning_information_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}

TEST_F(nrppa_impl_test, when_the_du_serving_the_ue_has_no_context_then_positioning_activation_request_is_rejected)
{
  cu_cp_notifier.ue_notifier = &ue_notifier;

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_activation_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(cu_cp_notifier.ul_pdu_sent);
  ASSERT_TRUE(
      test_helpers::is_valid_nrppa_positioning_activation_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}

TEST_F(nrppa_impl_test, when_the_du_is_registered_then_positioning_information_request_reaches_the_du)
{
  cu_cp_notifier.ue_notifier = &ue_notifier;
  nrppa.get_nrppa_du_context_handler().handle_du_addition(ue_notifier.get_du_index(), f1ap_notifier);

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_information_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(f1ap_notifier.positioning_information_requested);
}

TEST_F(nrppa_impl_test, when_the_du_is_removed_then_positioning_information_request_is_rejected_again)
{
  cu_cp_notifier.ue_notifier = &ue_notifier;
  nrppa.get_nrppa_du_context_handler().handle_du_addition(ue_notifier.get_du_index(), f1ap_notifier);
  nrppa.get_nrppa_du_context_handler().handle_du_removal(ue_notifier.get_du_index());

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(generate_valid_positioning_information_request(),
                                                         std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_FALSE(f1ap_notifier.positioning_information_requested);
  ASSERT_TRUE(
      test_helpers::is_valid_nrppa_positioning_information_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}

TEST_F(nrppa_impl_test, when_e_cid_meas_initiation_request_for_unknown_ue_is_received_then_failure_is_sent)
{
  cu_cp_notifier.ue_notifier = nullptr;

  nrppa.get_nrppa_message_handler().handle_new_nrppa_pdu(
      generate_valid_nrppa_e_cid_measurement_initiation_request(uint_to_lmf_ue_meas_id(1)),
      std::variant<cu_cp_ue_index_t, cu_cp_amf_index_t>{ue_index});

  ASSERT_TRUE(cu_cp_notifier.ul_pdu_sent);
  ASSERT_TRUE(test_helpers::is_valid_e_cid_meas_initiation_failure(unpack_nrppa_pdu(cu_cp_notifier.last_ul_nrppa_pdu)));
}
