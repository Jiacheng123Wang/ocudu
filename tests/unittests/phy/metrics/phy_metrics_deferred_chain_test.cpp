// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Deferred-chain wiring test for the metric decorators.
///
/// The PUSCH demodulator only takes the deferred chain when both the equalizer and the demapper
/// advertise it. When PHY metrics are enabled the decorators wrap the Metal back ends, so they must
/// forward submit(), wait() and supports_deferred_chain(): otherwise the chain silently degrades to
/// the synchronous per-symbol path. Stub back ends keep this check independent of the GPU.

#include "phy_metrics_channel_equalizer_decorator.h"
#include "phy_metrics_demodulation_mapper_decorator.h"
#include "ocudu/phy/support/re_buffer.h"
#include "ocudu/phy/upper/channel_modulation/demodulation_mapper.h"
#include "ocudu/phy/upper/equalization/channel_equalizer.h"
#include "ocudu/phy/upper/equalization/modular_ch_est_list.h"
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace ocudu;

namespace {

/// Metric notifier that counts the reported equalizer metrics.
class equalizer_notifier_spy : public channel_equalizer_metric_notifier
{
public:
  void on_new_metric(const channel_equalizer_metrics& metrics) override
  {
    ++count;
    last_nof_re = metrics.nof_re;
  }

  unsigned count       = 0;
  unsigned last_nof_re = 0;
};

/// Metric notifier that counts the reported demapper metrics.
class modulation_notifier_spy : public common_channel_modulation_metric_notifier
{
public:
  void on_new_metric(const modulation_mapper_metrics& metrics) override
  {
    ++count;
    last_nof_symbols = metrics.nof_symbols;
  }

  unsigned count            = 0;
  unsigned last_nof_symbols = 0;
};

/// Equalizer stub that advertises the deferred chain and records how it was driven.
class deferred_equalizer_stub : public channel_equalizer
{
public:
  bool is_supported(unsigned /*nof_ports*/, unsigned /*nof_layers*/) override { return true; }

  void equalize(span<cf_t> /*eq_symbols*/,
                span<float> /*eq_noise_vars*/,
                const re_buffer_reader<cbf16_t>& /*ch_symbols*/,
                const ch_est_list& /*ch_estimates*/,
                span<const float> /*noise_var_estimates*/,
                float /*tx_scaling*/) override
  {
    ++nof_equalize;
  }

  void submit(span<cf_t> /*eq_symbols*/,
              span<float> /*eq_noise_vars*/,
              const re_buffer_reader<cbf16_t>& /*ch_symbols*/,
              const ch_est_list& /*ch_estimates*/,
              span<const float> /*noise_var_estimates*/,
              float /*tx_scaling*/) override
  {
    ++nof_submit;
  }

  void wait() override { ++nof_wait; }

  bool supports_deferred_chain() const override { return true; }

  unsigned nof_equalize = 0;
  unsigned nof_submit   = 0;
  unsigned nof_wait     = 0;
};

/// Demapper stub that advertises the deferred chain and records how it was driven.
class deferred_demapper_stub : public demodulation_mapper
{
public:
  void demodulate_soft(span<log_likelihood_ratio> /*llrs*/,
                       span<const cf_t> /*symbols*/,
                       span<const float> /*noise_vars*/,
                       modulation_scheme /*mod*/) override
  {
    ++nof_demodulate;
  }

  void submit(span<log_likelihood_ratio> /*llrs*/,
              span<const cf_t> /*symbols*/,
              span<const float> /*noise_vars*/,
              modulation_scheme /*mod*/) override
  {
    ++nof_submit;
  }

  void wait() override { ++nof_wait; }

  bool supports_deferred_chain() const override { return true; }

  unsigned nof_demodulate = 0;
  unsigned nof_submit     = 0;
  unsigned nof_wait       = 0;
};

} // namespace

TEST(phy_metrics_deferred_chain, equalizer_decorator_forwards_the_deferred_chain)
{
  equalizer_notifier_spy notifier;
  auto                   stub_owned = std::make_unique<deferred_equalizer_stub>();
  deferred_equalizer_stub* stub     = stub_owned.get();
  phy_metrics_channel_equalizer_decorator decorator(std::move(stub_owned), notifier);

  ASSERT_TRUE(decorator.supports_deferred_chain());

  std::vector<cf_t>                    eq(4);
  std::vector<float>                   nv(4);
  modular_re_buffer_reader<cbf16_t, 1> ch_symbols(1, 4);
  modular_ch_est_list<1>               ch_est(4, 1, 1);
  std::vector<float>                   nv_est(1, 0.1F);

  decorator.equalize(eq, nv, ch_symbols, ch_est, nv_est, 1.0F);
  EXPECT_EQ(stub->nof_equalize, 1U);
  EXPECT_EQ(notifier.count, 1U);

  decorator.submit(eq, nv, ch_symbols, ch_est, nv_est, 1.0F);
  // The deferred submit must reach the back end's submit() and still be reported as a metric.
  EXPECT_EQ(stub->nof_submit, 1U);
  EXPECT_EQ(stub->nof_equalize, 1U);
  EXPECT_EQ(notifier.count, 2U);
  EXPECT_EQ(notifier.last_nof_re, 4U);

  decorator.wait();
  EXPECT_EQ(stub->nof_wait, 1U);
}

TEST(phy_metrics_deferred_chain, demodulation_mapper_decorator_forwards_the_deferred_chain)
{
  modulation_notifier_spy notifier;
  auto                    stub_owned = std::make_unique<deferred_demapper_stub>();
  deferred_demapper_stub* stub       = stub_owned.get();
  phy_metrics_demodulation_mapper_decorator decorator(std::move(stub_owned), notifier);

  ASSERT_TRUE(decorator.supports_deferred_chain());

  std::vector<cf_t>                 symbols(4, cf_t(0.5F, -0.5F));
  std::vector<float>                noise_vars(4, 0.1F);
  std::vector<log_likelihood_ratio> llrs(16);

  decorator.demodulate_soft(llrs, symbols, noise_vars, modulation_scheme::QPSK);
  EXPECT_EQ(stub->nof_demodulate, 1U);
  EXPECT_EQ(notifier.count, 1U);

  decorator.submit(llrs, symbols, noise_vars, modulation_scheme::QPSK);
  EXPECT_EQ(stub->nof_submit, 1U);
  EXPECT_EQ(stub->nof_demodulate, 1U);
  EXPECT_EQ(notifier.count, 2U);
  EXPECT_EQ(notifier.last_nof_symbols, 4U);

  decorator.wait();
  EXPECT_EQ(stub->nof_wait, 1U);
}
