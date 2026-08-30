// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI
//
// G2 head-to-head: metal_mmse (and the cpu average estimator) vs the synthetic
// PUSCH test set exported by ai_train/gen_pusch_dataset.py (R_test/Y_test .npy).
// Usage: helena_head2head_bench <R_test.npy> <Y_test.npy> [--nogpu]

#include "../port_channel_estimator_metal_mmse_impl.h"
#include "port_channel_estimator_helpers.h"
#include "ocudu/phy/support/resource_grid_reader.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/phy/support/time_alignment_estimator/time_alignment_estimator_factories.h"
#include "ocudu/ran/resource_allocation/rb_bitmap.h"
#include "ocudu/support/math/math_utils.h"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace ocudu;

namespace {

// Minimal NPY (v1.0, C-order, float32) reader.
std::vector<float> load_npy(const std::string& path, std::vector<unsigned>& shape)
{
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    std::fprintf(stderr, "cannot open %s\n", path.c_str());
    std::exit(1);
  }
  char magic[6];
  f.read(magic, 6);
  if (std::memcmp(magic, "\x93NUMPY", 6) != 0) {
    std::fprintf(stderr, "bad npy magic in %s\n", path.c_str());
    std::exit(1);
  }
  unsigned char ver[2];
  f.read(reinterpret_cast<char*>(ver), 2);
  unsigned short hlen = 0;
  f.read(reinterpret_cast<char*>(&hlen), 2);
  std::string header(hlen, '\0');
  f.read(header.data(), hlen);
  shape.clear();
  // Extract the shape tuple entries.
  size_t pos = header.find("'shape': (");
  if (pos == std::string::npos) {
    std::fprintf(stderr, "shape not found in %s\n", path.c_str());
    std::exit(1);
  }
  pos += 10;
  while (header[pos] != ')') {
    // Skip separators (',' and spaces) between entries.
    while (header[pos] == ',' || header[pos] == ' ') {
      ++pos;
    }
    unsigned v = 0;
    while (header[pos] >= '0' && header[pos] <= '9') {
      v = v * 10 + (header[pos] - '0');
      ++pos;
    }
    shape.push_back(v);
  }
  size_t n = 1;
  for (unsigned s : shape) {
    n *= s;
  }
  std::vector<float> data(n);
  f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(n * sizeof(float)));
  return data;
}

class grid_fake : public resource_grid_reader
{
public:
  explicit grid_fake(unsigned nof_subc) : symbols(nof_subc) {}

  void set_symbol(span<const cf_t> data)
  {
    for (unsigned k = 0; k != data.size(); ++k) {
      symbols[k] = to_cbf16(data[k]);
    }
    has_symbol = true;
  }

  unsigned     get_nof_ports() const override { return 1; }
  unsigned     get_nof_subc() const override { return static_cast<unsigned>(symbols.size()); }
  unsigned     get_nof_symbols() const override { return 1; }
  bool         is_empty(unsigned) const override { return !has_symbol; }
  bool         is_empty() const override { return !has_symbol; }
  crb_interval get_allocation_range(unsigned, unsigned) const override { return {0, 0}; }
  span<cf_t>   get(span<cf_t> s, unsigned, unsigned, unsigned, const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override { return s; }
  span<cbf16_t> get(span<cbf16_t> s, unsigned, unsigned, unsigned, const bounded_bitset<MAX_NOF_SUBCARRIERS>&) const override { return s; }
  void get(span<cf_t> s, unsigned, unsigned, unsigned, unsigned = 1) const override
  {
    for (unsigned k = 0; k != s.size(); ++k) {
      s[k] = to_cf(symbols[k]);
    }
  }
  void get(span<cbf16_t> s, unsigned, unsigned, unsigned) const override
  {
    for (unsigned k = 0; k != s.size(); ++k) {
      s[k] = symbols[k];
    }
  }
  span<const cbf16_t> get_view(unsigned, unsigned) const override { return symbols; }

private:
  std::vector<cbf16_t> symbols;
  bool                 has_symbol = false;
};

std::unique_ptr<time_alignment_estimator> make_ta_estimator()
{
  auto dft_factory = create_dft_processor_factory_generic();
  auto ta_factory  = create_time_alignment_estimator_dft_factory(dft_factory);
  return ta_factory->create();
}

port_channel_estimator::configuration make_config(unsigned n_prb)
{
  port_channel_estimator::configuration cfg;
  cfg.scs          = subcarrier_spacing::kHz15;
  cfg.cp           = cyclic_prefix::NORMAL;
  cfg.first_symbol = 0;
  cfg.nof_symbols  = MAX_NSYMB_PER_SLOT;
  cfg.rx_ports.emplace_back(0);
  cfg.scaling      = 1.0F;
  port_channel_estimator::layer_dmrs_pattern pattern;
  pattern.symbols.resize(MAX_NSYMB_PER_SLOT);
  pattern.symbols.set(2);
  pattern.symbols.set(7);
  pattern.symbols.set(11);
  crb_bitmap mask;
  mask.resize(MAX_NOF_PRBS);
  mask |= crb_interval{0, n_prb};
  pattern.rb_mask = mask;
  pattern.re_pattern.resize(NOF_SUBCARRIERS_PER_RB);
  for (unsigned k = 0; k != 12; k += 2) {
    pattern.re_pattern.set(k);
  }
  cfg.dmrs_pattern.emplace_back(pattern);
  return cfg;
}

dmrs_symbol_list make_pilots(unsigned n_prb, unsigned nof_symbols)
{
  dmrs_symbol_list pilots;
  pilots.resize({.nof_subc = n_prb * 6, .nof_symbols = nof_symbols, .nof_slices = 1});
  for (unsigned s = 0; s != nof_symbols; ++s) {
    span<cf_t> sym = pilots.get_symbol(s, 0);
    for (auto& v : sym) {
      v = {1.0F, 0.0F}; // unit pilots: LS == rx
    }
  }
  return pilots;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <R_test.npy> <Y_test.npy> [--nogpu]\n", argv[0]);
    return 1;
  }
  const std::string r_path = argv[1];
  const std::string y_path = argv[2];
  const bool        nogpu  = (argc > 3 && std::string(argv[3]) == "--nogpu");
  if (nogpu) {
    setenv("OCUDU_MMSE_NOGPU", "1", 1);
  }

