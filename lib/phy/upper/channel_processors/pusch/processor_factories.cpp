// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "logging_pusch_processor_decorator.h"
#include "pusch_codeblock_decoder.h"
#include "pusch_decoder_empty_impl.h"
#include "pusch_decoder_hw_impl.h"
#include "pusch_decoder_impl.h"
#include "pusch_depth3_model_map.h"
#include "pusch_depth3_input_plane.h"
#include "pusch_depth3_receiver_impl.h"
#include "pusch_processor_impl.h"
#include "pusch_processor_pool.h"
#include "pusch_processor_validator_impl.h"
#include "ulsch_demultiplex_impl.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/upper/channel_processors/pusch/factories.h"

using namespace ocudu;

namespace {

/// \brief Factory for empty PUSCH decoders.
///
/// It creates \ref pusch_decoder_empty_impl instances.
class pusch_decoder_factory_empty : public pusch_decoder_factory
{
public:
  /// Constructs a factory taking the maximum number of PRB and layers.
  pusch_decoder_factory_empty(unsigned nof_prb_, unsigned nof_layers_) : nof_prb(nof_prb_), nof_layers(nof_layers_)
  {
    ocudu_assert(nof_prb > 0, "Invalid number of PRB.");
    ocudu_assert(nof_layers > 0, "Invalid number of layers.");
  }

  // See interface for documentation.
  std::unique_ptr<pusch_decoder> create() override
  {
    return std::make_unique<pusch_decoder_empty_impl>(nof_prb, nof_layers);
  }

private:
  unsigned nof_prb;
  unsigned nof_layers;
};

class pusch_decoder_factory_generic : public pusch_decoder_factory
{
public:
  explicit pusch_decoder_factory_generic(pusch_decoder_factory_sw_configuration config) :
    crc_factory(std::move(config.crc_factory)),
    segmenter_factory(std::move(config.segmenter_factory)),
    executor(config.executor),
    nof_prb(config.nof_prb),
    nof_layers(config.nof_layers),
    ldpc_decoder_type(std::move(config.ldpc_decoder_type))
  {
    ocudu_assert(crc_factory, "Invalid CRC calculator factory.");
    ocudu_assert(config.decoder_factory, "Invalid LDPC decoder factory.");
    ocudu_assert(config.dematcher_factory, "Invalid LDPC dematcher factory.");
    ocudu_assert(segmenter_factory, "Invalid LDPC segmenter factory.");

    std::vector<std::unique_ptr<pusch_codeblock_decoder>> codeblock_decoders(
        std::max(1U, config.nof_pusch_decoder_threads));
    for (std::unique_ptr<pusch_codeblock_decoder>& codeblock_decoder : codeblock_decoders) {
      pusch_codeblock_decoder::sch_crc crcs1;
      crcs1.crc16  = crc_factory->create(crc_generator_poly::CRC16);
      crcs1.crc24A = crc_factory->create(crc_generator_poly::CRC24A);
      crcs1.crc24B = crc_factory->create(crc_generator_poly::CRC24B);

      codeblock_decoder = std::make_unique<pusch_codeblock_decoder>(
          config.dematcher_factory->create(), config.decoder_factory->create(), crcs1);
    }

    decoder_pool = std::make_unique<pusch_decoder_impl::codeblock_decoder_pool>(codeblock_decoders);
  }

