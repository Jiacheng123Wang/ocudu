// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// Core ML usage follows Apple's Core ML framework documentation (zero-copy
// MLMultiArray initWithDataPointer, MLPredictionOptions.outputBackings,
// MLModelConfiguration.computeUnits); no Apple sample-code blocks are copied.

#include "ocudu_coreml_nn_engine.h"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include "ocudu/ocudulog/ocudulog.h"

#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

using namespace ocudu;

namespace {

/// OFDM symbols per slot, which the contract folds into the channel axis.
constexpr unsigned NOF_SYMBOLS_PER_SLOT = 14;
// Input channels per symbol in the contract's SPATIAL plane (`model_network_design` 0.2(b)). The
// symbol axis is spatial, so this is a per-symbol channel count and not a folded total.
constexpr unsigned DEPTH3_IN_CHANNELS = 12;
/// The conditioning channels a MODULATION-CONDITIONED bundle appends to the plane: one per modulation the contract
/// can serve (QPSK, 16QAM, 64QAM, 256QAM), all zero but the reception's own. Declared by the bundle's metadata
/// (`depth3_layout=modcond`), because the plane's width alone cannot say which contract it belongs to -- see the
/// matching note in the training pipeline (design section 19.102).
constexpr unsigned DEPTH3_COND_CHANNELS = 4;
/// The main head's stride under the conditioned contract: the widest modulation, whatever the reception's own Qm
/// is (the bundle declares it as `depth3_head_stride`). The arm reads the head with it and the engine checks the
/// reception's bits against it, so the constant has one meaning in both places.
constexpr unsigned DEPTH3_COND_STRIDE = 8;

struct coreml_nn_engine_impl {
  MLModel* model     = nil;
  NSString* in_name  = nil;
  NSString* out_name = nil;

  // Depth-3 state: a second model with the contract's plane input and four named heads. The two
  // models never coexist in practice, and keeping them apart means the depth-1 refiner's callers
  // keep the layout they were trained in.
  MLModel*  model3      = nil;
  NSString* in3_name    = nil;
  NSString* out3_names[4] = {nil, nil, nil, nil};
  unsigned  model3_nof_subc = 612;
  // Channel count of each head AS THE MODEL DECLARES IT. The shapes of the output backings come
  // from here and not from the caller, because a backing whose shape differs from the model's is
  // rejected by Core ML at best and silently truncated at worst, and a truncated main head would
  // hand the decoder a plausible subset of the bits.
  unsigned  model3_channels[4] = {0, 0, 0, 0};
  /// The bundle's own DECLARATION, read from its metadata (`depth3_layout`): whether it is the
  /// MODULATION-CONDITIONED contract, which serves every modulation from one artifact with the Qm as extra input
  /// channels. It cannot be inferred: a conditioned bundle and a per-modulation 256QAM bundle have the same main
  /// head width (14 * 8 = 112 channels), and they are not interchangeable.
  bool      model3_conditioned = false;
  /// The SNR range (dB) the bundle's REPORTING heads were trained on, as the bundle declares it
  /// (`depth3_snr_range`). False in `model3_has_snr_range` means the bundle declares none, which is not the same
  /// as "any range": a measurement the model extrapolated cannot be told from one it was entitled to make, and the
  /// arm says so rather than pretending (design sections 19.115, 19.117).
  /// The plane width the bundle was exported for, as it declares it (`depth3_nsc`); 0 when it declares none.
  unsigned  model3_nsc          = 0;
  bool      model3_has_snr_range = false;
  float     model3_snr_min_db    = 0.0F;
  float     model3_snr_max_db    = 0.0F;
  /// The modulations a conditioned bundle declares it serves, as bits per symbol, ascending; empty for a
  /// per-modulation bundle (whose single modulation is the head width divided by the slot's symbols).
  unsigned  model3_qm_set[4]   = {0, 0, 0, 0};
  unsigned  model3_nof_qm      = 0;
  /// Input plane channels the bundle expects: DEPTH3_IN_CHANNELS, plus the conditioning channels when declared.
  unsigned  model3_in_channels = 0;
  /// The compute-unit tier the depth-3 bundle was LOADED with (see depth3_selected_compute_units). Kept so a run
  /// can say which tier it asked for; nothing can read back which one the runtime actually used.
  const char* model3_compute_units = "all";
  /// Per-modulation affine calibration of the PUBLISHED SINR (`depth3_sinr_cal`), keyed by bits per symbol:
  /// `published' = slope * published + offset` in dB. The arm's post-equalisation noise is its own estimate, and its
  /// scale does not coincide with the classical chain's, so the scheduler read a SINR 1.2-3.4 dB low -- in the
  /// default reporting mode too (design section 19.172). Absent keys publish the raw value.
  bool  model3_sinr_cal_set[9]    = {false};
  float model3_sinr_cal_slope[9]  = {0.0F};
  float model3_sinr_cal_offset[9] = {0.0F};
  // The shape of each head AS THE MODEL DECLARES IT, kept whole rather than reduced to a channel
  // count: the output backings must match it exactly, and the spatial contract's heads are
  // [1, nsc, C] while its scalar head stays [1, 1, 1, 1].
  NSArray<NSNumber*>* model3_shape[4] = {nil, nil, nil, nil};

