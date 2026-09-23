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

#include "ocudu/support/page_aligned_allocator.h"
#include "pusch_demodulator_impl.h"
#include "ocudu/adt/bf16.h"
#include "ocudu/adt/format.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_modulation/channel_modulation_factories.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_codeword_buffer.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/phy_pipeline_contract.h"
#include "ocudu/phy/phy_pipeline_mode.h"
#include "ocudu/phy/upper/equalization/channel_equalizer.h"
#include "ocudu/phy/upper/equalization/equalization_factories.h"
// The Metal back ends only exist in an Apple Silicon build (their targets are gated by the options
// below), so the tests that exercise them - and the counters they expose - are compiled in only
// there. Everything else in this file runs against the CPU back ends on every platform.
#if defined(OCUDU_METAL_EQUALIZER) && defined(OCUDU_METAL_DEMODULATION)
#include "channel_equalizer_metal.h"
#include "channel_equalizer_metal_factory.h"
#include "demodulation_mapper_metal_factory.h"
#define OCUDU_HAS_METAL_PUSCH_CHAIN 1
#endif
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
#include <utility>
#include <thread>
#include <atomic>
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
    nof_ports(nof_ports_), nof_layers(nof_layers_), nof_subc(nof_subc_), noise_var(noise_var_),
    channel(static_cast<size_t>(nof_ports_) * nof_layers_ * nof_subc_)
  {
    std::mt19937                          rgen(0x5eed1234);
    std::uniform_real_distribution<float> dist(-0.5F, 0.5F);
    for (cbf16_t& h : channel) {
      h = cbf16_t(1.0F + dist(rgen), dist(rgen));
    }
  }

  /// \brief Offers the noise variance as device-resident, with the given value.
  ///
  /// A consumer that reads the estimates off the device reads this instead of get_noise_variance(),
  /// so the host accessor must not be called at all (the counter below is the witness), and the
  /// kernel must apply the same validity predicate the host would have applied to \c value.
  void enable_device_noise_variance(float value)
  {
    device_noise_var_value[0] = value;
    device_noise_var_ready    = true;
  }

  /// Number of times the host noise variance was read (see enable_device_noise_variance()).
  unsigned get_nof_host_noise_var_reads() const { return nof_host_noise_var_reads; }

  float get_noise_variance(unsigned /*rx_port*/) const override
  {
    ++nof_host_noise_var_reads;
    return noise_var;
  }

  const float* get_device_noise_variance(unsigned /*rx_port*/) const override
  {
    return device_noise_var_ready ? &device_noise_var_value[0] : nullptr;
  }

  /// The double publishes the estimates of every symbol and the noise variance as device-resident
  /// when the test enabled them, so a consumer can read the whole pass in place - which is what the
  /// real estimator reports out of its K3/K4 buffers.
  bool device_results_cover_last_estimate() const override { return device_ready && device_noise_var_ready; }

  /// A double has no deferred work to complete.
  bool sync_device_estimates() const override { return true; }
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

  /// \brief Offers the device fast path: the same channel, gathered into the compressed per-symbol
  /// layout a device-side estimator produces (the allocation minus the DM-RS REs of a DM-RS symbol,
  /// in ascending subcarrier order, with the DC resource element erased).
  void enable_device_view(const pusch_demodulator::configuration& config)
  {
    // The demodulator slices the RE mask to the span of the allocation, so the compressed layout
    // follows that same span (the estimator derives it from the allocation too).
    const unsigned first_subc = config.rb_mask.find_lowest() * NOF_SUBCARRIERS_PER_RB;
    const unsigned nof_subc_alloc =
        (config.rb_mask.find_highest() + 1) * NOF_SUBCARRIERS_PER_RB - first_subc;
    const re_prb_mask dmrs_prb = get_dmrs_prb_mask(config.dmrs_type, config.nof_cdm_groups_without_data);

    offsets[0] = 0;
    for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
      const bool is_dmrs = config.dmrs_symb_pos.test(i_symbol);
      unsigned   count   = 0;
      for (unsigned i_subc = 0; i_subc != nof_subc_alloc; ++i_subc) {
        if (is_dmrs && dmrs_prb.test(i_subc % NOF_SUBCARRIERS_PER_RB)) {
          continue;
        }
        ++count;
      }
      offsets[i_symbol + 1] = offsets[i_symbol] + count;
    }
    total_re     = offsets[MAX_NSYMB_PER_SLOT];
    device_ready = true;
    device_buffer.assign(static_cast<size_t>(nof_ports) * nof_layers * total_re, cbf16_t());

    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      for (unsigned i_layer = 0; i_layer != nof_layers; ++i_layer) {
        for (unsigned i_symbol = 0; i_symbol != MAX_NSYMB_PER_SLOT; ++i_symbol) {
          const bool          is_dmrs = config.dmrs_symb_pos.test(i_symbol);
          span<const cbf16_t> src = span<const cbf16_t>(channel).subspan(offset(i_port, i_layer), nof_subc);
          unsigned            i   = offsets[i_symbol];
          for (unsigned i_subc = 0; i_subc != nof_subc_alloc; ++i_subc) {
            if (is_dmrs && dmrs_prb.test(i_subc % NOF_SUBCARRIERS_PER_RB)) {
              continue;
            }
            // The DC subcarrier carries no data: the producer of the device estimates erases it.
            const bool is_dc =
                config.dc_position.has_value() && (*config.dc_position == i_subc + first_subc);
            device_buffer[(static_cast<size_t>(i_port) * nof_layers + i_layer) * total_re + i] =
                is_dc ? cbf16_t() : src[i_subc];
            ++i;
          }
        }
      }
    }
  }

  /// Number of device views this double handed over (see get_device_ch_estimates()).
  unsigned get_nof_device_views() const { return nof_device_views; }

  std::optional<ch_est_device_view>
  get_device_ch_estimates(unsigned i_symbol, unsigned rx_port, unsigned tx_layer) const override
  {
    ++nof_device_queries;
    if (!device_ready || (i_symbol >= MAX_NSYMB_PER_SLOT) || (rx_port >= nof_ports) || (tx_layer >= nof_layers)) {
      return std::nullopt;
    }
    ++nof_device_views;
    ch_est_device_view view;
    view.data       = device_buffer.data() + static_cast<size_t>(rx_port) * nof_layers * total_re;
    view.offset     = offsets[i_symbol];
    view.nof_re     = offsets[i_symbol + 1] - offsets[i_symbol];
    view.total_re   = total_re;
    view.nof_layers = nof_layers;
    return view;
  }

