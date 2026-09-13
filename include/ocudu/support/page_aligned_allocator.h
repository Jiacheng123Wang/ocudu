// SPDX-FileCopyrightText: Copyright (C) 2026 Jiacheng Wang
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

/// \file
/// \brief Page-aligned allocator for buffers that hardware accelerators read or write
/// directly (Metal no-copy buffers require a page-aligned base address, and the accelerator
/// wraps a page-rounded length, so the allocation must cover whole pages).
///
/// Used by the upper PHY's zero-copy buffers (the PUSCH demodulator's equalized symbols, noise
/// variances and soft bits) and by the resource grid storage, which a GPU writer fills in place.

#pragma once

#include "ocudu/support/macos_compat.h"
#include <cstddef>
#include <new>

namespace ocudu {

/// \brief Allocator that returns page-aligned blocks whose size is rounded up to a whole
/// number of pages.
///
/// The rounded-up tail is part of the allocation, so a consumer that maps the block with a
/// page-rounded length (as the Metal engines do) never maps memory outside of it.
template <typename T>
class page_aligned_allocator
{
public:
  using value_type = T;

  page_aligned_allocator() = default;
  template <typename U>
  page_aligned_allocator(const page_aligned_allocator<U>&)
  {
  }

  [[nodiscard]] T* allocate(std::size_t n)
  {
    const std::size_t page    = compat::page_size();
    const std::size_t bytes   = n * sizeof(T);
    const std::size_t rounded = ((bytes + page - 1) / page) * page;
    void*             ptr     = compat::aligned_alloc(page, rounded);
    if (ptr == nullptr) {
      throw std::bad_alloc();
    }
    return static_cast<T*>(ptr);
  }

  void deallocate(T* ptr, std::size_t /*n*/) noexcept { compat::aligned_free(ptr); }

  template <typename U>
  bool operator==(const page_aligned_allocator<U>&) const noexcept
  {
    return true;
  }
  template <typename U>
  bool operator!=(const page_aligned_allocator<U>&) const noexcept
  {
    return false;
  }
};

} // namespace ocudu
