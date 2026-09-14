// Standalone check of the K1 inversion variants against a CPU reference.
//
// Two modes:
//   (no arguments)            well-conditioned synthetic matrices - the algorithm-level check.
//   <a.bin> [n]               a REAL A matrix (n x n float32, the estimator's own
//                             R_pp + (sigma2 + ridge) I), which is nearly singular, with the
//                             condition number measured and the error of every inversion form
//                             reported against a float64 reference.
//
// The second mode exists to answer a question the synthetic one cannot: at the block order this
// hardware runs (54), is the device inverse's error the algorithm's doing, or is it the float32
// floor set by the matrix's own conditioning? The verdict is printed next to the numbers.
#include "ocudu_metal_mmse_engine.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <string>
#include <vector>

using namespace ocudu;

/// Float32 Gauss-Jordan - the estimator's host inversion (gauss_jordan_invert()), on the [A | I] form.
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

/// Float64 Gauss-Jordan: the reference the float inversions are measured against.
static std::vector<double> cpu_inv_ref(const std::vector<float>& a, unsigned n)
{
  std::vector<double> gj(n * 2 * n, 0.0);
  for (unsigned r = 0; r != n; ++r) {
    for (unsigned c = 0; c != n; ++c) {
      gj[r * 2 * n + c] = static_cast<double>(a[r * n + c]);
    }
    gj[r * 2 * n + n + r] = 1.0;
  }
  for (unsigned col = 0; col != n; ++col) {
    const double inv = 1.0 / gj[col * 2 * n + col];
    for (unsigned c = 0; c != 2 * n; ++c) {
      gj[col * 2 * n + c] *= inv;
    }
    for (unsigned r = 0; r != n; ++r) {
      if (r == col) continue;
      const double f = gj[r * 2 * n + col];
      if (f == 0.0) continue;
      for (unsigned c = 0; c != 2 * n; ++c) {
        gj[r * 2 * n + c] -= f * gj[col * 2 * n + c];
      }
    }
  }
  std::vector<double> out(n * n);
  for (unsigned r = 0; r != n; ++r) {
    for (unsigned c = 0; c != n; ++c) {
      out[r * n + c] = gj[r * 2 * n + n + c];
    }
  }
  return out;
}

/// \brief cond_2(A) by power iteration on A (symmetric) and on its inverse.
///
/// A is symmetric positive definite, so its singular values are its eigenvalues: two power
/// iterations (on A and on A^-1) bracket the condition number without an SVD. Enough for a verdict
/// at the order of magnitude level, which is all this check needs.
static double cond_estimate(const std::vector<float>& a, const std::vector<double>& a_inv, unsigned n)
{
  std::vector<double> x(n, 1.0 / std::sqrt(static_cast<double>(n)));
  auto power = [&](bool inverse) {
    double lam = 0.0;
    for (unsigned it = 0; it != 200; ++it) {
      std::vector<double> y(n, 0.0);
      for (unsigned r = 0; r != n; ++r) {
        double acc = 0.0;
        for (unsigned c = 0; c != n; ++c) {
          acc += (inverse ? a_inv[r * n + c] : static_cast<double>(a[r * n + c])) * x[c];
        }
        y[r] = acc;
      }
      double norm = 0.0;
      for (double v : y) norm += v * v;
      norm = std::sqrt(norm);
      if (norm == 0.0) return 0.0;
      for (unsigned r = 0; r != n; ++r) x[r] = y[r] / norm;
      lam = norm;
    }
    return lam;
  };
  const double lmax = power(false);
  const double lmin_inv = power(true);
  return (lmin_inv == 0.0) ? 1e30 : lmax * lmin_inv;
}