private:
  size_t offset(unsigned port, unsigned layer) const
  {
    return (static_cast<size_t>(port) * nof_layers + layer) * nof_subc;
  }

  unsigned             nof_ports;
  unsigned             nof_layers;
  unsigned             nof_subc;
  float                noise_var;
  std::vector<cbf16_t> channel;

  /// Device view state (see enable_device_view()). The counters let a test prove that the device
  /// path was actually taken: a comparison that silently fell back to the host gather would pass
  /// without testing anything.
  mutable unsigned                      nof_device_queries = 0;
  mutable unsigned                      nof_device_views   = 0;
  bool                                  device_ready = false;
  unsigned                              total_re     = 0;
  std::array<unsigned, MAX_NSYMB_PER_SLOT + 1> offsets{};
  /// Page-aligned so that a backend really maps it instead of copying it: a Metal no-copy wrap
  /// needs a page-aligned base, and a backend that cannot map the buffer stages it through a copy -
  /// which would hide a broken binding behind identical soft bits.
  std::vector<cbf16_t, page_aligned_allocator<cbf16_t>> device_buffer;

  /// Device noise variance state (see enable_device_noise_variance()).
  mutable unsigned nof_host_noise_var_reads = 0;
  bool             device_noise_var_ready   = false;
  /// Page-aligned like the buffer the estimator really produces it in, so that a backend maps it
  /// instead of falling back to a copy (which would hide a broken binding behind equal soft bits).
  std::vector<float, page_aligned_allocator<float>> device_noise_var_value{0.0F};
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