  std::unique_ptr<pusch_decoder> create() override
  {
    pusch_decoder_impl::sch_crc crcs;
    crcs.crc16  = crc_factory->create(crc_generator_poly::CRC16);
    crcs.crc24A = crc_factory->create(crc_generator_poly::CRC24A);
    crcs.crc24B = crc_factory->create(crc_generator_poly::CRC24B);

    return std::make_unique<pusch_decoder_impl>(segmenter_factory->create(),
                                                decoder_pool,
                                                std::move(crcs),
                                                executor,
                                                nof_prb,
                                                nof_layers,
                                                ldpc_decoder_type);
  }

private:
  std::shared_ptr<pusch_decoder_impl::codeblock_decoder_pool> decoder_pool;
  std::shared_ptr<crc_calculator_factory>                     crc_factory;
  std::shared_ptr<ldpc_segmenter_rx_factory>                  segmenter_factory;
  task_executor*                                              executor;
  unsigned                                                    nof_prb;
  unsigned                                                    nof_layers;
  std::string                                                 ldpc_decoder_type;
};

/// HW-accelerated PUSCH decoder factory.
class pusch_decoder_factory_hw : public pusch_decoder_factory
{
public:
  explicit pusch_decoder_factory_hw(const pusch_decoder_factory_hw_configuration& config) :
    segmenter_factory(config.segmenter_factory), crc_factory(config.crc_factory), executor(config.executor)
  {
    ocudu_assert(segmenter_factory, "Invalid LDPC segmenter factory.");
    ocudu_assert(crc_factory, "Invalid CRC factory.");
    ocudu_assert(config.hw_decoder_factory, "Invalid hardware accelerator factory.");

    // Creates a vector of hardware decoders. These are shared for all the PUSCH decoders.
    std::vector<std::unique_ptr<hal::hw_accelerator_pusch_dec>> hw_decoders(
        std::max(1U, config.nof_pusch_decoder_threads));
    for (std::unique_ptr<hal::hw_accelerator_pusch_dec>& hw_decoder : hw_decoders) {
      hw_decoder = config.hw_decoder_factory->create();
    }

    // Creates the hardware decoder pool. The pool is common among all the PUSCH decoders.
    hw_decoder_pool = std::make_unique<pusch_decoder_hw_impl::hw_decoder_pool>(hw_decoders);
  }

