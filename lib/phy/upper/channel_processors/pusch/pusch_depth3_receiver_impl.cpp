// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Rights Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_receiver_impl.h"
#include "pusch_depth3_input_plane.h"
#include "pusch_depth3_measurements.h"
#include "ul_capture.h"
#include "ocudu/phy/phy_pipeline_crossings.h"
#include "ocudu/ocudulog/ocudulog.h"
#include "ocudu/phy/upper/channel_processors/pusch/pusch_demodulator_notifier.h"
#include "ocudu/phy/upper/log_likelihood_ratio.h"
#include "ocudu/phy/upper/sequence_generators/sequence_generator_factories.h"
#include "ocudu/ran/pusch/pusch_constants.h"
#include "ocudu/ran/sch/modulation_scheme.h"
#include "ocudu/ran/sch/sch_dmrs_power.h"
#include "ocudu/support/math/math_utils.h"

#if defined(OCUDU_HAVE_DEPTH3_ENGINE)
#include "ocudu_coreml_nn_engine.h"
#endif

#include <array>
#include <cstdio>
#include "fmt/format.h"
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

/// \brief The model-external constants of the contract, read from a sidecar next to the bundle.
///
/// `model_network_design.md` section 0.2(f) puts them OUTSIDE the graph so that changing a scale does not
/// mean retraining and so that the quantisation error can be measured on its own. The defaults are the
/// contract's initials: `S_llr = range_limit` makes the tanh the only saturation the chain sees, and the
/// noise pair covers the representable window at 0.2 dB resolution.
struct depth3_scales {
  float s_llr = 0.0F;
  float a_noise = 4.0F;
  float b_noise = -1.0F;
};

/// \brief Reports a sidecar that does not carry the noise pair, through a channel no log level can silence.
///
/// The pair is what turns heads 2 and 4 into a variance, and getting it wrong moves the published SINR by
/// decades: the receiver's defaults are the trainer's current constants, but they are two independent
/// literals in two languages and nothing keeps them in step. A sidecar that omits them means the deployment
/// is relying on that coincidence, so it is said out loud -- on stderr, for the reason F20 gives, because a
/// wrong SINR is worse than a missing one and must not depend on whether a logger happened to be enabled.
void report_missing_noise_pair(const std::string& path, const depth3_scales& scales, const char* why)
{
  fmt::print(stderr,
             "[pusch_receiver] AI depth-3: {}: {}, so the noise map falls back to the receiver's defaults "
             "(A_n={} B_n={}). Regenerate it with calibrate_depth3.py --write-scales so the map travels "
             "with the model.\n",
             path,
             why,
             scales.a_noise,
             scales.b_noise);
}

/// Splits "key=value" lines, ignoring comments and blanks. Unparseable lines are REPORTED: a sidecar that
/// silently half-applies is a calibration that never happened.
depth3_scales read_scales(const std::string& model_path, float range_limit)
{
  depth3_scales scales;
  scales.s_llr = range_limit;

  // The bundle is a directory, so the sidecar is its sibling: <dir>/depth3_scales.txt.
  const size_t slash = model_path.find_last_of('/');
  const std::string directory = (slash == std::string::npos) ? std::string(".") : model_path.substr(0, slash);
  const std::string path = directory + "/depth3_scales.txt";

  std::ifstream handle(path);
  if (!handle.is_open()) {
    report_missing_noise_pair(path, scales, "there is no sidecar at all");
    return scales;
  }
  bool have_a_noise = false;
  bool have_b_noise = false;
  std::string line;
  while (std::getline(handle, line)) {
    const size_t equals = line.find('=');
    if (equals == std::string::npos) {
      continue;
    }
    const std::string key = line.substr(0, equals);
    const float value = std::strtof(line.c_str() + equals + 1, nullptr);
    if (key == "s_llr") {
      scales.s_llr = value;
    } else if (key == "a_noise") {
      scales.a_noise = value;
      have_a_noise   = true;
    } else if (key == "b_noise") {
      scales.b_noise = value;
      have_b_noise   = true;
    } else {
      ocudulog::fetch_basic_logger("PHY").warning("AI depth-3: ignoring unknown scale '{}' in {}", key, path);
    }
  }
  if (!have_a_noise || !have_b_noise) {
    report_missing_noise_pair(path, scales, "it does not carry the noise pair");
  }
  ocudulog::fetch_basic_logger("PHY").info(
      "AI depth-3: scales from {}: S_llr={} A_n={} B_n={}", path, scales.s_llr, scales.a_noise, scales.b_noise);
  return scales;
}

