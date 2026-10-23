// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Rights Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "pusch_depth3_receiver_impl.h"
#include "pusch_depth3_model_map.h"
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

// The plane and measurement headers are Apple Silicon code (they store __fp16) and are only
// compiled when the depth-3 core is (ENABLE_DEPTH3_AI), so they are included under the same
// guard as the implementation that uses them.
#if defined(OCUDU_HAVE_DEPTH3_ENGINE)
#include "pusch_depth3_input_plane.h"
#include "pusch_depth3_measurements.h"
#include "ocudu_coreml_nn_engine.h"
#endif

#include <algorithm>
#include <array>
#include <map>
#include <tuple>
#include <chrono>
#include <cstdio>
#include "fmt/format.h"
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

using namespace ocudu;

namespace {
/// \brief Lowest and highest PRB the whole allocation occupies, for the crop-window arithmetic.
unsigned narrow_first_prb(const pusch_demodulator::configuration& config)
{
  unsigned prb = 0;
  while ((prb + 1 < config.rb_mask.size()) && !config.rb_mask.test(prb)) {
    ++prb;
  }
  return prb;
}

unsigned narrow_last_prb(const pusch_demodulator::configuration& config)
{
  unsigned prb = config.rb_mask.size() - 1;
  while ((prb > 0) && !config.rb_mask.test(prb)) {
    --prb;
  }
  return prb;
}



// The sidecar reader and its warning serve only the engine path below; they are compiled only where that
// path is. A build without the depth-3 engine (Ubuntu) would otherwise carry them as dead code, which GCC
// rejects with -Werror=unused-function (caught 2026-10-19). The file itself stays universal: it also
// carries the factory and the classical and identity arms.
#if defined(OCUDU_HAVE_DEPTH3_ENGINE)

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

#endif // OCUDU_HAVE_DEPTH3_ENGINE

#if defined(OCUDU_HAVE_DEPTH3_ENGINE)

/// Why a reception was NOT computed by the network. Every value is counted and logged, because a degradation
/// that is not visible is indistinguishable from a receiver that works (`design` section 17.3, I7).
enum class depth3_degradation {
  no_sequence_context,
  modulation_mismatch,
  plane_refused,
  /// The reporting heads cannot measure this reception: its SNR lies outside the range the bound bundle declares
  /// it was trained on. The reception degrades WHOLE rather than only its measurement, because the arm has no
  /// honest number to publish for it and the classical chain does -- running the classical chain is what makes the
  /// fallback visible in the logs and in the throughput, instead of a silently extrapolated SINR reaching the
  /// scheduler (design sections 19.115, 19.117).
  measurement_outside_domain,
  forward_failed,
  no_data_resource_elements,
  /// The arm is inside its startup grace: it does not compute so that ATTACH (a tight request/response exchange
  /// over SRB1) is not competing with the arm's occupancy of the lane. The bundle is loaded and warm throughout,
  /// so this costs one timer comparison per reception and no cold start at the end of it.
  startup_grace
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
    case depth3_degradation::measurement_outside_domain:
      return "the reception is outside the SNR range the bound model's reporting heads were trained on";
    case depth3_degradation::forward_failed:
      return "the Core ML forward pass failed";
    case depth3_degradation::startup_grace:
      return "the arm is inside its startup grace window";
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
/// The modulations the conditioned contract's one-hot channels stand for, in the order the training pipeline
/// writes them (`depth3.COND_QM_ORDER`). A mismatch here would condition the model with the wrong modulation, and
/// nothing would look broken -- every head would still produce plausible values.
static constexpr unsigned D3_COND_QM_ORDER[]    = {2, 4, 6, 8};
static constexpr unsigned D3_COND_QM_ORDER_SIZE = 4;
/// The LLR head's stride under the conditioned contract: the widest modulation, always.
static constexpr unsigned COND_STRIDE = 8;

/// \brief How far the network's RSRP and EPRE may sit from the classical estimator's before the reception counts
/// as disagreeing (R6's runtime evidence, design section 19.126).
///
/// Half a dB is the tolerance the adoption checklist uses, and it is a tolerance and not a target: the
/// conditioned bundle measures 0.02-0.10 dB from the estimator, while a bundle whose head 3 is shrunk sits
/// decibels away -- the difference this count exists to make visible on a live run.
static constexpr float R6_AGREEMENT_DB = 0.5F;

/// \brief The estimator results a PURE run passes to the chain: empty, and never read.
///
/// WHY IT IS NEEDED (found on the first pure leg, 2026-10-20). The PUSCH chain continues when the estimator calls
/// `on_estimation_complete` -- that call IS the trigger for estimation to hand over to demodulation. A pure run
/// does not call the classical estimator at all, so without this the reception never proceeds and the leg hangs
/// instead of failing. The notifier ignores the contents (it only uses the call as a signal), and the arm itself
/// never reads them in pure mode, so an empty implementation is exactly right: not a placeholder, the correct
/// answer for a run that measured nothing.
class depth3_empty_estimator_results : public dmrs_pusch_estimator_results
{
public:
  float get_noise_variance(unsigned) const override { return 0.0F; }
  float get_rsrp(unsigned, unsigned) const override { return 0.0F; }
  static_vector<float, MAX_PORTS> get_rsrp_all_ports(unsigned) const override { return {}; }
  float get_epre(unsigned) const override { return 0.0F; }
  float get_snr(unsigned) const override { return 0.0F; }
  float get_layer_average_snr(unsigned) const override { return 0.0F; }
  phy_time_unit get_time_alignment(unsigned) const override { return phy_time_unit(); }
  std::optional<float> get_cfo_Hz(unsigned) const override { return std::nullopt; }
  void get_symbol_ch_estimate(span<cbf16_t>, unsigned, unsigned, unsigned) const override {}
  void get_symbol_ch_estimate(span<cbf16_t>,
                              unsigned,
                              unsigned,
                              unsigned,
                              const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override
  {
  }
  bool sync_device_estimates() const override { return true; }
  /// \brief This hop is computed on the HOST side, by the arm, so the strict policy must not ask the classic
  /// device estimator whether it covered it.
  ///
  /// MEASURED ON THE FIRST PURE LEG (2026-10-20): the fused lane's strict policy failed EVERY grant with "the
  /// device did not cover the hop (no stage reported a cause)", so the phone could not attach at all. The policy
  /// asks this question precisely so that a receiver chosen by a knob and computing on the host is let through --
  /// the default answer ("the device did") is the opposite of what a pure run means.
  bool results_are_computed_on_host() const override { return true; }
  /// The host route here is the OPERATOR's choice (OCUDU_DEPTH3_FALLBACK=none), which is what exempts a hop from
  /// the strict policy rather than reporting it as a shortfall.
  bool device_shortfall_is_knob_requested() const override { return true; }
  // The chain also asks for the channel state information it would have reported; a pure run reports nothing here,
  // because its numbers come from the arm's own measurement layer and are published by the arm.
  void get_channel_state_information(channel_state_information&) const override {}
};

class pusch_depth3_receiver_ai_network_impl : public pusch_depth3_receiver
{
public:
  pusch_depth3_receiver_ai_network_impl(dmrs_pusch_estimator&                  estimator_,
                                        pusch_demodulator&                     demodulator_,
                                        const std::string&                     fallback_path_,
                                        const std::array<std::string, 4>&      slot_paths_,
                                        bool                                   fallback_is_build_default_,
                                        const std::string&                     reporting_source_,
                                        unsigned                               startup_grace_s_) :
    estimator(estimator_),
    demodulator(demodulator_),
    fallback_path(fallback_path_),
    fallback_is_build_default(fallback_is_build_default_),
    reporting_source(reporting_source_),
    startup_grace(std::chrono::seconds(startup_grace_s_)),
    bound_at(std::chrono::steady_clock::now()),
    descrambler(create_pseudo_random_generator_sw_factory()->create()),
    input_plane(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS *
                pusch_depth3_input_plane::NOF_CHANNELS),
    llr_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS * 8),
    noise_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * pusch_depth3_input_plane::NOF_SYMBOLS),
    channel_head(pusch_depth3_input_plane::NOF_SUBCARRIERS * 2 * pusch_depth3_input_plane::NOF_SYMBOLS)
  {
  // The gate switch is read ONCE, like the reporting source: a run either gates on the estimator throughout or
  // on the plane's own residual throughout, and which one it was is visible in the first degradation line.
  if (const char* env = std::getenv("OCUDU_DEPTH3_DOMAIN_GATE")) {
    domain_gate = (env[0] == 'n') ? "net" : "estimator";
  }
  if (const char* env = std::getenv("OCUDU_DEPTH3_GATE_REPORT")) {
    gate_report = (env[0] != '0');
  }
  // THE A/B SWITCH for the early measurement's assembly scope (design section 19.275): set it and prepare() assembles
  // the whole plane, as it did before the DM-RS-only change. It exists so the two modes can be compared inside ONE
  // environment -- two flights on different days cannot separate a twenty-microsecond effect from batch scatter.
  if (const char* env = std::getenv("OCUDU_DEPTH3_IGNORE_REPORTING_HEADS")) {
    ignore_reporting_heads = (env[0] != '0');
    if (ignore_reporting_heads) {
      fmt::print(stderr,
                 "[pusch_receiver] AI depth-3: publishing from the SHARED LAYER even though the bundle carries the "
                 "reporting heads (OCUDU_DEPTH3_IGNORE_REPORTING_HEADS=1, design section 22.7 gate 2)\n");
    }
  }
  // The shared layer's conversion, per modulation, fitted once (design sections 22.3 and 19.279). The identity is the
  // default for a modulation that was not fitted, and a run that needs a different fit says so through the knob.
  for (shared_calibration& cal : shared_sinr_cal) {
    cal = {1.0F, 0.0F};
  }
  // Fitted on 714 receptions across four corpora (design section 19.280), direction `classic = a * shared + b`:
  //   QPSK   n=300  a=0.982 b=+1.27  rms 0.19 dB
  //   16QAM  n=105  a=1.055 b=-0.13  rms 0.19 dB
  //   64QAM  n=105  a=1.073 b=-0.35  rms 0.47 dB
  //   256QAM n=204  a=0.963 b=+2.97  rms 2.09 dB
  // Every slope lands inside [0.9, 1.1], which is the acceptance rule for this conversion. The 256QAM scatter is the
  // one to watch: its mean is corrected, its spread is twice the others'.
  // THE COEFFICIENTS ARE THE FIT ITSELF, not its inverse: the fit already reads `classic = a * shared + b`, so `a` and
  // `b` ARE the conversion the receiver applies. Inverting them -- which the first version did, on the reflex that a
  // fitted line must be undone -- published 27.75 dB where the fit says 31.57 on the same reception.
  shared_sinr_cal[2] = {0.982F, +1.27F};   // QPSK
  shared_sinr_cal[4] = {1.055F, -0.13F};   // 16QAM
  shared_sinr_cal[6] = {1.073F, -0.35F};   // 64QAM
  shared_sinr_cal[8] = {0.963F, +2.97F};   // 256QAM
  if (const char* env = std::getenv("OCUDU_DEPTH3_SHARED_SINR_CAL")) {
    // "qm:slope,offset;qm:slope,offset" -- for a fit run, so the constants can be revised without a rebuild.
    std::string text(env);
    size_t      pos = 0;
    while (pos < text.size()) {
      const size_t end   = text.find(';', pos);
      const std::string one = text.substr(pos, (end == std::string::npos) ? std::string::npos : end - pos);
      unsigned    qm    = 0;
      float       slope = 1.0F;
      float       offset = 0.0F;
      if (std::sscanf(one.c_str(), "%u:%f,%f", &qm, &slope, &offset) == 3 && qm < shared_sinr_cal.size()) {
        shared_sinr_cal[qm] = {slope, offset};
      }
      if (end == std::string::npos) {
        break;
      }
      pos = end + 1;
    }
  }
  if (const char* env = std::getenv("OCUDU_DEPTH3_PREPARE_FULL_PLANE")) {
    prepare_full_plane = (env[0] != '0');
    fmt::print(stderr,
               "[pusch_receiver] AI depth-3: the early measurement assembles the {} plane ({}), so an A/B against the "
               "DM-RS-only default can be flown inside one environment\n",
               prepare_full_plane ? "WHOLE" : "DM-RS-only",
               prepare_full_plane ? "OCUDU_DEPTH3_PREPARE_FULL_PLANE=1" : "OCUDU_DEPTH3_PREPARE_FULL_PLANE=0");
  }
  // PREFERRED COMPUTE UNITS ARE PINNED BACK TO "all", AND LOUDLY (design section 19.374(b)). The engine accepts
  // `cpu_gpu` and `cpu_ne` because the latency probe needs them to measure each tier in isolation, but on air they are
  // not a preference -- they are a way to break the link. Measured 2026-10-23: with `cpu_gpu` the prediction took
  // 1677 microseconds against a 500-microsecond slot, so receptions arrived late and the decode rate fell to 15.2
  // percent, which shows up as iperf running at zero while ping still answers. `all` lets Core ML pick, and the worst
  // it does is land at 650 to 930 microseconds with decoding intact.
  if (const char* units = std::getenv("OCUDU_DEPTH3_COMPUTE_UNITS")) {
    const std::string requested(units);
    if ((requested == "cpu_gpu") || (requested == "cpu_ne") || (requested == "cpu")) {
      fmt::print(stderr,
                 "[pusch_receiver] AI depth-3: REFUSING OCUDU_DEPTH3_COMPUTE_UNITS={} for the receiver (it costs 1677 us "
                 "per reception and drops decoding to 15 percent, measured 2026-10-23); using 'all'. The tier knob "
                 "exists for the latency probe, which does not go through this receiver.\n",
                 requested);
      setenv("OCUDU_DEPTH3_COMPUTE_UNITS", "all", 1);
    }
  }

  // The fallback switch: `none` is the pure configuration the final comparison runs (design section 19.245).
  if (const char* env = std::getenv("OCUDU_DEPTH3_FALLBACK")) {
    fallback_enabled = !((env[0] == 'n') || (env[0] == 'N') || (env[0] == '0'));
  }
  // A PURE RUN HAS NO ESTIMATOR TO ASK (design section 19.245): the gate cannot read a number the classical chain
  // produced, and the published measurement cannot be the classical one. Both are forced onto the arm's own shared
  // measurement layer here, once, and said out loud -- a silent substitution would be the worst of both.
  if (!fallback_enabled) {
    if (domain_gate != "net") {
      domain_gate = "net";
    }
    if (reporting_source != "net") {
      reporting_source = "net";
    }
    fmt::print(stderr,
               "[pusch_receiver] AI depth-3: PURE mode (OCUDU_DEPTH3_FALLBACK=none): the classical estimator is not "
               "called, the gate judges on the arm's own plane residual and the published measurement is the arm's "
               "own. A reception that cannot be computed is erased and counted, not handed over.\n");
  }
  // Said once, at construction: which measurement gates this run, so a log never has to be guessed at.
  fmt::print(stderr, "[pusch_receiver] AI depth-3: domain gate on the {}\n",
             (domain_gate == "net") ? "arm's own plane residual (R7/S2)" : "classical estimator's SNR");

    fmt::print(stderr, "[pusch_receiver] AI depth-3: startup grace = {} s (0 = disabled)\n", startup_grace.count());
    if (startup_grace.count() > 0) {
      fmt::print(stderr,
                 "[pusch_receiver] AI depth-3: startup grace {} s: every reception degrades to the classical chain "
                 "until {} s after bind, then the arm takes over. The bundle is bound and kept warm meanwhile, so "
                 "this is not a cold start; it is here because attach competes with the arm for the lane "
                 "(--depth3_startup_grace_s, 0 disables)\n",
                 startup_grace.count(), startup_grace.count());
    }
    // EVERY MODEL IS BOUND HERE, at construction, and every outcome is kept and printed: an arm that was
    // SELECTED but cannot compute must never be reported as an AI result, and with more than one model the
    // question becomes per-modulation -- "which modulations does this run actually compute" is answered below
    // and by the coverage line the factory prints, not inferred from the command line.
    auto bind = [this](const std::string& path, unsigned slot_qm, bool fallback) {
      auto engine = std::make_unique<metal::coreml_nn_engine>();
      const bool loaded = engine->init_depth3(path.c_str());
      const unsigned declared_qm = loaded ? engine->depth3_bits_per_symbol() : 0;

      // THE GEOMETRY. This arm assembles a FIXED-WIDTH plane, so a bundle exported for another cell's bandwidth
      // cannot serve it: the runtime rejects an input whose width differs from the graph's, and that shows up as a
      // refusal on every single reception -- an operator reads a broken model rather than a model for another cell.
      // Only binding can see the declaration, so the refusal is here, with both numbers named, and the undeclared
      // case is said out loud rather than passed over (design section 19.162).
      // A NARROWER BUNDLE IS LEGAL AND IS FED A CROP (`model_network_design` section 49): the plane is assembled at
      // the contract's width and only the window the allocation needs is handed to the network, with four PRB of
      // context each side -- measured to reproduce the full-band outputs bit for bit. What cannot work is a bundle
      // WIDER than the assembled plane, because the data it would need is never produced.
      if (loaded && (engine->depth3_nsc() > pusch_depth3_input_plane::NOF_SUBCARRIERS)) {
        notes.push_back(fmt::format("model '{}' NOT bound: exported for {} subcarrier(s) and this build assembles "
                                    "{}; a wider window than the plane cannot be served",
                                    path, engine->depth3_nsc(), pusch_depth3_input_plane::NOF_SUBCARRIERS));
        return;
      }
      if (loaded && (engine->depth3_nsc() == 0)) {
        notes.push_back(fmt::format("model '{}' declares no geometry (depth3_nsc); binding it, but a width mismatch "
                                    "would surface only as a refused input on every reception",
                                    path));
      }
      // WHAT THE BUNDLE WAS TRAINED FOR, kept for the startup line: the plane width, so an operator can see that
      // the model in use matches the cell it is in.
      if (loaded && (bound_nsc_ == 0)) {
        bound_nsc_ = engine->depth3_nsc();
      }

      // THE MODULATION-CONDITIONED CONTRACT (design section 19.102): one bundle, every modulation it declares,
      // with the Qm as four extra input channels and a main head whose stride is the widest modulation. It cannot
      // be told apart from a per-modulation bundle by shape -- both carry 14 * 8 = 112 main-head channels -- so
      // the bundle's own declaration decides, and only binding can read it.
      if (loaded && engine->depth3_is_modulation_conditioned()) {
        // ONE CONTRACT PER ARM, refused rather than half-honoured. The two contracts need different input planes
        // (16 channels against 12), and the plane is assembled once per arm: binding both would feed one of them
        // the other's width, which the runtime rejects -- a degradation that looks like a missing model rather
        // than like the configuration error it is. Refusing here names the conflict instead.
        if (any_model_bound && !conditioned) {
          notes.push_back(fmt::format("modcond model '{}' NOT bound: this arm already carries per-modulation "
                                      "model(s), and the two contracts need different input planes ({} against "
                                      "{}); use one contract per run",
                                      path, engine->depth3_input_channels(),
                                      pusch_depth3_input_plane::NOF_CHANNELS));
          return;
        }
        std::vector<unsigned> declared;
        for (unsigned i = 0; i != engine->depth3_nof_declared_qm(); ++i) {
          declared.push_back(engine->depth3_declared_qm(i));
        }
        const depth3_model_map::outcome outcome = models.add_conditioned(declared, path);
        if (outcome.result == depth3_model_map::status::refused) {
          notes.push_back(fmt::format("modcond model '{}' NOT bound: {}", path, outcome.reason));
          return;
        }
        // ONE ENGINE SERVES THEM ALL, and it decides the plane the arm assembles and the stride it reads the LLR
        // head with: the contract's 12 channels plus the four conditioning ones, and a fixed stride of eight
        // whatever the reception's own Qm is.
        conditioned        = true;
        plane_channels     = engine->depth3_input_channels();
        // WHICH HEADS THIS ARTIFACT CARRIES (design section 22.2): a bundle trained to produce soft bits only is
        // still this contract's model, and the publication then comes from the shared measurement layer.
        if (!engine->depth3_has_reporting_heads(has_ch_est, has_dmrs_noise)) {
          has_ch_est     = true;
          has_dmrs_noise = true;
        }
        if (!has_ch_est || !has_dmrs_noise) {
          fmt::print(stderr,
                     "[pusch_receiver] AI depth-3: the bound bundle carries {}; {} will be published from the shared "
                     "measurement layer instead of from a head\n",
                     has_ch_est ? "no DM-RS noise head" : (has_dmrs_noise ? "no channel head" : "neither reporting head"),
                     "RSRP, EPRE and the SINR");
        }
        // prepare() assembles into its own plane with the same width as the reception (19.273).
        prepare_assembler  = std::make_unique<pusch_depth3_input_plane>(plane_channels);
        prepare_plane.assign(static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) *
                                 pusch_depth3_input_plane::NOF_SYMBOLS * plane_channels,
                             static_cast<__fp16>(0.0F));
        input_plane.assign(static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) *
                               pusch_depth3_input_plane::NOF_SYMBOLS * plane_channels,
                           0.0F);
        assembler.set_plane_channels(plane_channels);
        for (unsigned qm : declared) {
          scales_by_qm[qm] = read_scales(path, D3_RANGE_LIMIT_256QAM);
          engine_by_qm[qm] = nullptr; // one engine, shared by every modulation it declares; see `engine`.
          // The stride of the head THIS bundle computes, which is the widest modulation it declares whatever
          // the reception's own Qm is: the one-hot channels are what tell the network which bits are live.
          llr_stride_by_qm[qm] = COND_STRIDE;
          domain_by_qm[qm]     = declared_domain{engine->depth3_has_snr_range(), engine->depth3_snr_min_db(),
                                                 engine->depth3_snr_max_db()};
        }
        scales_by_qm[declared.front()] = read_scales(path, D3_RANGE_LIMIT_256QAM);
        // Read BEFORE the move, for the reason the per-modulation path below spells out: reading a moved-from
        // `unique_ptr` is a null dereference, and it took the arm down at construction the first time this line
        // was written after the move (measured 2026-10-17, the same trap as 2026-10-16).
        const char* const compute_units = engine->depth3_compute_units();
        // The published-SINR calibration, read before the move for the same reason (design section 19.172).
        for (unsigned qm : declared) {
          float slope = 0.0F;
          float offset = 0.0F;
          if (engine->depth3_sinr_calibration(qm, slope, offset)) {
            sinr_cal_by_qm[qm] = sinr_calibration{true, slope, offset};
          }
        }
        shared_engine      = std::move(engine);
        any_model_bound    = true;
        // THE BUCKET SET (model_network_design section 49.9). Three cases, in this order:
        //   OCUDU_DEPTH3_BUCKETS unset        -> the bundle's own `buckets/` directory, which is how the rig flies by
        //                                        default: a reception is computed over the window it needs, and the
        //                                        measured result on air is 44 percent off the median forward time at the
        //                                        same throughput and CRC.
        //   OCUDU_DEPTH3_BUCKETS=none|full|0   -> bucketing OFF, the single full-band bundle only.
        //   OCUDU_DEPTH3_BUCKETS=a,b,c         -> exactly those artefacts.
        {
          const char*          env     = std::getenv("OCUDU_DEPTH3_BUCKETS");
          std::string          listing = (env != nullptr) ? std::string(env) : std::string();
          const bool           opt_out = (listing == "none") || (listing == "full") || (listing == "0");
          std::vector<std::string> paths;
          if (opt_out) {
            notes.push_back("narrow-band bucketing DISABLED (OCUDU_DEPTH3_BUCKETS=none): the full-band bundle computes "
                            "every reception");
          }
          else if (!listing.empty()) {
            size_t pos = 0;
            while (pos <= listing.size()) {
              const size_t comma = listing.find(',', pos);
              const std::string one = listing.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
              if (!one.empty()) {
                paths.push_back(one);
              }
              if (comma == std::string::npos) {
                break;
              }
              pos = comma + 1;
            }
          }
          else if (!path.empty()) {
            // THE DEFAULT: the bundle's sibling `buckets/` directory. The widths are known constants, so the set is
            // probed rather than scanned -- a missing file simply means a narrower envelope, not a failure.
            const size_t slash = path.find_last_of('/');
            const std::string dir = ((slash == std::string::npos) ? std::string(".") : path.substr(0, slash)) + "/buckets";
            for (unsigned width : {108U, 180U, 252U, 324U, 396U, 468U, 540U, 612U}) {
              paths.push_back(fmt::format("{}/base_w{}.mlmodelc", dir, width));
            }
          }

          std::vector<std::pair<unsigned, std::unique_ptr<metal::coreml_nn_engine>>> bucket_set;
          unsigned missing = 0;
          for (const std::string& bucket_path : paths) {
            auto bucket = std::make_unique<metal::coreml_nn_engine>();
            if (bucket->init_depth3(bucket_path.c_str()) && (bucket->depth3_nsc() != 0) &&
                (bucket->depth3_nsc() <= pusch_depth3_input_plane::NOF_SUBCARRIERS)) {
              bucket_set.emplace_back(bucket->depth3_nsc(), std::move(bucket));
            }
            else {
              ++missing;
            }
          }
          std::sort(bucket_set.begin(), bucket_set.end(),
                    [](const auto& a, const auto& b) { return a.first < b.first; });
          for (auto& entry : bucket_set) {
            narrow_buckets.push_back(entry.first);
            narrow_engines.push_back(std::move(entry.second));
          }
          if (!narrow_buckets.empty()) {
            std::string widths;
            for (unsigned w : narrow_buckets) {
              widths += fmt::format("{} ", w);
            }
            notes.push_back(fmt::format("narrow-band buckets bound: {}subcarrier(s); the smallest that fits a reception's "
                                        "allocation plus four PRB of context is used{}",
                                        widths, (missing != 0) ? fmt::format(" ({} path(s) did not load)", missing)
                                                               : std::string()));
          }
          else if (!opt_out) {
            notes.push_back(fmt::format("no narrow-band buckets bound ({} path(s) tried): every reception uses the "
                                        "full-band bundle", paths.size()));
          }
        }
        notes.push_back(fmt::format("modcond model '{}' bound: one bundle computes {} modulation(s) "
                                    "(Qm set printed by the coverage line), plane {} channels, LLR stride {}; "
                                    "compute units '{}' requested",
                                    path, declared.size(), plane_channels, COND_STRIDE, compute_units));
        if (outcome.result == depth3_model_map::status::redundant) {
          notes.back() += fmt::format("; {}", outcome.reason);
        }
        return;
      }

