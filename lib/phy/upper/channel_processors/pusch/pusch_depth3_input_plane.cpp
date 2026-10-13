// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_input_plane.h"
#include "../../signal_processors/dmrs_helper.h"
#include "ocudu/adt/bounded_bitset.h"
#include "ocudu/phy/support/mask_types.h"
#include "ocudu/phy/upper/dmrs_mapping.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ran/dmrs/dmrs.h"
#include "ocudu/ran/resource_block.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/support/math/math_utils.h"
#include <array>
#include <cmath>

using namespace ocudu;

namespace {

/// The reference point the DM-RS sequence advances from, `dmrs_pusch_estimator_impl.h:53`. It is what
/// makes the allocation's POSITION part of the sequence: the generator consumes six reference
/// elements per resource block between k = 0 and the first allocated block, so an allocation that
/// does not start at block 0 consumes bits that begin later.
constexpr unsigned DMRS_REF_POINT_K_TO_POINT_A = 0;

/// Amplitude of one DM-RS reference symbol, `dmrs_pusch_estimator_impl.cpp:106`: the sequence is
/// QPSK-like with unit total power, so each component carries 1/sqrt(2).
constexpr float DMRS_SEQUENCE_AMPLITUDE = 0.70710678118654752440F;

} // namespace

// Qualified, not left to the using-directive: a free function DEFINED unqualified at global scope
// declares a new function there instead of defining the one `namespace ocudu` declared, and the linker
// is the only thing that notices.
pusch_depth3_dmrs_context ocudu::make_pusch_depth3_dmrs_context(const dmrs_pusch_estimator::configuration& config)
{
  pusch_depth3_dmrs_context context;
  context.slot            = config.slot;
  context.nof_tx_layers   = config.get_nof_tx_layers();
  using random_config = dmrs_pusch_estimator::pseudo_random_sequence_configuration;
  if (std::holds_alternative<random_config>(config.sequence_config)) {
    const auto& sequence_config = std::get<random_config>(config.sequence_config);
    // The estimator names it `scrambling_id`; the specification calls it n_ID and the contract's
    // sequence context keeps the specification's name.
    context.n_id   = sequence_config.scrambling_id;
    context.n_scid = sequence_config.n_scid;
  } else {
    // A low-PAPR sequence means transform precoding, which the contract does not describe.
    context.low_papr = true;
  }
  return context;
}

pusch_depth3_input_plane::pusch_depth3_input_plane() :
  prg(create_pseudo_random_generator_sw_factory()->create()), row(NOF_SUBCARRIERS)
{
}

const char* pusch_depth3_input_plane::to_string(status value)
{
  switch (value) {
    case status::ok:
      return "ok";
    case status::wrong_plane_size:
      return "the destination is not [612, 14, 12]";
    case status::unsupported_configuration:
      return "the configuration is outside the contract";
    case status::unsupported_bandwidth:
      return "the grid is narrower than the 51 PRB window";
    case status::allocation_outside_window:
      return "the allocation reaches outside the 51 PRB window";
    case status::allocation_too_narrow:
      return "the allocation is narrower than the trained envelope (43 PRB)";
    case status::no_rx_port:
      return "no receive port";
    case status::unsupported_layers:
      return "more than one layer";
    case status::no_dmrs_symbol:
      return "no DM-RS symbol";
    case status::bad_reference_amplitude:
      return "the reference amplitude is not usable";
  }
  return "unknown";
}

