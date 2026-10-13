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
#include <mutex>
#include <thread>
#include <vector>

using namespace ocudu;

namespace {

/// OFDM symbols per slot, which the contract folds into the channel axis.
constexpr unsigned NOF_SYMBOLS_PER_SLOT = 14;

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
  const unsigned llr_channels   = e->model3_channels[0];
  const unsigned noise_channels = e->model3_channels[1];
  const unsigned ch_channels    = e->model3_channels[2];

  NSArray<NSNumber*>* in_shape = @[ @1, @(nsc), @1, @64 ];
  MLMultiArray*       in_arr   = [[MLMultiArray alloc]
      initWithDataPointer:const_cast<uint16_t*>(in.plane)
                    shape:in_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:@[ @(nsc * 64), @64, @64, @1 ]
              deallocator:nil
                    error:nil];
  if (in_arr == nil) {
    return false;
  }

  NSArray<NSNumber*>* llr_shape    = @[ @1, @(nsc), @1, @(llr_channels) ];
  NSArray<NSNumber*>* noise_shape  = @[ @1, @(nsc), @1, @(noise_channels) ];
  NSArray<NSNumber*>* ch_shape     = @[ @1, @(nsc), @1, @(ch_channels) ];
  NSArray<NSNumber*>* scalar_shape = @[ @1, @1, @1, @1 ];

  MLMultiArray* llr_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.llr
                    shape:llr_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:@[ @(nsc * llr_channels), @(llr_channels), @(llr_channels), @1 ]
              deallocator:nil
                    error:nil];
  MLMultiArray* noise_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.post_eq_noise
                    shape:noise_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:@[ @(nsc * noise_channels), @(noise_channels), @(noise_channels), @1 ]
              deallocator:nil
                    error:nil];
  MLMultiArray* ch_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.ch_est
                    shape:ch_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:@[ @(nsc * ch_channels), @(ch_channels), @(ch_channels), @1 ]
              deallocator:nil
                    error:nil];
  MLMultiArray* dmrs_arr = [[MLMultiArray alloc]
      initWithDataPointer:out.dmrs_noise
                    shape:scalar_shape
                 dataType:MLMultiArrayDataTypeFloat16
                  strides:@[ @1, @1, @1, @1 ]
              deallocator:nil
                    error:nil];
  if (llr_arr == nil || noise_arr == nil || ch_arr == nil || dmrs_arr == nil) {
    return false;
  }

  MLDictionaryFeatureProvider* input =
      [[MLDictionaryFeatureProvider alloc] initWithDictionary:@{e->in3_name : in_arr} error:nil];
  if (input == nil) {
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
  MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
  cfg.computeUnits          = MLComputeUnitsAll; // ANE > GPU > CPU as the runtime sees fit
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
  MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
  cfg.computeUnits          = MLComputeUnitsAll;
  NSError* err              = nil;
  e->model3                 = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
  if (e->model3 == nil) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: model load failed: {}",
        err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }

  MLModelDescription* desc = e->model3.modelDescription;
  e->in3_name              = @"x";
  MLFeatureDescription* in_desc = desc.inputDescriptionsByName[e->in3_name];
  if (in_desc == nil || in_desc.type != MLFeatureTypeMultiArray || in_desc.multiArrayConstraint == nil) {
    ocudulog::fetch_basic_logger("PHY").error("AI depth-3: the model has no multi-array input x");
    return false;
  }
  NSArray<NSNumber*>* in_shape = in_desc.multiArrayConstraint.shape;
  // The contract's plane: [1, subcarriers, 1, 64], fp16. A model trained in another layout would
  // be fed nonsense and would produce plausible logits for it, so the shape is checked here.
  if (in_shape.count != 4 || in_shape[0].unsignedIntValue != 1 || in_shape[2].unsignedIntValue != 1 ||
      in_shape[3].unsignedIntValue != 64) {
    ocudulog::fetch_basic_logger("PHY").error("AI depth-3: input x is not [1, nsc, 1, 64]");
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
    if (out_desc.multiArrayConstraint != nil && out_desc.multiArrayConstraint.shape.count == 4) {
      e->model3_channels[head] = out_desc.multiArrayConstraint.shape[3].unsignedIntValue;
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

  e->keepalive_in3.resize(static_cast<size_t>(e->model3_nof_subc) * 64, 0);
  e->keepalive_llr3.resize(static_cast<size_t>(e->model3_nof_subc) * NOF_SYMBOLS_PER_SLOT * 8, 0);
  e->keepalive_noise3.resize(static_cast<size_t>(e->model3_nof_subc) * NOF_SYMBOLS_PER_SLOT, 0);
  e->keepalive_ch3.resize(static_cast<size_t>(e->model3_nof_subc) * 2 * NOF_SYMBOLS_PER_SLOT, 0);

  ocudulog::fetch_basic_logger("PHY").debug(
      "AI depth-3: Core ML model loaded from {} ({} subcarriers, four heads)",
      modelc_path,
      e->model3_nof_subc);

  // The worker thread may not have been started yet when only a depth-3 model is configured.
  start_worker(e);
  return true;
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
  if (NOF_SYMBOLS_PER_SLOT * out.bits_per_symbol != e->model3_channels[0]) {
    ocudulog::fetch_basic_logger("PHY").error(
        "AI depth-3: the bound model emits {} LLR channels, the caller expects {}",
        e->model3_channels[0], NOF_SYMBOLS_PER_SLOT * out.bits_per_symbol);
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