namespace {

/// Number of equalizer dispatches that read the channel estimates where the estimator produced them,
/// and of those that gathered them into the equalizer's own staging first.
///
/// The counters live in the Metal backend: without it nothing can bind device estimates, so both are
/// zero and the comparisons below degrade to "no dispatch took the device path", which is the truth
/// on a CPU-only build.
#if defined(OCUDU_HAS_METAL_PUSCH_CHAIN)
unsigned nof_device_ch_est_dispatches()
{
  return channel_equalizer_metal::nof_device_ch_est_dispatches();
}
unsigned nof_staged_ch_est_dispatches()
{
  return channel_equalizer_metal::nof_staged_ch_est_dispatches();
}
#else
unsigned nof_device_ch_est_dispatches()
{
  return 0;
}
unsigned nof_staged_ch_est_dispatches()
{
  return 0;
}
#endif

} // namespace

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
    /// Device view queries and views handed over (0 on runs that do not offer the device path).
    unsigned                                     nof_device_queries = 0;
  };

  result_t run(bool deferred, const pusch_demodulator::configuration& config, bool device_estimates = false)
  {
    const unsigned nof_ports         = static_cast<unsigned>(config.rx_ports.size());
    const unsigned nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    // The codeword buffer must hold every soft bit of the allocation.
    const unsigned max_softbits =
        config.nof_symbols * nof_re_per_symbol * config.nof_tx_layers * get_bits_per_symbol(config.modulation);

    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, config.nof_tx_layers, nof_re_per_symbol, 0.02F);
    if (device_estimates) {
      est.enable_device_view(config);
    }
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

    result.events             = buffer.get_events();
    result.nof_softbits       = buffer.get_nof_softbits();
    result.nof_device_queries = est.get_nof_device_views();

    if (deferred) {
      // The deferred chain is gated off for transform precoding and by the debug override that
      // forces the synchronous chain: in both cases the decorators stay unused.
      const char*    force_env       = std::getenv("OCUDU_PUSCH_FORCE_SERIAL");
      const bool     force_serial    = (force_env != nullptr) && (std::strtoul(force_env, nullptr, 10) != 0);
      const unsigned expected_symbols =
          (config.enable_transform_precoding || force_serial) ? 0 : expected_data_symbols(config);
      EXPECT_EQ(eq_decorator->nof_submits, expected_symbols);
      EXPECT_EQ(demapper_decorator->nof_submits, expected_symbols);
      if (expected_symbols != 0) {
        // One wait per stage and group: the stages of a group share one command buffer and the
        // pipeline change orders them, so no intermediate equalizer wait is needed.
        const unsigned nof_groups = divide_ceil(config.nof_symbols, expected_group_size());
        EXPECT_EQ(demapper_decorator->nof_waits, nof_groups)
            << "symbols of the allocation are grouped, including the ones without data";
        EXPECT_EQ(eq_decorator->nof_waits, nof_groups);
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
/// \brief The device channel-estimate path produces exactly the soft bits of the host gather.
///
/// The estimator may build the equalizer's channel estimates on the device (see
/// ch_est_device_view); the demodulator then hands the equalizer views of that buffer instead of
/// gathering the estimates RE by RE on the CPU. Everything else in the chain is untouched, so the
/// two paths must agree to the last soft bit - including the DC resource element, which only the
/// producer erases on the device path.
TEST_F(pusch_demodulator_deferred_chain_test, device_ch_estimates_match_the_host_path)
{
  struct test_case {
    unsigned                nof_symbols;
    unsigned                nof_ports;
    unsigned                nof_cdm_groups_without_data;
    std::optional<unsigned> dc_position;
  };
  const std::array<test_case, 4> cases = {{{12, 1, 1, std::nullopt},
                                           {12, 2, 1, std::nullopt},
                                           {12, 2, 2, std::nullopt},
                                           {12, 2, 1, 7 * NOF_SUBCARRIERS_PER_RB + 3}}};
  for (const test_case& test : cases) {
    for (bool deferred : {false, true}) {
      pusch_demodulator::configuration config = make_config(modulation_scheme::QAM16,
                                                            1,
                                                            test.nof_ports,
                                                            test.nof_symbols,
                                                            test.nof_cdm_groups_without_data,
                                                            false);
      config.dc_position = test.dc_position;

      const unsigned device_before = nof_device_ch_est_dispatches();
      const unsigned staged_before = nof_staged_ch_est_dispatches();

      const result_t host = run(deferred, config, /*device_estimates=*/false);
      const result_t dev  = run(deferred, config, /*device_estimates=*/true);

      // The comparison is only meaningful if the device run really used the device views.
      ASSERT_EQ(host.nof_device_queries, 0U);
      ASSERT_GT(dev.nof_device_queries, 0U) << "the device channel-estimate path was not taken";
      // This fixture runs the CPU equalizer, so no binding happens here and the counters must stay
      // put: the zero-copy path is asserted where the Metal backend runs
      // (metal_equalizer_binds_the_estimator_device_buffer).
      ASSERT_EQ(nof_device_ch_est_dispatches(), device_before);
      ASSERT_EQ(nof_staged_ch_est_dispatches(), staged_before);
      ASSERT_EQ(host.events.size(), dev.events.size());
      ASSERT_EQ(host.nof_softbits, dev.nof_softbits);
      unsigned nof_checked = 0;
      for (unsigned i_event = 0, i_event_end = host.events.size(); i_event != i_event_end; ++i_event) {
        ASSERT_EQ(host.events[i_event].type, dev.events[i_event].type);
        ASSERT_EQ(host.events[i_event].cursor_before, dev.events[i_event].cursor_before);
        ASSERT_EQ(host.events[i_event].block.size(), dev.events[i_event].block.size());
        for (unsigned i_bit = 0, i_bit_end = host.events[i_event].block.size(); i_bit != i_bit_end; ++i_bit) {
          ASSERT_EQ(host.events[i_event].block[i_bit].to_int(), dev.events[i_event].block[i_bit].to_int())
              << "soft bit " << i_bit << " of block " << i_event << (deferred ? " (deferred)" : " (serial)")
              << " differs between the host and the device channel-estimate paths";
          ++nof_checked;
        }
      }
      ASSERT_GT(nof_checked, 0U);
    }
  }

  // The host gather walks every RE of every symbol, port and layer; the device path hands the
  // equalizer the estimator's own buffer instead. The demodulator-side saving is proportional to
  // the allocation and is measured by the OTA probe ([ul_equalization_demod]) - a local
  // micro-benchmark of this harness is dominated by machine noise, so it is not asserted here.
}

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

// ---------------------------------------------------------------------------------------------
// Metal integration: the same caller, the same Metal kernels and the same codeword path, once
// with the deferred chain and once with the synchronous one, so any difference is caused by the
// chain and not by the kernels or by the back-end numerics.
// The failing over-the-air shape is used: a partial-bandwidth allocation (17 of 25 PRB) with
// DM-RS symbols that carry no data, so the per-symbol sizes differ and the LLR destinations are
// not page aligned.
// ---------------------------------------------------------------------------------------------

namespace {

/// \brief Equalizer that forwards everything but hides the deferred chain from the caller.
class synchronous_equalizer_double : public channel_equalizer
{
public:
  explicit synchronous_equalizer_double(std::unique_ptr<channel_equalizer> base_) : base(std::move(base_)) {}

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

  bool supports_deferred_chain() const override { return false; }

private:
  std::unique_ptr<channel_equalizer> base;
};

/// \brief Demapper that forwards everything but hides the deferred chain from the caller.
class synchronous_demapper_double : public demodulation_mapper
{
public:
  explicit synchronous_demapper_double(std::unique_ptr<demodulation_mapper> base_) : base(std::move(base_)) {}

  void demodulate_soft(span<log_likelihood_ratio> llrs,
                       span<const cf_t>           symbols,
                       span<const float>          noise_vars,
                       modulation_scheme          mod) override
  {
    base->demodulate_soft(llrs, symbols, noise_vars, mod);
  }

  bool supports_deferred_chain() const override { return false; }

private:
  std::unique_ptr<demodulation_mapper> base;
};

/// Codeword buffer that records the LLR stream and serves page-misaligned block views, like the
/// UL-SCH demultiplexer: the demodulator must go through the demapper's staging path.
class recording_codeword_buffer : public pusch_codeword_buffer
{
public:
  explicit recording_codeword_buffer(unsigned block_capacity_) : block_capacity(block_capacity_) {}

  span<log_likelihood_ratio> get_next_block_view(unsigned block_size) override
  {
    if (buffer.size() < cursor + block_size) {
      buffer.resize(cursor + block_size + 1024);
    }
    return span<log_likelihood_ratio>(buffer).subspan(cursor, std::min(block_size, block_capacity));
  }

  void on_new_block(span<const log_likelihood_ratio> new_data, const bit_buffer& /*new_scrambling_seq*/) override
  {
    llrs.insert(llrs.end(), new_data.begin(), new_data.end());
    cursor += new_data.size();
  }

  void on_end_codeword() override {}

  /// Concatenated LLR stream handed over by the demodulator.
  std::vector<log_likelihood_ratio> llrs;

private:
  unsigned                          block_capacity;
  std::vector<log_likelihood_ratio> buffer;
  unsigned                          cursor = 0;
};

} // namespace

#if defined(OCUDU_HAS_METAL_PUSCH_CHAIN)
/// The "ce device estimates" contract, BOTH arms.
///
/// The check claims the offloaded pipeline estimates on the DEVICE, and the failure it was written for is
/// S-7f-6a: a run whose device path has gone missing while the host silently covered for it. That is
/// exactly what a demodulator wired to the GENERIC (host) equalizer does - it exposes no device channel
/// estimate view, so every symbol is taken from the host copy - and this case drives that deliberately,
/// with the gpu mode published, and requires the check to go RED. Until this case existed the criterion
/// was only ever read on legs that passed it (see design document 5.9.102, pass 3).
///
/// The CONTROL runs after the arm, in the same case: the same demodulation with the device view available,
/// which must satisfy the claim. The verdict is `device > 0`, so the ARM is only meaningful while that
/// counter is still zero - which is why this case is registered as its own ctest entry (see CMakeLists.txt)
/// and why it SKIPS, loudly, when it is run inside a process where an earlier case already produced device
/// estimates.
///
/// \note Publishing the mode here cannot change what this binary runs: the only FUNCTIONAL reader of the
///       registry is lower_phy_factory.cpp:111, and this test builds no lower PHY. Every other reader is a
///       probe (the contract checks themselves).
TEST_F(pusch_demodulator_deferred_chain_test, the_device_estimate_contract_arms)
{
  phy_pipeline_mode_registry::set(phy_pipeline_mode::gpu);

  const phy_pipeline_check* ce_check = nullptr;
  for (const phy_pipeline_check& check : phy_pipeline_checks()) {
    if (std::strcmp(check.name, "ce device estimates") == 0) {
      ce_check = &check;
    }
  }
  ASSERT_NE(ce_check, nullptr) << "the 'ce device estimates' check is not registered";

  const unsigned                     nof_ports   = 2;
  const unsigned                     nof_layers  = 1;
  const unsigned                     nof_symbols = 14;
  const pusch_demodulator::configuration config =
      make_config(modulation_scheme::QPSK, nof_layers, nof_ports, nof_symbols, 2, false);

  // One demodulation of the same grant, with the fake estimator's DEVICE VIEW switched on or off. The view
  // is what the demodulator looks for (est_results::get_device_ch_estimates): with it, the claim's device
  // branch is taken; without it, the host copy is. That is the single variable this case needs, and it
  // keeps both arms on the same back end, so nothing but the claim itself can differ.
  auto run = [&](bool device_estimates) {
    const unsigned     nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, nof_layers, nof_re_per_symbol, 0.02F);
    if (device_estimates) {
      est.enable_device_view(config);
    }
    recording_codeword_buffer buffer(2048);
    notifier_double           notifier;
    std::unique_ptr<pusch_demodulator> demodulator = make_serial_demodulator();
    demodulator->demodulate(buffer, notifier, grid, est, config);
  };

  // Evaluates the check and captures the evidence it prints (the same line the operator reads), so the case
  // can tell a clean process from one whose counters an earlier case has already moved.
  auto evaluate = [&]() -> std::pair<std::optional<bool>, std::string> {
    FILE* capture = std::tmpfile();
    EXPECT_NE(capture, nullptr);
    std::fflush(stderr);
    const int saved = dup(fileno(stderr));
    EXPECT_NE(saved, -1);
    dup2(fileno(capture), fileno(stderr));
    std::optional<bool> verdict = ce_check->evaluate();
    std::fflush(stderr);
    dup2(saved, fileno(stderr));
    close(saved);
    std::rewind(capture);
    std::string text;
    char        buf[256];
    while (std::fgets(buf, sizeof(buf), capture) != nullptr) {
      text += buf;
    }
    std::fclose(capture);
    if (capture == nullptr) {
      verdict = std::nullopt;
    }
    return {verdict, text};
  };

  // ARM: no device view exists, so `device` stays untouched while `host` counts the symbol - the claim
  // broken, which the check has to report. Skipped, with the reason, in a process that already has device
  // estimates on the clock: then the verdict could not move no matter what this run does.
  run(/*device_estimates=*/false);
  auto [arm, arm_evidence] = evaluate();
  ASSERT_TRUE(arm.has_value()) << "the check answered 'not applicable' with host estimates on the clock";
  if (arm_evidence.find("0 device") == std::string::npos) {
    GTEST_SKIP() << "an earlier case in this binary already produced device estimates (" << arm_evidence
                 << "): run this case through its own ctest entry for the arm to mean anything";
  }
  EXPECT_FALSE(*arm) << "a run with NO device channel estimate must break the gpu-mode claim: " << arm_evidence;

  // CONTROL: the same demodulation with the device view available must satisfy the claim it is judged by.
  run(/*device_estimates=*/true);
  auto [control, control_evidence] = evaluate();
  ASSERT_TRUE(control.has_value()) << control_evidence;
  EXPECT_TRUE(*control) << "the device path must satisfy the claim it is judged by: " << control_evidence;
}

TEST_F(pusch_demodulator_deferred_chain_test, metal_back_ends_match_the_cpu_chain)
{
  // Exact grant shape of the over-the-air failure: 17 PRB starting at PRB 4 of a 25 PRB cell,
  // QPSK, one layer, two receive ports, 14 symbols, DM-RS in symbols 2 and 11 with two CDM groups
  // without data (so those symbols carry no PUSCH data at all).
  const unsigned allocation_first_prb = 4;
  const unsigned allocation_nof_prb   = 17;

  std::shared_ptr<channel_equalizer_factory> metal_eq_factory =
      create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::mmse);
  std::shared_ptr<demodulation_mapper_factory> metal_demod_factory = create_demodulation_mapper_metal_factory();
  ASSERT_NE(metal_eq_factory, nullptr);
  ASSERT_NE(metal_demod_factory, nullptr);
  if (!metal_eq_factory->create()->supports_deferred_chain() || !metal_demod_factory->create()->supports_deferred_chain()) {
    GTEST_SKIP() << "Metal deferred chain not available";
  }

  // Both demodulators use the very same Metal kernels: only the chain differs.
  auto make = [&](bool deferred) {
    std::unique_ptr<channel_equalizer> eq       = metal_eq_factory->create();
    std::unique_ptr<demodulation_mapper> demapper = metal_demod_factory->create();
    if (!deferred) {
      eq       = std::make_unique<synchronous_equalizer_double>(std::move(eq));
      demapper = std::make_unique<synchronous_demapper_double>(std::move(demapper));
    }
    return std::make_unique<pusch_demodulator_impl>(std::move(eq),
                                                    std::make_unique<precoder_double>(),
                                                    std::move(demapper),
                                                    evm_factory->create(),
                                                    prg_factory->create(),
                                                    max_nof_prb,
                                                    true);
  };

  // Two independent runs: CPU reference (serial chain) and Metal (deferred chain).
  const unsigned nof_ports  = 2;
  const unsigned nof_layers = 1;
  const unsigned nof_symbols = 14;

  auto run = [&](pusch_demodulator& demodulator) {
    const unsigned nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, nof_layers, nof_re_per_symbol, 0.02F);

    pusch_demodulator::configuration config        = {};
    config.rnti                                    = to_rnti(0x460e);
    config.rb_mask.resize(max_nof_prb);
    config.rb_mask.fill(allocation_first_prb, allocation_first_prb + allocation_nof_prb);
    config.modulation                              = modulation_scheme::QPSK;
    config.start_symbol_index                      = 0;
    config.nof_symbols                             = nof_symbols;
    config.dmrs_type                               = dmrs_config_type::type1;
    config.nof_cdm_groups_without_data             = 2;
    config.n_id                                    = 42;
    config.nof_tx_layers                           = nof_layers;
    config.dc_position                             = std::nullopt;
    config.enable_transform_precoding              = false;
    config.n_rapid                                 = std::nullopt;
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      config.rx_ports.push_back(static_cast<uint8_t>(i_port));
    }
    config.dmrs_symb_pos.set(2);
    config.dmrs_symb_pos.set(11);

    recording_codeword_buffer buffer(2048);
    notifier_double           notifier;
    demodulator.demodulate(buffer, notifier, grid, est, config);
    return buffer.llrs;
  };

  std::unique_ptr<pusch_demodulator> serial_demod   = make(false);
  std::unique_ptr<pusch_demodulator> deferred_demod = make(true);

  std::vector<log_likelihood_ratio> cpu_llrs   = run(*serial_demod);
  std::vector<log_likelihood_ratio> metal_llrs = run(*deferred_demod);

  ASSERT_EQ(cpu_llrs.size(), metal_llrs.size());
  ASSERT_FALSE(cpu_llrs.empty());

  unsigned nof_diff = 0;
  unsigned first    = 0;
  for (unsigned i = 0; i != cpu_llrs.size(); ++i) {
    if (cpu_llrs[i] != metal_llrs[i]) {
      if (nof_diff == 0) {
        first = i;
      }
      ++nof_diff;
    }
  }
  std::printf("[metal] partial-bandwidth PUSCH (17 of 25 PRB, 2 DM-RS symbols without data), "
              "Metal kernels, deferred vs synchronous chain: %u of %u LLRs differ\n",
              nof_diff,
              static_cast<unsigned>(cpu_llrs.size()));
  if (nof_diff != 0) {
    std::printf("[metal] first difference at %u: cpu=%d metal=%d\n",
                first,
                cpu_llrs[first].to_int(),
                metal_llrs[first].to_int());
  }
  EXPECT_EQ(nof_diff, 0U);
}

