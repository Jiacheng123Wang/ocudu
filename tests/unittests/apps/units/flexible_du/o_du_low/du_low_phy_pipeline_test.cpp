// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Tests of the uplink PHY pipeline mode: the CLI values and the per-mode backend resolution rules
/// (see du_low_phy_pipeline.h).
///
/// The rules are the contract of `expert_phy --phy_pipeline`: they decide which backend every module gets, which
/// combinations are configuration conflicts and what the effective-configuration log reports. They are shared by the
/// configuration validator and by the two sides of the lower/upper PHY boundary (the DFT reaches the lower PHY through
/// the radio-unit configuration), so they are worth locking down on their own.

#include "apps/units/flexible_o_du/o_du_low/du_low_config.h"
#include "apps/units/flexible_o_du/o_du_low/du_low_config_cli11_schema.h"
#include "apps/units/flexible_o_du/o_du_low/du_low_phy_pipeline.h"
#include <gtest/gtest.h>
#include <initializer_list>
#include <utility>

using namespace ocudu;

namespace {

/// All backends built in (Apple Silicon build), the AI receiver included (set by the field below).
constexpr phy_backend_availability all_available{true, true, true, true, true, true};
/// No Metal backend built in (any other platform).
constexpr phy_backend_availability none_available{false, false, false, false, false, false};

phy_pipeline_request make_request(std::string mode        = "auto",
                                  std::string dft         = "auto",
                                  std::string ch_est      = "auto",
                                  std::string equalizer   = "auto",
                                  std::string ldpc        = "auto",
                                  std::string device_grid = "auto",
                                  std::string receiver    = "auto")
{
  return phy_pipeline_request{std::move(mode),
                              std::move(dft),
                              std::move(ch_est),
                              std::move(equalizer),
                              std::move(ldpc),
                              std::move(device_grid),
                              std::move(receiver)};
}

/// Resolves \c request and fails the test when it is rejected.
phy_pipeline_effective resolve(const phy_pipeline_request&     request,
                               const phy_backend_availability& available = all_available)
{
  std::string           error;
  const auto            effective = resolve_phy_pipeline(request, available, error);
  EXPECT_TRUE(effective.has_value()) << "unexpected conflict: " << error;
  return effective.value_or(phy_pipeline_effective{});
}

/// Resolves \c request and returns the conflict message (fails the test when the configuration is accepted).
std::string resolve_conflict(const phy_pipeline_request& request, const phy_backend_availability& available = all_available)
{
  std::string error;
  const auto  effective = resolve_phy_pipeline(request, available, error);
  EXPECT_FALSE(effective.has_value()) << "expected a configuration conflict";
  EXPECT_FALSE(error.empty());
  return error;
}

TEST(phy_pipeline_mode_test, backend_classification)
{
  // CPU implementations, "auto" included (it selects the CPU one outside the fused lane).
  EXPECT_TRUE(is_cpu_phy_backend("auto"));
  EXPECT_TRUE(is_cpu_phy_backend("cpu"));
  EXPECT_TRUE(is_cpu_phy_backend("generic"));
  EXPECT_TRUE(is_cpu_phy_backend("neon"));
  EXPECT_TRUE(is_cpu_phy_backend("avx2"));
  EXPECT_TRUE(is_cpu_phy_backend("avx512"));
  // Offload backends.
  EXPECT_FALSE(is_cpu_phy_backend("metal"));
  EXPECT_FALSE(is_cpu_phy_backend("metal_mmse"));
  EXPECT_FALSE(is_cpu_phy_backend("metal_nn_mmse"));
  EXPECT_FALSE(is_cpu_phy_backend("metal_persistent"));
  EXPECT_FALSE(is_cpu_phy_backend("helena"));
}

TEST(phy_pipeline_mode_test, mode_strings_roundtrip)
{
  phy_pipeline_mode mode = phy_pipeline_mode::gpu;
  ASSERT_TRUE(phy_pipeline_mode_from_string("cpu", mode));
  EXPECT_EQ(mode, phy_pipeline_mode::cpu);
  ASSERT_TRUE(phy_pipeline_mode_from_string("cpu_gpu", mode));
  EXPECT_EQ(mode, phy_pipeline_mode::cpu_gpu);
  ASSERT_TRUE(phy_pipeline_mode_from_string("gpu", mode));
  EXPECT_EQ(mode, phy_pipeline_mode::gpu);
  EXPECT_EQ(std::string(to_string(phy_pipeline_mode::cpu_gpu)), "cpu_gpu");
  // "auto" is a command-line-only value: the resolver turns it into a concrete mode.
  EXPECT_FALSE(phy_pipeline_mode_from_string("auto", mode));
  EXPECT_FALSE(phy_pipeline_mode_from_string("", mode));
}

TEST(phy_pipeline_mode_test, default_configuration_is_the_cpu_pipeline)
{
  const phy_pipeline_effective effective = resolve(make_request());
  EXPECT_EQ(effective.mode, phy_pipeline_mode::cpu);
  EXPECT_FALSE(effective.lane_fused);
  EXPECT_EQ(effective.dft, "cpu");
  EXPECT_EQ(effective.ch_est, "cpu");
  EXPECT_EQ(effective.equalizer, "cpu");
  // The LDPC decoder keeps its own knob: "auto" means the fastest CPU implementation, not "follow the mode".
  EXPECT_EQ(effective.ldpc, "auto");
}

TEST(phy_pipeline_mode_test, unspecified_mode_follows_the_module_knobs)
{
  // Without --phy_pipeline, any offload module selects the module-level offload mode (historical behavior).
  EXPECT_EQ(resolve(make_request("auto", "metal")).mode, phy_pipeline_mode::cpu_gpu);
  EXPECT_EQ(resolve(make_request("auto", "auto", "metal_mmse")).mode, phy_pipeline_mode::cpu_gpu);
  EXPECT_EQ(resolve(make_request("auto", "auto", "auto", "metal")).mode, phy_pipeline_mode::cpu_gpu);
  EXPECT_EQ(resolve(make_request("auto", "auto", "auto", "auto", "metal")).mode, phy_pipeline_mode::cpu_gpu);
  // Every module on the CPU is the CPU pipeline.
  EXPECT_EQ(resolve(make_request("auto", "cpu", "cpu", "cpu", "neon")).mode, phy_pipeline_mode::cpu);
}

TEST(phy_pipeline_mode_test, cpu_mode_rejects_every_offload_module)
{
  for (const char* value : {"metal", "metal_mmse", "metal_nn_mmse", "helena"}) {
    EXPECT_FALSE(resolve_conflict(make_request("cpu", "auto", value)).empty());
  }
  EXPECT_NE(resolve_conflict(make_request("cpu", "metal")).find("--pusch_dft_type"), std::string::npos);
  EXPECT_NE(resolve_conflict(make_request("cpu", "auto", "auto", "metal")).find("--pusch_channel_equalizer_backend"),
            std::string::npos);
  EXPECT_NE(resolve_conflict(make_request("cpu", "auto", "auto", "auto", "metal_persistent"))
                .find("--pusch_ldpc_decoder_type"),
            std::string::npos);
  // A conflict is reported even when the requested backend is not built into the binary: the user asked for two
  // contradictory things and must not silently get a third.
  EXPECT_FALSE(resolve_conflict(make_request("cpu", "metal"), none_available).empty());
}

TEST(phy_pipeline_mode_test, cpu_mode_keeps_the_requested_cpu_flavors)
{
  const phy_pipeline_effective effective = resolve(make_request("cpu", "cpu", "cpu", "cpu", "avx512"));
  EXPECT_EQ(effective.mode, phy_pipeline_mode::cpu);
  EXPECT_EQ(effective.dft, "cpu");
  EXPECT_EQ(effective.ldpc, "avx512");
}

TEST(phy_pipeline_mode_test, cpu_gpu_mode_honors_the_module_knobs)
{
  const phy_pipeline_effective effective = resolve(make_request("cpu_gpu", "metal", "metal_mmse", "metal", "metal"));
  EXPECT_EQ(effective.mode, phy_pipeline_mode::cpu_gpu);
  EXPECT_FALSE(effective.lane_fused);
  EXPECT_EQ(effective.dft, "metal");
  EXPECT_EQ(effective.ch_est, "metal_mmse");
  EXPECT_EQ(effective.equalizer, "metal");
  EXPECT_EQ(effective.ldpc, "metal");

  // "auto" resolves to the CPU implementation, i.e. cpu_gpu with no offload module is the CPU pipeline.
  const phy_pipeline_effective all_cpu = resolve(make_request("cpu_gpu"));
  EXPECT_EQ(all_cpu.mode, phy_pipeline_mode::cpu_gpu);
  EXPECT_EQ(all_cpu.dft, "cpu");
  EXPECT_EQ(all_cpu.ch_est, "cpu");
  EXPECT_EQ(all_cpu.equalizer, "cpu");
}

TEST(phy_pipeline_mode_test, unavailable_backends_fall_back_to_the_cpu)
{
  // Same policy as the PHY factories: a backend that is not linked in falls back instead of failing the whole
  // application (the effective-configuration log reports the substitution).
  const phy_pipeline_effective effective = resolve(make_request("cpu_gpu", "metal", "metal_mmse", "metal", "metal"),
                                                   none_available);
  EXPECT_EQ(effective.dft, "cpu");
  EXPECT_EQ(effective.ch_est, "cpu");
  EXPECT_EQ(effective.equalizer, "cpu");
  // The LDPC decoder falls back to "auto" (the CPU-selecting value), not to "cpu".
  EXPECT_EQ(effective.ldpc, "auto");
}

TEST(phy_pipeline_mode_test, gpu_mode_takes_over_the_lane_modules)
{
  const phy_pipeline_effective effective = resolve(make_request("gpu"));
  EXPECT_EQ(effective.mode, phy_pipeline_mode::gpu);
  EXPECT_TRUE(effective.lane_fused);
  // The lane's DEFAULT DFT is the CPU since 2026-09-30 (dev doc 6.215): the host writes the grid and the other
  // three modules stay on the device. See gpu_mode_defaults_to_a_host_written_grid below for the whole rule.
  EXPECT_EQ(effective.dft, "cpu");
  EXPECT_TRUE(effective.host_grid);
  EXPECT_EQ(effective.ch_est, "metal_mmse");
  EXPECT_EQ(effective.equalizer, "metal");
  // The LDPC decoder is not part of the lane: the LLR still leaves the device for the CPU decoder.
  EXPECT_EQ(effective.ldpc, "auto");

  // An explicit lane backend is honored (it is how a different Metal estimator is selected).
  EXPECT_EQ(resolve(make_request("gpu", "auto", "metal_nn_mmse")).ch_est, "metal_nn_mmse");
  EXPECT_EQ(resolve(make_request("gpu", "auto", "auto", "auto", "metal")).ldpc, "metal");
}

TEST(phy_pipeline_mode_test, gpu_mode_has_no_cpu_fallback)
{
  // The default of the estimator and the equalizer knobs is "auto", so an explicit "cpu" is a deliberate
  // request that the mode cannot honor. The DFT is NOT one of them any more - the CPU is its DEFAULT there,
  // and the Metal DFT is what has to be asked for (see the test below).
  EXPECT_NE(resolve_conflict(make_request("gpu", "auto", "cpu")).find("--pusch_channel_estimator_algo"),
            std::string::npos);
  EXPECT_NE(resolve_conflict(make_request("gpu", "auto", "auto", "cpu")).find("--pusch_channel_equalizer_backend"),
            std::string::npos);
}

TEST(phy_pipeline_mode_test, gpu_mode_defaults_to_a_host_written_grid)
{
  // The delivered rule (dev doc 6.215): --phy_pipeline gpu with no DFT knob writes the frequency-domain grid
  // from the HOST and keeps the estimator, the equalizer and the demapper on the device. The grid stays in
  // device-visible storage, so what changes is its WRITER, not where it lives.
  const phy_pipeline_effective delivered = resolve(make_request("gpu"));
  EXPECT_TRUE(delivered.lane_fused);
  EXPECT_EQ(delivered.dft, "cpu");
  EXPECT_TRUE(delivered.host_grid);
  EXPECT_EQ(delivered.ch_est, "metal_mmse");
  EXPECT_EQ(delivered.equalizer, "metal");
  EXPECT_TRUE(delivered.device_grid);

  // Writing the knob out explicitly is the same configuration (it is what the delivered config file may say).
  const phy_pipeline_effective spelled = resolve(make_request("gpu", "cpu"));
  EXPECT_EQ(spelled.dft, delivered.dft);
  EXPECT_EQ(spelled.host_grid, delivered.host_grid);

  // The HISTORICAL configuration - the DFT on the device - is asked for explicitly, under either spelling; the
  // alias is normalized so that every consumer downstream sees one value.
  const phy_pipeline_effective metal = resolve(make_request("gpu", "metal"));
  EXPECT_EQ(metal.dft, "metal");
  EXPECT_FALSE(metal.host_grid);
  EXPECT_EQ(resolve(make_request("gpu", "gpu")).dft, "metal");
  EXPECT_FALSE(resolve(make_request("gpu", "gpu")).host_grid);
  // ... and the three device-side consumers are the same in both arms: only the grid's writer moves.
  EXPECT_EQ(metal.ch_est, delivered.ch_est);
  EXPECT_EQ(metal.equalizer, delivered.equalizer);
  EXPECT_TRUE(metal.device_grid);

  // Outside the fused lane the flag is simply the backend classification, and it is set for every mode.
  EXPECT_TRUE(resolve(make_request("cpu")).host_grid);
  EXPECT_TRUE(resolve(make_request("cpu_gpu", "cpu")).host_grid);
  EXPECT_FALSE(resolve(make_request("cpu_gpu", "metal")).host_grid);
}

TEST(phy_pipeline_mode_test, gpu_mode_honors_a_cpu_dft_as_the_host_grid_arm)
{
  // Superseded by gpu_mode_defaults_to_a_host_written_grid above (dev doc 6.215): the CPU DFT is the lane's
  // DEFAULT now, and the Metal one is the configuration that has to be asked for.
  SUCCEED();
}

TEST(phy_pipeline_mode_test, gpu_mode_requires_the_lane_backends)
{
  EXPECT_TRUE(check_phy_pipeline_lane_available(all_available).empty());
  EXPECT_FALSE(check_phy_pipeline_lane_available(none_available).empty());
  // The demapper follows the equalizer backend, but the lane needs the Metal one: a build without it cannot run the
  // fused chain at all.
  constexpr phy_backend_availability no_demapper{true, true, true, false, true};
  EXPECT_FALSE(check_phy_pipeline_lane_available(no_demapper).empty());

  // The host-grid arm (an explicit CPU DFT) does not need the Metal DFT - nothing on the device writes the grid -
  // while the three device-side consumers are still required.
  constexpr phy_backend_availability no_dft{false, true, true, true, true};
  EXPECT_FALSE(check_phy_pipeline_lane_available(no_dft).empty());
  EXPECT_TRUE(check_phy_pipeline_lane_available(no_dft, /*dft_on_cpu=*/true).empty());
  EXPECT_FALSE(check_phy_pipeline_lane_available(no_demapper, /*dft_on_cpu=*/true).empty());
  // The message names what is actually missing, so an operator can tell the two arms apart.
  EXPECT_NE(check_phy_pipeline_lane_available(no_dft).find("Metal DFT"), std::string::npos);
  EXPECT_EQ(check_phy_pipeline_lane_available(no_dft, /*dft_on_cpu=*/true).find("Metal DFT"), std::string::npos);
}

TEST(phy_pipeline_mode_test, device_resource_grid_follows_the_mode_by_default)
{
  // "auto" keeps a module-level offload run comparable with the ones recorded before the capability existed.
  EXPECT_FALSE(resolve(make_request()).device_grid);
  EXPECT_FALSE(resolve(make_request("cpu_gpu", "metal", "metal_mmse", "metal", "metal")).device_grid);
  // The fused lane owns the grid.
  EXPECT_TRUE(resolve(make_request("gpu")).device_grid);

  // The knob overrides the mode in both directions (it is the A/B control of the device grid).
  EXPECT_TRUE(resolve(make_request("cpu_gpu", "metal", "auto", "auto", "auto", "on")).device_grid);
  EXPECT_FALSE(resolve(make_request("gpu", "auto", "auto", "auto", "auto", "off")).device_grid);
  // ... and the fused lane is still the fused lane: the knob only moves where the grid lives.
  EXPECT_TRUE(resolve(make_request("gpu", "auto", "auto", "auto", "auto", "off")).lane_fused);
}

TEST(phy_pipeline_mode_test, device_resource_grid_is_rejected_by_the_cpu_pipeline)
{
  // Writing the grid from the device is an offload of the OFDM demodulation: the CPU pipeline has no DFT that can do
  // it, so asking for it there is a conflict rather than a silent no-op.
  EXPECT_NE(resolve_conflict(make_request("cpu", "auto", "auto", "auto", "auto", "on"))
                .find("--device_resource_grid"),
            std::string::npos);
  // ... while the CPU pipeline with the knob off (or on auto) is fine.
  EXPECT_FALSE(resolve(make_request("cpu", "auto", "auto", "auto", "auto", "off")).device_grid);
}

TEST(phy_pipeline_mode_test, invalid_device_resource_grid_is_rejected)
{
  EXPECT_NE(resolve_conflict(make_request("cpu_gpu", "auto", "auto", "auto", "auto", "yes"))
                .find("Invalid device resource grid"),
            std::string::npos);
}

TEST(phy_pipeline_mode_test, invalid_mode_is_rejected)
{
  EXPECT_NE(resolve_conflict(make_request("gpu_pipeline")).find("Invalid UL PHY pipeline mode"), std::string::npos);
  EXPECT_NE(resolve_conflict(make_request("")).find("Invalid UL PHY pipeline mode"), std::string::npos);
}

/// Parses \c args with the DU low command-line schema and returns the expert-phy configuration.
du_low_unit_expert_upper_phy_config parse_expert_phy(std::initializer_list<std::string> args)
{
  CLI::App          app("du_low_phy_pipeline_test");
  du_low_unit_config config;
  configure_cli11_with_du_low_config_schema(app, config);

  // The (argc, argv) overload skips the first element as the program name.
  std::vector<std::string> argv_storage{"du_low_phy_pipeline_test"};
  argv_storage.insert(argv_storage.end(), args.begin(), args.end());
  std::vector<const char*> argv;
  argv.reserve(argv_storage.size());
  for (const std::string& arg : argv_storage) {
    argv.push_back(arg.c_str());
  }
  app.parse(static_cast<int>(argv.size()), argv.data());
  return config.expert_phy_cfg;
}

TEST(phy_pipeline_cli_test, pipeline_mode_values)
{
  EXPECT_EQ(parse_expert_phy({"expert_phy"}).phy_pipeline, "auto");
  EXPECT_EQ(parse_expert_phy({"expert_phy", "--phy_pipeline", "cpu"}).phy_pipeline, "cpu");
  EXPECT_EQ(parse_expert_phy({"expert_phy", "--phy_pipeline", "cpu_gpu"}).phy_pipeline, "cpu_gpu");
  EXPECT_EQ(parse_expert_phy({"expert_phy", "--phy_pipeline", "gpu"}).phy_pipeline, "gpu");
  EXPECT_THROW(parse_expert_phy({"expert_phy", "--phy_pipeline", "gpu_lane"}), CLI::ParseError);
}

TEST(phy_pipeline_cli_test, module_backends_accept_auto)
{
  const du_low_unit_expert_upper_phy_config defaults = parse_expert_phy({"expert_phy"});
  EXPECT_EQ(defaults.pusch_dft_type, "auto");
  EXPECT_EQ(defaults.pusch_channel_estimator_algo, "auto");
  EXPECT_EQ(defaults.pusch_channel_equalizer_backend, "auto");
  EXPECT_EQ(defaults.ldpc_decoder_type, "auto");

  const du_low_unit_expert_upper_phy_config explicit_cpu = parse_expert_phy({"expert_phy",
                                                                            "--pusch_dft_type",
                                                                            "cpu",
                                                                            "--pusch_channel_estimator_algo",
                                                                            "cpu",
                                                                            "--pusch_channel_equalizer_backend",
                                                                            "cpu"});
  EXPECT_EQ(explicit_cpu.pusch_dft_type, "cpu");
  EXPECT_EQ(explicit_cpu.pusch_channel_estimator_algo, "cpu");
  EXPECT_EQ(explicit_cpu.pusch_channel_equalizer_backend, "cpu");
  // "gpu" is ACCEPTED as the alias of the device DFT since 2026-09-30 (dev doc 6.215): the historical
  // full-Metal configuration is what has to be asked for, and both spellings mean it.
  EXPECT_EQ(parse_expert_phy({"expert_phy", "--pusch_dft_type", "gpu"}).pusch_dft_type, "gpu");
  EXPECT_EQ(parse_expert_phy({"expert_phy", "--pusch_dft_type", "metal"}).pusch_dft_type, "metal");
  EXPECT_THROW(parse_expert_phy({"expert_phy", "--pusch_dft_type", "magic"}), CLI::ParseError);
  EXPECT_THROW(parse_expert_phy({"expert_phy", "--pusch_channel_equalizer_backend", "magic"}), CLI::ParseError);
  EXPECT_THROW(parse_expert_phy({"expert_phy", "--pusch_channel_estimator_algo", "magic"}), CLI::ParseError);
}


/// \brief `--phy_pipeline gpu` must select exactly what spelling every module out selects.
///
/// This is the promise that lets the four module knobs be treated as cpu_gpu-only: once the glue is
/// gone, the switch alone is the whole configuration. The two command lines differ only in how they
/// say it, so their effective configurations must be identical field by field.
///
/// \note The DFT is spelled `cpu` since 2026-09-30 (dev doc 6.215): the lane's own default moved to the
///       HOST-written grid, so that is what the equivalent command line says. The module-level default
///       ("auto" -> the CPU implementation, which is the same backend) is what makes the two agree.
TEST(DuLowPhyPipelineTest, GpuModeEqualsTheModuleKnobsSpelledOut)
{
  const phy_pipeline_effective via_mode = resolve(make_request("gpu"));
  const phy_pipeline_effective via_knobs =
      resolve(make_request("cpu_gpu", "cpu", "metal_mmse", "metal", "metal", "on"));

  // The mode LABEL differs by design (gpu also declares the fused lane), what must be identical is the
  // configuration those two command lines select.
  EXPECT_EQ(via_mode.dft, via_knobs.dft);
  EXPECT_EQ(via_mode.ch_est, via_knobs.ch_est);
  EXPECT_EQ(via_mode.equalizer, via_knobs.equalizer);
  EXPECT_EQ(via_mode.device_grid, via_knobs.device_grid);

  // And the historical configuration needs BOTH knobs spelled out on the mode side: the DFT on the device.
  const phy_pipeline_effective historical = resolve(make_request("gpu", "metal"));
  EXPECT_EQ(historical.dft, "metal");
  EXPECT_FALSE(historical.host_grid);
  EXPECT_EQ(historical.ch_est, via_mode.ch_est);
  EXPECT_EQ(historical.equalizer, via_mode.equalizer);
}

/// The gpu mode's knobs may only repeat the lane's own backend: everything else is a conflict rather
/// than a silent override (a "cpu" knob that runs on the device makes the command line lie).
TEST(DuLowPhyPipelineTest, GpuModeRejectsBackendsTheLaneDoesNotOwn)
{
  // The lane owns the estimator and the equalizer and has no CPU fallback: EVERY CPU backend (not only
  // "cpu") is a conflict for them - "generic", "neon", "avx2" and "avx512" select a CPU implementation
  // just as "cpu" does, and accepting them silently would let the command line claim the module runs on
  // the host.
  for (const char* cpu_backend : {"cpu", "generic", "neon", "avx2", "avx512"}) {
    EXPECT_FALSE(resolve_conflict(make_request("gpu", "metal", cpu_backend)).empty()) << cpu_backend;
    EXPECT_FALSE(resolve_conflict(make_request("gpu", "metal", "metal_mmse", cpu_backend)).empty()) << cpu_backend;
  }

  // The DFT is the ONE module of the lane whose CPU backend IS honored, and it is a deliberate measurement
  // arm rather than a fallback (dev doc 6.205-6.207): the frequency-domain grid is then written by the
  // host and the three device-side consumers keep the device. Every CPU spelling resolves for it and is
  // reported as a host-written grid.
  for (const char* cpu_backend : {"cpu", "generic", "neon", "avx2", "avx512"}) {
    const phy_pipeline_effective arm = resolve(make_request("gpu", cpu_backend));
    EXPECT_EQ(arm.dft, cpu_backend) << cpu_backend;
    EXPECT_TRUE(arm.host_grid) << cpu_backend;
    EXPECT_TRUE(arm.lane_fused) << cpu_backend;
  }

  // The LDPC decoder is NOT part of the lane (the LLR still leaves the device for the CPU decoder), so
  // its knob keeps its meaning in this mode: no conflict, and the requested flavor survives.
  EXPECT_EQ(resolve(make_request("gpu", "auto", "auto", "auto", "cpu")).ldpc, "cpu");
  EXPECT_EQ(resolve(make_request("gpu", "auto", "auto", "auto", "generic")).ldpc, "generic");

  // The grid knob stays what it has always been - the A/B control of where the grid lives, which the
  // mode only defaults (see device_resource_grid_follows_the_mode_by_default): asking for it here is not
  // a conflict, and the lane is still declared.
  EXPECT_TRUE(resolve(make_request("gpu", "auto", "auto", "auto", "auto", "on")).device_grid);
  EXPECT_TRUE(resolve(make_request("gpu", "auto", "auto", "auto", "auto", "on")).lane_fused);
  EXPECT_TRUE(resolve(make_request("gpu")).device_grid);

  // Any DEVICE flavor is accepted, spelled out or not: the knobs still pick which device backend
  // runs (the lane's own values are the defaults). resolve() is the helper that fails the test when a
  // configuration is REJECTED, which is what these must not be.
  EXPECT_EQ(resolve(make_request("gpu", "metal", "metal_mmse", "metal")).dft, "metal");
  EXPECT_EQ(resolve(make_request("gpu", "metal", "metal_nn_mmse")).ch_est, "metal_nn_mmse");
  EXPECT_EQ(resolve(make_request("gpu", "auto", "helena")).ch_est, "helena");
}

/// \brief The depth-3 receiver knob: one value per unit, and the two modes that cannot express it.
///
/// STAGE 1 of the LLR AI detection workflow (design section 1.5): "ai" is ACCEPTED and CARRIED, but
/// ai_receiver_bound() is false, so it resolves to the classic receiver. Both halves are locked down here,
/// because the dangerous failure is not "ai is refused" - it is "ai is accepted and the classic chain runs
/// without anyone being able to tell afterwards".
TEST(phy_pipeline_mode_test, receiver_backend_defaults_to_classic)
{
  // No knob: the classic receiver, and nothing about the resolved value depends on the mode.
  EXPECT_EQ(resolve(make_request()).receiver, "classic");
  EXPECT_EQ(resolve(make_request("cpu")).receiver, "classic");
  EXPECT_EQ(resolve(make_request("cpu_gpu")).receiver, "classic");
  EXPECT_EQ(resolve(make_request("gpu")).receiver, "classic");

  // Spelled out: the same value.
  EXPECT_EQ(resolve(make_request("gpu", "auto", "auto", "auto", "auto", "auto", "classic")).receiver, "classic");
}

TEST(phy_pipeline_mode_test, receiver_ai_is_carried_and_reported)
{
  // An invalid value is a conflict of its own (the CLI check carries the same list).
  std::string error;
  EXPECT_FALSE(resolve_phy_pipeline(make_request("gpu", "auto", "auto", "auto", "auto", "auto", "bogus"),
                                    all_available,
                                    error)
                   .has_value());
  EXPECT_NE(error.find("Accepted values [auto,classic,ai]"), std::string::npos) << error;
}

TEST(phy_pipeline_mode_test, receiver_ai_falls_back_to_classic_while_unbound)
{
  // STAGE 1: the value is carried into the request, but no backend is bound to it yet, so it resolves to the
  // classic receiver - and the caller is expected to report the substitution (the startup log does).
  ASSERT_FALSE(ai_receiver_bound()) << "this test describes stage 1; update it when the backend is wired";

  std::string           error;
  const auto            effective = resolve_phy_pipeline(
      make_request("gpu", "auto", "auto", "auto", "auto", "auto", "ai"), all_available, error);
  ASSERT_TRUE(effective.has_value()) << error;
  EXPECT_EQ(effective->receiver, "classic");
  // The rest of the configuration is untouched by the substitution.
  EXPECT_TRUE(effective->lane_fused);
  EXPECT_EQ(effective->ch_est, "metal_mmse");
}

TEST(phy_pipeline_mode_test, receiver_ai_conflicts_with_the_two_modes_that_cannot_express_it)
{
  std::string error;

  // cpu forbids every offload, so an AI receiver there is the same kind of conflict as a Metal DFT.
  EXPECT_FALSE(resolve_phy_pipeline(make_request("cpu", "auto", "auto", "auto", "auto", "auto", "ai"),
                                    all_available,
                                    error)
                   .has_value());
  EXPECT_NE(error.find("--pusch_receiver_backend"), std::string::npos) << error;

  // cpu_gpu is per-MODULE offload: it cannot say "these three modules at once", so accepting the knob there
  // would leave the mode unable to describe what it runs.
  error.clear();
  EXPECT_FALSE(resolve_phy_pipeline(make_request("cpu_gpu", "auto", "auto", "auto", "auto", "auto", "ai"),
                                    all_available,
                                    error)
                   .has_value());
  EXPECT_NE(error.find("depth 3 is one unit"), std::string::npos) << error;

  // The classic receiver is accepted in both, so the refusal is about "ai" and not about the knob existing.
  EXPECT_EQ(resolve(make_request("cpu", "auto", "auto", "auto", "auto", "auto", "classic")).receiver, "classic");
  EXPECT_EQ(resolve(make_request("cpu_gpu", "auto", "auto", "auto", "auto", "auto", "classic")).receiver, "classic");
}
} // namespace