  std::vector<unsigned> rshape, yshape;
  std::vector<float>    r = load_npy(r_path, rshape);
  std::vector<float>    y = load_npy(y_path, yshape);
  // R: [n, 3, 612, 2]; Y: [n, 612, 14, 2]
  const unsigned n   = rshape[0];
  const unsigned nff = rshape[2];
  std::fprintf(stderr, "loaded R %ux%ux%ux%u, Y %ux%ux%ux%u\n",
               rshape[0], rshape[1], rshape[2], rshape[3], yshape[0], yshape[1], yshape[2], yshape[3]);

  auto cfg    = make_config(nff / 12);
  auto pilots = make_pilots(nff / 12, 3);

  std::vector<std::unique_ptr<port_channel_estimator>> ests;
  {
    // compensate_cfo = false: the synthetic set has NO CFO, so the noise-derived
    // CFO phase at low SNR would randomly rotate the DMRS symbols and destroy the
    // TD average / MMSE coherence (the original +0.6 dB harness bug).
    auto cpu = std::make_unique<port_channel_estimator_average_impl>(
        create_interpolator(), make_ta_estimator(),
        port_channel_estimator_fd_smoothing_strategy::filter,
        port_channel_estimator_td_interpolation_strategy::average, false);
    ests.push_back(std::move(cpu));
    auto mmse = std::make_unique<port_channel_estimator_metal_mmse_impl>(
        create_interpolator(), make_ta_estimator(),
        std::make_shared<channel_statistics_estimator_fixed>(370e-9F, 0.0F), 3, false);
    ests.push_back(std::move(mmse));
  }
  const char* names[2] = {"cpu-average", "metal_mmse"};
  std::vector<double> err(2, 0.0);
  double              sig = 0.0;

  for (unsigned i = 0; i != n; ++i) {
    grid_fake grid(nff);
    for (unsigned s = 0; s != 3; ++s) {
      std::vector<cf_t> sym(nff);
      for (unsigned k = 0; k != nff; ++k) {
        sym[k] = {r[((i * 3 + s) * nff + k) * 2], r[((i * 3 + s) * nff + k) * 2 + 1]};
      }
      grid.set_symbol(sym);
    }
    const float* yb = &y[static_cast<size_t>(i) * nff * 14 * 2];
    for (unsigned e = 0; e != 2; ++e) {
      const auto& res = ests[e]->compute(grid, 0, pilots, cfg);
      for (unsigned l = 0; l != MAX_NSYMB_PER_SLOT; ++l) {
        std::vector<cbf16_t> est(nff);
        res.get_symbol_ch_estimate(est, l, 0);
        for (unsigned k = 0; k != nff; ++k) {
          const cf_t  h  = to_cf(est[k]);
          // Y_test.npy is [n, 612, 14, 2]: subcarrier-major, then symbol.
          const float tr = yb[(k * 14 + l) * 2];
          const float ti = yb[(k * 14 + l) * 2 + 1];
          const double dr = h.real() - tr;
          const double di = h.imag() - ti;
          err[e] += dr * dr + di * di;
        }
      }
    }
    for (size_t j = 0; j != static_cast<size_t>(nff) * 14 * 2; ++j) {
      sig += static_cast<double>(yb[j]) * yb[j];
    }
  }
  for (unsigned e = 0; e != 2; ++e) {
    std::printf("%-12s NMSE %8.2f dB\n", names[e], 10.0 * std::log10(err[e] / sig));
  }
  return 0;
}
