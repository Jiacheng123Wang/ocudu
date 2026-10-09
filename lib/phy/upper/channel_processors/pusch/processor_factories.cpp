// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
// Portions of this file may implement 3GPP specifications, which may be subject to additional licensing requirements.

#include "logging_pusch_processor_decorator.h"
#include "pusch_codeblock_decoder.h"
#include "pusch_decoder_empty_impl.h"
#include "pusch_decoder_hw_impl.h"
#include "pusch_decoder_impl.h"
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
    depth3_kind(config.pusch_receiver_backend == "ai" ? pusch_depth3_kind::ai_identity
                                                       : pusch_depth3_kind::classic),
    csi_sinr_calc_method(config.csi_sinr_calc_method)
  {
    if (depth3_kind == pusch_depth3_kind::ai_identity) {
      // Say it in as many words. Stage S-1 constructs a REAL object on its own call path, but that object
      // forwards to the classical units, so nothing downstream may report an AI result from this run.
      fmt::print(stderr,
                 "[pusch_receiver] backend=ai: STAGE S-1 IDENTITY. The AI arm is constructed and both depth-3 "
                 "entry points go through it, but it forwards to the classical estimator and demodulator, so "
                 "the output is bit-identical to backend=classic and is NOT an AI result. A real forward pass "
                 "replaces that delegation; until then this run may be used to prove the seam, never to "
                 "measure a model.\n");
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

      dependencies->set_depth3(create_pusch_depth3_receiver(
          depth3_kind, dependencies->get_estimator(), dependencies->get_demodulator()));
      return dependencies;
    });

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
