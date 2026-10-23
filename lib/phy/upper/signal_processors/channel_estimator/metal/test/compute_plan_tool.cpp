// Prints where Core ML places each operation of a model, without loading the engine.
//
// The receiver's latency flips between a fast state and one about twice as slow from leg to leg, and the engine reports only
// the compute units it REQUESTS. This tool is the only way to see the placement Core ML actually chose, and it takes a model
// path directly so it can be pointed at an `.mlpackage` as well as a compiled bundle (design section 19.348).

#include "coreml_compute_plan.h"

#include <cstdio>

int main(int argc, char** argv)
{
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s <model.mlmodelc|model.mlpackage>\n", argv[0]);
    return 1;
  }
  const unsigned counted = ocudu_print_compute_plan(argv[1]);
  return (counted == 0) ? 2 : 0;
}
