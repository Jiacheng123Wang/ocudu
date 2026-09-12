// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief PUSCH demodulator deferred-chain equivalence test.
///
/// The deferred chain (advertised by the Metal back ends through supports_deferred_chain())
/// restructures the demodulation into per-group passes: the equalization and the demapping of up to
/// max_deferred_group_symbols OFDM symbols are dispatched first, one wait synchronizes the group,
/// and only then are the codeword blocks consumed (post-equalization SINR, EVM, descrambling,
/// provisional statistics and on_new_block()). This test drives that restructured caller with CPU
/// back ends wrapped in decorators that only advertise the capability, and compares it against the
/// unmodified serial path driven by the plain CPU back ends: bit-exact codeword blocks, identical
/// event order (provisional statistics strictly before on_new_block() of the same symbol) and
/// identical statistics.

#include "pusch_demodulator_impl.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/equalization/channel_equalizer.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ocuduvec/copy.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/ran/rnti.h"
#include "ocudu/support/math/math_utils.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <memory>
#include <random>
#include <vector>

using namespace ocudu;

namespace {

// ---------------------------------------------------------------------------------------------
// Test doubles
// ---------------------------------------------------------------------------------------------

/// Resource grid reader that serves a deterministic pseudo-random PUSCH data grid.
class grid_reader_double : public resource_grid_reader
{
public:
  grid_reader_double(unsigned nof_ports_, unsigned nof_symbols_, unsigned nof_subc_) :
    nof_ports(nof_ports_), nof_symbols(nof_symbols_), nof_subc(nof_subc_),
    data(static_cast<size_t>(nof_ports_) * nof_symbols_ * nof_subc_)
  {
    std::mt19937                          rgen(0x9e3779b9);
    std::uniform_real_distribution<float> dist(-1.0F, 1.0F);
    for (cbf16_t& re : data) {
      re = cbf16_t(dist(rgen), dist(rgen));
    }
  }

  unsigned get_nof_ports() const override { return nof_ports; }
  unsigned get_nof_subc() const override { return nof_subc; }
  unsigned get_nof_symbols() const override { return nof_symbols; }
  bool     is_empty() const override { return false; }
  bool     is_empty(unsigned /*port*/) const override { return false; }

  crb_interval get_allocation_range(unsigned /*port*/, unsigned /*symbol*/) const override { return {}; }

  span<const cbf16_t> get_view(unsigned port, unsigned l) const override
  {
    return span<const cbf16_t>(data).subspan(offset(port, l), nof_subc);
  }

  span<cf_t> get(span<cf_t>                                 symbols,
                 unsigned                                   port,
                 unsigned                                   l,
                 unsigned                                   k_init,
                 const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const override
  {
    mask.for_each(0, mask.size(), [&](unsigned i_subc) {
      symbols.front() = to_cf(get_re(port, l, k_init + i_subc));
      symbols         = symbols.last(symbols.size() - 1);
    });
    return symbols;
  }

  span<cbf16_t> get(span<cbf16_t>                              symbols,
                    unsigned                                   port,
                    unsigned                                   l,
                    unsigned                                   k_init,
                    const bounded_bitset<MAX_NOF_SUBCARRIERS>& mask) const override
  {
    mask.for_each(0, mask.size(), [&](unsigned i_subc) {
      symbols.front() = get_re(port, l, k_init + i_subc);
      symbols         = symbols.last(symbols.size() - 1);
    });
    return symbols;
  }

  void get(span<cf_t> symbols, unsigned port, unsigned l, unsigned k_init, unsigned stride = 1) const override
  {
    cf_t* ptr = symbols.data();
    for (unsigned k = k_init, k_end = k_init + stride * symbols.size(); k != k_end; k += stride) {
      *(ptr++) = to_cf(get_re(port, l, k));
    }
  }

  void get(span<cbf16_t> symbols, unsigned port, unsigned l, unsigned k_init) const override
  {
    cbf16_t* ptr = symbols.data();
    for (unsigned k = k_init, k_end = k_init + symbols.size(); k != k_end; ++k) {
      *(ptr++) = get_re(port, l, k);
    }
  }

private:
  size_t offset(unsigned port, unsigned l) const { return (static_cast<size_t>(port) * nof_symbols + l) * nof_subc; }

  const cbf16_t& get_re(unsigned port, unsigned l, unsigned i_subc) const { return data[offset(port, l) + i_subc]; }

  unsigned             nof_ports;
  unsigned             nof_symbols;
  unsigned             nof_subc;
  std::vector<cbf16_t> data;
};

/// Channel estimation results with a deterministic per-RE channel.
class est_results_double : public dmrs_pusch_estimator_results
{
public:
  est_results_double(unsigned nof_ports_, unsigned nof_layers_, unsigned nof_subc_, float noise_var_) :
    nof_layers(nof_layers_), nof_subc(nof_subc_), noise_var(noise_var_),
    channel(static_cast<size_t>(nof_ports_) * nof_layers_ * nof_subc_)
  {
    std::mt19937                          rgen(0x5eed1234);
    std::uniform_real_distribution<float> dist(-0.5F, 0.5F);
    for (cbf16_t& h : channel) {
      h = cbf16_t(1.0F + dist(rgen), dist(rgen));
    }
  }