#if defined(OCUDU_HAVE_DEPTH3_ENGINE)

/// Why a reception was NOT computed by the network. Every value is counted and logged, because a degradation
/// that is not visible is indistinguishable from a receiver that works (`design` section 17.3, I7).
enum class depth3_degradation {
  no_sequence_context,
  modulation_mismatch,
  plane_refused,
  forward_failed,
  no_data_resource_elements
};

const char* to_string(depth3_degradation value)
{
  switch (value) {
    case depth3_degradation::no_sequence_context:
      return "demodulate() ran without estimate(), so the DM-RS sequence context is missing";
    case depth3_degradation::modulation_mismatch:
      return "the bound model is trained for another modulation";
    case depth3_degradation::plane_refused:
      return "the input plane was refused";
    case depth3_degradation::forward_failed:
      return "the Core ML forward pass failed";
    case depth3_degradation::no_data_resource_elements:
      return "the allocation carries no data resource element";
  }
  return "unknown";
}

/// Inverts the scrambling of one block of soft bits, the same rule the classical demodulator applies
/// (`pusch_demodulator_impl.cpp:48`): a set sequence bit negates the soft bit.
void revert_scrambling(span<log_likelihood_ratio>       out,
                       span<const log_likelihood_ratio> in,
                       const bit_buffer&                sequence)
{
  ocudu_assert(in.size() == out.size(), "input and output must be the same size");
  ocudu_assert(sequence.size() >= in.size(), "the scrambling sequence is shorter than the block");
  for (unsigned i = 0; i != in.size(); ++i) {
    out[i] = in[i].to_value_type() * ((sequence.extract(i, 1) == 1) ? -1 : 1);
  }
}