  // Every prediction runs on ONE dedicated worker thread: the first Core ML
  // prediction on a NEW thread pays a per-thread runtime initialization
  // (~12 ms for the ANE program - the E2E first full-bandwidth slot spike).
  // The worker performs the warm-up at construction, so live predictions on
  // the UL executor threads never touch that cost.
  std::thread             worker;
  std::mutex              mtx;
  std::condition_variable cv;
  bool                    has_job  = false;
  bool                    shutdown = false;
  const float*            job_in      = nullptr;
  float*                  job_out     = nullptr;
  unsigned                job_nof_subc = 0;
  bool                    job_done    = false;
  bool                    job_ok      = false;
  double                  last_predict_us = 0.0;
  // Which entry point queued the job, and its arguments. The worker serves both so that the
  // per-thread Core ML initialization is paid once for either model.
  bool                    job_is_depth3 = false;
  ocudu::metal::coreml_nn_engine::depth3_input  job_in3;
  ocudu::metal::coreml_nn_engine::depth3_output job_out3;

  // ANE keep-alive: the compiled ANE program is evicted after ~15-20 s of idle
  // (deep power-gate) and the next prediction recompiles it (~13.6 ms - the E2E
  // first full-bandwidth slot spike). The worker runs a dummy prediction every
  // KEEPALIVE_PERIOD so the program stays resident.
  static constexpr auto KEEPALIVE_PERIOD = std::chrono::seconds(2);
  unsigned              model_nof_subc   = 612;
  std::vector<float>    keepalive_in;
  std::vector<float>    keepalive_out;
  std::vector<uint16_t> keepalive_in3;
  std::vector<uint16_t> keepalive_llr3;
  std::vector<uint16_t> keepalive_noise3;
  std::vector<uint16_t> keepalive_ch3;
  uint16_t              keepalive_dmrs3 = 0;