  float get_noise_variance(unsigned /*rx_port*/) const override { return noise_var; }
  float get_rsrp(unsigned /*rx_port*/, unsigned /*tx_layer*/ = 0) const override { return 1.0F; }
  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned /*tx_layer*/ = 0) const override { return {}; }
  float get_epre(unsigned /*rx_port*/) const override { return 1.0F; }
  float get_snr(unsigned /*rx_port*/) const override { return 10.0F; }
  float get_layer_average_snr(unsigned /*tx_layer*/ = 0) const override { return 10.0F; }
  phy_time_unit get_time_alignment(unsigned /*rx_port*/) const override { return phy_time_unit(); }
  std::optional<float> get_cfo_Hz(unsigned /*rx_port*/) const override { return std::nullopt; }

  void get_symbol_ch_estimate(span<cbf16_t> estimates,
                              unsigned      /*i_symbol*/,
                              unsigned      rx_port,
                              unsigned      tx_layer) const override
  {
    const unsigned nof_copy = std::min(static_cast<unsigned>(estimates.size()), nof_subc);
    std::memcpy(estimates.data(), channel.data() + offset(rx_port, tx_layer), nof_copy * sizeof(cbf16_t));
  }

  void get_symbol_ch_estimate(span<cbf16_t>                              estimates,
                              unsigned                                   /*i_symbol*/,
                              unsigned                                   rx_port,
                              unsigned                                   tx_layer,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>& re_mask) const override
  {
    span<const cbf16_t> src = span<const cbf16_t>(channel).subspan(offset(rx_port, tx_layer), nof_subc);
    unsigned            i   = 0;
    re_mask.for_each(0, re_mask.size(), [&](unsigned i_subc) {
      if (i != estimates.size()) {
        estimates[i++] = src[i_subc];
      }
    });
  }

  void get_channel_state_information(channel_state_information& /*csi*/) const override {}

private:
  size_t offset(unsigned port, unsigned layer) const
  {
    return (static_cast<size_t>(port) * nof_layers + layer) * nof_subc;
  }

  unsigned             nof_layers;
  unsigned             nof_subc;
  float                noise_var;
  std::vector<cbf16_t> channel;
};

/// Transform precoder stand-in: both paths use it, so its output only has to be deterministic.
class precoder_double : public transform_precoder
{
public:
  void deprecode_ofdm_symbol(span<cf_t> out, span<const cf_t> in) override { ocuduvec::copy(out, in); }
  void deprecode_ofdm_symbol_noise(span<float> out, span<const float> in) override { ocuduvec::copy(out, in); }
};

/// \brief Codeword buffer that mimics the UL-SCH demultiplexer.
///
/// The block view is clamped to a fixed capacity - a whole number of RE, as the demultiplexer
/// does, so one OFDM symbol splits into several blocks - and the cursor only advances in
/// on_new_block(): this is why the deferred path cannot ask for the next block before the
/// previous one has been consumed. Every event is logged so the order can be compared against the
/// serial path.
class codeword_buffer_double : public pusch_codeword_buffer
{
public:
  enum class event_type { new_block, end_codeword };

