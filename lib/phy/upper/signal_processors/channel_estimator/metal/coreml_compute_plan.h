#pragma once

/// Prints where Core ML places each operation of a compiled bundle, and returns the count it read.
///
/// Why this exists (design section 19.348): the receiver's latency is bimodal across runs -- about 400 microseconds or about
/// 800 -- and the four compute-unit tiers show that neither the GPU path (656) nor the CPU (3465) explains the slow one, so
/// the remaining reading is a MIXED placement, part of the graph on the neural engine and part on the CPU. `MLComputePlan`
/// is the only public way to see the choice, and it must be read from the process whose state is in question.
///
/// Prints one line: the number of operations per device. Returns 0 when the platform has no plan API.
unsigned ocudu_print_compute_plan(const char* model_path);