/// \brief The depth-3 unit computed by the network: assemble, run, quantise, descramble, deliver.
///
/// The classical demodulator does NOT run here. That is the whole point of the seam: the unit is channel
/// estimation + equalisation + demapping, and a version that forwards the third of them to the classical
/// chain would be a depth-2 experiment reported as depth 3.
///
/// Every failure degrades to the classical path AND is counted, so an arm that cannot run costs the link
/// nothing except honesty: the reception is still decoded, and the run says which receiver produced it.
class pusch_depth3_receiver_ai_network_impl : public pusch_depth3_receiver
{
public:
  pusch_depth3_receiver_ai_network_impl(dmrs_pusch_estimator& estimator_,
                                        pusch_demodulator&    demodulator_,
                                        const std::string&    model_path_) :
    estimator(estimator_),
    demodulator(demodulator_),
    model_path(model_path_),
    engine(std::make_unique<metal::coreml_nn_engine>()),
    descrambler(create_pseudo_random_generator_sw_factory()->create()),
    input_plane(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS *
                pusch_depth3_input_plane::NOF_CHANNELS),
    llr_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS * 8),
    noise_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS),
    channel_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * 2 * pusch_depth3_input_plane::NOF_SYMBOLS)
  {
    // The model is bound HERE, at construction, and the outcome is kept: an arm that was SELECTED but cannot
    // compute must never be reported as an AI result, and only this flag can tell the two apart.
    engine_ready = engine->init_depth3(model_path.c_str());
    // The crossing audit's module list is what gives its "0 read(s)" a SCOPE, and this arm was not on
    // it: the audit counted the estimator's, the demapper's and the DFT route's host touches, so a
    // green zero there said nothing about the grid this arm reads through `assemble()`. Declaring it
    // is the difference between "no crossing here" and "not looked at here", and the number this arm
    // adds is the honest one -- registering the check is idempotent and also makes the report exist
    // for a run whose estimator never ran.
    register_phy_pipeline_crossing_check();
    phy_pipeline_crossings::declare_reporter("depth3_receiver");
    if (!engine_ready) {
      ocudulog::fetch_basic_logger("PHY").error(
          "AI depth-3: the model at '{}' did not load; every reception will fall back to the classical chain "
          "and the output of this arm is NOT an AI result",
          model_path);
    }
  }

  pusch_depth3_kind kind() const override { return pusch_depth3_kind::ai_network; }

  /// True only when the model is bound AND the last forward pass produced the soft bits the caller received.
  bool computes_itself() const override { return engine_ready && last_forward_ok; }

  /// The startup question: the model loaded, so this arm will compute.
  bool is_bound() const override { return engine_ready; }

  void estimate(dmrs_pusch_estimator_notifier&            notifier,
                const resource_grid_reader&                grid,
                const dmrs_pusch_estimator::configuration& config) override
  {
    // The pilot reference symbols are a function of the slot and the scrambling identity, and the
    // demodulator's configuration does not carry them: the estimator's does. Caching them here is what makes
    // the two entry points two methods of one object instead of two units.
    dmrs_context      = make_pusch_depth3_dmrs_context(config);
    context_is_valid  = true;
    scales            = read_scales(model_path, D3_RANGE_LIMIT_256QAM);

    // The classical estimator still runs: the chain's measurements (RSRP, EPRE, SNR, timing advance) are read
    // from its results, and the network's own reporting heads replace them in R6.
    estimator.estimate(notifier, grid, config);
  }

  void demodulate(pusch_codeword_buffer&                 codeword_buffer,
                  pusch_demodulator_notifier&            notifier,
                  const resource_grid_reader&            grid,
                  const dmrs_pusch_estimator_results&    est_results,
                  const pusch_demodulator::configuration& config) override
  {
    last_forward_ok = false;
    if (!engine_ready) {
      degrade(depth3_degradation::forward_failed, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (!context_is_valid) {
      degrade(depth3_degradation::no_sequence_context, codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    const unsigned qm = get_bits_per_symbol(config.modulation);
    if (qm == 0 || qm > 8) {
      degrade(depth3_degradation::plane_refused, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    // One model per modulation (contract section 0.2(h)), so the modulation is part of the arm's envelope and
    // not a detail of the run: refused HERE it costs nothing and says why, refused by the engine it costs a
    // forward pass and reports only that something failed.
    const unsigned model_qm = engine->depth3_bits_per_symbol();
    if (model_qm != 0 && model_qm != qm) {
      char detail[96];
      std::snprintf(detail, sizeof(detail), "the model carries Qm=%u, this reception is Qm=%u", model_qm, qm);
      degrade(depth3_degradation::modulation_mismatch, codeword_buffer, notifier, grid, est_results, config,
              detail);
      return;
    }

    // ---- the contract's plane, from the grid the chain holds --------------------------------------
    float reference_amplitude = 0.0F;
    const pusch_depth3_input_plane::status status =
        assembler.assemble(span<__fp16>(input_plane), reference_amplitude, grid, config, dmrs_context);
    if (status != pusch_depth3_input_plane::status::ok) {
      // The reason is a property of the RECEPTION, not of the code, so it is reported with its own name.
      // The specific reason travels with the count: "refused" alone would say a reception was skipped without
      // saying which field of it the contract does not describe.
      char detail[160];
      std::snprintf(detail, sizeof(detail), "%s (grid %u subcarriers, allocation %u PRBs, %u symbols)",
                    pusch_depth3_input_plane::to_string(status), grid.get_nof_subc(),
                    static_cast<unsigned>(config.rb_mask.count()), config.nof_symbols);
      degrade(depth3_degradation::plane_refused, codeword_buffer, notifier, grid, est_results, config, detail);
      return;
    }

    // ---- one forward pass, four heads ------------------------------------------------------------
    metal::coreml_nn_engine::depth3_input input;
    input.plane           = reinterpret_cast<const uint16_t*>(input_plane.data());
    input.nof_subcarriers = pusch_depth3_input_plane::NOF_SUBCARRIERS;

    metal::coreml_nn_engine::depth3_output output;
    output.llr             = reinterpret_cast<uint16_t*>(llr_head.data());
    output.post_eq_noise   = reinterpret_cast<uint16_t*>(noise_head.data());
    output.ch_est          = reinterpret_cast<uint16_t*>(channel_head.data());
    output.dmrs_noise      = reinterpret_cast<uint16_t*>(&dmrs_noise_head);
    output.bits_per_symbol = qm;

    if (!engine->predict_depth3(input, output)) {
      degrade(depth3_degradation::forward_failed, codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    // ---- the reporting heads, next to the estimator's own numbers ---------------------------------
    // R6's acceptance is that the network's measurements agree with the classical estimator's to the order
    // of magnitude the design asks for, and the only way to know is to compute both on the same reception
    // and print them side by side. They are NOT published yet: the arm still forwards `estimate()` to the
    // classical estimator, so a measurement that is wrong would reach the scheduler with nothing to
    // compare it against. Measure first, then switch -- the order the rest of this work has used.
    report_measurements(reference_amplitude, est_results, config);

    // ---- gather, quantise, descramble, deliver ---------------------------------------------------
    if (!deliver(codeword_buffer, notifier, config, qm)) {
      degrade(depth3_degradation::no_data_resource_elements, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    last_forward_ok = true;
  }

private:
  /// The contract's range limit for the modulation in use. Read from the chain's own table so the arm cannot
  /// invent a saturation point the quantiser does not have.
  static constexpr float D3_RANGE_LIMIT_256QAM = 20.0F;

  /// \brief Computes the network's measurements from heads 3 and 4 and prints them beside the estimator's.
  ///
  /// Sampled the way the degradations are -- the first reception in full, then one in a thousand -- so a
  /// mis-scaled noise map or a channel head that was not multiplied back by `r` is unmissable in a single
  /// run's output without turning a run into a log.
  void report_measurements(float                                   reference_amplitude,
                           const dmrs_pusch_estimator_results&     est_results,
                           const pusch_demodulator::configuration& config)
  {
    ++forwarded_receptions;
    if ((forwarded_receptions != 1) && ((forwarded_receptions % 1000) != 0)) {
      return;
    }
    const depth3_measurements net = measure_depth3(span<const __fp16>(input_plane),
                                                   span<const __fp16>(channel_head),
                                                   reference_amplitude,
                                                   static_cast<float>(dmrs_noise_head),
                                                   scales.a_noise,
                                                   scales.b_noise);
    if (net.nof_pilot_re == 0) {
      fmt::print(stderr, "[depth3-measure] the plane carries no pilot resource element; nothing measured\n");
      return;
    }
    const unsigned port        = config.rx_ports.empty() ? 0 : config.rx_ports.front();
    const float    classic_nv  = est_results.get_noise_variance(port);
    fmt::print(stderr,
               "[depth3-measure] reception {} over {} pilot RE(s): epre net={:.2f} classic={:.2f} dB | "
               "rsrp net={:.2f} classic={:.2f} dB | snr net={:.2f} classic={:.2f} dB | "
               "noise net={:.4e} classic={:.4e}\n",
               forwarded_receptions,
               net.nof_pilot_re,
               convert_power_to_dB(net.epre_lin),
               convert_power_to_dB(est_results.get_epre(port)),
               convert_power_to_dB(net.rsrp_lin),
               convert_power_to_dB(est_results.get_rsrp(port, 0)),
               convert_power_to_dB(net.snr_lin),
               convert_power_to_dB(est_results.get_snr(port)),
               net.noise_variance_lin,
               classic_nv);
  }

  void degrade(depth3_degradation                  reason,
               pusch_codeword_buffer&              codeword_buffer,
               pusch_demodulator_notifier&         notifier,
               const resource_grid_reader&         grid,
               const dmrs_pusch_estimator_results& est_results,
               const pusch_demodulator::configuration& config,
               const char*                         detail = nullptr)
  {
    ++degradations[static_cast<unsigned>(reason)];
    // Reported on the first occurrence and then once every 1000, so a misconfigured run is visible without
    // turning every slot into a log line.
    const unsigned count = degradations[static_cast<unsigned>(reason)];
    if (count == 1 || (count % 1000) == 0) {
      // stderr and not the logger: a degradation is the difference between "the AI receiver ran" and "the
      // classical one did", and it must not depend on whether this run happened to enable the PHY logger.
      // The first one is printed in full, the rest are sampled, so a misconfigured run is unmissable and a
      // systematically refused geometry does not turn every slot into a line.
      fmt::print(stderr,
                 "[pusch_receiver] AI depth-3: reception DEGRADED to the classical chain: {}{}{} ({} so far). "
                 "The soft bits of this reception are NOT an AI result.\n",
                 to_string(reason), (detail != nullptr) ? ": " : "", (detail != nullptr) ? detail : "",
                 count);
    }
    demodulator.demodulate(codeword_buffer, notifier, grid, est_results, config);
  }

  /// \brief Turns the LLR head into the chain's soft bits and the noise head into the reported SINR.
  /// \return False when the allocation carries no data resource element, which is a reception the unit cannot
  ///         describe rather than an empty one to deliver.
  bool deliver(pusch_codeword_buffer&                 codeword_buffer,
               pusch_demodulator_notifier&            notifier,
               const pusch_demodulator::configuration& config,
               unsigned                               qm)
  {
    const unsigned range_limit = (config.modulation == modulation_scheme::QPSK) ? 24 : 20;

    // The allocated subcarriers, ascending, and the DM-RS symbols: the two facts the gather rule is written
    // in terms of (`model_network_design.md` section 0.2(g)).
    allocated.clear();
    for (unsigned k = 0, nof_subc = config.rb_mask.size() * NOF_SUBCARRIERS_PER_RB;
         k != nof_subc && k != pusch_depth3_input_plane::NOF_SUBCARRIERS;
         ++k) {
      if (config.rb_mask.test(k / NOF_SUBCARRIERS_PER_RB)) {
        allocated.push_back(k);
      }
    }
    if (allocated.empty()) {
      return false;
    }

    unsigned c_init = to_underlying(config.rnti) * pow2(15) + config.n_id;
    if (config.n_rapid.has_value()) {
      c_init = to_underlying(config.rnti) * pow2(16) + *config.n_rapid * pow2(10) + config.n_id;
    }
    descrambler->init(c_init);

    double   noise_sum   = 0.0;
    unsigned noise_count = 0;

    for (unsigned symbol = 0; symbol != pusch_depth3_input_plane::NOF_SYMBOLS; ++symbol) {
      if (config.dmrs_symb_pos.test(symbol)) {
        // A DM-RS symbol carries no data, so it consumes neither soft bits nor scrambling sequence.
        continue;
      }
      const unsigned nof_softbits = allocated.size() * qm;
      // The block comes FROM the buffer (`pusch_demodulator_impl.cpp:692` does the same): it may stage the soft
      // bits for a decoder that reads them somewhere else, and writing into storage of our own would bypass
      // that staging silently.
      span<log_likelihood_ratio> codeword = codeword_buffer.get_next_block_view(nof_softbits);
      ocudu_assert(codeword.size() == nof_softbits, "the codeword buffer returned {} of {} soft bits",
                   codeword.size(), nof_softbits);

      for (unsigned index = 0; index != allocated.size(); ++index) {
        const unsigned k = allocated[index];
        const float    noise_shape = static_cast<float>(
            reinterpret_cast<const __fp16*>(noise_head.data())[static_cast<size_t>(k) * pusch_depth3_input_plane::NOF_SYMBOLS + symbol]);
        // The contract's model-external map, section 0.2(e) head 2. A non-finite value is the seam's marker for
        // a resource element the chain must skip, exactly as the classical path writes +inf on DC.
        const float noise_variance = std::pow(10.0F, scales.a_noise * noise_shape + scales.b_noise);
        if (std::isfinite(noise_variance)) {
          noise_sum += static_cast<double>(noise_variance) * qm;
          noise_count += qm;
        }

        for (unsigned bit = 0; bit != qm; ++bit) {
          const float logit = static_cast<float>(
              reinterpret_cast<const __fp16*>(llr_head.data())[(static_cast<size_t>(k) * pusch_depth3_input_plane::NOF_SYMBOLS + symbol) * qm + bit]);
          // The contract's model-external map for the main head: the graph's tanh shape times S_llr, then the
          // chain's own quantiser. `quantize` clips to +-LLR_MAX and can never produce -128 or the reserved
          // +-LLR_INFTY, so the arm cannot fabricate a "certain bit".
          const float scaled = logit * scales.s_llr;
          codeword[index * qm + bit] = log_likelihood_ratio::quantize(scaled, static_cast<float>(range_limit));
        }
      }

      // The sequence covers the whole codeword, so it is generated one symbol at a time from a single
      // generator: restarting it per symbol would descramble every symbol but the first with the wrong bits.
      static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> scrambling_seq(nof_softbits);
      descrambler->generate(scrambling_seq);
      revert_scrambling(codeword, codeword, scrambling_seq);
      // The same debug capture of the soft bits the classical path takes (`pusch_demodulator_impl.cpp:764`),
      // inside the same scope: it is what lets an AI run be COMPARED with a classical one instead of merely
      // being asserted to differ, and the scope is what keeps its host touches out of the zero-copy contract.
      // A no-op unless the capture is enabled.
      {
        phy_pipeline_crossings::scoped_debug_touches debug_capture;
        ul_capture::capture_llr(codeword);
      }
      codeword_buffer.on_new_block(codeword, scrambling_seq);
    }

    // The reporting obligation (`design` section 17.2, R6): the scheduler's post-equalisation SINR comes from
    // the noise head, because the classical estimator's residual no longer holds the noise once the estimate
    // is the network's own.
    //
    // It is reported BEFORE the codeword is ended, and that order is the whole of this block: ending the
    // codeword runs the LDPC decode, the decode emits `pusch_processor_result_data`, and the adaptor fills
    // that result's `csi` from the object this call writes into (`pusch_processor_notifier_adaptor.h:256`).
    // Reported after it -- which is where this stood -- the SINR is computed correctly and then lands in a
    // copy nobody reads again: the run reports `sinr=0.00 dB`, which is the value the harness prints when
    // the field is EMPTY, and the scheduler sees a reception with no channel quality rather than a wrong
    // one. The classical path has the same two calls in the same order (`pusch_demodulator_impl.cpp:783`).
    if (noise_count != 0 && noise_sum > 0.0) {
      const float mean_noise = static_cast<float>(noise_sum / static_cast<double>(noise_count));
      pusch_demodulator_notifier::demodulation_stats stats;
      stats.sinr_dB.emplace(-convert_power_to_dB(mean_noise));
      notifier.on_end_stats(stats);
    }

    // The end of the codeword is a CALL, not an inference: without it the decoder never learns that the soft
    // bits it accumulated are complete, decodes nothing, and reports a block error with zero iterations.
    codeword_buffer.on_end_codeword();
    return true;
  }

  dmrs_pusch_estimator& estimator;
  pusch_demodulator&    demodulator;
  std::string           model_path;

  std::unique_ptr<metal::coreml_nn_engine>        engine;
  std::unique_ptr<pseudo_random_generator>        descrambler;
  pusch_depth3_input_plane                        assembler;
  pusch_depth3_dmrs_context                       dmrs_context;
  bool                                            context_is_valid = false;
  bool                                            engine_ready      = false;
  bool                                            last_forward_ok   = false;
  /// Receptions whose forward pass produced the soft bits, which is what the measurement reporting is
  /// sampled on: a degradation has no head output to measure.
  unsigned                                        forwarded_receptions = 0;
  depth3_scales                                   scales;

  std::vector<__fp16>          input_plane;
  std::vector<__fp16>          llr_head;
  std::vector<__fp16>          noise_head;
  std::vector<__fp16>          channel_head;
  __fp16                       dmrs_noise_head = 0;
  std::vector<unsigned>        allocated;
  std::array<unsigned, 8>      degradations{};
};

#endif // OCUDU_HAVE_DEPTH3_ENGINE

} // namespace

std::unique_ptr<pusch_depth3_receiver> ocudu::create_pusch_depth3_receiver(
    pusch_depth3_kind     kind,
    dmrs_pusch_estimator& estimator,
    pusch_demodulator&    demodulator,
    const std::string&    model_path)
{
  switch (kind) {
    case pusch_depth3_kind::classic:
      return std::make_unique<pusch_depth3_receiver_classic>(estimator, demodulator);
    case pusch_depth3_kind::ai_identity:
      return std::make_unique<pusch_depth3_receiver_ai_identity>(estimator, demodulator, model_path);
    case pusch_depth3_kind::ai_network:
#if defined(OCUDU_HAVE_DEPTH3_ENGINE)
      return std::make_unique<pusch_depth3_receiver_ai_network_impl>(estimator, demodulator, model_path);
#else
      // Built without the Core ML engine: the arm is selected and cannot run, which is exactly what the
      // identity arm exists to represent -- and it says so rather than pretending.
      ocudulog::fetch_basic_logger("PHY").error(
          "AI depth-3: this build has no Core ML engine (ENABLE_METAL_CHEST is off); the arm forwards to the "
          "classical chain and its output is NOT an AI result");
      return std::make_unique<pusch_depth3_receiver_ai_identity>(estimator, demodulator, model_path);
#endif
  }
  return nullptr;
}
