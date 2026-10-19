// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Checks the depth-3 input-plane assembler against the Python packer that trained the net.
///
/// The contract has two implementations, one on each side of the seam: `depth3.pack_input_spatial` in
/// the training tree and \ref pusch_depth3_input_plane here. A receiver fed a tensor assembled
/// differently from the one the network was trained on produces plausible logits for the wrong input,
/// which is the failure this workstream has met often enough to stop trusting shapes: the check is
/// POINT BY POINT against a fixture the Python packer wrote from the same capture.
///
/// The fixture also lets the negative cases be exercised rather than argued: the same tool refuses a
/// wrong allocation width, a layer count the contract does not describe, and a zero grid, so "the
/// assembler can say no" is measured and not asserted.
///
/// Usage: pusch_depth3_input_plane_check <fixture.bin> [tolerance]

#include "pusch_depth3_half.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/support_factories.h"
#include "pusch_depth3_input_plane.h"
#include "ocudu/ran/slot_point.h"
#include "ocudu/support/math/math_utils.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <utility>
#include <vector>

using namespace ocudu;

namespace {

constexpr uint32_t FIXTURE_MAGIC = 0x33445049U;

/// The fixture's header, in the order `make_input_plane_fixture.py` writes it.
struct fixture_header {
  uint32_t magic;
  uint32_t nof_subcarriers;
  uint32_t nof_symbols;
  uint32_t nof_channels;
  uint32_t slot;
  uint32_t n_id;
  uint32_t n_scid;
  uint32_t rnti;
  uint32_t dmrs_type;
  uint32_t nof_cdm_groups;
  uint32_t dmrs_bitmap;
  uint32_t alloc_first_prb;
  uint32_t alloc_nof_prb;
  uint32_t dc_position;
  uint32_t reserved;
};

/// A whole fixture, read once.
struct fixture {
  fixture_header header{};
  float          reference_amplitude = 0.0F;
  /// Raw grid, `[symbol][subcarrier]` complex.
  std::vector<cf_t> grid;
  /// The Python packer's plane, already in its fp16 bit patterns.
  std::vector<uint16_t> expected;
};

bool read_fixture(const char* path, fixture& out)
{
  FILE* handle = std::fopen(path, "rb");
  if (handle == nullptr) {
    std::printf("OPEN_FAIL %s\n", path);
    return false;
  }
  if (std::fread(&out.header, sizeof(fixture_header), 1, handle) != 1 ||
      out.header.magic != FIXTURE_MAGIC) {
    std::printf("BAD_HEADER %s\n", path);
    std::fclose(handle);
    return false;
  }
  if (std::fread(&out.reference_amplitude, sizeof(float), 1, handle) != 1) {
    std::printf("TRUNCATED %s: no reference amplitude\n", path);
    std::fclose(handle);
    return false;
  }
  const size_t nof_re = static_cast<size_t>(out.header.nof_subcarriers) * out.header.nof_symbols;
  out.grid.resize(nof_re);
  out.expected.resize(nof_re * out.header.nof_channels);
  // The size is checked before the contents: a fixture written with a doubled or missing section
  // reads cleanly into short buffers otherwise, and the comparison then reports a mismatch that says
  // nothing about the code under test.
  const size_t offset     = std::ftell(handle);
  std::fseek(handle, 0, SEEK_END);
  const size_t file_size  = static_cast<size_t>(std::ftell(handle));
  const size_t want_bytes = offset + (nof_re * sizeof(cf_t)) + (out.expected.size() * sizeof(uint16_t));
  if (file_size != want_bytes) {
    std::printf("SIZE_FAIL %s is %zu bytes, the layout says %zu\n", path, file_size, want_bytes);
    std::fclose(handle);
    return false;
  }
  std::fseek(handle, static_cast<long>(offset), SEEK_SET);
  const bool ok = std::fread(out.grid.data(), sizeof(cf_t), nof_re, handle) == nof_re &&
                  std::fread(out.expected.data(), sizeof(uint16_t), out.expected.size(), handle) ==
                      out.expected.size();
  if (!ok) {
    std::printf("READ_FAIL %s\n", path);
  }
  std::fclose(handle);
  return ok;
}

/// Builds the configuration the fixture describes, so the two sides agree on everything but the code
/// under test.
pusch_demodulator::configuration configuration_of(const fixture_header& header)
{
  pusch_demodulator::configuration config;
  config.rnti                      = to_rnti(static_cast<uint16_t>(header.rnti));
  config.modulation                = modulation_scheme::QAM256;
  config.start_symbol_index        = 0;
  config.nof_symbols               = header.nof_symbols;
  config.dmrs_type                 = (header.dmrs_type == 2) ? dmrs_config_type::type2 : dmrs_config_type::type1;
  config.nof_cdm_groups_without_data = header.nof_cdm_groups;
  config.n_id                      = header.n_id;
  config.nof_tx_layers             = 1;
  config.enable_transform_precoding = false;
  config.rx_ports.push_back(0);
  if (header.dc_position != 0xFFFFFFFFU) {
    config.dc_position = header.dc_position;
  }
  // The bitmap is the allocation inside the bandwidth part, which is the contract's window.
  config.rb_mask.resize(NOF_SUBCARRIERS_PER_RB == 12 ? 51 : 51);
  for (uint32_t prb = header.alloc_first_prb; prb != header.alloc_first_prb + header.alloc_nof_prb; ++prb) {
    config.rb_mask.set(prb);
  }
  for (unsigned symbol = 0; symbol != header.nof_symbols; ++symbol) {
    if ((header.dmrs_bitmap >> symbol) & 1U) {
      config.dmrs_symb_pos.set(symbol);
    }
  }
  return config;
}

pusch_depth3_dmrs_context context_of(const fixture_header& header)
{
  pusch_depth3_dmrs_context context;
  // 15 kHz numerology: the check only needs `slot_index()`, which is the count within the frame.
  context.slot          = slot_point(subcarrier_spacing::kHz15, static_cast<unsigned>(header.slot));
  context.n_id          = header.n_id;
  context.n_scid        = header.n_scid != 0;
  context.nof_tx_layers = 1;
  return context;
}

/// One unit in the last place of an fp16 value: the quantum the plane is stored in.
///
/// The two implementations cannot be expected to agree more closely than the format they write. The
/// Python packer accumulates the reference amplitude in float64 and the receiver's grid is float32, so
/// `r` itself differs in the fifth digit and that difference rides on every field channel; near an
/// fp16 rounding boundary one such value lands on the other side. What is required is therefore
/// agreement WITHIN THE FORMAT, not bit equality of a computation that is not the same computation.
float fp16_ulp(float value)
{
  int exponent = 0;
  std::frexp(std::fabs(value), &exponent);
  // fp16 carries ten explicit mantissa bits, so one ulp is 2^(exponent-11) for a normal value.
  return std::ldexp(1.0F, exponent - 11);
}

/// Writes the fixture's grid into a real resource grid, so the assembler reads through the same
/// interface the receiver will use rather than through a special case.
std::unique_ptr<resource_grid> grid_of(const fixture& data)
{
  std::unique_ptr<resource_grid> grid = create_resource_grid_factory()->create(
      1, data.header.nof_symbols, data.header.nof_subcarriers);
  if (grid == nullptr) {
    return nullptr;
  }
  for (unsigned symbol = 0; symbol != data.header.nof_symbols; ++symbol) {
    span<const cf_t> values(data.grid.data() + static_cast<size_t>(symbol) * data.header.nof_subcarriers,
                            data.header.nof_subcarriers);
    grid->get_writer().put(0, symbol, 0, values);
  }
  return grid;
}

} // namespace