      const depth3_model_map::outcome outcome = fallback ? models.add_fallback(declared_qm, path)
                                                        : models.add_slot(slot_qm, declared_qm, path);
      const char* const what = fallback ? "fallback" : "slot";
      // The mirror of the refusal above: a per-modulation model arriving at an arm that already carries a
      // conditioned bundle cannot be served either, for the same reason -- one input plane per arm.
      if (conditioned) {
        notes.push_back(fmt::format("{} model '{}' NOT bound: this arm already carries a modulation-conditioned "
                                    "bundle, and the two contracts need different input planes ({} against {}); "
                                    "use one contract per run",
                                    what, path, plane_channels, pusch_depth3_input_plane::NOF_CHANNELS));
        return;
      }
      // The line is COLLECTED, not printed: the pool builds one arm per concurrent PUSCH thread, and the factory
      // prints these once for the pool.
      if (outcome.result != depth3_model_map::status::kept) {
        notes.push_back(fmt::format("{} model '{}' NOT bound: {}", what, path, outcome.reason));
        return;
      }
      // The calibration travels with the model (a sidecar next to the bundle), so it is read once, here, and the
      // reception picks the one of the model that computed it.
      scales_by_qm[declared_qm] = read_scales(path, D3_RANGE_LIMIT_256QAM);
      // EVERYTHING THE ENGINE DECLARES IS READ BEFORE IT IS MOVED. Reading it afterwards dereferences a
      // moved-from `unique_ptr`, i.e. null -- which is a segfault at construction, and it took the AI arm with a
      // model down while the arm without one still worked (measured 2026-10-16, on the per-modulation path; the
      // conditioned path reads inside its loop and was unaffected).
      const declared_domain domain{engine->depth3_has_snr_range(), engine->depth3_snr_min_db(),
                                   engine->depth3_snr_max_db()};
      const char* const     compute_units = engine->depth3_compute_units();
      {
        float slope  = 0.0F;
        float offset = 0.0F;
        if (engine->depth3_sinr_calibration(declared_qm, slope, offset)) {
          sinr_cal_by_qm[declared_qm] = sinr_calibration{true, slope, offset};
        }
      }
      engine_by_qm[declared_qm] = std::move(engine);
      // The LLR head's stride, by the SAME rule as the conditioned case: the head's width per symbol, which for a
      // per-modulation bundle is its own Qm. Leaving it at the member's default 0 is not a smaller stride but a
      // single repeated element -- it produced chance-level agreement (sign 0.50, mean |LLR| 19 against 64) on the
      // per-modulation path while every CRC still looked plausible (measured 2026-10-16).
      llr_stride_by_qm[declared_qm] = declared_qm;
      // The bundle's declared measurement domain, kept per modulation: a fallback bundle and a slot bundle are
      // different artifacts and were not necessarily trained on the same SNR range.
      domain_by_qm[declared_qm] = domain;
      notes.push_back(fmt::format("{} model '{}' bound for Qm={} {}; compute units '{}' requested", what, path,
                                  declared_qm, depth3_model_map::name(declared_qm), compute_units));
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
    // R6 asks for a startup line for every switch that changes what the chain publishes, and this one changes a
    // number the scheduler acts on: a calibrated run must SAY so, with the coefficients, or an operator comparing two
    // legs has no way to know which publication they are looking at (design section 19.172).
    {
      std::string calibrated;
      for (unsigned qm : {2U, 4U, 6U, 8U}) {
        if (!sinr_cal_by_qm[qm].has) {
          continue;
        }
        if (!calibrated.empty()) {
          calibrated += ", ";
        }
        calibrated += fmt::format("Qm={} slope {:.3f} offset {:+.3f}", qm, sinr_cal_by_qm[qm].slope,
                                  sinr_cal_by_qm[qm].offset);
      }
      any_calibrated = !calibrated.empty();
      if (!calibrated.empty()) {
        notes.push_back(fmt::format("the published SINR is CALIBRATED by the bundle "
                                    "(published' = slope * published + offset, dB): {}; the soft bits are not "
                                    "affected",
                                    calibrated));
      }
    }
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
      out.push_back(pusch_depth3_bound_model{e.qm, e.path, e.fallback, domain_by_qm[e.qm].has,
                                              domain_by_qm[e.qm].min_db, domain_by_qm[e.qm].max_db});
    }
    return out;
  }

  /// The modulations no bound model serves; what the run degrades to the classical chain.
  std::vector<unsigned> unserved_modulations() const override { return models.missing(); }

  std::vector<std::string> binding_notes() const override { return notes; }

  /// True when the fallback bound here came from the build rather than from the command line.
  bool fallback_is_the_build_default() const override { return fallback_is_build_default; }

  /// The plane width the first bound model declared; 0 when nothing is bound or nothing declared it.
  unsigned bound_nsc() const override { return bound_nsc_; }

  /// The conditioned contract is bound: one bundle, one set of weights, the plane carries the one-hot channels and
  /// the LLR head is read with a stride. What a reader must do with it, and the startup line the factory prints
  /// from it, are in the interface's comment.
  bool is_modulation_conditioned() const override { return conditioned; }

  /// The plane width the bound bundle declares. Zero when nothing is bound: the width would then describe no
  /// input at all, and the contract's constant is a plausible-looking answer to a question that has none.
  unsigned bound_plane_channels() const override { return any_model_bound ? plane_channels : 0; }

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
    //
    // ITS COST IS TIMED HERE, alone, because it is the one piece of the pre-existing chain the AI path still pays
    // for on EVERY reception while almost none of its output is used: the published value is the arm's own noise,
    // and the estimator's SNR is read by the domain gate and by nothing else. Without this segment the arm's cost
    // reads as the three path segments and the estimator's share is invisible (asked 2026-10-17: what does the
    // classical measurement chain cost the AI path in total). The segment is appended LAST in the report line so
    // that every reader of the existing four keeps working.
    // IN THE PURE CONFIGURATION THE CLASSICAL ESTIMATOR IS NOT CALLED AT ALL (design section 19.245, S3): there is
    // no fallback to feed, so its 700-800 us per reception -- the largest single item on the arm's path, and the
    // thing R7 exists to remove -- simply does not happen. Everything the arm still needs comes from the shared
    // measurement layer, which runs on the DM-RS alone. The two knobs it depends on are forced in the constructor,
    // because a run without an estimator cannot gate on the estimator or publish its numbers.
    if (!fallback_enabled) {
      estimate_skipped = true;
      // THE CHAIN CONTINUES THROUGH THIS CALL, not through the estimator's work: `on_estimation_complete` is what
      // hands estimation over to demodulation. Skipping both the work and the call hung the first pure leg.
      notifier.on_estimation_complete(empty_est_results);
      return;
    }
    estimate_started = std::chrono::steady_clock::now();
    estimator.estimate(notifier, grid, config);
    const auto estimate_submitted = std::chrono::steady_clock::now();
    pending_estimate_call_us       = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                          estimate_submitted - estimate_started)
                                                          .count());
    // THE END OF THE SEGMENT IS NOT HERE. With the device estimator this call only ENQUEUES the kernels; the
    // reception waits for them the first time its results are read, which is the domain gate below. Timing only the
    // submission would report microseconds for work that costs a millisecond, so the segment is closed at the read
    // and the two numbers are kept apart (`pending_estimate_call_us` for the submission).
    pending_estimate_open = true;
  }

  /// \brief Measures this reception from its DM-RS alone, before the network runs.
  ///
  /// Everything it needs is in the grid by the time the last DM-RS symbol has been written: the pilot least-squares
  /// values, the residual noise and the CFO all come from `measure_ul_slow`, the same shared layer demodulate()
  /// uses, so the two cannot disagree about the number the gate acts on. The result is kept per slot.
  void prepare(const resource_grid_reader&               grid,
               const dmrs_pusch_estimator::configuration& est_config,
               const pusch_demodulator::configuration&    config) override
  {
    const auto                     prepare_started = std::chrono::steady_clock::now();
    const pusch_depth3_dmrs_context context         = make_pusch_depth3_dmrs_context(est_config);
    float                           reference_amplitude = 0.0F;
    const size_t plane_samples =
        static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) *
        pusch_depth3_input_plane::NOF_SYMBOLS * plane_channels;
    // DM-RS SYMBOLS ONLY (design section 19.275): this plane is measured and thrown away, never run through the
    // network, and the measurement reads pilots -- which live on those two symbols. Assembling all fourteen cost
    // the receive thread twelve symbols of work nobody would look at.
    const pusch_depth3_input_plane::status status = prepare_assembler->assemble(
        span<__fp16>(prepare_plane).first(plane_samples), reference_amplitude, grid, config, context,
        !prepare_full_plane);
    if (status != pusch_depth3_input_plane::status::ok) {
      ++prepare_refused;
      return;
    }
    const depth3_measurements plane_only = measure_depth3(span<const __fp16>(prepare_plane).first(plane_samples),
                                                          plane_channels,
                                                          span<const __fp16>(),
                                                          reference_amplitude,
                                                          0.0F,
                                                          0.0F,
                                                          0.0F,
                                                          span<ul_pilot_sample>(prepare_scratch));
    // DIAGNOSTIC (design section 19.273): the hint stores a slot and the gate never finds it, so the two sides have
    // to say what they think the slot is. Three lines each, then silent.
    static std::atomic<unsigned> store_diagnostics{0};
    if (store_diagnostics.fetch_add(1, std::memory_order_relaxed) < 3) {
      fmt::print(stderr, "[depth3-cache] store  slot={} pilots={} snr={:.2f} dB\n",
                 est_config.slot.count(), plane_only.nof_pilot_re,
                 (plane_only.residual_snr_lin > 0.0F) ? convert_power_to_dB(plane_only.residual_snr_lin) : -99.0F);
    }
    slot_measurement entry;
    entry.slot             = est_config.slot;
    entry.residual_snr_lin = plane_only.residual_snr_lin;
    entry.cfo_cycles       = estimate_cfo_from_plane(span<const __fp16>(prepare_plane).first(plane_samples),
                                                     plane_channels);
    entry.nof_pilot_re     = plane_only.nof_pilot_re;
    entry.valid            = (plane_only.nof_pilot_re != 0);
    {
      std::lock_guard<std::mutex> lock(measured_mutex);
      measured[measured_next] = entry;
      measured_next           = (measured_next + 1) % MEASURE_RING;
    }
    ++prepared_receptions;
    // The hint's own cost, so a leg can say what the early measurement costs the receive thread instead of inferring
    // it from a difference between legs (design section 19.275).
    prepare_cost_us[prepare_cost_index] =
        static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - prepare_started)
                                  .count());
    prepare_cost_index = (prepare_cost_index + 1) % prepare_cost_us.size();
    if (prepare_cost_count != prepare_cost_us.size()) {
      ++prepare_cost_count;
    }
  }

  void demodulate(pusch_codeword_buffer&                 codeword_buffer,
                  pusch_demodulator_notifier&            notifier,
                  const resource_grid_reader&            grid,
                  const dmrs_pusch_estimator_results&    est_results,
                  const pusch_demodulator::configuration& config) override
  {
    last_forward_ok          = false;
    plane_measurement_valid  = false;
    publish_suppressed       = false;
    // THE STARTUP GRACE, checked before anything else and costing one comparison: attach is a tight
    // request/response exchange over SRB1 and the arm's occupancy of the lane competes with the phone's ACK path
    // (measured 2026-10-18: the failing leg retransmitted the 15-byte SRB1 packet 2841 times with PUCCH format-1
    // detected in 15.7% of the attach window, against 45.9% in a classical leg). The bundle stays loaded and warm
    // throughout -- the engine warms up at construction and runs a keep-alive prediction every two seconds -- so
    // the arm starts computing, warm, the moment the window closes.
    if (startup_grace.count() > 0) {
      if ((std::chrono::steady_clock::now() - bound_at) < startup_grace) {
        degrade_or_fail(depth3_degradation::startup_grace, codeword_buffer, notifier, grid, est_results, config);
        return;
      }
      if (!grace_ended_reported) {
        grace_ended_reported = true;
        fmt::print(stderr,
                   "[pusch_receiver] AI depth-3: the {} s startup grace is over; the arm takes over from the "
                   "classical chain from here (the bundle stayed loaded and warm throughout)\n",
                   startup_grace.count());
      }
    }
    if (!any_model_bound) {
      degrade_or_fail(depth3_degradation::forward_failed, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    if (!context_is_valid) {
      degrade_or_fail(depth3_degradation::no_sequence_context, codeword_buffer, notifier, grid, est_results, config);
      return;
    }

    const unsigned qm = get_bits_per_symbol(config.modulation);
    if (qm == 0 || qm > 8) {
      degrade_or_fail(depth3_degradation::plane_refused, codeword_buffer, notifier, grid, est_results, config);
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
      degrade_or_fail(depth3_degradation::modulation_mismatch, codeword_buffer, notifier, grid, est_results, config,
              detail);
      return;
    }
    // WHICH ENGINE COMPUTES is decided per modulation and not per arm: a per-modulation slot can win a
    // modulation over a conditioned bundle (`depth3_model_map` keeps the slot), and that reception must then
    // run the slot's engine with the slot's stride. The conditioned engine is the fallback for the
    // modulations no per-modulation model claimed.
    metal::coreml_nn_engine* computing_engine =
        engine_by_qm[qm] != nullptr ? engine_by_qm[qm].get() : shared_engine.get();

    // ---- WHICH BUCKET (model_network_design section 49.9) -------------------------------------------
    // A bucket set is bound per modulation and the SMALLEST ONE THAT FITS is used, so "the allocation does not fit" can
    // only happen when the 612-wide bucket is missing from the set -- 612 is the whole plane, so it always fits. The
    // width a reception needs is its allocation plus four PRB of context on each side, clipped to the plane.
    unsigned needed_nsc = pusch_depth3_input_plane::NOF_SUBCARRIERS;
    unsigned bucket_nsc = bound_nsc_;
    // RESET PER RECEPTION. Without this the member kept the last narrow width forever, so a leg whose first reception
    // used a bucket reported almost every later one as narrow -- the histogram was sticky and the four legs of the
    // first ABBA all looked like mixtures. Measured 2026-10-23: 53546 of 58500 receptions attributed to 612 while the
    // same leg's forward time said otherwise.
    bucket_in_use_ = 0;
    if (!narrow_buckets.empty()) {
      const unsigned ctx      = 4 * NOF_SUBCARRIERS_PER_RB;
      const unsigned first_sc = narrow_first_prb(config) * NOF_SUBCARRIERS_PER_RB;
      const unsigned first    = (first_sc > ctx) ? (first_sc - ctx) : 0U;
      const auto     tail     = static_cast<unsigned>((narrow_last_prb(config) + 1) * NOF_SUBCARRIERS_PER_RB) + ctx;
      const unsigned last     = (tail < pusch_depth3_input_plane::NOF_SUBCARRIERS)
                                    ? tail
                                    : static_cast<unsigned>(pusch_depth3_input_plane::NOF_SUBCARRIERS);
      needed_nsc              = last - first;
      const auto it            = std::lower_bound(narrow_buckets.begin(), narrow_buckets.end(), needed_nsc);
      if (it != narrow_buckets.end()) {
        bucket_nsc = *it;
        // The engine follows the bucket, and so does the modulation question: every bucket holds the same weights.
        auto iter = std::find(narrow_buckets.begin(), narrow_buckets.end(), bucket_nsc);
        const size_t index = static_cast<size_t>(std::distance(narrow_buckets.begin(), iter));
        if (index < narrow_engines.size() && narrow_engines[index] != nullptr) {
          computing_engine = narrow_engines[index].get();
        }
        bucket_in_use_ = bucket_nsc;
      }
    }
    // A reception computes only when a model was bound for its modulation, and every binding writes the stride at
    // the same time as the engine. Asserting it keeps a future binding path from computing with a zero stride,
    // which does not fail loudly: it reads one element of the head over and over (measured 2026-10-16).
    ocudu_assert(llr_stride_by_qm[qm] != 0, "Qm={} has a bound model but no main-head stride", qm);
    const depth3_scales&     model_scales     = scales_by_qm[qm];

    // ---- the measurement domain, before a single forward pass -------------------------------------
    // R6's gate (design sections 19.115, 19.117). The chain reports this reception's channel quality to the
    // scheduler, and the reporting heads can only report it where they were trained: measured 2026-10-16, a model
    // trained on 20-32 dB reported 26.4 dB for a prescribed 0 dB. The classical estimator ran for this reception
    // already (`estimate()` forwards to it), so its SNR is available here as an INDEPENDENT domain check -- it
    // decides only whether the network's measurement may be published, and the number published is never the
    // estimator's.
    // IT APPLIES IN EITHER MODE, because the published post-equalisation SINR is this arm's own in either mode
    // (see the note where it is published): a knob position may not decide whether an extrapolated measurement
    // reaches the scheduler, and the `classic` position is the DEFAULT.
    if (domain_by_qm[qm].has && (domain_gate == "estimator")) {
      const unsigned port   = config.rx_ports.empty() ? 0 : config.rx_ports.front();
      // The estimator reports a linear power (`report_measurements` prints it through `convert_power_to_dB`), so
      // it is converted once and the comparison is in the same units the declaration uses.
      const float snr_db = convert_power_to_dB(est_results.get_snr(port));
      // The estimator's result is READ here for the first time, so this is where the reception has paid for it --
      // submission plus device completion. That cost was outside every segment before 2026-10-17, so the arm's
      // reported total understated the path by exactly this much.
      if (pending_estimate_open) {
        pending_estimate_open    = false;
        pending_estimate_total_us = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
                                                             std::chrono::steady_clock::now() - estimate_started)
                                                             .count());
      }
      if ((snr_db < domain_by_qm[qm].min_db) || (snr_db > domain_by_qm[qm].max_db)) {
        char detail[224];
        std::snprintf(detail, sizeof(detail),
                      "the estimator measures %.2f dB and the bundle was trained on %.2f to %.2f dB, so this "
                      "reception's measurement would be an extrapolation",
                      snr_db, domain_by_qm[qm].min_db, domain_by_qm[qm].max_db);
        degrade_or_fail(depth3_degradation::measurement_outside_domain, codeword_buffer, notifier, grid, est_results,
                        config, detail);
        return;
      }
    }

    // ---- the contract's plane, from the grid the chain holds --------------------------------------
    const auto segment_start = std::chrono::steady_clock::now();
    float reference_amplitude = 0.0F;
    const pusch_depth3_input_plane::status status =
        assembler.assemble(span<__fp16>(input_plane), reference_amplitude, grid, config, dmrs_context);
    // ---- R7/S2: the gate on the PLANE's own residual, before any forward pass ------------------------
    // The estimator-based gate above needs the classical estimator to have run, which is the 516-766 us per
    // reception R7 exists to remove. This one reads the residual noise of the pilot LS that the assembler has
    // just written, which is INDEPENDENT of the network (design section 19.128 measured it at +0.999 log-domain
    // correlation against the datasets' true noise, p10/p90 spread 0.23 dB) and costs one pass over the pilots.
    // It runs here, after the assembly and before the forward, so an out-of-domain reception still costs no
    // inference.
    if ((status == pusch_depth3_input_plane::status::ok) && domain_by_qm[qm].has && (domain_gate == "net")) {
      // `reference_amplitude` MUST be the real one: the EPRE and the residual are built from the plane's LS,
      // which is stored divided by `r`, so passing zero here zeroes the signal and the residual with it -- the
      // gate then reads -inf on every reception and refuses the whole leg (measured, first version, 2026-10-20).
      //
      // IF prepare() ALREADY MEASURED THIS SLOT, its answer is used and the plane is not measured again: that is
      // the whole point of the early hook, and taking the number from the same shared layer means the two cannot
      // differ (design section 19.241).
      const std::optional<slot_measurement> prepared = prepared_measurement(dmrs_context.slot);
      // The plane half is measured HERE, once, and kept: the publication after the forward pass reuses it instead
      // of walking the plane again, so the gate and the scheduler judge on the same number (design section 19.263).
      if (!prepared.has_value()) {
        plane_measurement       = measure_depth3_plane(span<const __fp16>(input_plane),
                                                       plane_channels,
                                                       reference_amplitude,
                                                       span<ul_pilot_sample>(pilot_scratch));
        plane_measurement_valid = (plane_measurement.nof_pilot_re != 0);
      }
      const depth3_measurements plane_only =
          prepared.has_value()
              ? [&]() {
                  depth3_measurements from_prepare;
                  from_prepare.residual_snr_lin         = prepared->residual_snr_lin;
                  from_prepare.residual_plane_units_lin = 0.0F;
                  from_prepare.nof_pilot_re             = prepared->nof_pilot_re;
                  return from_prepare;
                }()
              : [&]() {
                  depth3_measurements from_plane;
                  from_plane.residual_snr_lin         = plane_measurement.snr_lin;
                  from_plane.residual_plane_units_lin = plane_measurement.noise_lin;
                  from_plane.nof_pilot_re             = plane_measurement.nof_pilot_re;
                  return from_plane;
                }();
      if (prepared.has_value()) {
        ++gate_from_prepare;
      }
      gate_history.note(plane_only);
      // The FILTERED value is what the gate decides on once it has warmed up; before that it accepts, so the
      // first receptions of a leg are never refused on an estimate that has not seen enough pilots (19.237).
      const bool  warm         = gate_history.warm();
      const float instant_db   = (plane_only.residual_snr_lin > 0.0F)
                                     ? convert_power_to_dB(plane_only.residual_snr_lin)
                                     : -std::numeric_limits<float>::infinity();
      const float plane_snr_db = warm ? gate_history.snr_db() : instant_db;
      // THE OFFSET BETWEEN THE TWO MEASURES, printed on the same sampled cadence as the other instruments.
      // The declared range was fitted against the estimator's SNR, so if the plane's residual sits at a
      // different level, the same numbers mean different things and the gate's boundary moves with it. S2
      // cannot choose a threshold until this is measured on the captures (design section 19.236).
      ++gate_samples;
      if ((gate_samples == 1) || ((gate_samples % 1000) == 0) || gate_report) {
        const unsigned port    = config.rx_ports.empty() ? 0 : config.rx_ports.front();
        const float    est_db  = convert_power_to_dB(est_results.get_snr(port));
        // The arm's own CFO, next to the classical estimator's, so the two can be compared on the same
        // reception before anything depends on the arm's value (design section 19.239). Cycles per symbol:
        // the plane does not carry the subcarrier spacing, so the comparison converts it outside.
        const float cfo_cycles = prepared.has_value() ? prepared->cfo_cycles : plane_measurement.cfo_cycles;
        const std::optional<float> cfo_classic = est_results.get_cfo_Hz(port);
        fmt::print(stderr,
                   "[depth3-gate] {} reception(s): plane residual {:.2f} dB (filtered {:.2f} dB over {:.0f} "
                   "pilot(s), {}), estimator {:.2f} dB, declared {:.2f} to {:.2f} dB\n",
                   gate_samples,
                   instant_db,
                   warm ? gate_history.snr_db() : instant_db,
                   gate_history.weight_sum,
                   warm ? "warm" : "cold: accepted",
                   est_db,
                   domain_by_qm[qm].min_db,
                   domain_by_qm[qm].max_db);
        // Whether the gate's number came from prepare()'s cache, i.e. whether the early measurement actually
        // happened on this run: the leg that turns OCUDU_DEPTH3_PREPARE_ON_SYMBOL on has to show it.
        fmt::print(stderr,
                   "[depth3-gate] early measurement: {} reception(s) measured ahead, {} refused, {} gate(s) took "
                   "the cached number instead of measuring again\n",
                   prepared_receptions.load(),
                   prepare_refused.load(),
                   gate_from_prepare.load());
        // One more line, only when every reception is being reported: the CFO comparison is an offline
        // measurement, not something a live leg needs in its log.
        if (gate_report) {
          if (cfo_classic.has_value()) {
            fmt::print(stderr,
                       "[depth3-cfo] arm {:.4f} cycles/symbol, classical {:.1f} Hz (ratio {:.3f} at 15 kHz)\n",
                       cfo_cycles,
                       *cfo_classic,
                       (cfo_classic.value() != 0.0F)
                           ? (cfo_cycles * 15000.0F / *cfo_classic)
                           : std::numeric_limits<float>::quiet_NaN());
          } else {
            fmt::print(stderr, "[depth3-cfo] arm {:.4f} cycles/symbol, classical n/a\n", cfo_cycles);
          }
        }
      }
      if (warm && ((plane_snr_db < domain_by_qm[qm].min_db) || (plane_snr_db > domain_by_qm[qm].max_db))) {
        // OUT OF RANGE SUPPRESSES THE MEASUREMENT, NOT THE RECEPTION (design section 19.272). Measured on air
        // 2026-10-20 23:05: the environment dropped to about 10 dB, the filtered measurement came out at 9.71 dB
        // against a declared floor of 12, and every reception was erased -- the uplink went to zero bytes for a
        // minute at a time and iperf3 could not finish. What is untrustworthy outside the trained range is the
        // REPORTED quantity, while the soft bits were measured to decode as well there as anywhere (19.265), so the
        // reception is computed and delivered and the publication is withheld. The scheduler then keeps its last
        // good value instead of receiving an extrapolation, and the link keeps working.
        ++measurements_suppressed;
        publish_suppressed = true;
        char detail[224];
        std::snprintf(detail, sizeof(detail),
                      "the plane's own residual measures %.2f dB and the bundle declares %.2f to %.2f dB "
                      "(%s pilot RE)",
                      plane_snr_db,
                      domain_by_qm[qm].min_db,
                      domain_by_qm[qm].max_db,
                      (plane_only.nof_pilot_re == 0) ? "no" : "with");
        if ((gate_samples == 1) || ((gate_samples % 1000) == 0)) {
          fmt::print(stderr,
                     "[depth3-gate] measurement SUPPRESSED for this reception: %s; the reception is computed and its "
                     "soft bits delivered, and no measurement is published\n",
                     detail);
        }
      }
    }
    if (conditioned) {
      // The Qm as the contract wants it: four constant channels per (subcarrier, symbol), all zero but the
      // reception's own. Written HERE, once per reception, because the assembler describes the grid and this
      // describes which modulation the grid carries -- and the value is the same in every block, so the loop is
      // 612 * 14 stores of four values rather than anything per-bit.
      const unsigned qm_index = [qm]() {
        for (unsigned i = 0; i != D3_COND_QM_ORDER_SIZE; ++i) {
          if (D3_COND_QM_ORDER[i] == qm) {
            return i;
          }
        }
        return D3_COND_QM_ORDER_SIZE;
      }();
      if (qm_index != D3_COND_QM_ORDER_SIZE) {
        const __fp16 one_hot[D3_COND_QM_ORDER_SIZE] = {qm_index == 0 ? __fp16(1) : __fp16(0),
                                                       qm_index == 1 ? __fp16(1) : __fp16(0),
                                                       qm_index == 2 ? __fp16(1) : __fp16(0),
                                                       qm_index == 3 ? __fp16(1) : __fp16(0)};
        for (unsigned k = 0; k != pusch_depth3_input_plane::NOF_SUBCARRIERS; ++k) {
          for (unsigned symbol_index = 0; symbol_index != pusch_depth3_input_plane::NOF_SYMBOLS;
               ++symbol_index) {
            __fp16* block = input_plane.data() +
                            (static_cast<size_t>(k) * pusch_depth3_input_plane::NOF_SYMBOLS + symbol_index) *
                                plane_channels +
                            pusch_depth3_input_plane::NOF_CHANNELS;
            for (unsigned c = 0; c != D3_COND_QM_ORDER_SIZE; ++c) {
              block[c] = one_hot[c];
            }
          }
        }
      }
    }
    const auto segment_assembled = std::chrono::steady_clock::now();
    if (status != pusch_depth3_input_plane::status::ok) {
      // The reason is a property of the RECEPTION, not of the code, so it is reported with its own name.
      // The specific reason travels with the count: "refused" alone would say a reception was skipped without
      // saying which field of it the contract does not describe.
      char detail[160];
      std::snprintf(detail, sizeof(detail), "%s (grid %u subcarriers, allocation %u PRBs, %u symbols)",
                    pusch_depth3_input_plane::to_string(status), grid.get_nof_subc(),
                    static_cast<unsigned>(config.rb_mask.count()), config.nof_symbols);
      degrade_or_fail(depth3_degradation::plane_refused, codeword_buffer, notifier, grid, est_results, config, detail);
      return;
    }

    // ---- one forward pass, four heads ------------------------------------------------------------
    // A NARROW-BAND BUNDLE IS FED A CROP, NOT THE WHOLE BAND (model_network_design section 49). The trunk is fully
    // convolutional, so a bundle exported for a narrower window reproduces the full-band outputs bit for bit provided
    // the window keeps four PRB of context on each side -- measured, not assumed. The plane is still ASSEMBLED at the
    // contract's 612 subcarriers, because assembling it is a memory write and the compute is two hundred million
    // multiply-adds; what changes is only how much of it the network sees. The heads are scattered back at their
    // absolute positions, so every reader downstream still indexes by subcarrier and nothing else has to know.
    unsigned plane_nsc = pusch_depth3_input_plane::NOF_SUBCARRIERS;
    unsigned crop_lo   = 0;
    // THE WIDTH IS THE BUCKET THE SELECTOR CHOSE, not the shared bundle's declaration. Using the latter left the crop
    // disabled while the bucket engine was already selected, so Core ML was handed a 612-wide plane for a 396-wide model
    // and answered "MultiArray shape (1 x 612 x 14 x 16) does not match (1 x 396 x 14 x 16)" -- three silent failures
    // after the selector was already right (model_network_design section 49.9).
    if ((bucket_nsc != 0) && (bucket_nsc < pusch_depth3_input_plane::NOF_SUBCARRIERS)) {
      crop_lo   = narrow_window_lo(config);
      plane_nsc = bucket_nsc;
      if ((crop_lo + plane_nsc) > pusch_depth3_input_plane::NOF_SUBCARRIERS) {
        crop_lo = pusch_depth3_input_plane::NOF_SUBCARRIERS - plane_nsc;
      }
      // THE WINDOW MUST CONTAIN THE ALLOCATION. A bucket that cannot cover it would silently crop real subcarriers
      // away and hand the demodulator LLRs from the wrong data -- plausible numbers, wrong decoding. The runtime is
      // meant to choose the smallest bucket that fits; when it was handed one that does not, refusing is the only
      // honest answer and the degradation counter records it.
      const unsigned first_prb = narrow_first_prb(config);
      const unsigned last_prb  = narrow_last_prb(config);
      const unsigned ctx = 4 * NOF_SUBCARRIERS_PER_RB;
      const unsigned first_sc = first_prb * NOF_SUBCARRIERS_PER_RB;
      const unsigned need_first = (first_sc > ctx) ? (first_sc - ctx) : 0U;
      const auto     tail      = static_cast<unsigned>((last_prb + 1) * NOF_SUBCARRIERS_PER_RB) + ctx;
      const unsigned need_last = (tail < pusch_depth3_input_plane::NOF_SUBCARRIERS)
                                     ? tail
                                     : static_cast<unsigned>(pusch_depth3_input_plane::NOF_SUBCARRIERS);
      if ((need_last - need_first) > plane_nsc) {
        char detail[160];
        std::snprintf(detail, sizeof(detail),
                      "bundle covers %u subcarriers, the allocation needs %u; a wider bucket would fit",
                      plane_nsc, need_last - need_first);
        degrade_or_fail(depth3_degradation::plane_refused, codeword_buffer, notifier, grid, est_results, config, detail);
        return;
      }
    }

    metal::coreml_nn_engine::depth3_input input;
    input.plane           = reinterpret_cast<const uint16_t*>(input_plane.data());
    input.nof_subcarriers = pusch_depth3_input_plane::NOF_SUBCARRIERS;
    if (plane_nsc != pusch_depth3_input_plane::NOF_SUBCARRIERS) {
      // THE ROW IS THE ASSEMBLED STRIDE, which for a conditioned bundle is 16 channels (12 plus the four Qm
      // one-hots), not the contract's 12. Taking the constant here made the copy read the wrong number of values per
      // subcarrier and the forward pass died inside the engine with no message -- the third "it looks right" bug of
      // this feature, and the reason the stride now comes from the bundle.
      // THE SCRATCH BUFFERS ARE SIZED FOR THE BUCKET IN USE, not for the shared bundle. The first version allocated
      // them at bind time only when the SHARED bundle was narrow -- which it never is, 612 -- so a bucketed reception
      // copied a whole window into an empty vector. That does not fail loudly: it corrupts memory and the process dies
      // with no message at all, which is what "the bucket arm prints nothing" was.
      const size_t row    = static_cast<size_t>(pusch_depth3_input_plane::NOF_SYMBOLS) * plane_channels;
      const size_t llr_w  = static_cast<size_t>(plane_nsc) * pusch_depth3_input_plane::NOF_SYMBOLS * 8;
      const size_t noise_w = static_cast<size_t>(plane_nsc) * pusch_depth3_input_plane::NOF_SYMBOLS;
      const size_t ch_w    = static_cast<size_t>(plane_nsc) * 2 * pusch_depth3_input_plane::NOF_SYMBOLS;
      if (narrow_plane.size() != static_cast<size_t>(plane_nsc) * row) {
        narrow_plane.assign(static_cast<size_t>(plane_nsc) * row, 0);
      }
      if (narrow_llr.size() != llr_w) {
        narrow_llr.assign(llr_w, 0);
      }
      if (narrow_noise.size() != noise_w) {
        narrow_noise.assign(noise_w, 0);
      }
      if (narrow_channel.size() != ch_w) {
        narrow_channel.assign(ch_w, 0);
      }
      std::copy_n(input_plane.begin() + static_cast<size_t>(crop_lo) * row, static_cast<size_t>(plane_nsc) * row,
                  narrow_plane.begin());
      input.plane           = reinterpret_cast<const uint16_t*>(narrow_plane.data());
      input.nof_subcarriers = plane_nsc;
    }

    metal::coreml_nn_engine::depth3_output output;
    output.llr             = reinterpret_cast<uint16_t*>(plane_nsc == pusch_depth3_input_plane::NOF_SUBCARRIERS
                                                             ? llr_head.data() : narrow_llr.data());
    output.post_eq_noise   = reinterpret_cast<uint16_t*>(plane_nsc == pusch_depth3_input_plane::NOF_SUBCARRIERS
                                                             ? noise_head.data() : narrow_noise.data());
    // A BUNDLE WITHOUT THE REPORTING HEADS gets null here, and the engine does not bind what the model does not
    // declare (design section 22.2).
    // THE CHANNEL HEAD NEEDS THE SAME TREATMENT AS THE OTHER TWO. It was left pointing at the 612-wide buffer while
    // the model asked for a 396-wide array, and the engine answered with a plain "forward pass failed" -- the kind of
    // mismatch that reads as a broken model rather than a broken arm.
    output.ch_est          = !has_ch_est ? nullptr
                             : reinterpret_cast<uint16_t*>(plane_nsc == pusch_depth3_input_plane::NOF_SUBCARRIERS
                                                               ? channel_head.data() : narrow_channel.data());
    output.dmrs_noise      = has_dmrs_noise ? reinterpret_cast<uint16_t*>(&dmrs_noise_head) : nullptr;
    output.bits_per_symbol = qm;

    if (!computing_engine->predict_depth3(input, output)) {
      degrade_or_fail(depth3_degradation::forward_failed, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    // Scatter the narrow heads back to their absolute subcarrier positions. Out-of-window entries are left untouched:
    // nothing outside the allocation is ever read, and the allocation is always inside the window by construction.
    if (plane_nsc != pusch_depth3_input_plane::NOF_SUBCARRIERS) {
      const size_t llr_row = static_cast<size_t>(pusch_depth3_input_plane::NOF_SYMBOLS) * 8;
      const size_t n_row   = pusch_depth3_input_plane::NOF_SYMBOLS;
      std::copy_n(narrow_llr.begin(), static_cast<size_t>(plane_nsc) * llr_row,
                  llr_head.begin() + static_cast<size_t>(crop_lo) * llr_row);
      std::copy_n(narrow_noise.begin(), static_cast<size_t>(plane_nsc) * n_row,
                  noise_head.begin() + static_cast<size_t>(crop_lo) * n_row);
      if (has_ch_est) {
        const size_t c_row = static_cast<size_t>(2) * pusch_depth3_input_plane::NOF_SYMBOLS;
        std::copy_n(narrow_channel.begin(), static_cast<size_t>(plane_nsc) * c_row,
                    channel_head.begin() + static_cast<size_t>(crop_lo) * c_row);
      }
    }
    const auto segment_forwarded = std::chrono::steady_clock::now();
    // The engine's own reading of the `predictionFromFeatures` call, taken before anything else can overwrite it.
    // `forward` above is the ARM's segment and contains the engine's glue as well: the worker-thread hand-off, the
    // five `MLMultiArray` bindings and the feature provider. Printing both is what separates "our glue" from
    // "Core ML and the ANE", which are different problems with different fixes (design section 19.171).
    const auto predict_us = static_cast<unsigned>(computing_engine->last_predict_us());

    // ---- the reporting heads, next to the estimator's own numbers ---------------------------------
    // R6's acceptance is that the network's measurements agree with the classical estimator's to the order
    // of magnitude the design asks for, and the only way to know is to compute both on the same reception
    // and print them side by side. They are NOT published yet: the arm still forwards `estimate()` to the
    // classical estimator, so a measurement that is wrong would reach the scheduler with nothing to
    // compare it against. Measure first, then switch -- the order the rest of this work has used.
    report_measurements(reference_amplitude, est_results, config, model_scales);
    // The end of the reporting, so the delivery segment below measures delivery and nothing else.
    const auto segment_reported = std::chrono::steady_clock::now();

    // ---- gather, quantise, descramble, deliver ---------------------------------------------------
    if (!deliver(codeword_buffer, notifier, grid, est_results, config, qm, model_scales, reference_amplitude)) {
      degrade_or_fail(depth3_degradation::no_data_resource_elements, codeword_buffer, notifier, grid, est_results, config);
      return;
    }
    record_timing(segment_start, segment_assembled, segment_forwarded, segment_reported,
                  std::chrono::steady_clock::now(), predict_us);
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
                     std::chrono::steady_clock::time_point reported,
                     std::chrono::steady_clock::time_point delivered,
                     unsigned                               predict_us)
  {
    const auto micros = [](std::chrono::steady_clock::time_point from,
                           std::chrono::steady_clock::time_point to) {
      return static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(to - from).count());
    };
    // THE SLOW STATE IS ANNOUNCED, NOT DISCOVERED LATER (design section 19.374(c)). The engine picks its compute units
    // once, when the model loads, and on this rig that choice sometimes lands on the GPU path, where a reception costs 650
    // to 930 microseconds instead of 400 -- through-put survives but the leg runs about half as fast. Saying so after the
    // first sixty-four receptions is what turns a puzzling log into a line anyone can read; a real repair (reloading to
    // re-draw the placement) needs the model-binding path made re-entrant and is not done here.
    if (timing_count == 64) {
      std::array<unsigned, 64> sorted{};
      for (unsigned i = 0; i != 64; ++i) {
        sorted[i] = timings[3][i];
      }
      std::sort(sorted.begin(), sorted.end());
      const unsigned median_us = sorted[32];
      if (median_us >= 600) {
        fmt::print(stderr,
                   "[depth3-timing] SLOW STATE: the first 64 predictions took {} us at the median (the fast state is about "
                   "400). Core ML placed this model on the GPU path for this run; throughput survives, latency roughly "
                   "doubles. Not repair-able from here -- the placement is chosen at load.\n",
                   median_us);
      }
    }

    // One linear scan over at most nine widths, on the same path that already stamps five timestamps.
    for (unsigned slot = 0; slot != bucket_hits_.size(); ++slot) {
      if (bucket_hits_[slot].second == 0 || bucket_hits_[slot].first == bucket_in_use_) {
        bucket_hits_[slot].first = bucket_in_use_;
        ++bucket_hits_[slot].second;
        bucket_predict_us_[slot].first = bucket_in_use_;
        bucket_predict_us_[slot].second += predict_us;
        break;
      }
    }
    timings[0][timing_index] = micros(start, assembled);
    timings[1][timing_index] = micros(assembled, forwarded);
    // THE DELIVERY SEGMENT IS SPLIT (2026-10-23). It used to run from the end of the forward pass to the
    // hand-off and absorb both the measurement reporting and the gather/quantise/descramble, so the two could
    // not be separated -- measured at 65 to 108 microseconds per reception in the legs, which is the same order
    // as halving the network. `report` is now its own slot; `deliver` is the delivery alone.
    timings[2][timing_index] = micros(reported, delivered);
    timings[5][timing_index] = micros(forwarded, reported);
    timings[3][timing_index] = predict_us;
    window_widths[timing_index] = bucket_in_use_;
    // The TOTAL when the gate closed the segment, otherwise the submission alone: a bundle that declares no
    // trained range skips the gate and its results are read later, where nothing here can see it.
    timings[4][timing_index] = pending_estimate_open ? pending_estimate_call_us : pending_estimate_total_us;
    pending_estimate_open    = false;
    pending_estimate_total_us = 0;
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
    // `estimate` last: it is the CLASSICAL estimator's time, which happens before the three path segments, but
    // appending it keeps every existing field at the position its readers expect.
    static const char* const names[6] = {"plane", "forward", "deliver", "predict", "estimate", "report"};
    fmt::print(stderr, "[depth3-timing] {} forward(s), microseconds:", timing_count);
    // `predict` last on purpose: it is the PART of `forward` that Core ML and the ANE spend, so the two are read
    // together and their difference is the engine's glue.
    // The allocation mix behind the timing, so a reader never has to guess whether two runs are comparable.
    std::string mix;
    unsigned    shown = 0;
    for (const auto& entry : bucket_hits_) {
      if (entry.second == 0) {
        continue;
      }
      mix += fmt::format("{}{}x{}", (shown++ == 0) ? "" : " ", (entry.first == 0) ? 612U : entry.first, entry.second);
    }
    if (!mix.empty()) {
      fmt::print(stderr, "[depth3-timing]   allocation mix (subcarriers x receptions): {}\n", mix);
      // THE WINDOW'S OWN MIX (design section 19.386). The percentiles above come from the last TIMING_RING receptions,
      // while the line just printed is cumulative over the whole leg -- so a reader who compares them is comparing two
      // different populations, which is exactly how "fast" and "slow" legs came to look like a two-state mystery when
      // their per-width means were in fact identical. This second line is the mix that the percentiles were measured on.
      std::map<unsigned, unsigned> window_hits;
      const unsigned               windowed = std::min(timing_count, TIMING_RING);
      for (unsigned i = 0; i != windowed; ++i) {
        ++window_hits[window_widths[i]];
      }
      std::string window_mix;
      unsigned    window_shown = 0;
      for (const auto& entry : window_hits) {
        window_mix += fmt::format("{}{}x{}", (window_shown++ == 0) ? "" : " ", (entry.first == 0) ? 612U : entry.first,
                                  entry.second);
      }
      fmt::print(stderr, "[depth3-timing]   window mix, the {} receptions the percentiles came from: {} \n",
                 windowed, window_mix);
      std::string per_width;
      for (const auto& entry : bucket_predict_us_) {
        if (entry.second == 0) {
          continue;
        }
        // The mean predict time of THIS width, which is what separates "wider grants" from "slow engine".
        for (const auto& hits : bucket_hits_) {
          if (hits.first == entry.first && hits.second != 0) {
            per_width += fmt::format(" {}:{:.0f}us", (entry.first == 0) ? 612U : entry.first,
                                     static_cast<double>(entry.second) / static_cast<double>(hits.second));
            break;
          }
        }
      }
      if (!per_width.empty()) {
        fmt::print(stderr, "[depth3-timing]   mean predict per width:{} \n", per_width);
      }
    }
    for (unsigned segment = 0; segment != 6; ++segment) {
      std::array<unsigned, TIMING_RING> sorted = timings[segment];
      std::sort(sorted.begin(), sorted.begin() + sampled);
      const unsigned p99_index = std::min((sampled * 99) / 100, sampled - 1);
      fmt::print(stderr, " {} p50 {} p99 {} max {} |", names[segment], sorted[sampled / 2], sorted[p99_index],
                 sorted[sampled - 1]);
    }
    fmt::print(stderr, "\n");
    // THE EARLY MEASUREMENT'S OWN LEDGER, on the same exit line group as the timings: a leg that turned the
    // symbol hook on has to be able to say whether it ran and whether its answer was used, without a debug knob
    // (found on the first such flight, 2026-10-20: the switch was on, the counters were invisible).
    fmt::print(stderr,
               "[depth3-timing] early measurement: {} reception(s) measured ahead, {} refused, {} gate(s) took the "
               "cached number\n",
               prepared_receptions.load(),
               prepare_refused.load(),
               gate_from_prepare.load());
    if (prepare_cost_count != 0) {
      // Sort only the samples taken: the ring's untouched slots are zeros and would read as a p50 of zero.
      std::array<unsigned, 512> sorted = prepare_cost_us;
      const unsigned            n      = prepare_cost_count;
      std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
      fmt::print(stderr,
                 "[depth3-timing] early measurement cost on the receive thread: p50 {} p99 {} max {} us over {} "
                 "sample(s)\n",
                 sorted[n / 2],
                 sorted[std::min((n * 99) / 100, n - 1)],
                 sorted[n - 1],
                 n);
    }

  }

  /// The published SINR per reception: raw, and after the bundle's calibration. A ring, like the timing one, so a
  /// long leg reports percentiles instead of the last value.
  std::array<std::tuple<float, float>, TIMING_RING> published_sinr{};
  unsigned                                          sinr_index = 0;
  unsigned                                          sinr_count = 0;

  /// \brief Prints the percentiles of what the scheduler was told, raw against calibrated.
  ///
  /// One line per thousand receptions: enough for a flight to compare two legs without turning the log into data.
  void report_published_sinr()
  {
    const unsigned sampled = std::min(sinr_count, TIMING_RING);
    std::array<float, TIMING_RING> raw{};
    std::array<float, TIMING_RING> calibrated{};
    for (unsigned i = 0; i != sampled; ++i) {
      raw[i]        = std::get<0>(published_sinr[i]);
      calibrated[i] = std::get<1>(published_sinr[i]);
    }
    std::sort(raw.begin(), raw.begin() + sampled);
    std::sort(calibrated.begin(), calibrated.begin() + sampled);
    const unsigned p10 = sampled / 10;
    const unsigned p90 = std::min((sampled * 9) / 10, sampled - 1);
    fmt::print(stderr,
               "[depth3-sinr] {} reception(s): PUBLISHED SINR raw p10 {:.2f} p50 {:.2f} p90 {:.2f} dB | calibrated "
               "p10 {:.2f} p50 {:.2f} p90 {:.2f} dB | calibration {} (this is the number the scheduler acts on)\n",
               sinr_count, raw[p10], raw[sampled / 2], raw[p90], calibrated[p10], calibrated[sampled / 2],
               calibrated[p90], any_calibrated ? "APPLIED" : "not declared");
  }

  std::array<std::array<unsigned, TIMING_RING>, 6> timings{};
  /// The bucket width each sampled reception used, so the report can say which mix its percentiles were measured over.
  std::array<unsigned, TIMING_RING> window_widths{};
  unsigned                                         timing_index = 0;
  unsigned                                         timing_count = 0;
  /// The classical estimator's cost for the reception in flight, carried from `estimate()` to `record_timing()`
  /// because the two are separate entry points and the ring is advanced by the second one. `..._total_us` is
  /// closed by the domain gate (submission plus device completion), `..._call_us` is the submission alone.
  std::chrono::steady_clock::time_point estimate_started{};
  unsigned                              pending_estimate_call_us  = 0;
  unsigned                              pending_estimate_total_us = 0;
  bool                                  pending_estimate_open     = false;

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
    // Nothing to compare when the bundle carries no reporting heads: the whole point of such a bundle is that these
    // numbers now come from the shared measurement layer (design section 22.3).
    if (!has_ch_est || !has_dmrs_noise) {
      return;
    }
    const depth3_measurements net = measure_depth3(span<const __fp16>(input_plane),
                                                   plane_channels,
                                                   span<const __fp16>(channel_head),
                                                   reference_amplitude,
                                                   static_cast<float>(dmrs_noise_head),
                                                   scales.a_noise,
                                                   scales.b_noise,
                                                   span<ul_pilot_sample>(pilot_scratch));
    if (net.nof_pilot_re == 0) {
      fmt::print(stderr,
                 "[depth3-measure] nothing measured: the plane (width {}) carries no pilot resource element, or it "
                 "is not the shape the measurement expects\n",
                 plane_channels);
      return;
    }
    const unsigned port        = config.rx_ports.empty() ? 0 : config.rx_ports.front();
    const float    classic_nv  = est_results.get_noise_variance(port);
    // R6's runtime evidence, and the reason it is counted HERE rather than declared by the artifact: the rule that
    // a bundle may only be used with `net` after passing a known-truth check has to be checkable on a LIVE run,
    // and the classical estimator computes the same two quantities independently on every reception anyway
    // (measured 2026-10-17: the conditioned bundle agrees to 0.10 dB and one superseded per-modulation bundle is
    // 2.5 dB out, which is exactly the state a declaration would have hidden). A reception counts as agreeing
    // when both RSRP and EPRE are within R6_AGREEMENT_DB of the estimator's values.
    // A PURE RUN HAS NO INDEPENDENT ESTIMATOR TO COMPARE AGAINST (design section 19.263): its results object is the
    // arm's own empty one, so "the network agrees with the estimator" would be comparing the arm with nothing. The
    // line is kept -- the other numbers in it are the arm's own and worth printing -- but the count is not taken,
    // and the summary says why instead of reporting a run of agreements that never happened.
    const float classic_rsrp = fallback_enabled ? est_results.get_rsrp(port, 0) : 0.0F;
    const float classic_epre = fallback_enabled ? est_results.get_epre(port) : 0.0F;
    if ((classic_rsrp > 0.0F) && (classic_epre > 0.0F)) {
      const float rsrp_delta = std::abs(convert_power_to_dB(net.rsrp_lin) - convert_power_to_dB(classic_rsrp));
      const float epre_delta = std::abs(convert_power_to_dB(net.epre_lin) - convert_power_to_dB(classic_epre));
      if ((rsrp_delta <= R6_AGREEMENT_DB) && (epre_delta <= R6_AGREEMENT_DB)) {
        ++measurements_agreeing;
      }
      else {
        ++measurements_disagreeing;
      }
    }
    fmt::print(stderr,
               "[depth3-measure] reception {} (Qm={} {}) over {} pilot RE(s): epre net={:.2f} classic={:.2f} dB "
               "| rsrp net={:.2f} classic={:.2f} dB | snr net={:.2f} classic={:.2f} dB | "
               "noise net={:.4e} classic={:.4e} residual={:.4e} plane_units={:.4e} ({} residual(s)) | "
               "snr plane-only={:.2f} dB\n",
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
               classic_nv,
               net.residual_noise_variance_lin,
               net.residual_plane_units_lin,
               net.nof_residuals,
               convert_power_to_dB(net.residual_snr_lin));
    // The two counts travel with every sampled line, so a run's own log says whether its network measurements are
    // inside the tolerance the adoption checklist asks for -- and a run that never agrees cannot be read as one
    // that did.
    if (fallback_enabled) {
      fmt::print(stderr,
                 "[depth3-measure]   RSRP/EPRE against the independent estimator: {} within {} dB, {} outside "
                 "(since the run started)\n",
                 measurements_agreeing, R6_AGREEMENT_DB, measurements_disagreeing);
    }
    else {
      fmt::print(stderr,
                 "[depth3-measure]   RSRP/EPRE against the independent estimator: n/a -- this is a PURE run, the "
                 "classical estimator is not called, so there is nothing to compare against\n");
    }
  }

  /// \brief What a reception does when the arm cannot compute it: fall back, or fail.
  ///
  /// THE FALLBACK IS SCAFFOLDING (design section 19.245). The comparison this receiver exists for is a PURE
  /// network path against a PURE classical path, back to back, so the final configuration has no classical chain
  /// behind it to fall back to. Development keeps the fallback, one environment variable turns it off, and this
  /// dispatcher is the single place the difference is made: every reason that would have degraded reaches it.
  void degrade_or_fail(depth3_degradation                  reason,
                       pusch_codeword_buffer&              codeword_buffer,
                       pusch_demodulator_notifier&         notifier,
                       const resource_grid_reader&         grid,
                       const dmrs_pusch_estimator_results& est_results,
                       const pusch_demodulator::configuration& config,
                       const char*                         detail = nullptr)
  {
    if (fallback_enabled) {
      degrade(reason, codeword_buffer, notifier, grid, est_results, config, detail);
      return;
    }
    fail(reason, codeword_buffer, notifier, config, detail);
  }

  /// \brief A reception the arm could not compute, with nothing to fall back to.
  ///
  /// It is reported, counted under its own reason, and ERASED: every soft bit is written as zero, which tells the
  /// decoder that nothing is known about that bit rather than pretending to a value. The reception then fails its
  /// CRC like any other failed reception, which is what a pure network path must do -- and what makes the pure
  /// legs comparable with the classical ones instead of quietly decoding somewhere else (design section 19.245).
  void fail(depth3_degradation                  reason,
            pusch_codeword_buffer&              codeword_buffer,
            pusch_demodulator_notifier&         notifier,
            const pusch_demodulator::configuration& config,
            const char*                         detail)
  {
    ++degradations[static_cast<unsigned>(reason)];
    const unsigned count = degradations[static_cast<unsigned>(reason)];
    if ((count == 1) || ((count % 1000) == 0)) {
      fmt::print(stderr,
                 "[pusch_receiver] AI depth-3: reception FAILED (no fallback): {}{}{} ({} so far). This build runs "
                 "the pure network path: the reception is erased and will not decode.\n",
                 to_string(reason),
                 (detail != nullptr) ? ": " : "",
                 (detail != nullptr) ? detail : "",
                 count);
    }
    ++failed_receptions;

    // The allocated subcarriers, the same walk `deliver` makes: the erasure has to cover exactly the soft bits
    // the real path would have written, or the decoder reads a block that does not exist.
    allocated.clear();
    for (unsigned k = 0, nof_subc = config.rb_mask.size() * NOF_SUBCARRIERS_PER_RB;
         k != nof_subc && k != pusch_depth3_input_plane::NOF_SUBCARRIERS;
         ++k) {
      if (config.rb_mask.test(k / NOF_SUBCARRIERS_PER_RB)) {
        allocated.push_back(k);
      }
    }
    const unsigned qm = get_bits_per_symbol(config.modulation);
    if (!allocated.empty() && (qm != 0)) {
      // THE BLOCKS HAVE TO BE HANDED OVER, NOT ONLY FILLED (found on the third pure leg, 2026-10-20): a view from
      // get_next_block_view is storage, and on_new_block is what passes the block to the decoder. Writing zeros
      // into the view and never handing it over left the decoder with nothing to decode, so it never reported --
      // the reception kept its HARQ buffer and the uplink stalled within a couple of minutes, exactly as the
      // missing on_end_codeword() had done before it.
      //
      // The scrambling sequence is the one the real path would have used, generated exactly as deliver() generates
      // it: zeros survive the sign flips, but the block still has to carry the right sequence, because that is
      // what the buffer forwards.
      unsigned c_init = to_underlying(config.rnti) * pow2(15) + config.n_id;
      if (config.n_rapid.has_value()) {
        c_init = to_underlying(config.rnti) * pow2(16) + *config.n_rapid * pow2(10) + config.n_id;
      }
      descrambler->init(c_init);
      for (unsigned symbol = 0; symbol != pusch_depth3_input_plane::NOF_SYMBOLS; ++symbol) {
        if (config.dmrs_symb_pos.test(symbol)) {
          continue;
        }
        const unsigned             nof_softbits = allocated.size() * qm;
        span<log_likelihood_ratio> codeword     = codeword_buffer.get_next_block_view(nof_softbits);
        if (codeword.size() != nof_softbits) {
          break;
        }
        // Zero is the erasure: no information, not a guess.
        std::fill(codeword.begin(), codeword.end(), log_likelihood_ratio{0});
        static_bit_buffer<pusch_constants::MAX_NOF_BITS_PER_OFDM_SYMBOL> scrambling_seq(nof_softbits);
        descrambler->generate(scrambling_seq);
        codeword_buffer.on_new_block(codeword, scrambling_seq);
      }
    }
    // THE END OF THE CODEWORD IS WHAT RELEASES THE HARQ BUFFER (found on the pure leg, 2026-10-20): the decoder
    // runs on this call, reports the CRC and the reception's buffer goes back to the pool. Erasing without it left
    // every failed reception holding its buffer -- 21 of 1008 were still locked at shutdown, the stop waited for
    // them until it was SIGKILLed, and the uplink stalled after enough failures.
    codeword_buffer.on_end_codeword();
    pusch_demodulator_notifier::demodulation_stats stats;
    notifier.on_end_stats(stats);
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
               const resource_grid_reader&            grid,
               const dmrs_pusch_estimator_results&    est_results,
               const pusch_demodulator::configuration& config,
               unsigned                               qm,
               const depth3_scales&                   scales,
               float                                  reference_amplitude)
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
        // `10^y` as `exp2(y * log2(10))`: the same function, but `std::pow` is the general routine and costs about
        // 40 ns per call here, which dominated this loop (7344 resource elements per reception, and it measured
        // 0.29 ms of deliver's 0.47-0.60 ms -- design section 19.165). exp2 is the hardware path, accuracy is one
        // ulp, and it touches only the noise/SINR path: the LLR path never sees this value, which is why the soft
        // bits stay bit-identical.
        static constexpr float LOG2_OF_10 = 3.32192809489F;
        const float noise_variance = std::exp2((scales.a_noise * noise_shape + scales.b_noise) * LOG2_OF_10);
        if (std::isfinite(noise_variance)) {
          noise_sum += static_cast<double>(noise_variance) * qm;
          noise_count += qm;
        }

        for (unsigned bit = 0; bit != qm; ++bit) {
          const float logit = static_cast<float>(
              reinterpret_cast<const __fp16*>(llr_head.data())[(static_cast<size_t>(k) * pusch_depth3_input_plane::NOF_SYMBOLS + symbol) * llr_stride_by_qm[qm] +
                                                                                  bit]);
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
    // WHICH MEASUREMENT IS PUBLISHED, and the default is the classical one: R6's switch exists to be measured
    // before it is trusted, and the measurement it replaces is one the scheduler acts on. `net` takes the value
    // from the noise head (the arm's own), `classic` from the estimator's residual -- the same quantity the
    // classical demodulator publishes, from the same place, so a `classic` run is comparable with a classical one
    // bit for bit.
    // THE POST-EQUALISATION NOISE IS THE ARM'S OWN IN EITHER MODE, and that is not a choice: the classical value
    // is the classical EQUALIZER's per-resource-element residual, which this arm does not compute because it does
    // not run that equalizer. Substituting the estimator's noise variance instead -- which is what this branch
    // first did -- publishes a DIFFERENT QUANTITY under the same name, measured at +5.6 dB on one capture
    // (31.61 against 25.98 dB), because it leaves out the equalizer's gain. A knob may not silently change what
    // the scheduler acts on, so the arm publishes its own post-eq noise in both modes; what `net` adds is the
    // DOMAIN GATE below, and what R6 will add is the rest of the set (RSRP, EPRE) once the estimator's results
    // can be synthesised from head 3 (design section 19.98's largest performance item).
    // WHICH NOISE IS PUBLISHED (design section 22.3). With the reporting heads, it is the mean of head 2's
    // per-resource-element noise, exactly as before. WITHOUT them, the arm still has head 2 -- it is what scales the
    // soft bits -- but the published quantity has to come from the shared measurement layer, which is the whole point
    // of the split: `1/SINR` in the chain's convention, from the plane's residual.
    float published_noise = (noise_count != 0) ? static_cast<float>(noise_sum / static_cast<double>(noise_count)) : 0.0F;
    if (publish_from_shared_layer()) {
      published_noise = (plane_measurement_valid && (plane_measurement.snr_lin > 0.0F))
                            ? (1.0F / plane_measurement.snr_lin)
                            : 0.0F;
    }
    if (published_noise > 0.0F) {
      const float mean_noise = published_noise;
      pusch_demodulator_notifier::demodulation_stats stats;
      // THE PUBLISHED SINR IS THE ARM'S OWN IN EITHER REPORTING MODE (see the note above), and that publication needs
      // the bundle's calibration: measured against the CLASSICAL ARM's own publication on the same receptions, the raw
      // value reads 1.2-3.4 dB low and the error grows with SNR, so it is a slope error and no constant factor fixes
      // it (design section 19.172). The correction is the artifact's declaration, applied here and only to what the
      // scheduler reads -- the soft bits are scaled by `s_llr`, never by this map, so the decisions cannot move.
      float published_dB = -convert_power_to_dB(mean_noise);
      const float raw_dB = published_dB;
      if (publish_from_shared_layer()) {
        // ONE CONSTANT, BUNDLE-INDEPENDENT (design section 22.3), and the artifact's own calibration is NOT applied:
        // that affine was fitted to the HEAD's convention, and putting it on the shared layer's number would be
        // calibrating one convention with another's ruler -- which is what the run measured before this branch was
        // ordered first (34.04 dB published against the heads' 30.17 on the same reception).
        const shared_calibration& cal = shared_sinr_cal[qm];
        published_dB                  = (cal.slope * published_dB) + cal.offset;
      }
      else if (sinr_cal_by_qm[qm].has) {
        published_dB = (sinr_cal_by_qm[qm].slope * published_dB) + sinr_cal_by_qm[qm].offset;
      }
      std::get<0>(published_sinr[sinr_index]) = raw_dB;
      std::get<1>(published_sinr[sinr_index]) = published_dB;
      sinr_index                              = (sinr_index + 1) % TIMING_RING;
      ++sinr_count;
      // WHAT THE SCHEDULER ACTS ON, reported on its own cadence, because a flight has no other way to read it: the
      // `[depth3-measure]` line compares the arm's plane-window estimate against the ESTIMATOR's SNR, which is a
      // different quantity and, on the live legs, disagrees in BOTH directions (measured: -8.3 dB on one reception,
      // +5.0 on another) -- that comparison told us nothing about the published number and once misled the design
      // record (section 19.172(a)). This line is the published value itself.
      if ((sinr_count == 1) || ((sinr_count % 1000) == 0)) {
        report_published_sinr();
      }
      // A SUPPRESSED MEASUREMENT IS NOT PUBLISHED (design section 19.272): the scheduler keeps its previous value
      // rather than receiving an extrapolation from outside the bundle's trained range. The soft bits are unaffected
      // -- they were delivered before this point.
      if (!publish_suppressed) {
        stats.sinr_dB.emplace(published_dB);
      }

      // R6: RSRP and EPRE from the reporting heads, in the same window and through the same call as the SINR.
      // ONLY UNDER `net`: under `classic` the chain's measurements are the classical chain's, and these two would
      // otherwise replace the estimator's numbers with the network's without the operator asking (design section
      // 19.123; the SINR is a separate case, because an arm that does not run the classical equalizer has no
      // other post-equalisation noise to publish -- section 19.116).
      if (reporting_source == "net" && !publish_suppressed && publish_from_shared_layer()) {
        // NO CHANNEL HEAD (design section 22.3): RSRP is the pilots' power minus the noise they sit on, both from the
        // shared measurement layer. Validated against the classical chain before any artifact may be the default.
        if (plane_measurement_valid && (plane_measurement.epre_lin > plane_measurement.noise_lin)) {
          stats.rsrp_dB.emplace(convert_power_to_dB(plane_measurement.epre_lin - plane_measurement.noise_lin));
        }
        if (plane_measurement_valid) {
          stats.epre_dB.emplace(convert_power_to_dB(plane_measurement.epre_lin));
        }
      }
      else if (reporting_source == "net" && !publish_suppressed) {
        // THE HEADS ONLY. The plane half was measured by the gate, before the forward pass, and is reused here:
        // recomputing it would walk the plane a second time and, worse, let the two numbers drift (19.263). When
        // the gate did not run (no declared range, or the estimator gate) it is measured now, exactly as before.
        const depth3_plane_measurements& plane_half =
            plane_measurement_valid
                ? plane_measurement
                : (plane_measurement = measure_depth3_plane(span<const __fp16>(input_plane),
                                                            plane_channels,
                                                            reference_amplitude,
                                                            span<ul_pilot_sample>(pilot_scratch)));
        const depth3_measurements net = measure_depth3_heads(plane_half,
                                                             span<const __fp16>(input_plane),
                                                             plane_channels,
                                                             span<const __fp16>(channel_head),
                                                             reference_amplitude,
                                                             static_cast<float>(dmrs_noise_head),
                                                             scales.a_noise,
                                                             scales.b_noise);
        if (net.nof_pilot_re != 0) {
          stats.rsrp_dB.emplace(convert_power_to_dB(net.rsrp_lin));
          stats.epre_dB.emplace(convert_power_to_dB(net.epre_lin));
          ++published_measurements;
          // Said out loud, and not only because the two sources now agree to 0.04 dB: a number that arrives at the
          // scheduler from the network has to be identifiable as such from the log alone.
          if ((published_measurements == 1) || ((published_measurements % 1000) == 0)) {
            fmt::print(stderr,
                       "[pusch_receiver] reporting source net: published measurement {} from the network's heads "
                       "(rsrp {:.2f} dB, epre {:.2f} dB)\n",
                       published_measurements, *stats.rsrp_dB, *stats.epre_dB);
          }
        }
      }
      notifier.on_end_stats(stats);
    }
    if (!domain_by_qm[qm].has) {
      if (!warned_unverified_domain) {
        // Until the bundle declares the SNR range its reporting heads were trained on, nothing here can tell a
        // measurement the model is entitled to make from one it extrapolated. Said ONCE per arm, on stderr, so a
        // `net` run is never read as a validated switch: the number reaches the scheduler either way.
        warned_unverified_domain = true;
        fmt::print(stderr,
                   "[pusch_receiver] the bound bundle declares no trained SNR range (depth3_snr_range): this arm "
                   "cannot tell a measurement inside the model's domain from one outside it, so the "
                   "post-equalisation SINR it publishes is gated by nothing. Measure it against known truth "
                   "before trusting it, or export a bundle that declares the range (design sections 19.114, "
                   "19.115, 19.117).\n");
      }
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
  /// \brief The narrow-band bucket set, ascending (model_network_design section 49.9), and its engines.
  ///
  /// A reception uses the SMALLEST bucket that fits its allocation plus four PRB of context, so "it does not fit" is
  /// impossible while the 612-wide bucket is in the set -- that one is the whole plane. The set is bound once, from the
  /// OCUDU_DEPTH3_BUCKETS environment variable, and every bucket holds the same weights.
  std::vector<unsigned>                                   narrow_buckets;
  std::vector<std::unique_ptr<metal::coreml_nn_engine>>   narrow_engines;
  std::array<depth3_scales, 9>                      scales_by_qm;
  /// True when at least one model bound: the arm will compute for the modulations it covers.
  bool                                              any_model_bound = false;
  /// The MODULATION-CONDITIONED contract is bound: one engine serves every modulation it declares and the plane
  /// carries the four conditioning channels. The LLR head's stride is NOT arm-wide but per modulation
  /// (`llr_stride_by_qm`), because a per-modulation slot can share the arm with a conditioned fallback.
  /// R6's knob, as resolved by the configuration: `classic` (the estimator's measurements, the default) or
  /// `net` (this arm's reporting heads). It is a string here for the same reason it is one at the command line:
  /// the value is meant to be read back in a log.
  std::string                                       reporting_source = "classic";
  /// Whether a reception the arm cannot compute falls back to the classical chain. ON during development, OFF
  /// for the pure comparison (design section 19.245): `OCUDU_DEPTH3_FALLBACK=none` turns it off, and then every
  /// degradation becomes a counted, erased failure instead of a quiet switch.
  bool                                              fallback_enabled = true;
  /// Receptions that failed for want of a fallback, i.e. that the arm could not compute and did not hand over.
  unsigned                                          failed_receptions = 0;
  /// Receptions prepare() measured ahead of the forward pass, and those it could not measure. Atomic because the
  /// hint runs on the receive thread while the receptions run on the PUSCH executor (design section 19.268).
  static std::atomic<unsigned>                      prepared_receptions;
  static std::atomic<unsigned>                      prepare_refused;
  /// Receptions whose gate took its measurement from prepare()'s cache instead of measuring the plane again.
  static std::atomic<unsigned>                      gate_from_prepare;
  /// \brief True when this reception's measurement is outside the bundle's trained range and must not be published.
  ///
  /// The reception is still computed: what the range governs is the REPORT, not the soft bits (design section
  /// 19.272). Reset at the start of every reception.
  bool                                              publish_suppressed = false;
  /// Receptions whose measurement was withheld for being outside the declared range.
  static std::atomic<unsigned>                      measurements_suppressed;
  /// \brief True when the early measurement must assemble the WHOLE plane, as it did before section 19.275.
  ///
  /// It exists for one purpose: an A/B inside a SINGLE environment. Comparing two flights that measured the same
  /// quantity on different days cannot separate a 20 us effect from the scatter between batches, which is exactly the
  /// mistake this project made once already (19.276). With this switch a leg toggles the two modes and the difference
  /// is the effect.
  bool                                              prepare_full_plane = false;
  /// \brief What the hint costs the receive thread, in microseconds (design section 19.275).
  ///
  /// Recorded so the cost of running the measurement three symbols early is MEASURED on every leg instead of inferred
  /// from a difference between two flights, which is how it was estimated wrongly the first time.
  static std::array<unsigned, 512>                  prepare_cost_us;
  static unsigned                                   prepare_cost_index;
  static unsigned                                   prepare_cost_count;
  /// \brief The plane half of this reception's measurement, taken by the gate and REUSED by the publication.
  ///
  /// The gate measures before the forward pass (that is the point of having the shared layer ahead of the network)
  /// and the publication needs the same numbers afterwards. Measuring twice was both a waste and a chance for the
  /// two to drift: the scheduler must judge on the number the gate judged on (design section 19.263).
  depth3_plane_measurements                         plane_measurement;
  bool                                              plane_measurement_valid = false;
  /// True when this run never calls the classical estimator at all (the pure configuration).
  bool                                              estimate_skipped    = false;
  /// The empty results a pure run hands the chain as its completion signal.
  depth3_empty_estimator_results                    empty_est_results;
  /// Which measurement decides the domain gate, read once from the environment at construction:
  ///   "estimator" -- the classical estimator's SNR, the behaviour before R7/S2 (the default, so this change
  ///                  is inert until a flight asks for it);
  ///   "net"       -- the arm's own plane-only residual SNR, which needs neither the estimator nor a forward
  ///                  pass. That is the measurement S2 replaces the gate with (design sections 19.234, 20.4).
  std::string                                       domain_gate = "estimator";
  /// Receptions the plane-based gate has measured, for its sampled report.
  unsigned                                          gate_samples = 0;
  /// \brief What prepare() measured for a slot, before the network ran.
  ///
  /// The measurements need the DM-RS symbols only, so a caller that sees them arrive can have the gate's answer
  /// ready before the slot ends (design section 19.241). Written by prepare(), read by demodulate(), hence the
  /// mutex: the two happen on different threads. A slot that is not in the ring is simply measured in
  /// demodulate() as before, so the cache is a hint and never a requirement.
  struct slot_measurement {
    slot_point slot;
    float      residual_snr_lin = 0.0F;
    float      cfo_cycles       = 0.0F;
    unsigned   nof_pilot_re     = 0;
    bool       valid            = false;
  };
  static constexpr unsigned MEASURE_RING = 64;
  /// \brief The early measurements, SHARED BY EVERY ARM INSTANCE (design section 19.270).
  ///
  /// WHY IT CANNOT BE A MEMBER. The PUSCH dependencies -- and with them the arms -- come from a pool, so the arm that
  /// receives the hint and the arm that processes the reception are not necessarily the same object: a per-instance
  /// cache is a cache nobody reads, which is what the first working hint measured (the forwarding line named the AI
  /// arm, the ledger stayed at zero because the instance that processed the slot never saw the hint). The reported
  /// quantity is a SLOW variable of the cell, not of an object, so the registry is process-wide and keyed by slot.
  static std::array<slot_measurement, MEASURE_RING> measured;
  static unsigned                                   measured_next;
  static std::mutex                                 measured_mutex;
  /// The plane prepare() assembles into, and the assembler that fills it: both separate from the ones demodulate()
  /// uses, because the two run on different threads and neither may read the other's buffer.
  std::vector<__fp16>                             prepare_plane = std::vector<__fp16>(
      static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) * pusch_depth3_input_plane::NOF_SYMBOLS * 16U);
  /// \brief The assembler prepare() fills its own plane with. Built when the bundle is bound, with the BUNDLE's plane
  /// width: a conditioned bundle writes 16 channels, and an assembler left at the contract's 12 would fill a narrower
  /// plane than the one the measurement then walks -- which read as "no pilot" on every hinted reception (design
  /// section 19.273).
  std::unique_ptr<pusch_depth3_input_plane>       prepare_assembler;
  /// The shared measurement layer's scratch for prepare(), for the same reason.
  std::vector<ul_pilot_sample>                    prepare_scratch = std::vector<ul_pilot_sample>(
      static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) * pusch_depth3_input_plane::NOF_SYMBOLS);
  /// Room for one pilot sample per resource element, so measuring a reception allocates nothing. The shared
  /// measurement layer fills it from the plane; a bigger plane than this is measured up to this many pilots.
  std::vector<ul_pilot_sample>                      pilot_scratch = std::vector<ul_pilot_sample>(
      static_cast<size_t>(pusch_depth3_input_plane::NOF_SUBCARRIERS) * pusch_depth3_input_plane::NOF_SYMBOLS);
  /// \brief The gate's time filter: the reported quantity is a SLOW variable, so a narrow allocation is judged
  /// on the pilots the arm has seen over the recent past rather than on its own few resource elements.
  ///
  /// WHY IT EXISTS (design section 19.237). The plane residual needs enough pilots to be trustworthy: with three
  /// or four PRBs it read 29.45 dB where two independent classical measures agreed on about 8 dB, because a
  /// second-order frequency difference over a flat channel finds no noise. Falling back to the classical
  /// estimator there would defeat R7, and the reported quantity does not change much between slots -- so the
  /// arm averages the residual and the pilot power over time instead, which is exactly the timescale on which
  /// the number it is deciding about is defined.
  ///
  /// Weighted by pilot count, so a narrow reception contributes what it measured and no more. THE COLD-START
  /// RULE is deliberate: until enough pilots have been seen the gate ACCEPTS, because the alternative is to
  /// refuse the first receptions of every leg on an estimate that has not warmed up -- and an operating point
  /// that is out of range persists over many slots, which is the same argument the filter rests on.
  struct gate_history {
    static constexpr double  DECAY          = 0.98;   // per reception: ~50 receptions of memory
    static constexpr double  MIN_PILOTS     = 200.0;  // below this the filter is not trusted yet
    double                   signal_sum     = 0.0;
    double                   noise_sum      = 0.0;
    double                   weight_sum     = 0.0;

    void note(const depth3_measurements& plane_only)
    {
      // The two sums are kept apart, not averaged as SNRs: averaging SNR linearly overweights the loud slots.
      const double weight = static_cast<double>(plane_only.nof_pilot_re);
      const double signal = static_cast<double>(plane_only.residual_snr_lin) *
                            static_cast<double>(plane_only.residual_plane_units_lin);
      signal_sum = DECAY * signal_sum + weight * signal;
      noise_sum  = DECAY * noise_sum + weight * static_cast<double>(plane_only.residual_plane_units_lin);
      weight_sum = DECAY * weight_sum + weight;
    }

    bool warm() const { return (weight_sum >= MIN_PILOTS) && (noise_sum > 0.0) && (signal_sum > 0.0); }
    float snr_db() const { return convert_power_to_dB(static_cast<float>(signal_sum / noise_sum)); }
  };
  gate_history                                      gate_history;

  /// The measurements prepare() took for this slot, if it took any.
  /// Whether the publication comes from the shared measurement layer rather than from the artifact's heads.
  bool publish_from_shared_layer() const { return ignore_reporting_heads || !has_ch_est || !has_dmrs_noise; }

  std::optional<slot_measurement> prepared_measurement(slot_point slot)
  {
    static std::atomic<unsigned> lookup_diagnostics{0};
    if (lookup_diagnostics.fetch_add(1, std::memory_order_relaxed) < 3) {
      unsigned valid = 0;
      unsigned long long nearest = 0;
      {
        std::lock_guard<std::mutex> lock(measured_mutex);
        for (const slot_measurement& entry : measured) {
          if (entry.valid) {
            ++valid;
            nearest = entry.slot.count();
          }
        }
      }
      fmt::print(stderr, "[depth3-cache] lookup slot={} (valid entries={}, last stored={})\n",
                 slot.count(), valid, nearest);
    }
    std::lock_guard<std::mutex> lock(measured_mutex);
    for (const slot_measurement& entry : measured) {
      if (entry.valid && (entry.slot == slot)) {
        return entry;
      }
    }
    return std::nullopt;
  }
  /// Report EVERY reception instead of the sampled cadence. For the offline measurement of the offset between
  /// the plane's residual and the estimator's SNR (design section 19.236); off unless asked for.
  bool                                              gate_report = false;
  /// How long after bind the arm refuses to compute (0 = never refuse). See `du_low_config.h` for the measurement
  /// that motivated it: attach is where the arm's occupancy was observed to cost the phone its ACK path.
  std::chrono::seconds                              startup_grace{};
  std::chrono::steady_clock::time_point             bound_at{};
  bool                                              grace_ended_reported = false;
  /// Whether the one-time note about an undeclared measurement domain has been printed for this arm.
  bool                                              warned_unverified_domain = false;
  /// The plane width the first bound model declared (for the startup line); 0 when undeclared or nothing bound.
  unsigned                                          bound_nsc_ = 0;
  /// Receptions whose RSRP and EPRE were published from the network's heads (R6's switch, `net` only).
  unsigned                                          published_measurements = 0;
  /// Sampled receptions whose network RSRP and EPRE agreed with the independent estimator's within
  /// \ref R6_AGREEMENT_DB, and those that did not. R6's live evidence; see `report_measurements`.
  unsigned                                          measurements_agreeing    = 0;
  unsigned                                          measurements_disagreeing = 0;
  bool                                              conditioned   = false;
  /// The main head's stride, per modulation, as the bundle that computes that modulation DECLARES it: its head
  /// width divided by the slot's symbols. Qm for a per-modulation bundle, the widest Qm for a conditioned one.
  /// Zero means nothing is bound for that modulation, and a zero stride must never be used to read a head.
  std::array<unsigned, 9>                           llr_stride_by_qm{};
  /// The declared measurement domain of the model that serves each modulation, in dB: what the reporting heads
  /// were trained on. `has` is false when that bundle declares none, and a reception is then never refused for
  /// being outside a domain nobody stated -- it is warned about once instead.
  struct declared_domain {
    bool  has = false;
    float min_db = 0.0F;
    float max_db = 0.0F;
  };
  std::array<declared_domain, 9>                    domain_by_qm{};
  unsigned                                          plane_channels = pusch_depth3_input_plane::NOF_CHANNELS;
  /// \brief Whether the bound artifact carries the two reporting heads (design section 22.2).
  ///
  /// Absent, the publication is computed by the shared measurement layer from the plane alone: RSRP as the pilots'
  /// power minus the residual, and the SINR from the residual SNR converted once, bundle-independently.
  bool                                              has_ch_est     = true;
  // THE WIDTH THIS RECEPTION ACTUALLY USED (0 = the full-band bundle). Recorded so a timing line can say WHICH
  // MIX of allocations produced it: two legs of a comparison can bind the same bucket set, run the same model and
  // still differ in median forward time purely because one carried wider grants -- measured 2026-10-23, where that
  // ambiguity turned an A/B into "inconclusive" and cost a flight.
  unsigned                                          bucket_in_use_ = 0;
  std::array<std::pair<unsigned, unsigned>, 9>      bucket_hits_{};
  // PREDICT TIME PER WIDTH. The engine-level benchmark says a narrow call costs 193 microseconds for the two-head bundle
  // and 206 for the four-head one, yet the air legs showed 758 against 380 -- so the question is not which model is
  // faster but what the engine does per width in situ. Summing predict time into the same nine slots answers it in one
  // leg instead of another flight of guesswork.
  std::array<std::pair<unsigned, unsigned long long>, 9> bucket_predict_us_{};
  bool                                              has_dmrs_noise = true;
  /// \brief The conversion from the shared layer's residual SNR to the chain's post-equalisation convention.
  ///
  /// BUNDLE-INDEPENDENT by construction (design section 22.3): an affine between two MEASUREMENT CONVENTIONS, fitted
  /// once against the classical chain, then valid for every artifact -- which is what the per-bundle
  /// `depth3_sinr_cal` could not be.
  ///
  /// Fitted on 313 attach receptions with the plane gate active (design section 19.279), in the direction that means
  /// something: `classic = a * shared + b`, so the receiver publishes `(shared - b) / a`. Regressing the other way
  /// (`shared = a * classic + b`) attenuates the slope through the noise in the reference, which is exactly how this
  /// fit first read 0.649 at 256QAM before the direction was corrected.
  ///
  /// QPSK and 16QAM have too few receptions in that set to fit; they keep the identity until a set that covers them
  /// is measured, and the arm says so when it uses them (19.279).
  struct shared_calibration {
    float slope  = 1.0F;
    float offset = 0.0F;
  };
  /// Indexed by bits per symbol (QPSK 2, 16QAM 4, 64QAM 6, 256QAM 8), so the array must reach 8: a five-entry array
  /// indexed at 8 read past its end, which is how the first version of this table produced no reception line at all.
  std::array<shared_calibration, 12>                shared_sinr_cal{};
  /// \brief Publication source override: ignore the reporting heads even when the artifact carries them.
  ///
  /// It exists for ONE experiment, and that experiment gates the whole LLR-only change (design section 22.7, gate 2):
  /// the shared layer's publication must be compared against the heads' on the SAME receptions, and its single offset
  /// constant fitted, BEFORE a bundle without heads can be trusted. Without this switch the comparison would need the
  /// trained artifact first, which would make the acceptance test depend on the thing it is testing.
  bool                                              ignore_reporting_heads = false;
  std::unique_ptr<metal::coreml_nn_engine>          shared_engine;
  /// \brief The bundle's declared calibration of the PUBLISHED SINR, read at BIND time per modulation.
  ///
  /// Read at construction rather than at delivery because the engine is moved into its slot during binding (and
  /// reading a moved-from engine is a null dereference, measured twice), and because the declaration is static: a
  /// run either publishes calibrated numbers from its first reception or says why it does not.
  struct sinr_calibration {
    bool  has    = false;
    float slope  = 1.0F;
    float offset = 0.0F;
  };
  std::array<sinr_calibration, 9> sinr_cal_by_qm;
  /// Whether ANY bound modulation declares a published-SINR calibration (the reporter says which state it is in).
  bool any_calibrated = false;

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
  /// \brief Scratch buffers for a narrow-band bundle, sized for the bundle's own window.
  ///
  /// A bundle exported for fewer than the contract's 612 subcarriers is fed a CROP of the assembled plane
  /// (`model_network_design` section 49); these hold the window and its heads, and the results are scattered back
  /// to absolute positions immediately after the call so no reader downstream changes.
  std::vector<__fp16>          narrow_plane;
  std::vector<__fp16>          narrow_llr;
  std::vector<__fp16>          narrow_noise;
  std::vector<__fp16>          narrow_channel;
  /// \brief First subcarrier of the crop: the allocation's first, less four PRB of context.
  unsigned narrow_window_lo(const pusch_demodulator::configuration& config) const;
  std::vector<__fp16>          llr_head;
  std::vector<__fp16>          noise_head;
  std::vector<__fp16>          channel_head;
  __fp16                       dmrs_noise_head = 0;
  std::vector<unsigned>        allocated;
  std::array<unsigned, 9>      degradations{};
};

