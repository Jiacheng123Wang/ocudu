// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Rights Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_receiver_impl.h"
#include "pusch_depth3_model_map.h"
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

#include <algorithm>
#include <array>
#include <chrono>
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
  pusch_depth3_receiver_ai_network_impl(dmrs_pusch_estimator&                  estimator_,
                                        pusch_demodulator&                     demodulator_,
                                        const std::string&                     fallback_path_,
                                        const std::array<std::string, 4>&      slot_paths_,
                                        bool                                   fallback_is_build_default_) :
    estimator(estimator_),
    demodulator(demodulator_),
    fallback_path(fallback_path_),
    fallback_is_build_default(fallback_is_build_default_),
    descrambler(create_pseudo_random_generator_sw_factory()->create()),
    input_plane(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS *
                pusch_depth3_input_plane::NOF_CHANNELS),
    llr_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS * 8),
    noise_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS),
    channel_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * 2 * pusch_depth3_input_plane::NOF_SYMBOLS)
  {
    // EVERY MODEL IS BOUND HERE, at construction, and every outcome is kept and printed: an arm that was
    // SELECTED but cannot compute must never be reported as an AI result, and with more than one model the
    // question becomes per-modulation -- "which modulations does this run actually compute" is answered below
    // and by the coverage line the factory prints, not inferred from the command line.
    auto bind = [this](const std::string& path, unsigned slot_qm, bool fallback) {
      auto engine = std::make_unique<metal::coreml_nn_engine>();
      const bool loaded = engine->init_depth3(path.c_str());
      const unsigned declared_qm = loaded ? engine->depth3_bits_per_symbol() : 0;

      const depth3_model_map::outcome outcome = fallback ? models.add_fallback(declared_qm, path)
                                                        : models.add_slot(slot_qm, declared_qm, path);
      const char* const what = fallback ? "fallback" : "slot";
      // The line is COLLECTED, not printed: the pool builds one arm per concurrent PUSCH thread, and the factory
      // prints these once for the pool.
      if (outcome.result != depth3_model_map::status::kept) {
        notes.push_back(fmt::format("{} model '{}' NOT bound: {}", what, path, outcome.reason));
        return;
      }
      // The calibration travels with the model (a sidecar next to the bundle), so it is read once, here, and the
      // reception picks the one of the model that computed it.
      scales_by_qm[declared_qm] = read_scales(path, D3_RANGE_LIMIT_256QAM);
      engine_by_qm[declared_qm] = std::move(engine);
      notes.push_back(fmt::format("{} model '{}' bound for Qm={} {}", what, path, declared_qm,
                                  depth3_model_map::name(declared_qm)));
      any_model_bound = true;
    };

    for (unsigned i = 0; i != slot_paths_.size(); ++i) {
      if (!slot_paths_[i].empty()) {
        // The slot's modulation is fixed by the KNOB, not by the file: QPSK, 16QAM, 64QAM, 256QAM.
        bind(slot_paths_[i], depth3_model_map::candidate_qm[i], false);
      }
    }
    if (!fallback_path.empty()) {
      bind(fallback_path, 0, true);
    }
    // The crossing audit's module list is what gives its "0 read(s)" a SCOPE, and this arm was not on
    // it: the audit counted the estimator's, the demapper's and the DFT route's host touches, so a
    // green zero there said nothing about the grid this arm reads through `assemble()`. Declaring it
    // is the difference between "no crossing here" and "not looked at here", and the number this arm
    // adds is the honest one -- registering the check is idempotent and also makes the report exist
    // for a run whose estimator never ran.
    register_phy_pipeline_crossing_check();
    phy_pipeline_crossings::declare_reporter("depth3_receiver");
    if (!any_model_bound) {
      // Nothing bound: every reception forwards to the classical chain. The notes above say why each model failed,
      // and the factory turns this state into an error for the whole pool -- a run in this state must not be read
      // as an AI result.
      notes.push_back("no model bound: every reception degrades to the classical chain");
    }
  }

  pusch_depth3_kind kind() const override { return pusch_depth3_kind::ai_network; }

  /// True only when a model is bound AND the last forward pass produced the soft bits the caller received.
  bool computes_itself() const override { return any_model_bound && last_forward_ok; }

  /// The startup question: at least one model bound, so this arm will compute for what it covers.
  bool is_bound() const override { return any_model_bound; }

  /// The first (lowest) modulation a bound model serves; 0 when none is bound. See \ref bound_models for the
  /// full list, which is what the coverage line reports.
  unsigned bound_qm() const override { return models.entries().empty() ? 0 : models.entries().front().qm; }

  /// Every model that serves at least one modulation, with its declaration and where it came from: this is what
  /// the startup coverage line is printed from, so that "which modulations does this run compute" is read and not
  /// inferred (the fallback is filled by the build's default, so the command line alone does not say).
  std::vector<pusch_depth3_bound_model> bound_models() const override
  {
    std::vector<pusch_depth3_bound_model> out;
    for (const depth3_model_map::entry& e : models.entries()) {
      out.push_back(pusch_depth3_bound_model{e.qm, e.path, e.fallback});
    }
    return out;
  }

  /// The modulations no bound model serves; what the run degrades to the classical chain.
  std::vector<unsigned> unserved_modulations() const override { return models.missing(); }

  std::vector<std::string> binding_notes() const override { return notes; }

  /// True when the fallback bound here came from the build rather than from the command line.
  bool fallback_is_the_build_default() const override { return fallback_is_build_default; }

  void estimate(dmrs_pusch_estimator_notifier&            notifier,
                const resource_grid_reader&                grid,
                const dmrs_pusch_estimator::configuration& config) override
  {
    // The pilot reference symbols are a function of the slot and the scrambling identity, and the
    // demodulator's configuration does not carry them: the estimator's does. Caching them here is what makes
    // the two entry points two methods of one object instead of two units.
    dmrs_context      = make_pusch_depth3_dmrs_context(config);
    context_is_valid  = true;

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
    if (!any_model_bound) {
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
    // One model per modulation (contract section 0.2(h)), so the modulation is the arm's envelope and not a
    // detail of the run -- and with a slot per modulation plus a fallback, WHICH model serves this reception is a
    // lookup, not a comparison. Refused HERE it costs nothing and says why (including which modulations ARE
    // served); refused by the engine it would cost a forward pass and report only that something failed.
    const depth3_model_map::entry* const model = models.find(qm);
    if (model == nullptr) {
      std::string served;
      for (const depth3_model_map::entry& e : models.entries()) {
        served += (served.empty() ? "" : ", ");
        served += std::to_string(e.qm);
      }
      char detail[224];
      std::snprintf(detail, sizeof(detail), "no model is bound for Qm=%u (%s); bound: %s", qm,
                    depth3_model_map::name(qm), served.empty() ? "none" : served.c_str());
      degrade(depth3_degradation::modulation_mismatch, codeword_buffer, notifier, grid, est_results, config,
              detail);
      return;
    }
    metal::coreml_nn_engine& computing_engine = *engine_by_qm[qm];
    const depth3_scales&     model_scales     = scales_by_qm[qm];

    // ---- the contract's plane, from the grid the chain holds --------------------------------------
    const auto segment_start = std::chrono::steady_clock::now();
    float reference_amplitude = 0.0F;
    const pusch_depth3_input_plane::status status =
        assembler.assemble(span<__fp16>(input_plane), reference_amplitude, grid, config, dmrs_context);
    const auto segment_assembled = std::chrono::steady_clock::now();
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

    if (!computing_engine.predict_depth3(input, output)) {
      degrade(depth3_degradation::forward_failed, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    const auto segment_forwarded = std::chrono::steady_clock::now();

    // ---- the reporting heads, next to the estimator's own numbers ---------------------------------
    // R6's acceptance is that the network's measurements agree with the classical estimator's to the order
    // of magnitude the design asks for, and the only way to know is to compute both on the same reception
    // and print them side by side. They are NOT published yet: the arm still forwards `estimate()` to the
    // classical estimator, so a measurement that is wrong would reach the scheduler with nothing to
    // compare it against. Measure first, then switch -- the order the rest of this work has used.
    report_measurements(reference_amplitude, est_results, config, model_scales);

    // ---- gather, quantise, descramble, deliver ---------------------------------------------------
    if (!deliver(codeword_buffer, notifier, config, qm, model_scales)) {
      degrade(depth3_degradation::no_data_resource_elements, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    record_timing(segment_start, segment_assembled, segment_forwarded, std::chrono::steady_clock::now());
    last_forward_ok = true;
  }

private:
  /// \brief The three segments of the AI path, timed apart, as design section 1.5.5 requires.
  ///
  /// That section asks for the split and says why: the lane's gain comes from one submission per hop, and the AI
  /// path inserts a new three-hop hand-over -- grid ready, then the engine has its input, then the LLR is ready,
  /// then the LDPC decoder can read it. A fast forward pass does not help if the hand-over around it is slow, and
  /// the three have different fixes, so they are measured apart rather than as one number.
  ///
  /// ADDED 2026-10-16, before the first live AI leg: the arm's cost would otherwise have shown up only as a
  /// collapse of the kind section 19.60 documents -- the UL HARQ pools exhausting under a pipeline that fell
  /// behind -- with nothing in the log to attribute it to. The anchors for the forward pass are 141 us and
  /// 690 us (memo_03 section 1.1).
  ///
  /// The sampled measurement reporting sits OUTSIDE the three: it happens once per thousand receptions and is
  /// reporting rather than path, so folding it into the delivery segment would put a spike in one sample of a
  /// thousand and nothing anywhere else.
  static constexpr unsigned TIMING_RING         = 512;
  static constexpr unsigned TIMING_REPORT_EVERY = 500;

  void record_timing(std::chrono::steady_clock::time_point start,
                     std::chrono::steady_clock::time_point assembled,
                     std::chrono::steady_clock::time_point forwarded,
                     std::chrono::steady_clock::time_point delivered)
  {
    const auto micros = [](std::chrono::steady_clock::time_point from,
                           std::chrono::steady_clock::time_point to) {
      return static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
    };
    timings[0][timing_index] = micros(start, assembled);
    timings[1][timing_index] = micros(assembled, forwarded);
    timings[2][timing_index] = micros(forwarded, delivered);
    timing_index             = (timing_index + 1) % TIMING_RING;
    ++timing_count;

    // The first forward is reported on its own, because a leg can be short and the first reading is the one that
    // says whether the cost is in the range the design assumed; then one line every TIMING_REPORT_EVERY, which is
    // frequent enough to see a drift and rare enough not to become the log.
    if ((timing_count == 1) || ((timing_count % TIMING_REPORT_EVERY) == 0)) {
      report_timing();
    }
  }

  void report_timing()
  {
    const unsigned sampled = std::min(timing_count, TIMING_RING);
    static const char* const names[3] = {"plane", "forward", "deliver"};
    fmt::print(stderr, "[depth3-timing] {} forward(s), microseconds:", timing_count);
    for (unsigned segment = 0; segment != 3; ++segment) {
      std::array<unsigned, TIMING_RING> sorted = timings[segment];
      std::sort(sorted.begin(), sorted.begin() + sampled);
      const unsigned p99_index = std::min((sampled * 99) / 100, sampled - 1);
      fmt::print(stderr, " {} p50 {} p99 {} max {} |", names[segment], sorted[sampled / 2], sorted[p99_index],
                 sorted[sampled - 1]);
    }
    fmt::print(stderr, "\n");
  }

  std::array<std::array<unsigned, TIMING_RING>, 3> timings{};
  unsigned                                         timing_index = 0;
  unsigned                                         timing_count = 0;

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
                           const pusch_demodulator::configuration& config,
                           const depth3_scales&                    scales)
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
               "[depth3-measure] reception {} (Qm={} {}) over {} pilot RE(s): epre net={:.2f} classic={:.2f} dB "
               "| rsrp net={:.2f} classic={:.2f} dB | snr net={:.2f} classic={:.2f} dB | "
               "noise net={:.4e} classic={:.4e}\n",
               forwarded_receptions,
               get_bits_per_symbol(config.modulation),
               depth3_model_map::name(get_bits_per_symbol(config.modulation)),
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
               unsigned                               qm,
               const depth3_scales&                   scales)
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
  /// The fallback path as given (for the startup lines), and whether it came from the build rather than the
  /// command line -- the two look identical in a log otherwise, and the difference is what the coverage means.
  std::string           fallback_path;
  bool                  fallback_is_build_default = false;

  /// Which bound model serves which modulation, and the engine and calibration of each. Indexed by the
  /// modulation's bits per symbol (2, 4, 6, 8), so a reception picks its model with one lookup.
  depth3_model_map                                  models;
  std::array<std::unique_ptr<metal::coreml_nn_engine>, 9> engine_by_qm;
  std::array<depth3_scales, 9>                      scales_by_qm;
  /// True when at least one model bound: the arm will compute for the modulations it covers.
  bool                                              any_model_bound = false;
  /// What happened to each model, for the factory to print once for the pool (see binding_notes()).
  std::vector<std::string>                          notes;

  std::unique_ptr<pseudo_random_generator>        descrambler;
  pusch_depth3_input_plane                        assembler;
  pusch_depth3_dmrs_context                       dmrs_context;
  bool                                            context_is_valid = false;
  bool                                            last_forward_ok   = false;
  /// Receptions whose forward pass produced the soft bits, which is what the measurement reporting is
  /// sampled on: a degradation has no head output to measure.
  unsigned                                        forwarded_receptions = 0;

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
    pusch_depth3_kind               kind,
    dmrs_pusch_estimator&           estimator,
    pusch_demodulator&              demodulator,
    const pusch_depth3_model_paths& models)
{
  switch (kind) {
    case pusch_depth3_kind::classic:
      return std::make_unique<pusch_depth3_receiver_classic>(estimator, demodulator);
    case pusch_depth3_kind::ai_identity:
      // The identity arm has nothing to compute with, so only the fallback path is meaningful to it: the slots
      // describe models it will never run.
      return std::make_unique<pusch_depth3_receiver_ai_identity>(estimator, demodulator, models.fallback);
    case pusch_depth3_kind::ai_network:
#if defined(OCUDU_HAVE_DEPTH3_ENGINE)
      return std::make_unique<pusch_depth3_receiver_ai_network_impl>(
          estimator, demodulator, models.fallback, models.slots, models.fallback_is_build_default);
#else
      // Built without the Core ML engine: the arm is selected and cannot run, which is exactly what the
      // identity arm exists to represent -- and it says so rather than pretending.
      ocudulog::fetch_basic_logger("PHY").error(
          "AI depth-3: this build has no Core ML engine (ENABLE_METAL_CHEST is off); the arm forwards to the "
          "classical chain and its output is NOT an AI result");
      return std::make_unique<pusch_depth3_receiver_ai_identity>(estimator, demodulator, models.fallback);
#endif
  }
  return nullptr;
}