  ~coreml_nn_engine_impl()
  {
    {
      std::lock_guard<std::mutex> lock(mtx);
      shutdown = true;
    }
    cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }
};

// Out-of-class definition (odr-used by the worker lambda's wait_for).
constexpr std::chrono::seconds coreml_nn_engine_impl::KEEPALIVE_PERIOD;

bool run_prediction(coreml_nn_engine_impl* e, const float* in, float* out, unsigned nof_subc)
{
  // Zero-copy wrappers over the host buffers (shared memory; no copies). The grid
  // width is the 51/52-PRB trained shape (612 / 624 subcarriers).
  const unsigned      nsc = nof_subc;
  NSArray<NSNumber*>* shape = @[@1, @(nsc), @14, @2];
  MLMultiArray*       in_arr = [[MLMultiArray alloc] initWithDataPointer:const_cast<float*>(in)
                                                                  shape:shape
                                                               dataType:MLMultiArrayDataTypeFloat32
                                                                strides:@[@(nsc * 14 * 2), @(14 * 2), @2, @1]
                                                            deallocator:nil
                                                                  error:nil];
  MLMultiArray*       out_arr = [[MLMultiArray alloc] initWithDataPointer:out
                                                                    shape:shape
                                                                 dataType:MLMultiArrayDataTypeFloat32
                                                                  strides:@[@(nsc * 14 * 2), @(14 * 2), @2, @1]
                                                              deallocator:nil
                                                                    error:nil];
  if (in_arr == nil || out_arr == nil) {
    return false;
  }

  MLDictionaryFeatureProvider* input =
      [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{e->in_name : in_arr} error:nil];
  if (input == nil) {
    return false;
  }
  // Provide the output backing so Core ML writes the result into the host buffer.
  MLPredictionOptions* opts = [[MLPredictionOptions alloc] init];
  opts.outputBackings       = @{e->out_name : out_arr};

  const auto t0 = std::chrono::steady_clock::now();
  NSError*   err = nil;
  id<MLFeatureProvider> result = [e->model predictionFromFeatures:input options:opts error:&err];
  const auto t1 = std::chrono::steady_clock::now();
  e->last_predict_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

  if (result == nil) {
    ocudulog::fetch_basic_logger("PHY").error("AI-CE: prediction failed: {}",
                                              err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }
  return true;
}

// The depth-3 forward pass, in the contract's layout and half precision. Every buffer is wrapped
// rather than copied, which is why the caller keeps ownership: the plane is assembled once, the
// heads are read where the receiver wants them.
bool run_prediction_depth3(coreml_nn_engine_impl* e,
                           const ocudu::metal::coreml_nn_engine::depth3_input&  in,
                           const ocudu::metal::coreml_nn_engine::depth3_output& out)
{
  const unsigned nsc = in.nof_subcarriers;

  // Row-major strides of a declared shape, so the backings follow the model instead of a guess.
  auto strides_of = [](NSArray<NSNumber*>* shape) {
    NSMutableArray<NSNumber*>* strides = [NSMutableArray arrayWithCapacity:shape.count];
    long                       running = 1;
    for (NSInteger i = static_cast<NSInteger>(shape.count) - 1; i >= 0; --i) {
      [strides insertObject:@(running) atIndex:0];
      running *= shape[i].longValue;
    }
    return strides;
  };

  // The contract's input plane: [1, subcarriers, 14 symbols, 12 channels]. The symbol axis is a
  // SPATIAL axis here, which is the whole point of the layout: a 3x3 kernel mixes frequency AND
  // time. The buffer is the caller's, bound with a nil deallocator so nothing is copied.
  // The PLANE WIDTH comes from the bundle's declaration, not from the contract's constant: a conditioned bundle
  // takes the contract's twelve channels plus its four conditioning ones, and a multi-array of the wrong width is
  // rejected by the runtime -- which is how the first conditioned run degraded every reception instead of
  // computing (measured 2026-10-16).
  NSArray<NSNumber*>* in_shape = @[ @1, @(nsc), @(NOF_SYMBOLS_PER_SLOT), @(e->model3_in_channels) ];
  NSError*            in_err   = nil;
  MLMultiArray*       in_arr   = [[MLMultiArray alloc]
      initWithDataPointer:const_cast<uint16_t*>(in.plane)
                    shape:in_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:strides_of(in_shape)
              deallocator:nil
                    error:&in_err];
  if (in_arr == nil) {
    // A SILENT `return false` here is indistinguishable, one level up, from a model that cannot run at all:
    // both arrive as "the forward pass failed" and the reception degrades. The reason is available and is the
    // difference between a wrong shape and a wrong buffer, so it is printed (2026-10-16).
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: the input plane could not be bound: shape [1, {}, 14, {}] over {} element(s); {}",
        nsc, e->model3_in_channels, in.nof_subcarriers * NOF_SYMBOLS_PER_SLOT * e->model3_in_channels,
        in_err != nil ? in_err.localizedDescription.UTF8String : "no reason given");
    return false;
  }

  // Each backing takes the shape the model declared for that head. A 3D head and a 4D backing (or
  // the reverse) is rejected by Core ML, and building them from the declaration is what keeps this
  // code from having to be edited every time the contract's rank changes.
  MLMultiArray* llr_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.llr
                    shape:e->model3_shape[0]
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:strides_of(e->model3_shape[0])
              deallocator:nil
                    error:nil];
  MLMultiArray* noise_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.post_eq_noise
                    shape:e->model3_shape[1]
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:strides_of(e->model3_shape[1])
              deallocator:nil
                    error:nil];
  MLMultiArray* ch_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.ch_est
                    shape:e->model3_shape[2]
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:strides_of(e->model3_shape[2])
              deallocator:nil
                    error:nil];
  MLMultiArray* dmrs_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.dmrs_noise
                    shape:e->model3_shape[3]
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:strides_of(e->model3_shape[3])
              deallocator:nil
                    error:nil];
  if (llr_arr == nil || noise_arr == nil || ch_arr == nil || dmrs_arr == nil) {
    // Same reason as the input above: the caller only ever sees "the forward pass failed", so the head whose
    // declared shape did not fit its backing -- the one fact that identifies the contract mismatch -- is named
    // here.
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: a declared head does not fit the arm's backing (llr {}, post_eq_noise {}, ch_est {}, "
        "dmrs_noise {})",
        llr_arr != nil ? "ok" : "FAILED", noise_arr != nil ? "ok" : "FAILED", ch_arr != nil ? "ok" : "FAILED",
        dmrs_arr != nil ? "ok" : "FAILED");
    return false;
  }

  NSError*                     provider_err = nil;
  MLDictionaryFeatureProvider* input        = [[MLDictionaryFeatureProvider alloc]
      initWithDictionary:@{e->in3_name : in_arr}
                   error:&provider_err];
  if (input == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: the model refused the input named '{}': {}", e->in3_name.UTF8String,
        provider_err != nil ? provider_err.localizedDescription.UTF8String : "no reason given");
    return false;
  }
  MLPredictionOptions* opts = [[MLPredictionOptions alloc] init];
  opts.outputBackings       = @{e->out3_names[0] : llr_arr,
                                e->out3_names[1] : noise_arr,
                                e->out3_names[2] : ch_arr,
                                e->out3_names[3] : dmrs_arr};

  const auto t0 = std::chrono::steady_clock::now();
  NSError*   err = nil;
  id<MLFeatureProvider> result = [e->model3 predictionFromFeatures:input options:opts error:&err];
  const auto t1 = std::chrono::steady_clock::now();
  e->last_predict_us = std::chrono::duration<double, std::micro>(t1 - t0).count();

  if (result == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: prediction failed: {}",
        err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }
  return true;
}

