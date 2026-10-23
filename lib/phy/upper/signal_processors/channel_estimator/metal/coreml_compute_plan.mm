#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

#include "coreml_compute_plan.h"

#include <cstdio>

namespace {

void count_operations(MLModelStructureProgramBlock* block, MLComputePlan* plan,
                      unsigned& ane, unsigned& gpu, unsigned& cpu, unsigned& other);

void count_operation(MLModelStructureProgramOperation* operation, MLComputePlan* plan,
                     unsigned& ane, unsigned& gpu, unsigned& cpu, unsigned& other)
{
  if (operation == nil) {
    return;
  }
  id<MLComputeDeviceProtocol> device = [plan computeDeviceUsageForMLProgramOperation:operation];
  if ([device isKindOfClass:[MLNeuralEngineComputeDevice class]]) {
    ++ane;
  }
  else if ([device isKindOfClass:[MLGPUComputeDevice class]]) {
    ++gpu;
  }
  else if ([device isKindOfClass:[MLCPUComputeDevice class]]) {
    ++cpu;
  }
  else {
    ++other;
  }
  for (MLModelStructureProgramBlock* inner in operation.blocks) {
    count_operations(inner, plan, ane, gpu, cpu, other);
  }
}

void count_operations(MLModelStructureProgramBlock* block, MLComputePlan* plan,
                      unsigned& ane, unsigned& gpu, unsigned& cpu, unsigned& other)
{
  if (block == nil) {
    return;
  }
  for (MLModelStructureProgramOperation* operation in block.operations) {
    count_operation(operation, plan, ane, gpu, cpu, other);
  }
}

}  // namespace

unsigned ocudu_print_compute_plan(const char* model_path)
{
  if (@available(macOS 14.0, *)) {
    NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:model_path]];
    MLModelConfiguration* configuration = [[MLModelConfiguration alloc] init];
    configuration.computeUnits = MLComputeUnitsAll;

    __block unsigned ane = 0, gpu = 0, cpu = 0, other = 0;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    [MLComputePlan loadContentsOfURL:url
                       configuration:configuration
                   completionHandler:^(MLComputePlan* plan, NSError* error) {
                     if (plan == nil) {
                       std::fprintf(stderr, "[plan] could not load a plan: %s\n",
                                    error.localizedDescription.UTF8String ?: "unknown");
                     }
                     else {
                       // THE PLAN CAN REFUSE. A structure that is not a program (or a URL the API does not accept) raises
                       // an NSException rather than returning nil, and an uncaught one takes the whole probe down -- which
                       // is exactly what the first run did. Everything inside the handler is therefore guarded.
                       @try {
                         MLModelStructure* structure = plan.modelStructure;
                         MLModelStructureProgram* program = structure.program;
                         // `functions` is a DICTIONARY keyed by function name, not an array: iterating it directly
                         // yields the keys, and sending `-block` to an NSString is what threw on the first attempt.
                         for (NSString* name in program.functions) {
                           MLModelStructureProgramFunction* function = program.functions[name];
                           count_operations(function.block, plan, ane, gpu, cpu, other);
                         }
                       }
                       @catch (NSException* exception) {
                         std::fprintf(stderr, "[plan] the plan could not be walked: %s\n", exception.reason.UTF8String);
                       }
                     }
                     dispatch_semaphore_signal(done);
                   }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    std::fprintf(stderr, "[plan] operations: ane %u, gpu %u, cpu %u, other %u\n", ane, gpu, cpu, other);
    return ane + gpu + cpu + other;
  }
  std::fprintf(stderr, "[plan] this platform has no MLComputePlan\n");
  return 0;
}