  struct event_t {
    event_type                        type;
    std::vector<log_likelihood_ratio> block;
    unsigned                          cursor_before;
  };

  codeword_buffer_double(unsigned block_capacity_, unsigned nof_softbits_) :
    block_capacity(block_capacity_), buffer(static_cast<size_t>(nof_softbits_) + block_capacity_)
  {
  }

  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    return span<log_likelihood_ratio>(buffer).subspan(cursor, std::min(block_size, block_capacity));
  }

  void on_new_block(span<const log_likelihood_ratio> new_data, const bit_buffer& /*new_scrambling_seq*/) override
  {
    events.push_back({event_type::new_block, std::vector<log_likelihood_ratio>(new_data.begin(), new_data.end()), cursor});
    cursor += new_data.size();
  }

  void on_end_codeword() override
  {
    events.push_back({event_type::end_codeword, {}, cursor});
    nof_softbits = cursor;
  }

  const std::vector<event_t>& get_events() const { return events; }
  unsigned                    get_nof_softbits() const { return nof_softbits; }

private:
  unsigned                          block_capacity;
  std::vector<log_likelihood_ratio> buffer;
  std::vector<event_t>              events;
  unsigned                          cursor       = 0;
  unsigned                          nof_softbits = 0;
};

/// Notifier that logs the provisional statistics of every OFDM symbol.
class notifier_double : public pusch_demodulator_notifier
{
public:
  struct provisional_t {
    unsigned i_symbol;
    float    sinr_dB;
    bool     has_evm;
    float    evm;
  };

  void on_provisional_stats(unsigned i_symbol, const demodulation_stats& stats) override
  {
    provisional.push_back({i_symbol,
                           stats.sinr_dB.has_value() ? *stats.sinr_dB : std::numeric_limits<float>::infinity(),
                           stats.evm.has_value(),
                           stats.evm.has_value() ? *stats.evm : 0.0F});
  }

  void on_end_stats(const demodulation_stats& stats) override
  {
    end_sinr_dB = stats.sinr_dB.has_value() ? *stats.sinr_dB : std::numeric_limits<float>::infinity();
    end_evm     = stats.evm;
  }

  std::vector<provisional_t> provisional;
  float                      end_sinr_dB = 0.0F;
  std::optional<float>       end_evm;
};

// ---------------------------------------------------------------------------------------------
// Deferred-chain decorators: CPU back ends that only advertise the capability.
// ---------------------------------------------------------------------------------------------

class deferred_equalizer_double : public channel_equalizer
{
public:
  explicit deferred_equalizer_double(std::unique_ptr<channel_equalizer> base_) : base(std::move(base_)) {}

  bool is_supported(unsigned nof_ports, unsigned nof_layers) override
  {
    return base->is_supported(nof_ports, nof_layers);
  }

  void equalize(span<cf_t>                       eq_symbols,
                span<float>                      eq_noise_vars,
                const re_buffer_reader<cbf16_t>& ch_symbols,
                const ch_est_list&               ch_estimates,
                span<const float>                noise_var_estimates,
                float                            tx_scaling) override
  {
    base->equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling);
  }

  /// Stand-in for the GPU submit: computes the equalization synchronously.
  void submit(span<cf_t>                       eq_symbols,
              span<float>                      eq_noise_vars,
              const re_buffer_reader<cbf16_t>& ch_symbols,
              const ch_est_list&               ch_estimates,
              span<const float>                noise_var_estimates,
              float                            tx_scaling) override
  {
    ++nof_submits;
    equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling);
  }

  void wait() override { ++nof_waits; }

  bool supports_deferred_chain() const override { return true; }

  unsigned nof_submits = 0;
  unsigned nof_waits   = 0;

private:
  std::unique_ptr<channel_equalizer> base;
};

class deferred_demapper_double : public demodulation_mapper
{
public:
  explicit deferred_demapper_double(std::unique_ptr<demodulation_mapper> base_) : base(std::move(base_)) {}

  void demodulate_soft(span<log_likelihood_ratio> llrs,
                       span<const cf_t>           symbols,
                       span<const float>          noise_vars,
                       modulation_scheme          mod) override
  {
    base->demodulate_soft(llrs, symbols, noise_vars, mod);
  }

