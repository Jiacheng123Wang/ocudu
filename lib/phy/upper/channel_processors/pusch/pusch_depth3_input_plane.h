// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Assembles the depth-3 receiver's input plane at the seam.
///
/// The contract (`model_network_design.md` section 0.2) freezes the tensor as [612, 14, 12] fp16 with
/// the symbol axis SPATIAL, so the network's 3x3 kernels mix time as well as frequency. Everything the
/// tensor carries is reachable from the seam's own arguments -- the grid, the demodulator's
/// configuration, and the part of the DM-RS that only the estimator's configuration holds -- and
/// nothing comes from the estimator's RESULTS, so the arm's input does not depend on how the channel
/// was estimated.
///
/// A refusal is a RETURN VALUE rather than an exception or an assertion: a reception outside the
/// contract's envelope must degrade to the classical chain and be counted (I7), never be run through
/// a network that was not trained for it. The plane's layout has exactly one implementation on this
/// side of the seam; its twin is `depth3.pack_input_spatial` in the training tree, and
/// `pusch_depth3_input_plane_check` compares the two point by point.
#pragma once

#include "ocudu/adt/complex.h"
#include "ocudu/adt/span.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator.h"
#include "ocudu/phy/upper/sequence_generators/pseudo_random_generator.h"
#include "ocudu/phy/upper/signal_processors/pusch/dmrs_pusch_estimator.h"
#include "ocudu/ran/phy_time_unit.h"
#include "ocudu/ran/slot_point.h"
#include <memory>
#include <vector>

namespace ocudu {

/// \brief The part of the DM-RS the demodulator's configuration does not carry.
///
/// The pilot reference symbols are a deterministic function of the slot, the symbol and the scrambling
/// identity, and `pusch_demodulator::configuration` is silent about all three: the classical
/// demodulator never needs them because it reads the estimator's interpolated result. The AI arm does,
/// because the contract's pilot LS is measured at the pilot resource elements — which is the whole
/// point of handing the network a grid instead of an estimate.
///
/// The estimator's configuration is the only source, and it arrives through `estimate()`, so this is
/// why the two entry points have to be two methods of one object rather than two units.
struct pusch_depth3_dmrs_context {
  /// Slot the transmission was received in.
  slot_point slot;
  /// Scrambling identity, `n_ID` in TS38.211 section 6.3.1.1.
  unsigned n_id = 0;
  /// Scrambling identity selection, `n_scid`.
  bool n_scid = false;
  /// True when the estimator was configured with a low-PAPR sequence, which the contract does not
  /// describe: the arm must refuse rather than generate a Gold sequence that was never transmitted.
  bool low_papr = false;
  /// Number of transmit layers the estimator was configured for, checked against the demodulator's.
  unsigned nof_tx_layers = 1;
};

/// \brief Extracts the sequence context from the estimator's configuration.
/// \return The context, with \c low_papr set when the sequence is not the pseudo-random one.
pusch_depth3_dmrs_context make_pusch_depth3_dmrs_context(const dmrs_pusch_estimator::configuration& config);

/// \brief The contract's spatial input plane, assembled from a received grid and a configuration.
class pusch_depth3_input_plane
{
public:
  /// Contract shape, `model_network_design.md` section 0.2(a): the symbol axis is the second one.
  static constexpr unsigned NOF_SYMBOLS          = 14;
  static constexpr unsigned NOF_CHANNELS         = 12;
  static constexpr unsigned NOF_SUBCARRIERS      = 612;
  /// The pilot LS channels carry one complex value per DM-RS symbol, so the contract describes at
  /// most this many.
  static constexpr unsigned NOF_DMRS_SYMBOLS_MAX = 4;
  /// \brief Narrowest allocation the trained model describes, in resource blocks.
  ///
  /// `model_network_design.md` section 0.2(h) records the precision envelope as ">= 43 PRB, allocation
  /// not starting at PRB 0, slot > 40", and the training and validation sets honour it to the block:
  /// every sample is 43, 46, 50 or 51 PRB wide. A narrower allocation is inside the CONTRACT and
  /// outside what the network has ever seen, and the difference is measurable rather than theoretical:
  /// on the 44 decodable narrow captures of the corpus the classical chain decoded 40 and the network
  /// 8, while on the wide ones there was nothing to compare. Handing those receptions to the network
  /// costs the link decodes it would otherwise have, so they are refused and counted like any other
  /// reception outside the envelope (I7).
  static constexpr unsigned MIN_PRB = 43;

  /// Channel indices, `model_network_design.md` section 0.2(b). Named because a plane written with
  /// the wrong one is a plane that trains and receives nothing.
  enum channel_index : unsigned {
    CH_GRID_RE          = 0,
    CH_GRID_IM          = 1,
    CH_DMRS_MASK        = 2,
    CH_ALLOCATION       = 3,
    CH_DC               = 4,
    CH_LS_RE            = 5,
    CH_LS_IM            = 6,
    CH_NOF_TX_LAYERS    = 7,
    CH_NOF_CDM_GROUPS   = 8,
    CH_DMRS_TYPE        = 9,
    CH_NOF_DMRS_SYMBOLS = 10,
    CH_SCALING          = 11
  };

  /// Why an assembly was refused. Every value other than \c ok is a degradation the caller must
  /// count, which is what makes the refusal queryable rather than silent.
  enum class status {
    ok,
    /// The destination is not the contract's size.
    wrong_plane_size,
    /// A configuration the contract does not describe: transform precoding, a partial slot, a DM-RS
    /// type or CDM group count outside the table, more DM-RS symbols than the LS channels hold, or a
    /// sequence context that disagrees with the demodulator.
    unsupported_configuration,
    /// A grid narrower than the contract's 51 PRB window.
    unsupported_bandwidth,
    /// An allocation that reaches outside the contract's window, which means the bandwidth part does not
    /// start where the window does.
    allocation_outside_window,
    /// An allocation narrower than the trained model describes (\ref MIN_PRB).
    allocation_too_narrow,
    /// No receive port to read.
    no_rx_port,
    /// The contract describes one layer.
    unsupported_layers,
    /// No DM-RS symbol, so no pilot LS and no reference amplitude.
    no_dmrs_symbol,
    /// The reference amplitude came out non-finite or non-positive, which is what an all-zero grid
    /// produces; the arm must not run.
    bad_reference_amplitude
  };

  pusch_depth3_input_plane();

  /// \brief Fills \c plane with the contract's tensor and reports the reference amplitude.
  /// \param[out] plane                 `[612, 14, 12]` fp16, subcarrier major, channel contiguous.
  /// \param[out] reference_amplitude   RMS of the pilot LS, the divisor the field channels carry.
  /// \param[in]  grid                  The received resource grid.
  /// \param[in]  config                The demodulator's configuration.
  /// \param[in]  dmrs                  The sequence context the estimator's configuration carried.
  /// \return \c status::ok, or the first reason the reception is outside the contract.
  status assemble(span<__fp16>                           plane,
                  float&                                 reference_amplitude,
                  const resource_grid_reader&            grid,
                  const pusch_demodulator::configuration& config,
                  const pusch_depth3_dmrs_context&       dmrs);

  /// Human-readable reason, for the log line that accompanies a degradation count.
  static const char* to_string(status value);

private:
  /// The DM-RS reference symbols are rebuilt here rather than taken from the estimator, because the
  /// contract's pilot LS is measured at the pilot resource elements.
  std::unique_ptr<pseudo_random_generator> prg;
  /// One symbol's resource elements, reused for every read so that assembly allocates nothing.
  std::vector<cf_t>                        row;
};

} // namespace ocudu