// The dedicated worker thread. Both entry points start it, and both go through it, so the
// per-thread Core ML initialization is paid once and never on a receive thread, and the idle
// keep-alive touches whichever model the engine holds.
void start_worker(coreml_nn_engine_impl* e)
{
  if (e->worker.joinable()) {
    return;
  }
  e->worker = std::thread([e]() {
    for (;;) {
      const float* in  = nullptr;
      float*       out = nullptr;
      unsigned     nsc = e->model_nof_subc;
      bool         keepalive = false;
      ocudu::metal::coreml_nn_engine::depth3_input  in3;
      ocudu::metal::coreml_nn_engine::depth3_output out3;
      bool         depth3 = false;
      {
        std::unique_lock<std::mutex> lock(e->mtx);
        e->cv.wait_for(lock, coreml_nn_engine_impl::KEEPALIVE_PERIOD,
                       [e]() { return e->has_job || e->shutdown; });
        if (e->shutdown) {
          return;
        }
        if (!e->has_job) {
          keepalive = true;
          depth3    = (e->model3 != nil);
          if (depth3) {
            in3.plane            = e->keepalive_in3.data();
            in3.nof_subcarriers  = e->model3_nof_subc;
            out3.llr             = e->keepalive_llr3.data();
            out3.post_eq_noise   = e->keepalive_noise3.data();
            out3.ch_est          = e->keepalive_ch3.data();
            out3.dmrs_noise      = &e->keepalive_dmrs3;
            out3.bits_per_symbol = 8;
          } else {
            in  = e->keepalive_in.data();
            out = e->keepalive_out.data();
            nsc = e->model_nof_subc;
          }
        } else {
          depth3 = e->job_is_depth3;
          in3    = e->job_in3;
          out3   = e->job_out3;
          in     = e->job_in;
          out    = e->job_out;
          nsc    = e->job_nof_subc;
        }
      }
      const bool ok = depth3 ? run_prediction_depth3(e, in3, out3) : run_prediction(e, in, out, nsc);
      if (!keepalive) {
        std::lock_guard<std::mutex> lock(e->mtx);
        e->job_ok   = ok;
        e->job_done = true;
        e->has_job  = false;
        e->cv.notify_all();
      }
    }
  });
}

} // namespace