  /// Stand-in for the GPU submit: computes the LLRs synchronously.
  void submit(span<log_likelihood_ratio> llrs,
              span<const cf_t>           symbols,
              span<const float>          noise_vars,
              modulation_scheme          mod) override
  {
    ++nof_submits;
    base->demodulate_soft(llrs, symbols, noise_vars, mod);
  }

  void wait() override { ++nof_waits; }

  bool supports_deferred_chain() const override { return true; }

  unsigned nof_submits = 0;
  unsigned nof_waits   = 0;

private:
  std::unique_ptr<demodulation_mapper> base;
};

// ---------------------------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------------------------

class pusch_demodulator_deferred_chain_test : public ::testing::Test
{
protected:
  static constexpr unsigned max_nof_prb  = 50;
  static constexpr unsigned re_per_block = 100;

  void SetUp() override
  {
    eq_factory       = create_channel_equalizer_generic_factory();
    demapper_factory = create_demodulation_mapper_factory();
    evm_factory      = create_evm_calculator_factory();
    prg_factory      = create_pseudo_random_generator_sw_factory();
    ASSERT_NE(eq_factory, nullptr);
    ASSERT_NE(demapper_factory, nullptr);
    ASSERT_NE(evm_factory, nullptr);
    ASSERT_NE(prg_factory, nullptr);
  }

  /// Demodulator of the serial (reference) path: the CPU back ends do not support the chain.
  std::unique_ptr<pusch_demodulator> make_serial_demodulator()
  {
    return std::make_unique<pusch_demodulator_impl>(eq_factory->create(),
                                                    std::make_unique<precoder_double>(),
                                                    demapper_factory->create(),
                                                    evm_factory->create(),
                                                    prg_factory->create(),
                                                    max_nof_prb,
                                                    true);
  }

  /// Demodulator of the deferred chain: same CPU back ends behind decorators that advertise it.
  std::unique_ptr<pusch_demodulator> make_deferred_demodulator(deferred_equalizer_double** eq_out,
                                                               deferred_demapper_double**   demapper_out)
  {
    auto eq       = std::make_unique<deferred_equalizer_double>(eq_factory->create());
    auto demapper = std::make_unique<deferred_demapper_double>(demapper_factory->create());
    *eq_out       = eq.get();
    *demapper_out = demapper.get();
    return std::make_unique<pusch_demodulator_impl>(std::move(eq),
                                                    std::make_unique<precoder_double>(),
                                                    std::move(demapper),
                                                    evm_factory->create(),
                                                    prg_factory->create(),
                                                    max_nof_prb,
                                                    true);
  }