  std::unique_ptr<pusch_decoder> create() override
  {
    pusch_decoder_hw_impl::sch_crc crc = {
        crc_factory->create(crc_generator_poly::CRC16),
        crc_factory->create(crc_generator_poly::CRC24A),
        crc_factory->create(crc_generator_poly::CRC24B),
    };
    return std::make_unique<pusch_decoder_hw_impl>(segmenter_factory->create(), crc, hw_decoder_pool, executor);
  }

private:
  std::shared_ptr<ldpc_segmenter_rx_factory>              segmenter_factory;
  std::shared_ptr<crc_calculator_factory>                 crc_factory;
  std::shared_ptr<pusch_decoder_hw_impl::hw_decoder_pool> hw_decoder_pool;
  task_executor*                                          executor;
};

/// Whether any per-modulation slot carries a path. A run whose only model is a slot IS an AI run: the slots are
/// how a modulation is covered, and the fallback is only what serves the modulations no slot claims.
static bool has_any_slot(const std::array<std::string, 4>& slots)
{
  return std::any_of(slots.begin(), slots.end(), [](const std::string& slot) { return !slot.empty(); });
}

class pusch_processor_factory_generic : public pusch_processor_factory
{
public:
  explicit pusch_processor_factory_generic(pusch_processor_factory_sw_configuration& config) :
    estimator_factory(config.estimator_factory),
    demodulator_factory(config.demodulator_factory),
    demux_factory(config.demux_factory),
    decoder_factory(config.decoder_factory),
    uci_dec_factory(config.uci_dec_factory),
    ch_estimate_dimensions(config.ch_estimate_dimensions),
    dec_nof_iterations(config.dec_nof_iterations),
    dec_enable_early_stop(config.dec_enable_early_stop),
    // A model path selects the arm that COMPUTES; without one the arm is the stage-S-1 identity, which
    // forwards and says so. The choice is made here because only the configuration knows whether a model was
    // asked for, and the arm reports the OUTCOME (a bundle that fails to load leaves it inactive).
    depth3_kind(config.pusch_receiver_backend == "ai"
                    ? ((config.pusch_receiver_model_path.empty() && !has_any_slot(config.pusch_receiver_model_slots))
                           ? pusch_depth3_kind::ai_identity
                           : pusch_depth3_kind::ai_network)
                    : pusch_depth3_kind::classic),
    depth3_models{.fallback                   = config.pusch_receiver_model_path,
                  .fallback_is_build_default  = config.pusch_receiver_model_is_build_default,
                  .slots                      = config.pusch_receiver_model_slots},
    reporting_source(config.pusch_reporting_source),
    csi_sinr_calc_method(config.csi_sinr_calc_method)
  {
    if (depth3_kind == pusch_depth3_kind::ai_identity && depth3_models.fallback.empty()) {
      // The arm was SELECTED but has nothing to compute with. Said here rather than at startup because only the
      // arm knows whether the estimator accepted a binding; a run that reports "ai" while the classical chain
      // executes is the failure mode this workstream's discipline exists to catch.
      fmt::print(stderr,
                 "[pusch_receiver] backend=ai WITHOUT a model: the AI depth-3 arm is constructed but has no "
                 "model to run, so it forwards to the classical estimator and demodulator and its output is NOT "
                 "an AI result. Pass --expert_phy.pusch_receiver_model <model.mlmodelc>.\n");
    }
    ocudu_assert(estimator_factory, "Invalid channel estimation factory.");
    ocudu_assert(demodulator_factory, "Invalid demodulation factory.");
    ocudu_assert(demux_factory, "Invalid demux factory.");
    ocudu_assert(decoder_factory, "Invalid decoder factory.");
    ocudu_assert(uci_dec_factory, "Invalid UCI decoder factory.");

    // Create common dependencies.
    std::vector<std::unique_ptr<pusch_processor_impl::concurrent_dependencies>> dependencies(
        config.max_nof_concurrent_threads);
    std::generate(dependencies.begin(), dependencies.end(), [this]() {
      // The DEPTH-3 unit is built as ONE object: channel estimation + equalization + demapping, so that the
      // arm is chosen at the granularity of the whole unit rather than per module (see pusch_depth3_receiver.h).
      //
      // OWNERSHIP: the dependencies own the two units and the arm merely REFERENCES them. That is what makes
      // the strict policy ask about the very modules the unit runs -- an arm owning its own copies would leave
      // the policy interrogating instances nothing ever executes, silently.
      auto dependencies = std::make_unique<pusch_processor_impl::concurrent_dependencies>(
          estimator_factory->create(), demodulator_factory->create(), demux_factory->create(),
          uci_dec_factory->create(), ch_estimate_dimensions);

      dependencies->set_depth3(create_pusch_depth3_receiver(depth3_kind, dependencies->get_estimator(),
                                                            dependencies->get_demodulator(), depth3_models,
                                                            reporting_source));
      return dependencies;
    });

    // The binding's OUTCOME, reported once for the whole pool. The arm decides, the estimator refines, and this
    // line is what tells an operator which of the two actually happened: "ai" on the command line is a request,
    // not a fact, and a run must never be read as an AI result on the strength of the request alone.
    if (depth3_kind != pusch_depth3_kind::classic) {
      const bool           bound  = dependencies.front()->depth3_refinement_bound();
      pusch_depth3_receiver& arm   = dependencies.front()->get_depth3();
      // The binding notes come from the arm (one per model: kept, redundant, refused), and are printed ONCE for
      // the pool: the pool builds one arm per concurrent PUSCH thread, so printing them per arm would repeat the
      // same seven lines and hide the one that matters.
      for (const std::string& note : arm.binding_notes()) {
        fmt::print(stderr, "[pusch_receiver] AI depth-3: {}\n", note);
      }
      if (bound && depth3_kind == pusch_depth3_kind::ai_network) {
        // THE COVERAGE, printed from what the arm BOUND -- not from the command line. Two reasons, both measured
        // on 2026-10-16: a model's name is not a declaration (the default was called `d3_mixed` and carried
        // 256QAM alone, so a minimal `gpu + ai` leg computed 80% of its receptions and degraded 20% silently),
        // and the fallback is filled by the build's default when the knob is absent, so which modulations a run
        // computes cannot be read off the command line at all.
        fmt::print(stderr,
                   "[pusch_receiver] backend=ai: AI ARM ACTIVE over {} model(s). Each bound model COMPUTES "
                   "channel estimation, equalisation and demapping for the modulation it declares; the classical "
                   "demodulator does not run for those receptions. The chain's measurements come from {}; a "
                   "reception of any other modulation degrades to the classical chain, is counted, and says so.\n",
                   arm.bound_models().size(),
                   reporting_source == "net"
                       ? "the network's reporting heads (--pusch_reporting_source net); their domain is gated by "
                         "the bundle's own declaration"
                       : "the classical estimator, except the post-equalisation noise, which the arm measures "
                         "itself in either mode (--pusch_reporting_source classic, the default)");
        // THE GEOMETRY, likewise from the bundle: the plane width it was exported for. A model in a cell it was not
        // trained for is the failure this line makes visible at startup instead of per reception (19.162).
        if (arm.bound_nsc() != 0) {
          fmt::print(stderr, "[pusch_receiver]   geometry: {} subcarrier(s) -- what the bound model was exported for\n",
                     arm.bound_nsc());
        }
        // THE CONTRACT, printed from the bundle's own declaration rather than from the command line. It is the
        // only way to see the declaration in the artifact that actually loaded: a conditioned bundle and a
        // per-modulation 256QAM bundle have the same 112-channel main head and the same file name shape, and
        // differ in nothing else a log could show. A conditioned bundle fed a twelve-channel plane, or read with
        // a per-modulation stride, does not fail loudly -- it computes the wrong thing.
        if (arm.is_modulation_conditioned()) {
          std::string declared;
          for (const pusch_depth3_bound_model& model : arm.bound_models()) {
            declared += fmt::format("{}{}", declared.empty() ? "" : ",", model.qm);
          }
          fmt::print(stderr,
                     "[pusch_receiver]   contract=modcond: ONE bundle computes Qm={{{}}} (plane {} channels = {} + "
                     "the one-hot ones, LLR head read with a stride); the bundle declares this itself.\n",
                     declared, arm.bound_plane_channels(), pusch_depth3_input_plane::NOF_CHANNELS);
        }
        for (const pusch_depth3_bound_model& model : arm.bound_models()) {
          fmt::print(stderr, "[pusch_receiver]   computes Qm={} {:<6s} <- '{}' [{}]\n", model.qm,
                     depth3_model_map::name(model.qm), model.path,
                     model.fallback ? (arm.fallback_is_the_build_default() ? "fallback: the build's default"
                                                                           : "fallback: given")
                                    : "slot");
          // R6: the MEASUREMENT DOMAIN, from the bundle's own declaration. A `net` run publishes numbers the
          // scheduler acts on, and the range they mean something in is not visible anywhere else -- a bundle that
          // declares none cannot be gated, and saying so here is what keeps that state from passing unread.
          if (reporting_source == "net") {
            if (model.has_snr_range) {
              fmt::print(stderr, "[pusch_receiver]     reporting heads trained on {:.2f} to {:.2f} dB: a reception "
                                 "outside that range degrades whole, counted and named\n",
                         model.snr_min_db, model.snr_max_db);
            }
            else {
              fmt::print(stderr, "[pusch_receiver]     the bundle declares NO trained SNR range "
                                 "(depth3_snr_range): the domain cannot be gated, so its measurements are "
                                 "published wherever they are computed -- measure them against known truth\n");
            }
          }
        }
        for (unsigned qm : arm.unserved_modulations()) {
          fmt::print(stderr, "[pusch_receiver]   degrades Qm={} {:<6s} -> the classical chain (no model bound)\n",
                     qm, depth3_model_map::name(qm));
        }
      } else if (bound) {
        fmt::print(stderr,
                   "[pusch_receiver] backend=ai: AI ARM IS THE IDENTITY (no model path was given). The "
                   "estimator refines its estimate and the demodulator is the classical one. This run's "
                   "depth-3 output must NOT be reported as an AI result.\n");
      } else {
        fmt::print(stderr,
                   "[pusch_receiver] backend=ai: AI ARM INACTIVE (the estimator could not bind the model(s): "
                   "fallback '{}', {} slot(s)). This run's depth-3 output is the CLASSICAL result and must NOT "
                   "be reported as an AI result.\n",
                   depth3_models.fallback,
                   [&]() {
                     unsigned n = 0;
                     for (const std::string& slot : depth3_models.slots) {
                       n += slot.empty() ? 0U : 1U;
                     }
                     return n;
                   }());
      }
    }

    // Create common dependencies pool.
    dependencies_pool = std::make_shared<pusch_processor_impl::concurrent_dependencies_pool_type>(dependencies);
  }