namespace ocudu {
namespace metal {

coreml_nn_engine::~coreml_nn_engine()
{
  delete static_cast<coreml_nn_engine_impl*>(impl);
}

/// \brief Which compute units a bundle is loaded with, from a measurement variable (`all` when unset).
///
/// `MLComputeUnitsAll` lets Core ML choose among the ANE, the GPU and the CPU, and nothing reports the choice
/// back, so "the network runs on the ANE" has been an assumption in this design rather than a measurement
/// (section 19.164). Loading the SAME bundle at one tier at a time is what makes placement measurable from
/// outside: `cpu_gpu` matching `all` means the ANE is not carrying the forward pass, and `all` being much faster
/// means it is. The default is `all`, i.e. unchanged behaviour -- this is a measurement knob, not a deployment
/// one, and a run that does not set the variable asks for exactly what it asked for before.
struct compute_units_choice {
  MLComputeUnits units = MLComputeUnitsAll;
  const char*    name  = "all";
};

compute_units_choice compute_units_from_env(const char* variable)
{
  const char* env = std::getenv(variable);
  if (env == nullptr) {
    return compute_units_choice{};
  }
  if (std::strcmp(env, "all") == 0) {
    return {MLComputeUnitsAll, "all"};
  }
  if (std::strcmp(env, "cpu_gpu") == 0) {
    return {MLComputeUnitsCPUAndGPU, "cpu_gpu"};
  }
  if (std::strcmp(env, "cpu") == 0) {
    return {MLComputeUnitsCPUOnly, "cpu"};
  }
  if (std::strcmp(env, "cpu_ne") == 0) {
#if defined(__MAC_OS_X_VERSION_MAX_ALLOWED) && (__MAC_OS_X_VERSION_MAX_ALLOWED >= 130000)
    if (@available(macOS 13.0, *)) {
      return {MLComputeUnitsCPUAndNeuralEngine, "cpu_ne"};
    }
#endif
    ocudulog::fetch_basic_logger("PHY").warning(
        "AI depth-3: OCUDU_DEPTH3_COMPUTE_UNITS=cpu_ne needs macOS 13 or a newer SDK than this build has; "
        "using all");
    return compute_units_choice{};
  }
  ocudulog::fetch_basic_logger("PHY").warning(
      "AI engine: {}='{}' is not one of all|cpu_gpu|cpu_ne|cpu; using all", variable, env);
  return compute_units_choice{};
}

bool coreml_nn_engine::init(const char* modelc_path)
{
  if (impl == nullptr) {
    impl = new coreml_nn_engine_impl;
  }
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e->model != nil) {
    return true;
  }

  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:modelc_path]];
  if (url == nil || ![[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
    ocudulog::fetch_basic_logger("PHY").error("AI-CE: model bundle not found at {}", modelc_path);
    return false;
  }
  const compute_units_choice units_choice = compute_units_from_env("OCUDU_CE_COMPUTE_UNITS");
  MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
  // ANE > GPU > CPU as the runtime sees fit, unless the measurement knob names one tier (see depth3_compute_units;
  // the same reasoning applies to this network, and design section 19.171 compares the two on the same units).
  cfg.computeUnits          = units_choice.units;
  NSError* err              = nil;
  e->model                  = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
  if (e->model == nil) {
    ocudulog::fetch_basic_logger("PHY").error("AI-CE: model load failed: {}",
                                              err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }
  MLModelDescription* desc = e->model.modelDescription;
  e->in_name               = desc.inputDescriptionsByName.allKeys.firstObject;
  e->out_name              = desc.outputDescriptionsByName.allKeys.firstObject;
  ocudulog::fetch_basic_logger("PHY").debug("AI-CE: Core ML model loaded from {} (in={} out={})",
                                            modelc_path,
                                            e->in_name.UTF8String,
                                            e->out_name.UTF8String);
  if (e->in_name == nil || e->out_name == nil) {
    return false;
  }
  // The model's trained grid width (612 / 624 subcarriers) from the input description.
  {
    MLFeatureDescription* fd = desc.inputDescriptionsByName[e->in_name];
    if (fd != nil && fd.type == MLFeatureTypeMultiArray && fd.multiArrayConstraint != nil &&
        fd.multiArrayConstraint.shape.count > 1) {
      e->model_nof_subc = fd.multiArrayConstraint.shape[1].unsignedIntValue;
    }
  }
  e->keepalive_in.resize(static_cast<size_t>(e->model_nof_subc) * 14 * 2, 0.0F);
  e->keepalive_out.resize(static_cast<size_t>(e->model_nof_subc) * 14 * 2);

  // Dedicated worker thread: all predictions (including the warm-up) run here, so the per-thread
  // Core ML/ANE initialization is paid exactly once, at construction, never on a receive thread.
  start_worker(e);
  return true;
}

bool coreml_nn_engine::init_depth3(const char* modelc_path)
{
  if (impl == nullptr) {
    impl = new coreml_nn_engine_impl;
  }
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e->model3 != nil) {
    return true;
  }

  NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:modelc_path]];
  if (url == nil || ![[NSFileManager defaultManager] fileExistsAtPath:url.path]) {
    ocudulog::fetch_basic_logger("PHY").error("AI depth-3: model bundle not found at {}",
                                              modelc_path);
    return false;
  }
  const compute_units_choice units_choice = compute_units_from_env("OCUDU_DEPTH3_COMPUTE_UNITS");
  MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
  cfg.computeUnits          = units_choice.units;
  e->model3_compute_units   = units_choice.name;
  NSError* err              = nil;
  e->model3                 = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
  if (e->model3 == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: model load failed: {}",
        err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }
  // What the run ASKED FOR, once per process. Deliberately not phrased as placement: Core ML offers no way to
  // read back which units a prediction went to, and a log line that implied it had would be the assumption this
  // knob exists to replace (design section 19.169).
  ocudulog::fetch_basic_logger("PHY").info(
      "AI depth-3: bundle loaded with compute units '{}' requested (OCUDU_DEPTH3_COMPUTE_UNITS); Core ML does "
      "not report which units it used",
      e->model3_compute_units);

  MLModelDescription* desc = e->model3.modelDescription;
  e->in3_name              = @"x";
  MLFeatureDescription* in_desc = desc.inputDescriptionsByName[e->in3_name];
  if (in_desc == nil || in_desc.type != MLFeatureTypeMultiArray || in_desc.multiArrayConstraint == nil) {
    ocudulog::fetch_basic_logger("PHY").error("AI depth-3: the model has no multi-array input x");
    return false;
  }
  NSArray<NSNumber*>* in_shape = in_desc.multiArrayConstraint.shape;
  // THE DECLARATION IS READ FIRST, because it decides what the input shape is allowed to be. A conditioned bundle
  // carries the Qm as four extra plane channels, so it is a DIFFERENT contract -- and a shape-based check that
  // accepted both would run a per-modulation bundle on a plane whose four extra channels mean nothing, or feed a
  // conditioned bundle a plane with no Qm at all.
  NSDictionary* creator_meta = desc.metadata[MLModelCreatorDefinedKey];
  if ([creator_meta[@"depth3_layout"] isEqualToString:@"modcond"]) {
    e->model3_conditioned = true;
    NSString* qm_set = creator_meta[@"depth3_qm_set"];
    for (NSString* token in [qm_set componentsSeparatedByString:@","]) {
      unsigned qm = (unsigned)[token intValue];
      if (qm != 0 && e->model3_nof_qm != 4) {
        e->model3_qm_set[e->model3_nof_qm++] = qm;
      }
    }
    if (e->model3_nof_qm == 0) {
      ocudulog::fetch_basic_logger("PHY").error(
          "AI depth-3: the bundle declares depth3_layout=modcond but names no modulation in depth3_qm_set");
      return false;
    }
  }
  // THE MEASUREMENT DOMAIN, declared by the bundle for the same reason the layout is: nothing about the graph
  // says what it was trained on, and a reporting head asked about a reception it never saw answers anyway.
  // Measured 2026-10-16: a model trained on 20-32 dB reported 26.4 dB at 0 dB, 6 dB and 24 dB alike.
  // THE GEOMETRY: read with the other declarations, and checked against the graph below so a bundle whose
  // declaration and whose graph disagree is refused at load rather than at the first reception.
  NSString* nsc_decl = creator_meta[@"depth3_nsc"];
  if (nsc_decl != nil) {
    e->model3_nsc = (unsigned)[nsc_decl intValue];
  }

  NSString* snr_range = creator_meta[@"depth3_snr_range"];
  if (snr_range != nil) {
    NSArray<NSString*>* bounds = [snr_range componentsSeparatedByString:@","];
    if (bounds.count == 2) {
      e->model3_snr_min_db    = bounds[0].floatValue;
      e->model3_snr_max_db    = bounds[1].floatValue;
      e->model3_has_snr_range = (e->model3_snr_max_db > e->model3_snr_min_db);
    }
    if (!e->model3_has_snr_range) {
      ocudulog::fetch_basic_logger("PHY").error(
          "AI depth-3: the bundle declares depth3_snr_range='{}' which is not a pair of increasing bounds; the "
          "measurement domain stays undeclared",
          snr_range.UTF8String);
    }
  }

  // THE PUBLISHED SINR'S CALIBRATION (design section 19.172): per modulation, affine in dB.
  NSString* sinr_cal = creator_meta[@"depth3_sinr_cal"];
  if (sinr_cal != nil) {
    for (NSString* entry in [sinr_cal componentsSeparatedByString:@";"]) {
      NSArray<NSString*>* parts = [entry componentsSeparatedByString:@":"];
      if (parts.count != 2) {
        continue;
      }
      NSArray<NSString*>* pair = [parts[1] componentsSeparatedByString:@","];
      if (pair.count != 2) {
        continue;
      }
      const long qm = parts[0].integerValue;
      const float slope = pair[0].floatValue;
      const float offset = pair[1].floatValue;
      // A slope that is not positive would INVERT the measurement (a better channel reported as worse), which is
      // worse than no calibration at all, so it is refused here rather than applied.
      if ((qm < 2) || (qm > 8) || !(slope > 0.0F) || !std::isfinite(slope) || !std::isfinite(offset)) {
        ocudulog::fetch_basic_logger("PHY").error(
            "AI depth-3: depth3_sinr_cal entry '{}' is not usable (need qm in 2..8 and a positive finite slope); "
            "the SINR stays uncalibrated",
            entry.UTF8String);
        continue;
      }
      e->model3_sinr_cal_set[qm]    = true;
      e->model3_sinr_cal_slope[qm]  = slope;
      e->model3_sinr_cal_offset[qm] = offset;
    }
    ocudulog::fetch_basic_logger("PHY").info(
        "AI depth-3: the bundle declares a published-SINR calibration for {} modulation(s) (depth3_sinr_cal)",
        static_cast<unsigned>(std::count(std::begin(e->model3_sinr_cal_set), std::end(e->model3_sinr_cal_set), true)));
  }

  // A DECLARATION THAT DISAGREES WITH THE GRAPH IS REFUSED HERE. The two state the same fact, so a bundle that
  // declares one width and carries another would otherwise fail at the first reception; at load time the mismatch
  // names itself (design section 19.162).
  if ((e->model3_nsc != 0) && (in_shape.count == 4) && (e->model3_nsc != in_shape[1].unsignedIntValue)) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: the bundle declares depth3_nsc={} but its input is {} subcarrier(s) wide",
        e->model3_nsc, in_shape[1].unsignedIntValue);
    return false;
  }

  e->model3_in_channels = DEPTH3_IN_CHANNELS + (e->model3_conditioned ? DEPTH3_COND_CHANNELS : 0);
  // The contract's plane: [1, subcarriers, 14, 12], fp16 -- the symbol axis is SPATIAL, which is
  // what lets a 3x3 kernel mix time as well as frequency. A model trained in the superseded folded
  // layout would be fed nonsense and would produce plausible logits for it, so the shape is checked
  // here rather than at the first prediction.
  if (in_shape.count != 4 || in_shape[0].unsignedIntValue != 1 ||
      in_shape[2].unsignedIntValue != NOF_SYMBOLS_PER_SLOT ||
      in_shape[3].unsignedIntValue != e->model3_in_channels) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: input x is [1, nsc, {}, {}] where this contract needs [1, nsc, {}, {}]{}",
        NOF_SYMBOLS_PER_SLOT, in_shape[3].unsignedIntValue, NOF_SYMBOLS_PER_SLOT, e->model3_in_channels,
        e->model3_conditioned ? " (a modulation-conditioned bundle takes the Qm as four extra channels)"
                              : "");
    return false;
  }
  e->model3_nof_subc = in_shape[1].unsignedIntValue;

  const char* head_names[4] = { "llr", "post_eq_noise", "ch_est", "dmrs_noise" };
  for (unsigned head = 0; head != 4; ++head) {
    NSString* name = [NSString stringWithUTF8String:head_names[head]];
    if (desc.outputDescriptionsByName[name] == nil) {
      ocudulog::fetch_basic_logger("PHY").error("AI depth-3: the model has no output named {}",
                                                head_names[head]);
      return false;
    }
    e->out3_names[head] = name;
    MLFeatureDescription* out_desc = desc.outputDescriptionsByName[name];
    if (out_desc.multiArrayConstraint != nil && out_desc.multiArrayConstraint.shape.count >= 2) {
      e->model3_shape[head]    = out_desc.multiArrayConstraint.shape;
      // The channel count is the LAST axis whatever the rank is: the spatial contract's heads are
      // [1, nsc, C] and its scalar head is [1, 1, 1, 1]. Reading axis 3 unconditionally is how a 3D
      // head would be read as zero channels and rejected for the wrong reason.
      e->model3_channels[head] = e->model3_shape[head].lastObject.unsignedIntValue;
    }
  }
  // The contract fixes what each head carries: 14 * Qm coded bits, 14 post-equalization noise
  // values, 28 channel-estimate components (14 symbols real and imaginary) and one scalar. A model
  // whose heads are shaped otherwise is not this contract's model.
  if (e->model3_channels[0] == 0 || e->model3_channels[0] % NOF_SYMBOLS_PER_SLOT != 0 ||
      e->model3_channels[1] != NOF_SYMBOLS_PER_SLOT ||
      e->model3_channels[2] != 2 * NOF_SYMBOLS_PER_SLOT || e->model3_channels[3] != 1) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: head shapes {} / {} / {} / {} do not match the contract",
        e->model3_channels[0], e->model3_channels[1], e->model3_channels[2], e->model3_channels[3]);
    return false;
  }

  e->keepalive_in3.resize(
      static_cast<size_t>(e->model3_nof_subc) * NOF_SYMBOLS_PER_SLOT * DEPTH3_IN_CHANNELS, 0);
  // Product of the DECLARED shape, so a head whose spatial axes differ from the expected ones makes
  // the keep-alive write out of bounds rather than silently predicting into a short buffer.
  auto elements_of = [](NSArray<NSNumber*>* shape) {
    size_t count = 1;
    for (NSNumber* dim in shape) {
      count *= dim.unsignedIntValue;
    }
    return count;
  };
  e->keepalive_llr3.resize(elements_of(e->model3_shape[0]), 0);
  e->keepalive_noise3.resize(elements_of(e->model3_shape[1]), 0);
  e->keepalive_ch3.resize(elements_of(e->model3_shape[2]), 0);

  ocudulog::fetch_basic_logger("PHY").debug(
      "AI depth-3: Core ML model loaded from {} ({} subcarriers, four heads)",
      modelc_path,
      e->model3_nof_subc);

  // The worker thread may not have been started yet when only a depth-3 model is configured.
  start_worker(e);
  return true;
}