// Four PRB of context, MEASURED (model_network_design section 49.1): zeroing everything outside a window this wide
// leaves the LLRs inside the allocation bit-identical to the full-band run, while two PRB is not quite enough and none
// loses half a percent of the signs.

unsigned pusch_depth3_receiver_ai_network_impl::narrow_window_lo(const pusch_demodulator::configuration& config) const
{
  constexpr unsigned context_sc = 4 * NOF_SUBCARRIERS_PER_RB;
  const unsigned     first_sc   = narrow_first_prb(config) * NOF_SUBCARRIERS_PER_RB;
  return (first_sc > context_sc) ? (first_sc - context_sc) : 0U;
}

#endif // OCUDU_HAVE_DEPTH3_ENGINE

} // namespace

// The process-wide early-measurement registry and its counters (design section 19.270): shared because the arm that
// receives the hint and the arm that processes the reception both come from the dependencies pool.
std::array<pusch_depth3_receiver_ai_network_impl::slot_measurement,
           pusch_depth3_receiver_ai_network_impl::MEASURE_RING>
    pusch_depth3_receiver_ai_network_impl::measured{};
unsigned pusch_depth3_receiver_ai_network_impl::measured_next = 0;
std::mutex pusch_depth3_receiver_ai_network_impl::measured_mutex;
std::atomic<unsigned> pusch_depth3_receiver_ai_network_impl::prepared_receptions{0};
std::atomic<unsigned> pusch_depth3_receiver_ai_network_impl::prepare_refused{0};
std::atomic<unsigned> pusch_depth3_receiver_ai_network_impl::gate_from_prepare{0};
std::atomic<unsigned> pusch_depth3_receiver_ai_network_impl::measurements_suppressed{0};
std::array<unsigned, 512> pusch_depth3_receiver_ai_network_impl::prepare_cost_us{};
unsigned pusch_depth3_receiver_ai_network_impl::prepare_cost_index = 0;
unsigned pusch_depth3_receiver_ai_network_impl::prepare_cost_count = 0;

std::unique_ptr<pusch_depth3_receiver> ocudu::create_pusch_depth3_receiver(
    pusch_depth3_kind               kind,
    dmrs_pusch_estimator&           estimator,
    pusch_demodulator&              demodulator,
    const pusch_depth3_model_paths& models,
    const std::string&              reporting_source,
    unsigned                        startup_grace_s)
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
          estimator, demodulator, models.fallback, models.slots, models.fallback_is_build_default,
          reporting_source, startup_grace_s);
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