  std::unique_ptr<pusch_processor> create() override
  {
    pusch_processor_impl::configuration config;
    config.dependencies_pool     = dependencies_pool;
    config.decoder               = decoder_factory->create();
    config.dec_nof_iterations    = dec_nof_iterations;
    config.dec_enable_early_stop = dec_enable_early_stop;
    config.csi_sinr_calc_method  = csi_sinr_calc_method;
    config.ce_dims               = ch_estimate_dimensions;
    return std::make_unique<pusch_processor_impl>(config);
  }

  std::unique_ptr<pusch_pdu_validator> create_validator() override
  {
    return std::make_unique<pusch_processor_validator_impl>(ch_estimate_dimensions);
  }

private:
  std::shared_ptr<pusch_processor_impl::concurrent_dependencies_pool_type> dependencies_pool;
  std::shared_ptr<dmrs_pusch_estimator_factory>                            estimator_factory;
  std::shared_ptr<pusch_demodulator_factory>                               demodulator_factory;
  std::shared_ptr<ulsch_demultiplex_factory>                               demux_factory;
  std::shared_ptr<pusch_decoder_factory>                                   decoder_factory;
  std::shared_ptr<uci_decoder_factory>                                     uci_dec_factory;
  pusch_processor::channel_size                                            ch_estimate_dimensions;
  unsigned                                                                 dec_nof_iterations;
  bool                                                                     dec_enable_early_stop;
  /// Which depth-3 arm to build. Resolved from pusch_receiver_backend by the upper PHY factory, which already
  /// refuses any value it cannot honour.
  pusch_depth3_kind                                                        depth3_kind;
  /// Compiled model the ai arm binds; empty means it cannot compute, which the constructor reports.
  /// The model paths the arm is built from: the fallback and the per-modulation slots.
  pusch_depth3_model_paths                                                 depth3_models;
  /// R6's knob, carried to the arm unchanged: `classic` (the default) or `net`.
  std::string                                                             reporting_source = "classic";
  channel_state_information::sinr_type                                     csi_sinr_calc_method;
};

class pusch_processor_pool_factory : public pusch_processor_factory
{
public:
  pusch_processor_pool_factory(pusch_processor_pool_factory_config& config) :
    regular_factory(config.factory),
    uci_factory(config.uci_factory),
    nof_regular_processors(config.nof_regular_processors),
    nof_uci_processors(config.nof_uci_processors)
  {
    ocudu_assert(regular_factory, "Invalid PUSCH factory.");
    ocudu_assert(uci_factory, "Invalid PUSCH factory for UCI.");
  }

