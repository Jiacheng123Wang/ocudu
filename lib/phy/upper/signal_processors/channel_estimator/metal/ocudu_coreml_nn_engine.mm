// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_coreml_nn_engine.h"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include "ocudu/ocudulog/ocudulog.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace ocudu;

namespace {

struct coreml_nn_engine_impl {
  MLModel* model     = nil;
  NSString* in_name  = nil;
  NSString* out_name = nil;

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

  // Dedicated worker thread: all predictions (including the warm-up) run here,
  // so the per-thread Core ML/ANE initialization is paid exactly once, at
  // construction, never on the UL executor threads.
  e->worker = std::thread([e]() {
    for (;;) {
      const float* in;
      float*       out;
      unsigned     nsc;
      {
        std::unique_lock<std::mutex> lock(e->mtx);
        e->cv.wait(lock, [e]() { return e->has_job || e->shutdown; });
        if (e->shutdown) {
          return;
        }
        in  = e->job_in;
        out = e->job_out;
        nsc = e->job_nof_subc;
      }
      const bool ok = run_prediction(e, in, out, nsc);
      {
        std::lock_guard<std::mutex> lock(e->mtx);
        e->job_ok   = ok;
        e->job_done = true;
        e->has_job  = false;
      }
      e->cv.notify_all();
    }
  });
  return true;
}

bool coreml_nn_engine::predict(const float* in, float* out, unsigned nof_subc)
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e == nullptr || e->model == nil) {
    return false;
  }

  {
    std::unique_lock<std::mutex> lock(e->mtx);
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
