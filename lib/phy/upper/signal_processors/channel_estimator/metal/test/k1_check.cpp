// Standalone check of the K1 inversion variants against a CPU reference.
#include "ocudu_metal_mmse_engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <vector>

using namespace ocudu;

static void cpu_inv(std::vector<float>& a, unsigned n)
{
  std::vector<float> gj(n * 2 * n, 0.0F);
  for (unsigned r = 0; r != n; ++r) {
    for (unsigned c = 0; c != n; ++c) {
      gj[r * 2 * n + c] = a[r * n + c];
    }
    gj[r * 2 * n + n + r] = 1.0F;
  }
  for (unsigned col = 0; col != n; ++col) {
    const float inv = 1.0F / gj[col * 2 * n + col];
    for (unsigned c = 0; c != 2 * n; ++c) {
      gj[col * 2 * n + c] *= inv;
    }
    for (unsigned r = 0; r != n; ++r) {
      if (r == col) continue;
      const float f = gj[r * 2 * n + col];
      if (f == 0.0F) continue;
      for (unsigned c = 0; c != 2 * n; ++c) {
        gj[r * 2 * n + c] -= f * gj[col * 2 * n + c];
      }
    }
  }
  for (unsigned r = 0; r != n; ++r) {
    for (unsigned c = 0; c != n; ++c) {
      a[r * n + c] = gj[r * 2 * n + n + c];
    }
  }
}

int main()
{
  for (unsigned n : {18U, 54U}) {
    // A symmetric positive definite matrix with unit diagonal (the shape of R_pp + loading).
    std::vector<float> a(n * n);
    for (unsigned r = 0; r != n; ++r) {
      for (unsigned c = 0; c != n; ++c) {
        const float d = static_cast<float>((r > c) ? (r - c) : (c - r));
        a[r * n + c] = 1.0F / (1.0F + 0.5F * d * d);
      }
      // Well conditioned on purpose: the estimate's own A is R_pp + (sigma2 + ridge) I with a small
      // sigma2, i.e. nearly singular, and there the element-wise relative error of ANY inversion
      // (CPU Gauss-Jordan included) is huge - so a cross-check needs a matrix where the inverse is
      // actually determined by the input to float precision.
      a[r * n + r] += 10.0F;
    }
    std::vector<float> ref = a;
    cpu_inv(ref, n);

    metal::mmse_engine engine;
    if (!engine.init()) {
      std::printf("engine init failed\n");
      return 1;
    }
    if (!engine.reserve_buffer(a.data(), a.size() * sizeof(float))) {
      std::printf("reserve failed\n");
      return 1;
    }
    std::vector<float> got = a;
    // Warm-up (first call pays the pipeline setup) and then a timed loop.
    if (!engine.invert(got.data(), n, 1)) {
      std::printf("n=%u invert() failed\n", n);
      return 1;
    }
    double gpu_us = 0.0;
    const unsigned reps = 20;
    for (unsigned k = 0; k != reps; ++k) {
      got = a;
      if (!engine.invert(got.data(), n, 1)) {
        return 1;
      }
      gpu_us += engine.last_gpu_wait_us();
    }
    std::printf("n=%u: invert GPU time = %.1f us/call (%u systems)\n", n, gpu_us / reps, 1);
    for (unsigned sys = 2; sys <= 8; sys += 2) {
      std::vector<float> multi(static_cast<std::size_t>(sys) * n * n);
      for (unsigned s = 0; s != sys; ++s) {
        std::copy(a.begin(), a.end(), multi.begin() + static_cast<std::size_t>(s) * n * n);
      }
      double us = 0.0;
      for (unsigned k = 0; k != reps; ++k) {
        if (!engine.invert(multi.data(), n, sys)) return 1;
        us += engine.last_gpu_wait_us();
      }
      std::printf("n=%u: %u systems -> %.1f us/call (%.1f us/system)\n", n, sys, us / reps, us / reps / sys);
    }
    double max_rel = 0.0;
    unsigned bad = 0;
    for (unsigned i = 0; i != n * n; ++i) {
      const double rel = std::fabs(got[i] - ref[i]) / std::fmax(std::fabs(ref[i]), 1e-30);
      if (rel > max_rel) max_rel = rel;
      if (rel > 1e-4) ++bad;
    }
    std::printf("n=%u: max_rel=%.3e bad=%u/%u\n", n, max_rel, bad, n * n);
    for (unsigned k = 0; k != 4; ++k) {
      std::printf("   [%u] got=%g ref=%g\n", k, got[k], ref[k]);
    }
  }
  return 0;
}