  std::unique_ptr<pusch_processor> create() override
  {
    if (nof_regular_processors <= 1) {
      return regular_factory->create();
    }

    std::vector<std::unique_ptr<pusch_processor_wrapper>> processors(nof_regular_processors);
    for (std::unique_ptr<pusch_processor_wrapper>& processor : processors) {
      processor = std::make_unique<pusch_processor_wrapper>(regular_factory->create());
    }

    if (!uci_processor_pool) {
      std::vector<std::unique_ptr<pusch_processor>> uci_processors(nof_uci_processors);
      for (std::unique_ptr<pusch_processor>& processor : uci_processors) {
        processor = uci_factory->create();
      }
      uci_processor_pool = std::make_shared<pusch_processor_pool::uci_processor_pool>(uci_processors);
    }

    return std::make_unique<pusch_processor_pool>(processors, uci_processor_pool);
  }

  std::unique_ptr<pusch_processor> create(ocudulog::basic_logger& logger) override
  {
    if (nof_regular_processors <= 1) {
      return regular_factory->create(logger);
    }

    std::vector<std::unique_ptr<pusch_processor_wrapper>> processors(nof_regular_processors);
    for (std::unique_ptr<pusch_processor_wrapper>& processor : processors) {
      processor = std::make_unique<pusch_processor_wrapper>(regular_factory->create(logger));
    }

    if (!uci_processor_pool) {
      std::vector<std::unique_ptr<pusch_processor>> uci_processors(nof_uci_processors);
      for (std::unique_ptr<pusch_processor>& processor : uci_processors) {
        processor = uci_factory->create(logger);
      }
      uci_processor_pool = std::make_shared<pusch_processor_pool::uci_processor_pool>(uci_processors);
    }

    return std::make_unique<pusch_processor_pool>(processors, uci_processor_pool);
  }