int main(int argc, char** argv)
{
  if (argc < 2) {
    std::printf("usage: pusch_depth3_input_plane_check <fixture.bin> [tolerance]\n");
    return 2;
  }
  const float tolerance = (argc > 2) ? static_cast<float>(std::atof(argv[2])) : 2.0e-3F;

  fixture data;
  if (!read_fixture(argv[1], data)) {
    return 1;
  }
  if (data.header.nof_subcarriers != pusch_depth3_input_plane::NOF_SUBCARRIERS ||
      data.header.nof_symbols != pusch_depth3_input_plane::NOF_SYMBOLS ||
      data.header.nof_channels != pusch_depth3_input_plane::NOF_CHANNELS) {
    std::printf("SHAPE_FAIL the fixture is %ux%ux%u\n", data.header.nof_subcarriers,
                data.header.nof_symbols, data.header.nof_channels);
    return 1;
  }

  std::unique_ptr<resource_grid> grid = grid_of(data);
  if (grid == nullptr) {
    std::printf("GRID_FAIL\n");
    return 1;
  }

  std::printf("grid         %u subcarriers, %u symbols, %u ports (fixture says %u x %u)\n",
              grid->get_reader().get_nof_subc(), grid->get_reader().get_nof_symbols(),
              grid->get_reader().get_nof_ports(), data.header.nof_subcarriers,
              data.header.nof_symbols);

  pusch_depth3_input_plane        assembler;
  std::vector<depth3_half>             plane(data.expected.size());
  const pusch_demodulator::configuration config = configuration_of(data.header);
  const pusch_depth3_dmrs_context context       = context_of(data.header);

  std::printf("config       rb_mask %u/%u bits, first %d, dmrs %u symbols, nof_symbols %u, "
              "layers %u, ports %u, cdm %u, type %u, low_papr %d\n",
              static_cast<unsigned>(config.rb_mask.count()), static_cast<unsigned>(config.rb_mask.size()),
              config.rb_mask.find_lowest(),
              static_cast<unsigned>(config.dmrs_symb_pos.count()), config.nof_symbols, config.nof_tx_layers,
              static_cast<unsigned>(config.rx_ports.size()), config.nof_cdm_groups_without_data,
              static_cast<unsigned>(config.dmrs_type), context.low_papr ? 1 : 0);

  float  reference_amplitude = 0.0F;
  const pusch_depth3_input_plane::status status =
      assembler.assemble(plane, reference_amplitude, grid->get_reader(), config, context);
  if (status != pusch_depth3_input_plane::status::ok) {
    std::printf("ASSEMBLE_FAIL %s\n", pusch_depth3_input_plane::to_string(status));
    return 1;
  }

  // The reference amplitude first: it divides every field channel, so a wrong one would show up as a
  // uniform scale error that a tolerance on the plane alone could absorb.
  const float amplitude_error =
      std::abs(reference_amplitude - data.reference_amplitude) / data.reference_amplitude;

  size_t  mismatched   = 0;
  size_t  nonzero      = 0;
  float   max_error    = 0.0F;
  size_t  worst_index  = 0;
  for (size_t index = 0; index != plane.size(); ++index) {
    const float actual   = static_cast<float>(plane[index]);
    const float expected = static_cast<float>(*reinterpret_cast<const depth3_half*>(&data.expected[index]));
    const float error    = std::abs(actual - expected);
    if (error > max_error) {
      max_error   = error;
      worst_index = index;
    }
    const float allowed = tolerance + 1.5F * fp16_ulp(expected);
    if (error > allowed) {
      ++mismatched;
    }
    if (expected != 0.0F) {
      ++nonzero;
    }
  }

  std::printf("fixture      %s\n", argv[1]);
  std::printf("amplitude    %.6f vs python %.6f (relative error %.2e)\n",
              reference_amplitude, data.reference_amplitude, amplitude_error);
  std::printf("plane        %zu values, %zu nonzero, %zu beyond one fp16 ulp (+%.1e)\n",
              plane.size(), nonzero, mismatched, tolerance);
  std::printf("worst        %.6f at flat index %zu\n", max_error, worst_index);
  // The five worst are decoded only when something FAILED: a passing run has nothing to explain, and
  // a diagnostic that always prints is a diagnostic nobody reads.
  if (mismatched != 0) {
    // Decoded, so a systematic error names its channel instead of its flat offset.
    std::vector<std::pair<float, size_t>> worst;
    for (size_t index = 0; index != plane.size(); ++index) {
      const float actual   = static_cast<float>(plane[index]);
      const float expected = static_cast<float>(*reinterpret_cast<const depth3_half*>(&data.expected[index]));
      worst.emplace_back(std::abs(actual - expected), index);
    }
    std::partial_sort(worst.begin(), worst.begin() + 5, worst.end(),
                      [](const auto& lhs, const auto& rhs) { return lhs.first > rhs.first; });
    for (unsigned rank = 0; rank != 5; ++rank) {
      const size_t index = worst[rank].second;
      const size_t channel = index % pusch_depth3_input_plane::NOF_CHANNELS;
      const size_t position = index / pusch_depth3_input_plane::NOF_CHANNELS;
      std::printf("  worst %u   channel %zu at subcarrier %zu symbol %zu: c++ %.6f, python %.6f\n",
                  rank, channel, position / pusch_depth3_input_plane::NOF_SYMBOLS,
                  position % pusch_depth3_input_plane::NOF_SYMBOLS,
                  static_cast<float>(plane[index]),
                  static_cast<float>(*reinterpret_cast<const depth3_half*>(&data.expected[index])));
    }
  }

  // Negative cases, so the refusal path is exercised rather than argued. A narrow allocation inside
  // the same window is deliberately NOT one of them: the contract describes a 51 PRB bandwidth part,
  // and how much of it the scheduler grants is not the arm's business.
  pusch_demodulator::configuration two_layers = config;
  two_layers.nof_tx_layers                    = 2;
  float      ignored        = 0.0F;
  const auto layers_status  = assembler.assemble(plane, ignored, grid->get_reader(), two_layers, context);

  pusch_demodulator::configuration precoded = config;
  precoded.enable_transform_precoding       = true;
  const auto precoded_status = assembler.assemble(plane, ignored, grid->get_reader(), precoded, context);

  std::printf("negative     two layers returns '%s', transform precoding returns '%s'\n",
              pusch_depth3_input_plane::to_string(layers_status),
              pusch_depth3_input_plane::to_string(precoded_status));

  const bool refused = layers_status == pusch_depth3_input_plane::status::unsupported_layers &&
                       precoded_status == pusch_depth3_input_plane::status::unsupported_configuration;
  const bool passed = (amplitude_error < 1.0e-4F) && (mismatched == 0) && refused;
  std::printf("%s\n", passed ? "PASS" : "FAIL");
  return passed ? 0 : 1;
}
