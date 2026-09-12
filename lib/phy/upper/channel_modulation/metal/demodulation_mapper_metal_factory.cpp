// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "demodulation_mapper_metal_factory.h"
#include "demodulation_mapper_metal.h"
#include <memory>

using namespace ocudu;

namespace {

/// Composite demapper: Metal for the supported schemes, the CPU generic implementation
/// otherwise (the modulation scheme is fixed per demodulate_soft call, so the per-call
/// routing decision stays valid for the whole call).
class demodulation_mapper_metal_or_generic : public demodulation_mapper
{
public:
  demodulation_mapper_metal_or_generic() :
    metal_(std::make_unique<demodulation_mapper_metal>()), generic_(create_demodulation_mapper_factory()->create())
  {
  }

  void demodulate_soft(span<log_likelihood_ratio> llrs,
                       span<const cf_t>           symbols,
                       span<const float>          noise_vars,
                       modulation_scheme          mod) override
  {
    select(mod).demodulate_soft(llrs, symbols, noise_vars, mod);
  }

  void submit(span<log_likelihood_ratio> llrs,
              span<const cf_t>           symbols,
              span<const float>          noise_vars,
              modulation_scheme          mod) override
  {
    // The routing decision is per call: the Metal backend defers, while the CPU one executes
    // synchronously (its submit() default is demodulate_soft()), so a mixed sequence stays correct.
    select(mod).submit(llrs, symbols, noise_vars, mod);
  }

  void wait() override
  {
    // Each backend waits for its own in-flight submits; the unused one is a no-op.
    metal_->wait();
    generic_->wait();
  }

  bool supports_deferred_chain() const override { return metal_->supports_deferred_chain(); }

private:
  /// Returns the backend in charge of the given modulation scheme.
  demodulation_mapper& select(modulation_scheme mod)
  {
    return metal_->is_supported(mod) ? static_cast<demodulation_mapper&>(*metal_)
                                     : static_cast<demodulation_mapper&>(*generic_);
  }

  std::unique_ptr<demodulation_mapper_metal> metal_;
  std::unique_ptr<demodulation_mapper>       generic_;
};

class demodulation_mapper_metal_factory_impl : public demodulation_mapper_factory
{
public:
  std::unique_ptr<demodulation_mapper> create() override
  {
    return std::make_unique<demodulation_mapper_metal_or_generic>();
  }
};

} // namespace

std::shared_ptr<demodulation_mapper_factory> ocudu::create_demodulation_mapper_metal_factory()
{
  return std::make_shared<demodulation_mapper_metal_factory_impl>();
}
