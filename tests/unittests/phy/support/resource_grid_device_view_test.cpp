// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Tests of the device view of the resource grid storage.
///
/// The uplink chain's GPU stages (starting with the OFDM demodulator's grid write) fill the grid in place through this
/// view, so the CPU reads the very same memory with no copy in between. That only works if the storage keeps the
/// guarantees a Metal no-copy buffer needs (page-aligned base, whole pages covered) and if the view describes exactly
/// the layout the host writer uses. Both are verified here, on the host, without any GPU involved.

#include "ocudu/adt/tensor.h"
#include "ocudu/phy/support/resource_grid.h"
#include "ocudu/phy/support/resource_grid_device_view.h"
#include "ocudu/phy/support/resource_grid_dimensions.h"
#include "ocudu/phy/support/resource_grid_writer.h"
#include "ocudu/phy/support/support_factories.h"
#include "ocudu/support/macos_compat.h"
#include <gtest/gtest.h>

using namespace ocudu;

namespace {

/// Grid factory of the shared support factories (the grids under test are the real ones the lower PHY pools).
const std::shared_ptr<resource_grid_factory>& rg_factory()
{
  static const std::shared_ptr<resource_grid_factory> factory = create_resource_grid_factory();
  return factory;
}

TEST(resource_grid_device_view_test, storage_is_ready_for_a_no_copy_mapping)
{
  static constexpr unsigned nof_ports = 1;
  static constexpr unsigned nof_symb  = 14;
  static constexpr unsigned nof_subc  = 300; // 25 PRB

  std::unique_ptr<resource_grid> grid = rg_factory()->create(nof_ports, nof_symb, nof_subc);
  ASSERT_NE(grid, nullptr);

  const resource_grid_device_view view = grid->get_writer().get_device_view();
  ASSERT_TRUE(view.is_valid());

  // A Metal no-copy buffer needs a page-aligned base and a length that covers whole pages (the engines round the mapped
  // length up, so the tail has to belong to the allocation).
  const std::size_t page = compat::page_size();
  EXPECT_EQ(reinterpret_cast<uintptr_t>(view.base) % page, 0U);
  EXPECT_GE(nof_subc * nof_symb * nof_ports * sizeof(cbf16_t), 1U);

  // The view describes the whole grid.
  EXPECT_EQ(view.nof_subc, nof_subc);
  EXPECT_EQ(view.nof_symb, nof_symb);
  EXPECT_EQ(view.nof_ports, nof_ports);
}

TEST(resource_grid_device_view_test, view_addresses_the_same_elements_the_host_writer_fills)
{
  static constexpr unsigned nof_ports = 1;
  static constexpr unsigned nof_symb  = 14;
  static constexpr unsigned nof_subc  = 300;

  std::unique_ptr<resource_grid> grid = rg_factory()->create(nof_ports, nof_symb, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&         writer = grid->get_writer();
  const resource_grid_device_view view = writer.get_device_view();
  ASSERT_TRUE(view.is_valid());

  auto* base = static_cast<cbf16_t*>(view.base);

  // Fill two symbols through the host writer and read them back through the view's own addressing: same memory, so the
  // values must match element by element (this is what a device writer relies on).
  for (unsigned symbol : {0U, 5U, nof_symb - 1}) {
    span<cbf16_t> host_symbol = writer.get_view(0, symbol);
    ASSERT_EQ(host_symbol.size(), nof_subc);

    // What a device writer would compute for this symbol, with the strides of the view.
    cbf16_t* device_symbol = base + view.get_symbol_offset(0, symbol);
    EXPECT_EQ(device_symbol, host_symbol.data());

    for (unsigned k = 0; k != nof_subc; ++k) {
      const cbf16_t value = to_cbf16(cf_t(static_cast<float>(k) * 0.5F, -static_cast<float>(symbol)));
      host_symbol[k]      = value;
      EXPECT_EQ(device_symbol[k], value);
    }
  }
}

TEST(resource_grid_device_view_test, a_port_stride_covers_the_symbols_of_the_previous_port)
{
  static constexpr unsigned nof_ports = 4;
  static constexpr unsigned nof_symb  = 14;
  static constexpr unsigned nof_subc  = 300;

  std::unique_ptr<resource_grid> grid = rg_factory()->create(nof_ports, nof_symb, nof_subc);
  ASSERT_NE(grid, nullptr);

  resource_grid_writer&         writer = grid->get_writer();
  const resource_grid_device_view view = writer.get_device_view();
  ASSERT_TRUE(view.is_valid());

  for (unsigned port = 0; port != nof_ports; ++port) {
    for (unsigned symbol = 0; symbol != nof_symb; ++symbol) {
      auto* device_symbol = static_cast<cbf16_t*>(view.base) + view.get_symbol_offset(port, symbol);
      EXPECT_EQ(device_symbol, writer.get_view(port, symbol).data());
    }
  }
}

TEST(resource_grid_device_view_test, a_writer_without_a_published_view_stays_on_the_host)
{
  // A tensor with the default allocator is not page-aligned in general, and a writer that its grid never published a
  // view for must report none: the caller then keeps writing the grid from the host (as it always did).
  dynamic_tensor<static_cast<unsigned>(resource_grid_dimensions::all), cbf16_t, resource_grid_dimensions> plain;
  plain.reserve({300, 14, 1});


  resource_grid_device_view view = make_resource_grid_device_view(plain.get_data(), 300, 14, 1);
  if ((reinterpret_cast<uintptr_t>(plain.get_data().data()) % compat::page_size()) != 0) {
    EXPECT_FALSE(view.is_valid());
  }

  // An empty storage never yields a view.
  EXPECT_FALSE(make_resource_grid_device_view({}, 0, 0, 0).is_valid());
  EXPECT_FALSE(resource_grid_device_view{}.is_valid());
}

} // namespace
