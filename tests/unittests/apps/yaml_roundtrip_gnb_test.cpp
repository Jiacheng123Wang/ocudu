// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "apps/gnb/gnb_appconfig.h"
#include "apps/gnb/gnb_appconfig_cli11_schema.h"
#include "apps/gnb/gnb_appconfig_yaml_writer.h"
#include "apps/units/flexible_o_du/flexible_o_du_application_unit.h"
#include "apps/units/o_cu_cp/o_cu_cp_application_unit.h"
#include "apps/units/o_cu_up/o_cu_up_application_unit.h"
#include "yaml_roundtrip_test_helpers.h"
#include "ocudu/adt/format.h"
#include "ocudu/support/config_parsers.h"
#include "CLI/CLI11.hpp"

#ifndef CONFIGS_DIR
#error "CONFIGS_DIR must be defined to point at the repo's configs/ directory"
#endif

using namespace ocudu;
using namespace ocudu::yaml_roundtrip_test;

namespace {

YAML::Node load_and_emit(const std::string& yaml_text)
{
  temp_yaml_file tmp(yaml_text);

  CLI::App app("gnb yaml-roundtrip-test");
  app.config_formatter(create_yaml_config_parser());
  app.allow_config_extras(CLI::config_extras_mode::error);
  std::string cfg_path;
  app.set_config("-c,", cfg_path, "Read config from file", false);

  gnb_appconfig gnb_cfg;
  configure_cli11_with_gnb_appconfig_schema(app, gnb_cfg);

  auto o_cu_cp = create_o_cu_cp_application_unit("gnb");
  auto o_cu_up = create_o_cu_up_application_unit("gnb");
  auto o_du    = create_flexible_o_du_application_unit("gnb");
  o_cu_cp->on_parsing_configuration_registration(app);
  o_cu_up->on_parsing_configuration_registration(app);
  o_du->on_parsing_configuration_registration(app);

  // Mirror the unit-level autoderivation hooks the real gnb main() registers as a CLI11 callback. The
  // gnb-level static autoderivations are not externally linkable, so they remain skipped.
  app.callback([&]() {
    o_cu_cp->on_configuration_parameters_autoderivation(app);
    o_cu_up->on_configuration_parameters_autoderivation(app);
    o_du->on_configuration_parameters_autoderivation(app);
  });

  std::vector<const char*> argv = {"gnb", "-c", tmp.path().c_str()};
  app.parse(static_cast<int>(argv.size()), argv.data());

  YAML::Node node;
  fill_gnb_appconfig_in_yaml_schema(node, gnb_cfg);
  o_cu_cp->dump_config(node);
  o_cu_up->dump_config(node);
  o_du->dump_config(node);
  return node;
}

YAML::Node emit_defaults()
{
  gnb_appconfig gnb_cfg;
  auto          o_cu_cp = create_o_cu_cp_application_unit("gnb");
  auto          o_cu_up = create_o_cu_up_application_unit("gnb");
  auto          o_du    = create_flexible_o_du_application_unit("gnb");
  YAML::Node    node;
  fill_gnb_appconfig_in_yaml_schema(node, gnb_cfg);
  o_cu_cp->dump_config(node);
  o_cu_up->dump_config(node);
  o_du->dump_config(node);
  return node;
}

const std::string CONFIGS = CONFIGS_DIR;

class gnb_example_config_test : public ::testing::TestWithParam<std::string>
{};

TEST_P(gnb_example_config_test, roundtrip)
{
  const std::string& name = GetParam();
  assert_roundtrip(read_file(CONFIGS + "/" + name), &load_and_emit, name);
}

INSTANTIATE_TEST_SUITE_P(,
                         gnb_example_config_test,
                         ::testing::Values("gnb_rf_b200_tdd_n78_20mhz.yml",
                                           "gnb_rf_b210_fdd_srsUE.yml",
                                           "gnb_rf_n310_fdd_n3_20mhz.yml",
                                           "gnb_custom_cell_properties.yml",
                                           "gnb_ru_picocom_scb_tdd_n78_20mhz.yml",
                                           "gnb_ru_ran550_tdd_n78_100mhz_4x2.yml",
                                           "gnb_ru_rpqn4800e_tdd_n78_20mhz_2x2.yml",
                                           // The UL-throughput A/B template (2026-09-27). Listed here so the
                                           // REAL parser checks it: `allow_config_extras(error)` turns an
                                           // unknown key into a parse failure, which is the validation it needs,
                                           // and the leaf-diff check runs over what it does set - including the
                                           // `cell_cfg.pusch` node, whose A/B knob lines ship commented out.
                                           "gnb_rf_b200_tdd_n78_20mhz_ul_ab.yml"));
// NOT listed: the n1 mirror of that template
// (gnb_rf_b200_fdd_n1_5mhz_bridge_ul_ab.yml). It PARSES (that is what `allow_config_extras(error)`
// proves, and its new keys are identical to the n78 template's above), but its leaf comparison fails on
// two keys that the ORIGINAL config carries and the writer does not emit in that form:
// `cu_cp.amf.addr` and `cu_cp.amf.bind_addr` (the writer emits the plural `addrs`/`bind_addrs`). That
// mismatch is why gnb_rf_b200_fdd_n1_5mhz_bridge.yml itself is not in this list either - it is inherited,
// not introduced here, and the mirror stays a faithful copy of what the bench actually runs.

TEST(gnb_default_config_test, roundtrip)
{
  YAML::Node a = emit_defaults();
  assert_roundtrip(YAML::Dump(a), &load_and_emit, "gnb defaults");
}

} // namespace