unsigned coreml_nn_engine::depth3_nsc() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr) ? 0U : e->model3_nsc;
}

bool coreml_nn_engine::depth3_has_snr_range() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e != nullptr) && e->model3_has_snr_range;
}

float coreml_nn_engine::depth3_snr_min_db() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr) ? 0.0F : e->model3_snr_min_db;
}

float coreml_nn_engine::depth3_snr_max_db() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr) ? 0.0F : e->model3_snr_max_db;
}

bool coreml_nn_engine::depth3_is_modulation_conditioned() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e != nullptr) && e->model3_conditioned;
}

unsigned coreml_nn_engine::depth3_nof_declared_qm() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr) ? 0U : e->model3_nof_qm;
}

unsigned coreml_nn_engine::depth3_declared_qm(unsigned index) const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr || index >= e->model3_nof_qm) ? 0U : e->model3_qm_set[index];
}

unsigned coreml_nn_engine::depth3_input_channels() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  return (e == nullptr) ? 0U : e->model3_in_channels;
}

bool coreml_nn_engine::depth3_sinr_calibration(unsigned bits_per_symbol, float& slope, float& offset) const
{
  const auto* e = static_cast<const coreml_nn_engine_impl*>(impl);
  if ((e == nullptr) || (bits_per_symbol >= 9) || !e->model3_sinr_cal_set[bits_per_symbol]) {
    return false;
  }
  slope  = e->model3_sinr_cal_slope[bits_per_symbol];
  offset = e->model3_sinr_cal_offset[bits_per_symbol];
  return true;
}

