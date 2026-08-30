// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#include "ocudu_coreml_nn_engine.h"

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include "ocudu/ocudulog/ocudulog.h"
#include <chrono>

using namespace ocudu;

namespace {

struct coreml_nn_engine_impl {
  MLModel* model      = nil;
  NSString* in_name   = nil;
  NSString* out_name  = nil;
};

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
  return e->in_name != nil && e->out_name != nil;
}

bool coreml_nn_engine::predict(const float* in, float* out, unsigned nof_subc)
{
  auto* e = static_cast<coreml_nn_engine_impl*>(impl);
  if (e == nullptr || e->model == nil) {
    return false;
  }

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
  last_predict_us_ = std::chrono::duration<double, std::micro>(t1 - t0).count();

  if (result == nil) {
    ocudulog::fetch_basic_logger("PHY").error("AI-CE: prediction failed: {}",
                                              err != nil ? err.localizedDescription.UTF8String : "nil error");
    return false;
  }
  return true;
}

} // namespace metal
} // namespace ocudu