// ---------------------------------------------------------------------------------------------
// Concurrency: the uplink processor defers each PDU to an executor, so several demodulations run
// at the same time, each with its own demodulator instance from the dependencies pool. The Metal
// engines of those instances share the process-wide command queue, which is the last dimension the
// single-threaded checks above cannot cover.
// ---------------------------------------------------------------------------------------------

#endif // OCUDU_HAS_METAL_PUSCH_CHAIN

#if defined(OCUDU_HAS_METAL_PUSCH_CHAIN)
TEST_F(pusch_demodulator_deferred_chain_test, concurrent_metal_demodulations_match_the_serial_chain)
{
  std::shared_ptr<channel_equalizer_factory> metal_eq_factory =
      create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::mmse);
  std::shared_ptr<demodulation_mapper_factory> metal_demod_factory = create_demodulation_mapper_metal_factory();
  ASSERT_NE(metal_eq_factory, nullptr);
  ASSERT_NE(metal_demod_factory, nullptr);
  if (!metal_eq_factory->create()->supports_deferred_chain() || !metal_demod_factory->create()->supports_deferred_chain()) {
    GTEST_SKIP() << "Metal deferred chain not available";
  }

  // A PDU of the over-the-air failing shape, one instance per concurrent worker.
  struct worker_ctx {
    unsigned                                          index = 0;
    std::vector<log_likelihood_ratio>                 llrs;
  };

  constexpr unsigned nof_workers    = 4;
  constexpr unsigned nof_iterations = 15;
  const unsigned     allocation_first_prb = 4;
  const unsigned     allocation_nof_prb   = 17;
  const unsigned     nof_ports            = 2;
  const unsigned     nof_layers           = 1;
  const unsigned     nof_symbols          = 14;

  auto build_config = [&](unsigned nof_prb_shift) {
    pusch_demodulator::configuration config = {};
    config.rnti                             = to_rnti(0x460e);
    config.rb_mask.resize(max_nof_prb);
    config.rb_mask.fill(allocation_first_prb + nof_prb_shift, allocation_first_prb + nof_prb_shift + allocation_nof_prb);
    config.modulation                  = modulation_scheme::QPSK;
    config.start_symbol_index          = 0;
    config.nof_symbols                 = nof_symbols;
    config.dmrs_type                   = dmrs_config_type::type1;
    config.nof_cdm_groups_without_data = 2;
    config.n_id                        = 42;
    config.nof_tx_layers               = nof_layers;
    config.dc_position                 = std::nullopt;
    config.enable_transform_precoding  = false;
    config.n_rapid                     = std::nullopt;
    for (unsigned i_port = 0; i_port != nof_ports; ++i_port) {
      config.rx_ports.push_back(static_cast<uint8_t>(i_port));
    }
    config.dmrs_symb_pos.set(2);
    config.dmrs_symb_pos.set(11);
    return config;
  };

  auto run_one = [&](pusch_demodulator& demodulator, const pusch_demodulator::configuration& config) {
    const unsigned     nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, nof_layers, nof_re_per_symbol, 0.02F);
    recording_codeword_buffer buffer(2048);
    notifier_double           notifier;
    demodulator.demodulate(buffer, notifier, grid, est, config);
    return buffer.llrs;
  };

  // Reference: the same shape, synchronous chain, one PDU at a time.
  std::vector<std::vector<log_likelihood_ratio>> reference(nof_workers);
  for (unsigned w = 0; w != nof_workers; ++w) {
    auto demodulator = std::make_unique<pusch_demodulator_impl>(
        std::make_unique<synchronous_equalizer_double>(metal_eq_factory->create()),
        std::make_unique<precoder_double>(),
        std::make_unique<synchronous_demapper_double>(metal_demod_factory->create()),
        evm_factory->create(),
        prg_factory->create(),
        max_nof_prb,
        true);
    reference[w] = run_one(*demodulator, build_config(w));
  }

  // Deferred chain, all workers at the same time.
  // Create the demodulators (and therefore their Metal engines) before starting the threads, so
  // an initialization race can be told apart from a run-time one.
  std::vector<std::unique_ptr<pusch_demodulator>> demodulators;
  for (unsigned w = 0; w != nof_workers; ++w) {
    demodulators.push_back(std::make_unique<pusch_demodulator_impl>(metal_eq_factory->create(),
                                                                   std::make_unique<precoder_double>(),
                                                                   metal_demod_factory->create(),
                                                                   evm_factory->create(),
                                                                   prg_factory->create(),
                                                                   max_nof_prb,
                                                                   true));
  }

  std::atomic<unsigned> nof_diff{0};
  std::atomic<unsigned> nof_detail{0};
  std::vector<std::thread> threads;
  for (unsigned w = 0; w != nof_workers; ++w) {
    threads.emplace_back([&, w]() {
      pusch_demodulator::configuration config = build_config(w);
      for (unsigned it = 0; it != nof_iterations; ++it) {
        std::vector<log_likelihood_ratio> llrs = run_one(*demodulators[w], config);
        if (llrs != reference[w]) {
          nof_diff.fetch_add(1, std::memory_order_relaxed);
          if (nof_detail.fetch_add(1, std::memory_order_relaxed) < 3) {
            unsigned nz_ref = 0;
            unsigned nz_def = 0;
            unsigned first  = 0;
            for (unsigned i = 0; i != llrs.size(); ++i) {
              nz_ref += (reference[w][i].to_int() == 0) ? 1 : 0;
              nz_def += (llrs[i].to_int() == 0) ? 1 : 0;
              if ((llrs[i] != reference[w][i]) && (first == 0)) {
                first = i;
              }
            }
            std::printf("[metal]   worker %u iteration %u: zeros ref=%u def=%u, first diff at %u (ref=%d def=%d)\n",
                        w,
                        it,
                        nz_ref,
                        nz_def,
                        first,
                        reference[w][first].to_int(),
                        llrs[first].to_int());
          }
        }
      }
    });
  }
  for (std::thread& t : threads) {
    t.join();
  }

  std::printf("[metal] concurrent deferred demodulations (%u workers x %u iterations): %u mismatching runs\n",
              nof_workers,
              nof_iterations,
              nof_diff.load());
  EXPECT_EQ(nof_diff.load(), 0U);
}