/// \brief Per-element relative error (max), the residual max|A X - I|, and the median |X|.
static void report(const char* name, const std::vector<float>& got, const std::vector<double>& ref, unsigned n,
                   const std::vector<float>& a)
{
  double max_rel = 0.0;
  unsigned bad = 0;
  std::vector<float> med(got.size());
  for (unsigned i = 0; i != n * n; ++i) {
    const double rel = std::fabs(static_cast<double>(got[i]) - ref[i]) / std::fmax(std::fabs(ref[i]), 1e-30);
    max_rel = std::max(max_rel, rel);
    if (rel > 1e-3) ++bad;
    med[i] = std::fabs(got[i]);
  }
  double max_res = 0.0;
  for (unsigned r = 0; r != n; ++r) {
    for (unsigned c = 0; c != n; ++c) {
      double acc = 0.0;
      for (unsigned k = 0; k != n; ++k) {
        acc += static_cast<double>(a[r * n + k]) * static_cast<double>(got[k * n + c]);
      }
      const double expect = (r == c) ? 1.0 : 0.0;
      max_res = std::max(max_res, std::fabs(acc - expect));
    }
  }
  std::sort(med.begin(), med.end());
  std::printf("  %-14s max_rel=%.3e (>1e-3: %u/%u)  max|A*X-I|=%.3e  median|X|=%.4g\n",
              name, max_rel, bad, n * n, max_res, med[med.size() / 2]);
}

/// Pulls one real A matrix out of a capture run and drops it here, so this check runs on the matrix
/// the estimator actually inverts (the synthetic ones are well conditioned on purpose and cannot
/// answer the accuracy question).
static std::vector<float> load_real(const char* path, unsigned& n)
{
  FILE* f = std::fopen(path, "rb");
  if (f == nullptr) {
    std::printf("cannot open %s\n", path);
    std::exit(1);
  }
  std::fseek(f, 0, SEEK_END);
  const long bytes = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  std::vector<float> a(static_cast<std::size_t>(bytes) / sizeof(float));
  if (std::fread(a.data(), sizeof(float), a.size(), f) != a.size()) {
    std::printf("short read on %s\n", path);
    std::exit(1);
  }
  std::fclose(f);
  n = static_cast<unsigned>(std::lround(std::sqrt(static_cast<double>(a.size()))));
  if (static_cast<std::size_t>(n) * n != a.size()) {
    std::printf("%s holds %zu floats, which is not a square matrix\n", path, a.size());
    std::exit(1);
  }
  return a;
}

static int check_real(const char* path)
{
  unsigned n = 0;
  std::vector<float> a = load_real(path, n);
  std::printf("real A: n=%u  A[0]=%.9g  diag=%.9g\n", n, a[0], a[static_cast<std::size_t>(n) + 1]);

  const std::vector<double> ref = cpu_inv_ref(a, n);
  const double              cond = cond_estimate(a, ref, n);
  const double              floor = cond * 1.2e-7; // ~ eps(float32) * cond(A)
  std::printf("cond_2(A) ~= %.3e  =>  float32 error floor ~ %.3e\n", cond, floor);

  std::vector<float> host = a;
  cpu_inv(host, n);
  report("host float32", host, ref, n, a);

  metal::mmse_engine engine;
  if (!engine.init()) {
    std::printf("engine init failed\n");
    return 1;
  }
  if (!engine.reserve_buffer(a.data(), a.size() * sizeof(float))) {
    std::printf("reserve failed\n");
    return 1;
  }
  std::vector<float> dev = a;
  if (!engine.invert(dev.data(), n, 1)) {
    std::printf("device invert() failed\n");
    return 1;
  }
  report("device K1", dev, ref, n, a);

  double us = 0.0;
  const unsigned reps = 20;
  for (unsigned k = 0; k != reps; ++k) {
    std::vector<float> tmp = a;
    if (!engine.invert(tmp.data(), n, 1)) return 1;
    us += engine.last_gpu_wait_us();
  }
  std::printf("  device K1 time = %.1f us/call\n", us / reps);

  // Verdict (the criterion this tool was written for).
  double dev_rel = 0.0;
  for (unsigned i = 0; i != n * n; ++i) {
    dev_rel = std::max(dev_rel,
                       std::fabs(static_cast<double>(dev[i]) - ref[i]) / std::fmax(std::fabs(ref[i]), 1e-30));
  }
  std::printf("VERDICT: device max_rel=%.3e vs float32 floor=%.3e (ratio %.1f) -> %s\n",
              dev_rel,
              floor,
              dev_rel / floor,
              (dev_rel < 10.0 * floor) ? "CONDITIONING-BOUND (the algorithm is at the float32 limit of "
                                         "this matrix; only a different form or higher precision helps)"
                                       : "IMPLEMENTATION-LIMITED (the error is above what this "
                                         "matrix's conditioning allows: the kernel can be improved)");
  return 0;
}

int main(int argc, char** argv)
{
  if (argc > 1) {
    return check_real(argv[1]);
  }
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