const char* coreml_nn_engine::depth3_compute_units() const
{
  const auto* e = static_cast<const coreml_nn_engine_impl*>(impl);
  // A null impl means nothing was loaded, which asked for the default and got it.
  return (e == nullptr) ? "all" : e->model3_compute_units;
}

unsigned coreml_nn_engine::depth3_bits_per_symbol() const
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e == nullptr || e->model3 == nil || e->model3_channels[0] == 0) {
    return 0;
  }
  return e->model3_channels[0] / NOF_SYMBOLS_PER_SLOT;
}

bool coreml_nn_engine::predict_depth3(const depth3_input& in, const depth3_output& out)
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e == nullptr || e->model3 == nil) {
    return false;
  }
  if (in.plane == nullptr || out.llr == nullptr || out.post_eq_noise == nullptr ||
      out.ch_est == nullptr || out.dmrs_noise == nullptr) {
    return false;
  }
  // The caller states which modulation's model it expects; the model states what it is. A mismatch
  // means the wrong model was bound for this reception, which must fail rather than run.
  //
  // A CONDITIONED bundle is the case the declaration exists for, and comparing the two numbers there is wrong:
  // it emits ONE head for every modulation it declares, so the reception's Qm is not the head's width. The arm
  // reads that head with the head's own stride and only the first Qm bits of each group are live. Comparing them
  // refused every conditioned reception whose modulation was not the widest one -- QPSK, 16QAM and 64QAM degraded
  // deterministically while 256QAM ran, which reads like a modulation-specific model fault (measured 2026-10-16).
  // The rule for a conditioned bundle is instead: the reception's modulation must be one the bundle DECLARES, and
  // its bits must fit the head's stride.
  bool modulation_ok = false;
  if (e->model3_conditioned) {
    // ANY modulation the bundle declares, not every one: the set is what the single bundle serves, and a
    // reception carries exactly one of them. (Written as a loop that stops at the first mismatch, this refused
    // all four modulations including the widest, which had passed the per-modulation comparison before.)
    bool declared = false;
    for (unsigned i = 0; i != e->model3_nof_qm; ++i) {
      declared = declared || (e->model3_qm_set[i] == out.bits_per_symbol);
    }
    modulation_ok = declared && (out.bits_per_symbol <= DEPTH3_COND_STRIDE);
  }
  else {
    modulation_ok = (NOF_SYMBOLS_PER_SLOT * out.bits_per_symbol == e->model3_channels[0]);
  }
  if (!modulation_ok) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: the bound model emits {} LLR channels{} and does not serve Qm={}", e->model3_channels[0],
        e->model3_conditioned ? " (modulation-conditioned)" : "", out.bits_per_symbol);
    return false;
  }
  {
    std::unique_lock<std::mutex> lock(e->mtx);
    e->job_is_depth3 = true;
    e->job_in3       = in;
    e->job_out3      = out;
    e->job_done      = false;
    e->has_job       = true;
  }
  e->cv.notify_all();
  {
    std::unique_lock<std::mutex> lock(e->mtx);
    e->cv.wait(lock, [e]() { return e->job_done; });
  }
  last_predict_us_ = e->last_predict_us;
  return e->job_ok;
}

bool coreml_nn_engine::predict(const float* in, float* out, unsigned nof_subc)
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e == nullptr || e->model == nil) {
    return false;
  }

  {
    std::unique_lock<std::mutex> lock(e->mtx);
    e->job_is_depth3 = false;
    e->job_in       = in;
    e->job_out      = out;
    e->job_nof_subc = nof_subc;
    e->job_done     = false;
    e->has_job      = true;
  }
  e->cv.notify_all();
  {
    std::unique_lock<std::mutex> lock(e->mtx);
    e->cv.wait(lock, [e]() { return e->job_done; });
  }
  last_predict_us_ = e->last_predict_us;
  return e->job_ok;
}

} // namespace metal
} // namespace ocudu