#endif // OCUDU_HAS_METAL_PUSCH_CHAIN

#if defined(OCUDU_HAS_METAL_PUSCH_CHAIN)
TEST_F(pusch_demodulator_deferred_chain_test, metal_equalizer_binds_the_estimator_device_buffer)
{
  // The zero-copy target of the device-estimate path: with a single receive port the equalizer
  // binds the buffer the estimator produced the estimates in, so they are read where the GPU wrote
  // them and never travel through host memory. Wire it end to end - estimator double with a device
  // view, demodulator, Metal equalizer - and prove both halves: the soft bits are unchanged, and
  // the binding really happened. Both sources produce the same soft bits, so only the counters can
  // tell them apart; the multi-port case checks the other side of the guard (one buffer per port
  // cannot be described by a single dispatch, so those keep staging).
  std::shared_ptr<channel_equalizer_factory> metal_eq_factory =
      create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type::mmse);
  std::shared_ptr<demodulation_mapper_factory> metal_demod_factory = create_demodulation_mapper_metal_factory();
  ASSERT_NE(metal_eq_factory, nullptr);
  ASSERT_NE(metal_demod_factory, nullptr);
  if (!metal_eq_factory->create()->supports_deferred_chain() ||
      !metal_demod_factory->create()->supports_deferred_chain()) {
    GTEST_SKIP() << "Metal deferred chain not available";
  }

  const unsigned nof_symbols = 12;

  // \param device_noise_var  Value the estimator publishes as device-resident, or no value at all
  //                          (std::nullopt) to leave the noise variance on the host.
  auto run = [&](unsigned nof_ports, bool device_estimates, std::optional<float> device_noise_var, float host_noise_var = 0.02F) {
    pusch_demodulator::configuration config =
        make_config(modulation_scheme::QAM16, 1, nof_ports, nof_symbols, 1, false);
    const unsigned     nof_re_per_symbol = max_nof_prb * NOF_SUBCARRIERS_PER_RB;
    grid_reader_double grid(nof_ports, MAX_NSYMB_PER_SLOT, nof_re_per_symbol);
    est_results_double est(nof_ports, 1, nof_re_per_symbol, host_noise_var);
    if (device_estimates) {
      est.enable_device_view(config);
    }
    if (device_noise_var.has_value()) {
      est.enable_device_noise_variance(*device_noise_var);
    }
    recording_codeword_buffer buffer(4096);
    notifier_double           notifier;
    pusch_demodulator_impl    demodulator(metal_eq_factory->create(),
                                       std::make_unique<precoder_double>(),
                                       metal_demod_factory->create(),
                                       evm_factory->create(),
                                       prg_factory->create(),
                                       max_nof_prb,
                                       true);
    demodulator.demodulate(buffer, notifier, grid, est, config);
    return std::make_pair(buffer.llrs, est.get_nof_host_noise_var_reads());
  };

  for (unsigned nof_ports : {1U, 2U}) {
    const unsigned device_before = nof_device_ch_est_dispatches();
    const unsigned staged_before = nof_staged_ch_est_dispatches();
    const auto [host, host_nv_reads] = run(nof_ports, /*device_estimates=*/false, std::nullopt);
    const unsigned host_device   = nof_device_ch_est_dispatches() - device_before;
    const unsigned host_staged   = nof_staged_ch_est_dispatches() - staged_before;

    const unsigned device_mid = nof_device_ch_est_dispatches();
    const unsigned staged_mid = nof_staged_ch_est_dispatches();
    // The device run publishes both the estimates and the noise variance as device-resident, which
    // is the shape the estimator produces on the GPU: nothing of it may be read on the host.
    const auto [dev, dev_nv_reads] = run(nof_ports, /*device_estimates=*/true, 0.02F);
    const unsigned dev_device = nof_device_ch_est_dispatches() - device_mid;
    const unsigned dev_staged = nof_staged_ch_est_dispatches() - staged_mid;

    // The reference run gathered the estimates and read the noise variance on the host, as it must:
    // the counters are not vacuous.
    ASSERT_EQ(host_device, 0U);
    ASSERT_GT(host_staged, 0U);
    ASSERT_GT(host_nv_reads, 0U);
    if (nof_ports == 1) {
      // Reading a device-produced value on the host is what forces a synchronization, so this shape
      // must not do it - not even once.
      ASSERT_EQ(dev_nv_reads, 0U) << "the device noise variance was read on the host";
      ASSERT_GT(dev_device, 0U) << "the estimates were not bound where they were produced";
      ASSERT_EQ(dev_staged, 0U) << "the single-port case must not gather the estimates";
    } else {
      // One buffer per port is not bindable yet, so this shape reads both values on the host.
      ASSERT_GT(dev_nv_reads, 0U);
      ASSERT_EQ(dev_device, 0U) << "more than one port is one buffer per port: not bindable yet";
      ASSERT_GT(dev_staged, 0U);
    }

    ASSERT_FALSE(dev.empty());
    ASSERT_EQ(host.size(), dev.size());
    for (unsigned i = 0; i != host.size(); ++i) {
      ASSERT_EQ(host[i].to_int(), dev[i].to_int())
          << "soft bit " << i << " differs between the staged and the bound channel estimates ("
          << nof_ports << " port(s))";
    }
  }

  // An ill-formed noise variance makes the CPU path drop the port (and, with no port left, produce
  // invalid output). The kernel applies the same predicate to the device value, so both paths must
  // still agree - this is the gate of moving that predicate into the kernel.
  for (float bad_nv : {0.0F, std::numeric_limits<float>::infinity()}) {
    const auto [host_bad, host_reads] = run(1, /*device_estimates=*/true, std::nullopt, bad_nv);
    const auto [dev_bad, dev_reads]   = run(1, /*device_estimates=*/true, bad_nv, bad_nv);
    ASSERT_EQ(dev_reads, 0U);
    ASSERT_FALSE(dev_bad.empty());
    ASSERT_EQ(host_bad.size(), dev_bad.size());
    unsigned nof_diff = 0;
    for (unsigned i = 0; i != host_bad.size(); ++i) {
      nof_diff += (host_bad[i].to_int() != dev_bad[i].to_int()) ? 1U : 0U;
    }
    EXPECT_EQ(nof_diff, 0U) << "an ill-formed noise variance (value " << bad_nv
                            << ") is not handled like the host path handles it";
    ASSERT_GT(host_reads, 0U);
  }
}
#endif // OCUDU_HAS_METAL_PUSCH_CHAIN