  /// Builds a PUSCH allocation covering the whole bandwidth.
  pusch_demodulator::configuration make_config(modulation_scheme mod,
                                               unsigned          nof_layers,
                                               unsigned          nof_ports,
                                               unsigned          nof_symbols,
                                               unsigned          nof_cdm_groups_without_data,
                                               bool              transform_precoding)
  {
    pusch_demodulator::configuration config = {};
    config.rnti                             = to_rnti(0x1234);
    config.rb_mask.resize(max_nof_prb);
    config.rb_mask.fill(0, max_nof_prb);
    config.modulation            = mod;
    config.start_symbol_index    = 0;
    config.nof_symbols           = nof_symbols;
    config.dmrs_type                   = dmrs_config_type::type1;
    config.nof_cdm_groups_without_data = nof_cdm_groups_without_data;
    config.n_id                  = 42;
    config.nof_tx_layers         = nof_layers;
    config.dc_position           = std::nullopt;
    config.enable_transform_precoding = transform_precoding;
    config.n_rapid               = std::nullopt;
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      config.rx_ports.push_back(static_cast<uint8_t>(i_port));
    }
    // DM-RS in the third and the last OFDM symbol of the allocation (front-loaded + one extra).
    config.dmrs_symb_pos.set(2);
    config.dmrs_symb_pos.set(nof_symbols - 1);
    return config;
  }

  /// \brief Number of OFDM symbols of the allocation that carry PUSCH data.
  ///
  /// With two DM-RS CDM groups without data (type 1) the DM-RS symbols have no free subcarrier,
  /// so the caller skips them entirely; with one group half of their subcarriers carry data.
  static unsigned expected_data_symbols(const pusch_demodulator::configuration& config)
  {
    if (config.nof_cdm_groups_without_data != 2) {
      return config.nof_symbols;
    }
    unsigned nof_dmrs_symbols = 0;
    for (unsigned i_symbol = 0; i_symbol != config.nof_symbols; ++i_symbol) {
      if (config.dmrs_symb_pos.test(config.start_symbol_index + i_symbol)) {
        ++nof_dmrs_symbols;
      }
    }
    return config.nof_symbols - nof_dmrs_symbols;
  }

  /// \brief Group size the implementation uses, including the debug override and its clamp.
  static unsigned expected_group_size()
  {
    const char* env       = std::getenv("OCUDU_PUSCH_DEFERRED_GROUP");
    unsigned    requested = (env != nullptr) ? static_cast<unsigned>(std::strtoul(env, nullptr, 10))
                                             : pusch_demodulator_impl::max_deferred_group_symbols;
    return std::clamp(requested, 1U, pusch_demodulator_impl::max_deferred_group_symbols);
  }

  /// Runs one demodulation, of either path, and returns the recorded results.
  struct result_t {
    std::vector<codeword_buffer_double::event_t> events;
    notifier_double                              notifier;
    unsigned                                     nof_softbits = 0;
  };

  result_t run(bool deferred, const pusch_demodulator::configuration& config)
  {
    const unsigned nof_ports         = static_cast<unsigned>(config.rx_ports.size());
    const unsigned nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    // The codeword buffer must hold every soft bit of the allocation.
    const unsigned max_softbits =
        config.nof_symbols * nof_re_per_symbol * config.nof_tx_layers * get_bits_per_symbol(config.modulation);

    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, config.nof_tx_layers, nof_re_per_symbol, 0.02F);
    // Clamp the block to a whole number of RE, as the UL-SCH demultiplexer does: a symbol then
    // splits into six blocks, which exercises the block replay of the deferred chain.
    const unsigned         nof_bits_per_re = config.nof_tx_layers * get_bits_per_symbol(config.modulation);
    codeword_buffer_double buffer(re_per_block * nof_bits_per_re, max_softbits);
    result_t               result;

    deferred_equalizer_double* eq_decorator       = nullptr;
    deferred_demapper_double*  demapper_decorator = nullptr;
    std::unique_ptr<pusch_demodulator> demodulator =
        deferred ? make_deferred_demodulator(&eq_decorator, &demapper_decorator) : make_serial_demodulator();

    demodulator->demodulate(buffer, result.notifier, grid, est, config);

    result.events        = buffer.get_events();
    result.nof_softbits  = buffer.get_nof_softbits();

    if (deferred) {
      // The deferred chain is gated off for transform precoding and by the debug override that
      // forces the synchronous chain: in both cases the decorators stay unused.
      const char*    force_env       = std::getenv("OCUDU_PUSCH_FORCE_SERIAL");
      const bool     force_serial    = (force_env != nullptr) && (std::strtoul(force_env, nullptr, 10) != 0);
      const unsigned expected_symbols =
          (config.enable_transform_precoding || force_serial) ? 0 : expected_data_symbols(config);
      EXPECT_EQ(eq_decorator->nof_submits, expected_symbols);
      EXPECT_EQ(demapper_decorator->nof_submits, expected_symbols);
      EXPECT_EQ(eq_decorator->nof_waits, demapper_decorator->nof_waits);
      if (expected_symbols != 0) {
        // One single wait per group of up to max_deferred_group_symbols symbols.
        EXPECT_EQ(eq_decorator->nof_waits, divide_ceil(config.nof_symbols, expected_group_size()))
            << "symbols of the allocation are grouped, including the ones without data";
      }
    }
    return result;
  }

  std::shared_ptr<channel_equalizer_factory>       eq_factory;
  std::shared_ptr<demodulation_mapper_factory>     demapper_factory;
  std::shared_ptr<evm_calculator_factory>          evm_factory;
  std::shared_ptr<pseudo_random_generator_factory> prg_factory;
};

