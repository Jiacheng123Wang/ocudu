// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "channel_equalizer_metal_factory.h"
#include "channel_equalizer_metal.h"
#include <memory>

using namespace ocudu;

namespace {

/// Composite equalizer: Metal for the supported topologies (1..4 Tx layers x 1/2/4/8 Rx
/// ports), the CPU generic implementation otherwise. The topology is fixed per cell, so the
/// routing decision made on the first call stays valid for the instance lifetime.
class channel_equalizer_metal_or_generic : public channel_equalizer
{
public:
  channel_equalizer_metal_or_generic(channel_equalizer_algorithm_type type) :
    metal_(std::make_unique<channel_equalizer_metal>(type == channel_equalizer_algorithm_type::mmse)),
    generic_(create_channel_equalizer_generic_factory(type)->create())
  {
  }

  bool is_supported(unsigned nof_ports, unsigned nof_layers) override
  {
    // The union of both backends is the acceptance set of the CPU implementation.
    return generic_->is_supported(nof_ports, nof_layers);
  }

  void equalize(span<cf_t>                       eq_symbols,
                span<float>                      eq_noise_vars,
                const re_buffer_reader<cbf16_t>& ch_symbols,
                const ch_est_list&               ch_estimates,
                span<const float>                noise_var_estimates,
                float                            tx_scaling) override
  {
    select(ch_estimates).equalize(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling);
  }

  void submit(span<cf_t>                       eq_symbols,
              span<float>                      eq_noise_vars,
              const re_buffer_reader<cbf16_t>& ch_symbols,
              const ch_est_list&               ch_estimates,
              span<const float>                noise_var_estimates,
              float                            tx_scaling) override
  {
    // The routing decision is per call: the Metal backend defers, while the CPU one executes
    // synchronously (its submit() default is equalize()), so a mixed sequence stays correct.
    select(ch_estimates).submit(eq_symbols, eq_noise_vars, ch_symbols, ch_estimates, noise_var_estimates, tx_scaling);
  }

  void submit_group(span<const group_symbol> group) override
  {
    // Forward the group as a GROUP, split by the backend each symbol routes to: the routing is per
    // call (Metal covers the 1..4 layer topologies, the generic one the rest), so a group whose
    // symbols all route to Metal reaches channel_equalizer_metal::submit_group() as one call - which
    // is what lets it encode the whole group as a few dispatches. Falling back to the interface
    // default here (one submit() per symbol) is what silently turned the batched path into the
    // per-symbol one, so this wrapper must not do that.
    unsigned i = 0;
    while (i != group.size()) {
      channel_equalizer& backend = select(*group[i].ch_estimates);
      unsigned           n_run   = 1;
      while ((i + n_run != group.size()) && (&select(*group[i + n_run].ch_estimates) == &backend)) {
        ++n_run;
      }
      backend.submit_group(group.subspan(i, n_run));
      i += n_run;
    }
  }

  void wait() override
  {
    // Each backend waits for its own in-flight submits; the unused one is a no-op.
    metal_->wait();
    generic_->wait();
  }

  bool get_post_eq_sinr(span<float> out) override
  {
    // Only the Metal backend can reduce the equalized noise variances without a CPU pass.
    return metal_->get_post_eq_sinr(out);
  }

  bool supports_deferred_chain() const override { return metal_->supports_deferred_chain(); }

private:
  /// Returns the backend in charge of the given topology.
  channel_equalizer& select(const ch_est_list& ch_estimates)
  {
    return metal_->is_supported(ch_estimates.get_nof_rx_ports(), ch_estimates.get_nof_tx_layers())
               ? static_cast<channel_equalizer&>(*metal_)
               : static_cast<channel_equalizer&>(*generic_);
  }

  std::unique_ptr<channel_equalizer_metal> metal_;
  std::unique_ptr<channel_equalizer>     generic_;
};

class channel_equalizer_metal_factory_impl : public channel_equalizer_factory
{
public:
  explicit channel_equalizer_metal_factory_impl(channel_equalizer_algorithm_type type) : type_(type) {}

  std::unique_ptr<channel_equalizer> create() override
  {
    return std::make_unique<channel_equalizer_metal_or_generic>(type_);
  }

private:
  channel_equalizer_algorithm_type type_;
};

} // namespace

std::shared_ptr<channel_equalizer_factory>
ocudu::create_channel_equalizer_metal_factory(channel_equalizer_algorithm_type type)
{
  return std::make_shared<channel_equalizer_metal_factory_impl>(type);
}