  std::unique_ptr<pusch_pdu_validator> create_validator() override { return regular_factory->create_validator(); }

private:
  std::shared_ptr<pusch_processor_factory>                  regular_factory;
  std::shared_ptr<pusch_processor_factory>                  uci_factory;
  std::shared_ptr<pusch_processor_pool::uci_processor_pool> uci_processor_pool;
  unsigned                                                  nof_regular_processors;
  unsigned                                                  nof_uci_processors;
};

class ulsch_demultiplex_factory_sw : public ulsch_demultiplex_factory
{
public:
  std::unique_ptr<ulsch_demultiplex> create() override { return std::make_unique<ulsch_demultiplex_impl>(); }
};

} // namespace

std::shared_ptr<pusch_decoder_factory> ocudu::create_pusch_decoder_empty_factory(unsigned nof_prb, unsigned nof_layers)
{
  return std::make_shared<pusch_decoder_factory_empty>(nof_prb, nof_layers);
}

std::shared_ptr<pusch_decoder_factory>
ocudu::create_pusch_decoder_factory_sw(pusch_decoder_factory_sw_configuration config)
{
  return std::make_shared<pusch_decoder_factory_generic>(std::move(config));
}

std::shared_ptr<pusch_decoder_factory>
ocudu::create_pusch_decoder_factory_hw(const pusch_decoder_factory_hw_configuration& config)
{
  return std::make_shared<pusch_decoder_factory_hw>(config);
}

std::shared_ptr<pusch_processor_factory>
ocudu::create_pusch_processor_factory_sw(pusch_processor_factory_sw_configuration& config)
{
  return std::make_shared<pusch_processor_factory_generic>(config);
}

std::shared_ptr<pusch_processor_factory> ocudu::create_pusch_processor_pool(pusch_processor_pool_factory_config& config)
{
  return std::make_shared<pusch_processor_pool_factory>(config);
}

std::unique_ptr<pusch_processor> pusch_processor_factory::create(ocudulog::basic_logger& logger)
{
  return std::make_unique<logging_pusch_processor_decorator>(logger, create());
}

std::shared_ptr<ulsch_demultiplex_factory> ocudu::create_ulsch_demultiplex_factory_sw()
{
  return std::make_shared<ulsch_demultiplex_factory_sw>();
}