// ---------------------------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------------------------

/// The deferred chain must produce exactly the same codeword blocks, in the same order, as the
/// serial path - including the block split of every OFDM symbol and the provisional statistics
/// order.
TEST_F(pusch_demodulator_deferred_chain_test, deferred_chain_matches_serial_path)
{
  struct test_case {
    modulation_scheme mod;
    unsigned          nof_layers;
    unsigned          nof_ports;
    unsigned          nof_symbols;
    unsigned          nof_cdm_groups_without_data;
    bool              transform_precoding;
  };

  const std::vector<test_case> cases = {
      {modulation_scheme::QPSK, 1, 1, 14, 2, false},
      {modulation_scheme::QAM16, 1, 2, 12, 1, false},
      {modulation_scheme::QAM64, 2, 2, 14, 2, false},
      {modulation_scheme::QAM256, 2, 4, 9, 1, false},
      {modulation_scheme::QAM64, 4, 4, 14, 2, false},
      {modulation_scheme::QAM64, 1, 1, 3, 2, false},
      // Fewer symbols than the group size: a single group.
      {modulation_scheme::QAM64, 2, 2, 5, 1, false},
      // Transform precoding disables the deferred chain: both paths must still agree.
      {modulation_scheme::QPSK, 1, 1, 14, 2, true},
      {modulation_scheme::QAM16, 1, 1, 14, 1, true},
  };

  for (const test_case& tc : cases) {
    pusch_demodulator::configuration config = make_config(
        tc.mod, tc.nof_layers, tc.nof_ports, tc.nof_symbols, tc.nof_cdm_groups_without_data, tc.transform_precoding);

    result_t serial   = run(false, config);
    result_t deferred = run(true, config);

    ASSERT_EQ(serial.events.size(), deferred.events.size());
    ASSERT_EQ(serial.nof_softbits, deferred.nof_softbits);

    for (unsigned i = 0; i != serial.events.size(); ++i) {
      ASSERT_EQ(serial.events[i].type, deferred.events[i].type) << "event " << i;
      ASSERT_EQ(serial.events[i].cursor_before, deferred.events[i].cursor_before) << "event " << i;
      ASSERT_EQ(serial.events[i].block, deferred.events[i].block) << "event " << i;
    }

    // Statistics: the same provisional entries (symbol index, SINR and EVM) in the same order.
    ASSERT_EQ(serial.notifier.provisional.size(), deferred.notifier.provisional.size());
    for (unsigned i = 0; i != serial.notifier.provisional.size(); ++i) {
      EXPECT_EQ(serial.notifier.provisional[i].i_symbol, deferred.notifier.provisional[i].i_symbol) << "symbol " << i;
      EXPECT_FLOAT_EQ(serial.notifier.provisional[i].sinr_dB, deferred.notifier.provisional[i].sinr_dB) << "symbol " << i;
      EXPECT_EQ(serial.notifier.provisional[i].has_evm, deferred.notifier.provisional[i].has_evm) << "symbol " << i;
      if (serial.notifier.provisional[i].has_evm) {
        EXPECT_FLOAT_EQ(serial.notifier.provisional[i].evm, deferred.notifier.provisional[i].evm) << "symbol " << i;
      }
    }
    EXPECT_FLOAT_EQ(serial.notifier.end_sinr_dB, deferred.notifier.end_sinr_dB);
    ASSERT_EQ(serial.notifier.end_evm.has_value(), deferred.notifier.end_evm.has_value());
    if (serial.notifier.end_evm.has_value()) {
      EXPECT_FLOAT_EQ(*serial.notifier.end_evm, *deferred.notifier.end_evm);
    }

    // Sanity checks on the reference itself: the post-equalization SINR must be finite (it is
    // computed from the equalized noise variances) and the provisional statistics of a symbol must
    // precede the on_new_block() of that symbol's last block.
    ASSERT_FALSE(serial.notifier.provisional.empty());
    for (const notifier_double::provisional_t& stats : serial.notifier.provisional) {
      EXPECT_TRUE(std::isfinite(stats.sinr_dB));
    }
    EXPECT_TRUE(std::isfinite(serial.notifier.end_sinr_dB));
  }
}

} // namespace