pusch_depth3_input_plane::status
pusch_depth3_input_plane::assemble(span<__fp16>                           plane,
                                   float&                                 reference_amplitude,
                                   const resource_grid_reader&            grid,
                                   const pusch_demodulator::configuration& config,
                                   const pusch_depth3_dmrs_context&       dmrs)
{
  if (plane.size() != NOF_SUBCARRIERS * NOF_SYMBOLS * NOF_CHANNELS) {
    return status::wrong_plane_size;
  }
  // The envelope the contract describes. Every case here is a property of the RECEPTION rather than
  // of the implementation, so each one is a degradation to count and not a programming error.
  // The contract's window is the first 612 subcarriers of the grid, which is the bandwidth part when the cell
  // IS the bandwidth part (612 of 612) and stays the same window when the chain's grid is the whole carrier
  // (the replay harness builds a 275 PRB grid for a 51 PRB capture, `ul_chain_replay.cpp:1254`). A window that
  // is not the bandwidth part -- a cell whose BWP does not start at subcarrier zero -- must NOT be assembled:
  // it would feed the network a different 51 PRBs than it was trained on, and the only sign of it would be a
  // slightly worse constellation. That case is caught below, where an allocation outside the window refuses.
  if (grid.get_nof_subc() < NOF_SUBCARRIERS) {
    return status::unsupported_bandwidth;
  }
  if (config.enable_transform_precoding || config.start_symbol_index != 0 ||
      config.nof_symbols != NOF_SYMBOLS || dmrs.low_papr) {
    return status::unsupported_configuration;
  }
  if (config.nof_tx_layers != 1 || dmrs.nof_tx_layers != 1) {
    return status::unsupported_layers;
  }
  if (config.rx_ports.empty()) {
    return status::no_rx_port;
  }
  if (config.nof_cdm_groups_without_data < 1 || config.nof_cdm_groups_without_data > 3 ||
      (config.dmrs_type != dmrs_config_type::type1 && config.dmrs_type != dmrs_config_type::type2)) {
    return status::unsupported_configuration;
  }

  const unsigned port = config.rx_ports.front();

  // Every allocated resource block must lie inside the contract's window. This is the check that makes "the
  // window is the bandwidth part" a fact rather than an assumption: an allocation that reaches past subcarrier
  // 612 belongs to a bandwidth part that does not start where the window does.
  if (config.rb_mask.find_highest() >= NOF_SUBCARRIERS / NOF_SUBCARRIERS_PER_RB) {
    return status::allocation_outside_window;
  }
  // And it must be wide enough for the model to have seen anything like it: MIN_PRB is the
  // precision envelope of section 0.2(h), and the measurement behind it is in the design document.
  if (config.rb_mask.count() < MIN_PRB) {
    return status::allocation_too_narrow;
  }

  // The DM-RS symbols, ascending, which is the order the pilot LS channels are filled in.
  std::array<unsigned, NOF_DMRS_SYMBOLS_MAX> dmrs_symbols{};
  unsigned                                   nof_dmrs_symbols = 0;
  for (unsigned symbol = 0; symbol != NOF_SYMBOLS; ++symbol) {
    if (config.dmrs_symb_pos.test(symbol)) {
      if (nof_dmrs_symbols == NOF_DMRS_SYMBOLS_MAX) {
        return status::unsupported_configuration;
      }
      dmrs_symbols[nof_dmrs_symbols++] = symbol;
    }
  }
  if (nof_dmrs_symbols == 0) {
    return status::no_dmrs_symbol;
  }

  // Pilot positions inside a resource block: THIS port's comb, not the CDM groups' union. With two
  // CDM groups without data the union covers all twelve offsets -- `get_dmrs_prb_mask` returns
  // exactly that, and the demodulator complements it to obtain the DATA resource elements of a DM-RS
  // symbol, which are none. The DM-RS sequence, on the other hand, has `get_nof_re_per_prb` values
  // per resource block, one per pilot of THIS port, and `depth3.pilot_subcarriers` reproduces the
  // same six offsets for type 1. Taking the union here would put the mask and the LS on the wrong
  // half of the comb and consume six sequence values per resource block instead of twelve.
  const re_prb_mask pilot_offsets = get_pxsch_dmrs_params(config.dmrs_type, port).re_pattern;

  // The DM-RS to data amplitude ratio, as the contract's channel 11 carries it. The estimator's own
  // `configuration::scaling` is the reciprocal, so deriving it here from the CDM group count keeps
  // the two ends from disagreeing about which direction the ratio points.
  const float scaling =
      1.0F / convert_dB_to_amplitude(get_sch_to_dmrs_ratio_dB(config.nof_cdm_groups_without_data));

  // The allocation, expanded to subcarriers. `kronecker_product` is the same expansion the classical
  // demodulator builds its resource-element mask with (`pusch_demodulator_impl.cpp:407`), including
  // the COMPLEMENT: a default `re_prb_mask` is empty, and it is `~` of it that means "every offset
  // within the block". Expanding the empty one produces an allocation of nothing, which trains
  // nothing and receives nothing while looking like a mask.
  const bounded_bitset<MAX_NOF_SUBCARRIERS> allocated =
      config.rb_mask.kronecker_product<NOF_SUBCARRIERS_PER_RB>(~re_prb_mask());

  // Every resource element of the window is read once per pass, so the values land at their own
  // subcarrier indices and no per-element read is needed.
  bounded_bitset<MAX_NOF_SUBCARRIERS> all_subcarriers(NOF_SUBCARRIERS);
  all_subcarriers.fill(0, NOF_SUBCARRIERS);

  // ---- first pass: the pilot LS of every DM-RS symbol, and the reference amplitude --------------
  std::array<std::array<cf_t, NOF_SUBCARRIERS>, NOF_DMRS_SYMBOLS_MAX> ls{};
  std::array<cf_t, NOF_SUBCARRIERS>                                   sequence{};
  const unsigned nof_sequence = config.rb_mask.count() * get_nof_re_per_prb(config.dmrs_type);
  if (nof_sequence > NOF_SUBCARRIERS) {
    return status::unsupported_configuration;
  }

  double   power_sum  = 0.0;
  unsigned nof_pilots = 0;
  for (unsigned index = 0; index != nof_dmrs_symbols; ++index) {
    const unsigned symbol = dmrs_symbols[index];
    // `get` returns the UNUSED TAIL of the destination and not the number of values it wrote, so the
    // result is not a length to check: with a mask covering the whole window the tail is empty and the
    // values sit at their own subcarrier indices, which is exactly the indexing used below.
    grid.get(row, port, symbol, 0, all_subcarriers);

    // The reference symbols of this symbol, generated the way the chain's own estimator generates
    // them: one PRG seeding per (slot, symbol, scrambling identity), and a walk over the allocation
    // that consumes six values per resource block from reference point k = 0.
    const unsigned c_init =
        ((NOF_SYMBOLS * dmrs.slot.slot_index() + symbol + 1) * (2 * dmrs.n_id + 1) * (1U << 17) +
         (2 * dmrs.n_id + (dmrs.n_scid ? 1U : 0U))) %
        (1U << 31);
    prg->init(c_init);
    dmrs_sequence_generate(span<cf_t>(sequence).first(nof_sequence),
                           *prg,
                           DMRS_SEQUENCE_AMPLITUDE,
                           DMRS_REF_POINT_K_TO_POINT_A,
                           get_nof_re_per_prb(config.dmrs_type),
                           config.rb_mask);

    unsigned sequence_index = 0;
    for (unsigned rb = 0, nof_rb = config.rb_mask.size(); rb != nof_rb; ++rb) {
      if (!config.rb_mask.test(rb)) {
        continue;
      }
      for (unsigned offset = 0; offset != NOF_SUBCARRIERS_PER_RB; ++offset) {
        if (!pilot_offsets.test(offset)) {
          continue;
        }
        const unsigned k = rb * NOF_SUBCARRIERS_PER_RB + offset;
        ls[index][k]     = row[k] / (sequence[sequence_index] * scaling);
        power_sum += std::norm(ls[index][k]);
        ++nof_pilots;
        ++sequence_index;
      }
    }
    if (sequence_index != nof_sequence) {
      return status::unsupported_configuration;
    }
  }
  if (nof_pilots == 0) {
    return status::no_dmrs_symbol;
  }

  const double r = std::sqrt(power_sum / static_cast<double>(nof_pilots));
  if (!std::isfinite(r) || r <= 0.0) {
    return status::bad_reference_amplitude;
  }
  reference_amplitude   = static_cast<float>(r);
  const float inverse_r = static_cast<float>(1.0 / r);

  // ---- second pass: the plane itself -----------------------------------------------------------
  const int dc_position = config.dc_position.has_value() ? static_cast<int>(*config.dc_position) : -1;

  for (unsigned symbol = 0; symbol != NOF_SYMBOLS; ++symbol) {
    grid.get(row, port, symbol, 0, all_subcarriers);

    // Which pilot LS slot this symbol owns, if any. The slots are the DM-RS symbols in ascending
    // order, which is the contract's rule and not a configuration field.
    int ls_index = -1;
    for (unsigned index = 0; index != nof_dmrs_symbols; ++index) {
      if (dmrs_symbols[index] == symbol) {
        ls_index = static_cast<int>(index);
      }
    }

    for (unsigned k = 0; k != NOF_SUBCARRIERS; ++k) {
      const bool is_allocated = allocated.test(k);
      const bool is_pilot = is_allocated && (ls_index >= 0) && pilot_offsets.test(k % NOF_SUBCARRIERS_PER_RB);

      __fp16* destination = plane.data() + (static_cast<size_t>(k) * NOF_SYMBOLS + symbol) * NOF_CHANNELS;
      destination[CH_GRID_RE]   = static_cast<__fp16>(row[k].real() * inverse_r);
      destination[CH_GRID_IM]   = static_cast<__fp16>(row[k].imag() * inverse_r);
      destination[CH_DMRS_MASK] = static_cast<__fp16>(is_pilot ? 1.0F : 0.0F);
      destination[CH_ALLOCATION] = static_cast<__fp16>(is_allocated ? 1.0F : 0.0F);
      destination[CH_DC] = static_cast<__fp16>((static_cast<int>(k) == dc_position) ? 1.0F : 0.0F);
      destination[CH_LS_RE] =
          static_cast<__fp16>((ls_index >= 0) ? ls[ls_index][k].real() * inverse_r : 0.0F);
      destination[CH_LS_IM] =
          static_cast<__fp16>((ls_index >= 0) ? ls[ls_index][k].imag() * inverse_r : 0.0F);
      destination[CH_NOF_TX_LAYERS] = static_cast<__fp16>(static_cast<float>(config.nof_tx_layers));
      destination[CH_NOF_CDM_GROUPS] =
          static_cast<__fp16>(static_cast<float>(config.nof_cdm_groups_without_data));
      destination[CH_DMRS_TYPE] = static_cast<__fp16>(static_cast<float>(config.dmrs_type));
      destination[CH_NOF_DMRS_SYMBOLS] = static_cast<__fp16>(static_cast<float>(nof_dmrs_symbols));
      destination[CH_SCALING]          = static_cast<__fp16>(scaling);
    }
  }
  return status::ok;
}
