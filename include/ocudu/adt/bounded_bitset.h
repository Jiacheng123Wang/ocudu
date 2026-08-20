// SPDX-FileCopyrightText: Copyright (C) 2021-2026 Software Radio Systems Limited
// SPDX-License-Identifier: BSD-3-Clause-Open-MPI

#pragma once

#include "ocudu/adt/ranges/iota.h"
#include "ocudu/adt/ranges/transform.h"
#include "ocudu/adt/span.h"
#include "ocudu/adt/static_vector.h"
#include "ocudu/support/math/bit_ops.h"
#include "ocudu/support/math/math_utils.h"
#include "ocudu/support/ocudu_assert.h"
#include <algorithm>
#include <cinttypes>
#include <functional>

namespace ocudu {

namespace detail {

struct default_bounded_bitset_tag {};

} // namespace detail

namespace bounded_bitset_detail {

/// Number of bits held by a single word of a bounded_bitset, regardless of its template parameters.
inline constexpr size_t bits_per_word = 8U * sizeof(uint64_t);

/// Index of the word that holds bit position \c bitidx.
constexpr size_t get_word_idx(size_t bitidx) noexcept
{
  return bitidx / bits_per_word;
}

/// Transform predicate that computes, for a word index, the mask of bits (within that word) that fall inside
/// [start, stop).
///
/// \tparam LowestInfoBitIsMSB Bit index order in memory. See \c bounded_bitset for more details.
template <bool LowestInfoBitIsMSB>
struct word_mask_functor {
  /// Value yielded by the range: the index of a word intersecting [start, stop) and its selected-bit mask.
  struct value_type {
    size_t   word_idx;
    uint64_t mask;
  };

  size_t start_word;
  size_t end_word;
  size_t startmod;
  size_t stopmod;
  size_t tail_unused;

  value_type operator()(size_t idx) const noexcept
  {
    uint64_t mask = ~static_cast<uint64_t>(0);
    if (idx == start_word and startmod != 0) {
      if constexpr (LowestInfoBitIsMSB) {
        mask &= mask_msb_zeros<uint64_t>(startmod);
      } else {
        mask &= mask_lsb_zeros<uint64_t>(startmod);
      }
    }
    if (idx == end_word - 1 and stopmod != 0) {
      if constexpr (LowestInfoBitIsMSB) {
        mask &= mask_lsb_zeros<uint64_t>(tail_unused);
      } else {
        mask &= mask_msb_zeros<uint64_t>(tail_unused);
      }
    }
    return {idx, mask};
  }
};

/// \brief Base class of \c bounded_bitset holding the current bitset size and the operations that depend solely on
/// it and/or on the bit index order (\c LowestInfoBitIsMSB), but not on the bitset capacity.
///
/// \tparam LowestInfoBitIsMSB Bit index order in memory. See \c bounded_bitset for more details.
template <bool LowestInfoBitIsMSB>
class base_bounded_bitset
{
public:
  constexpr base_bounded_bitset() = default;
  constexpr explicit base_bounded_bitset(size_t cur_size_) noexcept : cur_size(cur_size_) {}

  static constexpr bool bit_order() noexcept { return LowestInfoBitIsMSB; }

  /// Current size of the bounded_bitset in bits.
  OCUDU_FORCE_INLINE constexpr size_t size() const noexcept { return cur_size; }

  /// Returns true if the bounded_bitset size is 0.
  OCUDU_FORCE_INLINE constexpr bool empty() const noexcept { return size() == 0; }

protected:
  /// Number of words currently in use.
  OCUDU_FORCE_INLINE constexpr size_t nof_words_() const noexcept { return divide_ceil(size(), bits_per_word); }

  constexpr void assert_within_bounds_(size_t pos, bool strict) const noexcept
  {
    ocudu_assert(pos < size() or (not strict and pos == size()),
                 "ERROR: index='{}' is out-of-bounds for bitset of size='{}'",
                 pos,
                 size());
  }

  constexpr void assert_range_bounds_(size_t startpos, size_t endpos) const noexcept
  {
    ocudu_assert(startpos <= endpos and endpos <= size(),
                 "ERROR: range ['{}', '{}') out-of-bounds for bitsize of size='{}'",
                 startpos,
                 endpos,
                 size());
  }

  OCUDU_FORCE_INLINE size_t convert_bitpos_(size_t bitpos) const noexcept
  {
    if constexpr (LowestInfoBitIsMSB) {
      return bits_per_word - 1 - (bitpos % bits_per_word);
    } else {
      return bitpos;
    }
  }

  OCUDU_FORCE_INLINE static constexpr uint64_t maskbit(size_t pos) noexcept
  {
    if constexpr (LowestInfoBitIsMSB) {
      return static_cast<uint64_t>(1U) << (bits_per_word - 1 - (pos % bits_per_word));
    } else {
      return static_cast<uint64_t>(1U) << (pos % bits_per_word);
    }
  }

  /// \brief Returns a range that enumerates, for a bit interval [start, stop), the indices of the words that
  /// intersect the interval, together with the mask of bits (within each word) that fall inside it.
  ///
  /// \param start first bit index of the bounded_bitset.
  /// \param stop end bit index of the bounded_bitset.
  auto make_word_mask_range(size_t start, size_t stop) const noexcept
  {
    ocudu_assert(start <= stop and stop <= size(),
                 "ERROR: range ['{}', '{}') out-of-bounds for bitsize of size='{}'",
                 start,
                 stop,
                 size());
    const size_t start_word  = get_word_idx(start);
    const size_t end_word    = start == stop ? start_word : get_word_idx(stop - 1) + 1;
    const size_t startmod    = start % bits_per_word;
    const size_t stopmod     = stop % bits_per_word;
    const size_t tail_unused = stopmod == 0 ? 0 : bits_per_word - stopmod;

    return views::transform(
        views::iota(start_word, end_word),
        word_mask_functor<LowestInfoBitIsMSB>{start_word, end_word, startmod, stopmod, tail_unused});
  }

  size_t cur_size = 0;
};

} // namespace bounded_bitset_detail

/// \brief Represents a dynamically-sized bitset with an upper bound capacity of N bits.
///
/// The bounded_bitset is represented internally via an array of uint64_t, with each integer storing a bitmap.
/// This class also offers many standard logic manipulation methods, like ::any(), operators &=, &, |=, |, etc. and
/// utility methods to convert the bitset into strings or integers.
///
/// Depending on the passed \c LowestInfoBitIsMSB template argument, this class can represent the bits of the bitset in
/// different orders. E.g.
///
/// bounded_bitset<6, false> a(5); // Bitset of 5 bits. LSB is Lowest Information Bit (bit 0).
/// a.set(1);
/// assert(a.to_uint64() == 0b00010);
/// bounded_bitset<6, true> b(5); // Bitset of 5 bits. MSB is Lowest Information Bit (bit 0).
/// b.set(1);
/// assert(a.to_uint64() == 0b0100000000000000);
///
/// The \c LowestInfoBitIsMSB template argument also affects the default string representation of the bitset. E.g.
/// fmt::print("{:b}", a); // prints "00010".
/// fmt::print("{:x}", a); // prints "2".
/// fmt::print("{:b}", b); // prints "01000".
/// fmt::print("{:x}", b); // prints "8".
///
/// However, it does not affect the information bit position string representation, e.g.
/// fmt::print("{:n}", a); // prints "0".
/// fmt::print("{:n}", b); // prints "1".
///
/// \tparam N Upper bound for bitset size in number of bits.
/// \tparam LowestInfoBitIsMSB Bit index order in memory. If set to (false / true), the bit index 0 (Lowest Information
/// Bit) corresponds to either the LSB or MSB of the bitset. Note that this argument has an effect on the underlying
/// bitset memory layout.
template <size_t N, bool LowestInfoBitIsMSB = false, typename Tag = detail::default_bounded_bitset_tag>
class bounded_bitset : public bounded_bitset_detail::base_bounded_bitset<LowestInfoBitIsMSB>
{
  using word_t = uint64_t;
  using base_t = bounded_bitset_detail::base_bounded_bitset<LowestInfoBitIsMSB>;

public:
  using base_t::bit_order;
  using base_t::empty;
  using base_t::size;

  constexpr bounded_bitset() = default;

  constexpr explicit bounded_bitset(size_t cur_size_) noexcept : base_t(cur_size_)
  {
    report_fatal_error_if_not(cur_size_ <= max_size(),
                              "The bounded_bitset current size cannot exceed its maximum size");
  }

  /// \brief Constructs a bitset using iterators.
  ///
  /// The constructed bitset size is equal to <tt> end - begin </tt> size. The values in the list are mapped one to one
  /// starting from begin.
  ///
  /// \tparam Iterator Boolean iterator type.
  /// \param[in] begin Begin iterator.
  /// \param[in] end End iterator.
  template <typename Iterator,
            std::enable_if_t<std::is_convertible_v<typename std::iterator_traits<Iterator>::value_type, bool>, int> = 0>
  constexpr bounded_bitset(Iterator begin, Iterator end)
  {
    resize(end - begin);
    auto it = begin;
    for (size_t count = 0; count != cur_size; ++count) {
      set_(count, *it);
      ++it;
    }
  }

  /// \brief Constructs a bitset from an initializer list.
  ///
  /// The constructed bitset size is equal to \c values size. The values in the list are mapped one to one.
  ///
  /// \param[in] values Boolean initializer list.
  constexpr bounded_bitset(const std::initializer_list<const bool>& values) noexcept
  {
    resize(values.size());
    auto it = values.begin();
    for (size_t count = 0; count != cur_size; ++count) {
      set_(count, *it);
      ++it;
    }
  }

  template <typename BoundedBitSet>
  BoundedBitSet convert_to() const noexcept
  {
    static_assert(BoundedBitSet::max_size() == max_size() and BoundedBitSet::bit_order() == bit_order(),
                  "Conversion only supported for same N and LowestInfoBitIsMSB");
    // Just the tag changes.
    BoundedBitSet ret(size());
    ret.buffer = buffer;
    return ret;
  }

  /// Capacity of the bounded_bitset in bits.
  static constexpr size_t max_size() noexcept { return N; }

  /// Number of bits held by a single word of the bounded_bitset.
  static constexpr size_t bits_per_word = bounded_bitset_detail::bits_per_word;

  /// \brief Resize of the bounded_bitset. If <tt> new_size > max_size() </tt>, an assertion is triggered. The newly
  /// created are set to zero.
  constexpr void resize(size_t new_size) noexcept
  {
    if (new_size == cur_size) {
      return;
    }
    report_fatal_error_if_not(
        new_size <= max_size(), "ERROR: new size='{}' exceeds bitset capacity='{}'", new_size, max_size());
    const size_t prev_size = size();
    cur_size               = new_size;
    if (new_size < prev_size) {
      // Shrinking case. Need to sanitize removed bits.
      sanitize_();
      // Note: the clamping to the buffer capacity is redundant (prev_size <= max_size() always holds), but it lets the
      // compiler statically bound the loop and avoid a spurious -Warray-bounds.
      const size_t prev_nof_words = std::min(divide_ceil(prev_size, bits_per_word), max_nof_words_());
      const size_t new_nof_words  = divide_ceil(new_size, bits_per_word);
      for (size_t i = new_nof_words; i < prev_nof_words; ++i) {
        buffer[i] = static_cast<word_t>(0);
      }
    }
  }

  /// \brief Set bit with provided index to either true or false. Assertion is triggered if pos >= max_size().
  /// \param[in] pos Position in bitset.
  /// \param[in] val Value to set the bit.
  void set(size_t pos, bool val) noexcept
  {
    assert_within_bounds_(pos, true);
    set_(pos, val);
  }

  /// \brief Set bit with provided index to true. Assertion is triggered if pos >= N.
  /// \param[in] pos Position in bitset.
  void set(size_t pos) noexcept
  {
    assert_within_bounds_(pos, true);
    set_(pos);
  }

  /// \brief Set bit with provided index to false. Assertion is triggered if pos >= N.
  /// \param[in] pos Position in bitset.
  void reset(size_t pos) noexcept
  {
    assert_within_bounds_(pos, true);
    reset_(pos);
  }

  /// Reset all bits in the bounded_bitset to false. The size of the bitset is maintained.
  void reset() noexcept
  {
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      buffer[i] = static_cast<word_t>(0);
    }
  }

  /// Set all bits in the bounded_bitset to true/false. The size of the bitset is maintained.
  void fill(bool val = true) noexcept
  {
    if (not val) {
      reset();
      return;
    }
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      buffer[i] = static_cast<word_t>(-1);
    }
    sanitize_();
  }

  /// \brief Appends a bit with value \c val to the set.
  ///
  /// Assertion is triggered if the resultant size exceeds the maximum size of the bitset.
  void push_back(bool val) noexcept
  {
    size_t bitpos = size();
    resize(bitpos + 1);
    set(bitpos, val);
  }

  /// \brief Appends \c nof_bits bits to the set.
  ///
  /// The least \c nof_bits significant bits of \c val are appended to the set, starting from the most significant bit
  /// and finishing with the least significant bit.
  ///
  /// Assertion is triggered if the resultant size exceeds the maximum size of the bitset.
  template <typename Integer>
  void push_back(Integer val, unsigned nof_bits) noexcept
  {
    static_assert(std::is_unsigned_v<Integer>, "push_back only works for unsigned integers");
    unsigned bitpos = size();
    resize(bitpos + nof_bits);
    for (unsigned bit_index = 0; bit_index != nof_bits; ++bit_index) {
      set(bitpos + bit_index, (val >> (nof_bits - 1 - bit_index)) & 1U);
    }
  }

  /// \brief Extracts \c nof_bits starting from \c startpos.
  ///
  /// \return An unsigned integer containing \c nof_bits of the set where starting with the most significant bit and
  /// finishing with the most significant bit.
  /// \remark An assertion is triggered if the bit range exceed the set size.
  template <typename Integer = unsigned>
  Integer extract(unsigned startpos, unsigned nof_bits) const noexcept
  {
    static_assert(std::is_unsigned_v<Integer>, "Extract only works for unsigned integers");
    ocudu_assert(nof_bits <= sizeof(Integer) * 8,
                 "The number of bits (i.e., {}) exceeds the destination bit-width (i.e., {}).",
                 nof_bits,
                 sizeof(Integer) * 8);
    ocudu_assert(startpos + nof_bits <= size(),
                 "The start position (i.e., {}) plus the number of bits (i.e., {}) exceed the current size (i.e., {})",
                 startpos,
                 nof_bits,
                 size());

    Integer val = 0;

    for (unsigned bit_index = 0; bit_index != nof_bits; ++bit_index) {
      if (test(startpos + bit_index)) {
        val |= Integer{1} << (nof_bits - 1 - bit_index);
      }
    }

    return val;
  }

  /// \brief Kronecker product of the bitset with another bitset.
  ///
  /// Expands the bitset by a factor of \c other.size() replacing every \c true bit with the contents of \c other and
  /// every \c false bit with \c other.size() \c false bits.
  ///
  /// \tparam Factor    Maximum expansion factor.
  /// \param[in] other  Bitset used for expansion.
  /// \return The result of the bitset product.
  /// \remark The current implementation supports only a bitset containing one word. An assertion is triggered if \c
  /// other contains more than one word.
  template <unsigned long Factor>
  bounded_bitset<Factor * N> kronecker_product(const bounded_bitset<Factor>& other) const noexcept
  {
    static_assert(Factor <= bits_per_word,
                  "The current algorithm does not support a filter containing more than one word.");

    // Prepare an empty result.
    bounded_bitset<Factor * N> result(size() * other.size());

    // Places the contents of other centered at the positions indicated by the true bits.
    auto kronecker_expansion = [&other, &result](unsigned bit_index) {
      unsigned bitpos = bit_index * Factor;
      word_t   word   = other.buffer[0];

      unsigned bit_offset = bitpos % bits_per_word;
      unsigned word_index = bitpos / bits_per_word;

      result.buffer[word_index] |= (word << bit_offset);
      if (bit_offset && (bit_offset + other.size() > bits_per_word)) {
        result.buffer[word_index + 1] |= (word >> (bits_per_word - bit_offset));
      }
    };

    if (is_contiguous(true)) {
      int i_begin = find_lowest(true);
      int i_end   = find_highest(true) + 1;

      if ((i_begin < 0) || (i_end <= 0)) {
        // Empty bitset.
        return result;
      }

      // If the bitset in contiguous and the other bitset is all set, then use fill.
      if (other.all()) {
        result.fill(i_begin * other.size(), i_end * other.size());
      } else {
        // Otherwise, place the contents of other into contiguous bit positions.
        for (int i_bit = i_begin; i_bit != i_end; ++i_bit) {
          kronecker_expansion(i_bit);
        }
      }

      ocudu_assert(count() * other.count() == result.count(),
                   "The resultant number of ones is not consistent with inputs. It expected {} but got {}.",
                   count() * other.count(),
                   result.count());
      return result;
    }

    // Place the contents of other into arbitrary bit positions.
    for_each(0, size(), kronecker_expansion);

    ocudu_assert(count() * other.count() == result.count(),
                 "The resultant number of ones is not consistent with inputs. It expected {} but got {}.",
                 count() * other.count(),
                 result.count());

    return result;
  }

  /// \brief Check if bit with provided index is set to true.
  /// \param[in] pos Position in bitset.
  /// \return Returns true if bit at position pos is set.
  [[nodiscard]] constexpr bool test(size_t pos) const noexcept
  {
    assert_within_bounds_(pos, true);
    return test_(pos);
  }

  /// Gets a pointer to the underlying array of raw words (i.e. \c bounded_bitset_detail::bits_per_word bits each).
  [[nodiscard]] constexpr const uint64_t* data() const noexcept { return buffer.data(); }

  /// \brief Toggle the value at position pos. Assertion is triggered if pos >= N.
  /// \param[in] pos Position in bitset.
  void flip(size_t pos) noexcept
  {
    assert_within_bounds_(pos, true);
    if (test(pos)) {
      reset_(pos);
    } else {
      set_(pos);
    }
  }

  /// \brief Toggle values of bits in bitset.
  /// \return Returns this object.
  bounded_bitset& flip() noexcept
  {
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      buffer[i] = ~buffer[i];
    }
    sanitize_();
    return *this;
  }

  /// \brief Fills range of bits to either true or false.
  /// \param[in] startpos Starting bit index that will be set.
  /// \param[in] endpos End bit index (excluding) where the bits stop being set.
  /// \param[in] value Set bit range values to either true or false.
  /// \return Returns a reference to this object.
  bounded_bitset& fill(size_t startpos, size_t endpos, bool value = true) noexcept
  {
    for (const auto& [word_idx, mask] : make_word_mask_range(startpos, endpos)) {
      if (value) {
        buffer[word_idx] |= mask;
      } else {
        buffer[word_idx] &= ~mask;
      }
    }
    return *this;
  }

  /// \brief Returns bounded_bitset<> that represents a slice or subview of the original bounded_bitset. Unless
  /// it is specified, the returned slice has the same template parameters "N" and "LowestInfoBitIsMSB" of "this".
  ///
  /// \param[in] startpos The bit index where the subview starts.
  /// \param[in] endpos The bit index where the subview stops.
  template <size_t N2 = N, typename NewTag = Tag>
  bounded_bitset<N2, LowestInfoBitIsMSB, NewTag> slice(size_t startpos, size_t endpos) const noexcept
  {
    bounded_bitset<N2, LowestInfoBitIsMSB, NewTag> sliced(endpos - startpos);
    const unsigned                                 start_word = startpos / bits_per_word;
    unsigned                                       start_mod  = startpos % bits_per_word;
    const auto                                     nwords     = nof_words_();

    if (start_mod != 0) {
      if constexpr (LowestInfoBitIsMSB) {
        const auto left_mask  = mask_msb_ones<word_t>(bits_per_word - start_mod);
        const auto right_mask = mask_msb_ones<word_t>(start_mod);
        for (unsigned i = 0, sl_nw = sliced.nof_words_(); i != sl_nw; ++i) {
          sliced.buffer[i] = (buffer[i + start_word] << start_mod) & left_mask;
          if (i + start_word + 1 < nwords) {
            ocudu_assume(i < sliced.buffer.size());
            sliced.buffer[i] |= (buffer[i + start_word + 1] & right_mask) >> (bits_per_word - start_mod);
          }
        }
      } else {
        const auto left_mask  = mask_lsb_ones<word_t>(bits_per_word - start_mod);
        const auto right_mask = mask_lsb_ones<word_t>(start_mod);
        for (unsigned i = 0, sl_nw = sliced.nof_words_(); i != sl_nw; ++i) {
          sliced.buffer[i] = (buffer[i + start_word] >> start_mod) & left_mask;
          if (i + start_word + 1 < nwords) {
            sliced.buffer[i] |= (buffer[i + start_word + 1] & right_mask) << (bits_per_word - start_mod);
          }
        }
      }
    } else {
      for (unsigned i = 0, sl_nw = sliced.nof_words_(); i != sl_nw; ++i) {
        sliced.buffer[i] = buffer[i + start_word];
      }
    }
    sliced.sanitize_();
    return sliced;
  }

  /// \brief Finds the lowest bit with value set to the value passed as argument.
  /// \param[in] value The bit value to find, either true (1) or false (0).
  /// \return Returns the lowest found bit index or -1 in case no bit was found with the provided value argument.
  int find_lowest(bool value = true) const noexcept { return find_lowest(0, size(), value); }

  /// \brief Finds, within a range of bit indexes, the lowest bit with value set to the value passed as argument.
  /// \param[in] startpos Starting bit index for the search.
  /// \param[in] endpos End bit index for the search.
  /// \param[in] value The bit value to find, either true (1) or false (0).
  /// \return Returns the lowest found bit index or -1 in case no bit was found with the provided value argument.
  int find_lowest(size_t startpos, size_t endpos, bool value = true) const noexcept
  {
    for (const auto& [word_idx, mask] : make_word_mask_range(startpos, endpos)) {
      word_t w = value ? buffer[word_idx] : ~buffer[word_idx];
      w &= mask;
      if (w != 0) {
        // Found bit. Store its position.
        int pos = static_cast<int>(word_idx * bits_per_word);
        if constexpr (LowestInfoBitIsMSB) {
          pos += convert_bitpos_(find_first_msb_one(w));
        } else {
          pos += find_first_lsb_one(w);
        }
        return pos;
      }
    }
    return -1;
  }

  /// \brief Executes a function for all \c true (or all \c false) bits in the given bitset interval.
  ///
  /// \param[in] startpos Smallest bit index considered for the function execution (included).
  /// \param[in] endpos   Largest bit index considered for the function execution (excluded).
  /// \param[in] function Function to execute - the signature should be compatible with <tt>void ()(unsigned)</tt>.
  /// \param[in] value    Bit value that triggers the function execution.
  template <class T>
  void for_each(size_t startpos, size_t endpos, T&& function, bool value = true) const noexcept
  {
    static_assert(std::is_convertible_v<T, std::function<void(size_t)>>,
                  "The function must have void(size_t) signature.");
    static_assert(!LowestInfoBitIsMSB, "The for_each method is not yet available for reversed bitsets.");

    assert_range_bounds_(startpos, endpos);

    if (startpos == endpos) {
      return;
    }

    if ((value && all(startpos, endpos)) || (!value && none(startpos, endpos))) {
      for (size_t bitpos = startpos; bitpos != endpos; ++bitpos) {
        function(bitpos);
      }
      return;
    }

    size_t startword = startpos / bits_per_word;
    size_t lastword  = (endpos + bits_per_word - 1) / bits_per_word;
    for (size_t i = startword; i != lastword; ++i) {
      word_t w = buffer[i];
      if (not value) {
        w = ~w;
      }

      if (w == 0) {
        continue;
      }

      if (i == startword) {
        w &= mask_lsb_zeros<word_t>(startpos % bits_per_word);
      }

      if ((i == lastword - 1) && (endpos % bits_per_word != 0)) {
        w &= mask_lsb_ones<word_t>(endpos % bits_per_word);
      }

      // Process presets of 4 bits.
      unsigned bitpos = i * bits_per_word;
      for (; w != 0; w = w >> 4, bitpos += 4) {
        switch (w & 0xf) {
          case 0B0000:
            // All bits are false, skip.
            break;
          case 0B0001:
            function(bitpos + 0);
            break;
          case 0B0010:
            function(bitpos + 1);
            break;
          case 0B0011:
            function(bitpos + 0);
            function(bitpos + 1);
            break;
          case 0B0100:
            function(bitpos + 2);
            break;
          case 0B0101:
            function(bitpos + 0);
            function(bitpos + 2);
            break;
          case 0B0110:
            function(bitpos + 1);
            function(bitpos + 2);
            break;
          case 0B0111:
            function(bitpos + 0);
            function(bitpos + 1);
            function(bitpos + 2);
            break;
          case 0B1000:
            function(bitpos + 3);
            break;
          case 0B1001:
            function(bitpos + 0);
            function(bitpos + 3);
            break;
          case 0B1010:
            function(bitpos + 1);
            function(bitpos + 3);
            break;
          case 0B1011:
            function(bitpos + 0);
            function(bitpos + 1);
            function(bitpos + 3);
            break;
          case 0B1100:
            function(bitpos + 2);
            function(bitpos + 3);
            break;
          case 0B1101:
            function(bitpos + 0);
            function(bitpos + 2);
            function(bitpos + 3);
            break;
          case 0B1110:
            function(bitpos + 1);
            function(bitpos + 2);
            function(bitpos + 3);
            break;
          case 0B1111:
          default:
            function(bitpos + 0);
            function(bitpos + 1);
            function(bitpos + 2);
            function(bitpos + 3);
            break;
        }
      }
    }
  }

  /// \brief Checks if all bits in the bitset are set to 1.
  /// \return Returns true if all bits are 1.
  bool all() const noexcept
  {
    const size_t nw = nof_words_();
    if (nw == 0) {
      return true;
    }
    word_t allset = ~static_cast<word_t>(0);
    for (size_t i = 0; i < nw - 1; i++) {
      if (buffer[i] != allset) {
        return false;
      }
    }
    if constexpr (LowestInfoBitIsMSB) {
      return buffer[nw - 1] == (allset << (nw * bits_per_word - size()));
    } else {
      return buffer[nw - 1] == (allset >> (nw * bits_per_word - size()));
    }
  }

  /// \brief Checks if all bits within a bit index range are set to 1.
  /// \return Returns true if all the bits within the range are 1.
  bool all(size_t start, size_t stop) const noexcept
  {
    for (const auto& [word_idx, mask] : make_word_mask_range(start, stop)) {
      if ((buffer[word_idx] | ~mask) != ~static_cast<word_t>(0)) {
        return false;
      }
    }
    return true;
  }

  /// \brief Finds the highest bit with value set to the value passed as argument.
  /// \param[in] value The bit value to find, either true (1) or false (0).
  /// \return Returns the highest found bit index or -1 in case no bit was found with the provided value argument.
  int find_highest(bool value = true) const noexcept { return find_highest(0, size(), value); }

  /// \brief Finds, within a range of bit indexes, the highest bit with value set to the value passed as argument.
  /// \param[in] startpos Starting bit index for the search.
  /// \param[in] endpos End bit index for the search.
  /// \param[in] value The bit value to find, either true (1) or false (0).
  /// \return Returns the highest found bit index or -1 in case no bit was found with the provided value argument.
  int find_highest(size_t startpos, size_t endpos, bool value = true) const noexcept
  {
    assert_range_bounds_(startpos, endpos);
    if (startpos == endpos) {
      return -1;
    }
    size_t startword = startpos / bits_per_word;
    size_t lastword  = (endpos - 1) / bits_per_word;

    for (size_t i = lastword; i != startword - 1; --i) {
      word_t w = buffer[i];
      if (not value) {
        w = ~w;
      }

      if (i == startword) {
        size_t removed_bits = startpos % bits_per_word;
        if constexpr (LowestInfoBitIsMSB) {
          w &= mask_msb_zeros<word_t>(removed_bits);
        } else {
          w &= mask_lsb_zeros<word_t>(removed_bits);
        }
      }
      if (i == lastword) {
        size_t kept_bits = ((endpos - 1) % bits_per_word) + 1;
        if constexpr (LowestInfoBitIsMSB) {
          w &= mask_msb_ones<word_t>(kept_bits);
        } else {
          w &= mask_lsb_ones<word_t>(kept_bits);
        }
      }
      if (w != 0) {
        if constexpr (LowestInfoBitIsMSB) {
          return static_cast<int>(i * bits_per_word + convert_bitpos_(find_first_lsb_one(w)));
        } else {
          return static_cast<int>(i * bits_per_word + find_first_msb_one(w));
        }
      }
    }
    return -1;
  }

  /// \brief Checks if at least one bit in the bitset is set to 1.
  /// \return Returns true if at least one bit is 1.
  bool any() const noexcept
  {
    for (size_t i = 0, sz = nof_words_(); i != sz; ++i) {
      if (buffer[i] != static_cast<word_t>(0)) {
        return true;
      }
    }
    return false;
  }

  /// \brief Checks if at least one bit in the bitset is set to 1 within a bit index range.
  /// \return Returns true if at least one bit equal to 1 was found within the range.
  bool any(size_t start, size_t stop) const noexcept
  {
    for (const auto& [word_idx, mask] : make_word_mask_range(start, stop)) {
      if ((buffer[word_idx] & mask) != static_cast<word_t>(0)) {
        return true;
      }
    }
    return false;
  }

  /// \brief Checks if all bits set to 1 in this bitset, within a bit index range, are also set to 1 in "other".
  /// \param[in] other Bitset to compare against. Must have the same size as this bitset.
  /// \param[in] start Starting bit index of the range (included).
  /// \param[in] stop End bit index of the range (excluded).
  /// \return Returns true if this bitset restricted to [start, stop) is a subset of "other", false otherwise.
  bool is_subset_of(const bounded_bitset& other, size_t start, size_t stop) const noexcept
  {
    ocudu_assert(other.size() == size(),
                 "ERROR: is_subset_of called for bitsets of different sizes ('{}'!='{}')",
                 size(),
                 other.size());
    // A bit set in "this" but not in "other" (within the mask) breaks the subset relation.
    for (const auto& [word_idx, mask] : make_word_mask_range(start, stop)) {
      if ((buffer[word_idx] & ~other.buffer[word_idx] & mask) != static_cast<word_t>(0)) {
        return false;
      }
    }
    return true;
  }

  /// \brief Checks if at no bit in the bitset is set to 1.
  /// \return Returns true if no bit equal to 1 was found.
  bool none() const noexcept { return !any(); }

  /// \brief Checks whether no bit is set within the given index range.
  /// \return True if no bit is equal to 1, false otherwise.
  bool none(size_t start, size_t stop) const noexcept { return !any(start, stop); }

  /// \brief Determines whether all bits with value set to \c value are contiguous.
  ///
  /// Bits with the same value are contiguous if:
  /// 1. one bit with \c value is found,
  /// 2. no bit with \c value is found, and
  /// 3. all bits with \c value are consecutive.
  ///
  /// \param[in] startpos Starting bit index for the search.
  /// \param[in] endpos End bit index for the search.
  /// \param[in] value The bit value to find, either true (1) or false (0).
  /// \return Returns the lowest found bit index or -1 in case no bit was found with the provided value argument.
  bool is_contiguous(bool value = true) const noexcept
  {
    // Find the lowest value.
    int startpos = find_lowest(0, size(), value);

    // Condition 1. No value is found.
    if (startpos == -1) {
      return true;
    }

    // Find the highest value.
    int endpos = find_highest(startpos + 1, size(), value);

    // Condition 2. There is only one bit with the value (in startpos).
    if (endpos == -1) {
      return true;
    }

    // Count the number of elements set to value.
    size_t value_count = count();
    if (not value) {
      value_count = size() - value_count;
    }

    // Condition 3. The number of elements must match with the start to end number of elements.
    return (value_count == static_cast<size_t>((endpos + 1) - startpos));
  }

  /// \brief Counts the number of bits set to 1.
  /// \return Returns the number of bits set to 1.
  size_t count() const noexcept
  {
    int result = 0;
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      result += count_ones(buffer[i]);
    }
    return result;
  }

  /// \brief Compares two bitsets.
  /// \return Returns true if both bitsets are equal in size and values of bits.
  bool operator==(const bounded_bitset& other) const noexcept
  {
    if (size() != other.size()) {
      return false;
    }
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      if (buffer[i] != other.buffer[i]) {
        return false;
      }
    }
    return true;
  }

  bool operator!=(const bounded_bitset& other) const noexcept { return not(*this == other); }

  /// \brief Applies bitwise OR operation lhs |= rhs.
  /// \param[in] other Bitset which corresponds to the rhs of the operation.
  /// \return This object updated after the bitwise OR operation.
  bounded_bitset& operator|=(const bounded_bitset& other) noexcept
  {
    ocudu_assert(other.size() == size(),
                 "ERROR: operator|= called for bitsets of different sizes ('{}'!='{}')",
                 size(),
                 other.size());
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      buffer[i] |= other.buffer[i];
    }
    return *this;
  }

  /// \brief Applies bitwise AND operation lhs &= rhs.
  /// \param[in] other Bitset which corresponds to the rhs of the operation.
  /// \return This object updated after the bitwise AND operation.
  bounded_bitset& operator&=(const bounded_bitset& other) noexcept
  {
    ocudu_assert(other.size() == size(),
                 "ERROR: operator&= called for bitsets of different sizes ('{}'!='{}')",
                 size(),
                 other.size());
    for (size_t i = 0, nw = nof_words_(); i != nw; ++i) {
      buffer[i] &= other.buffer[i];
    }
    return *this;
  }

  /// \brief Flips values of bits in the bitset.
  /// \return Returns reference to this object, updated after the flip operation.
  bounded_bitset operator~() const noexcept
  {
    bounded_bitset ret(*this);
    ret.flip();
    return ret;
  }

  /// \brief Conversion of bounded_bitset to unsigned integer of 64 bits. If bitset size is larger than 64 bits, an
  /// assertion is triggered.
  /// \return Unsigned integer representation of the bounded_bitset.
  uint64_t to_uint64() const noexcept
  {
    ocudu_assert(nof_words_() == 1, "ERROR: cannot convert bitset of size='{}' to uint64_t", size());
    if constexpr (LowestInfoBitIsMSB) {
      const size_t rem = size() % bits_per_word;
      return (rem == 0) ? buffer[0] : (buffer[0] >> (bits_per_word - rem));
    }
    return buffer[0];
  }

  /// \brief Conversion of unsigned integer of 64 bits to bounded_bitset. If passed bitmap doesn't fit in the bitset,
  /// an assertion is triggered.
  /// \param[in] v Integer bitmap that is going to be stored in the bitset.
  void from_uint64(uint64_t v) noexcept
  {
    ocudu_assert(nof_words_() == 1, "ERROR: cannot convert bitset of size='{}' to uint64_t", size());
    ocudu_assert((size() == 64U) || (v < (static_cast<uint64_t>(1U) << size())),
                 "ERROR: Provided mask='{}' does not fit in bitset of size='{}'",
                 v,
                 size());
    if constexpr (LowestInfoBitIsMSB) {
      const size_t rem = size() % bits_per_word;
      buffer[0]        = (rem == 0) ? v : (v << (bits_per_word - rem));
    } else {
      buffer[0] = v;
    }
  }

  /// \brief Converts the bitset to an array of packed bits. Each element of the resulting array will contain a bitmap.
  /// The LowInfoBitIsMSB template parameter defines the order of bits in the resulting array.
  /// \tparam UnsignedInteger Value type of the array where packed bits will be stored. It must be an unsigned integer.
  /// \param[in] packed_bits Array where packed bits will be stored. The array size must be equal or larger than the
  /// bitset size (in bits) divided by \c sizeof(UnsignedInteger) * 8U (the number of bits per integer).
  /// \return Returns the number of positions of \c packed_bits that were written during the function call.
  template <typename UnsignedInteger>
  size_t to_packed_bits(span<UnsignedInteger> packed_bits) const noexcept
  {
    static_assert(sizeof(UnsignedInteger) <= sizeof(word_t), "ERROR: provided array type is too large");
    static_assert(std::is_unsigned_v<UnsignedInteger>, "Only unsigned integers are supported");
    static constexpr size_t steps_per_word   = sizeof(word_t) / sizeof(UnsignedInteger);
    static constexpr size_t bits_per_integer = sizeof(UnsignedInteger) * 8U;
    static constexpr auto   integer_mask     = mask_lsb_ones<word_t>(bits_per_integer);
    const word_t            sz               = size();
    const unsigned          last_word_steps =
        (sz % bits_per_word) ? divide_ceil(sz % bits_per_word, bits_per_integer) : steps_per_word;
    const unsigned nof_words           = nof_words_();
    const unsigned nof_integers_packed = (nof_words - 1) * steps_per_word + last_word_steps;
    ocudu_assert(
        packed_bits.size() >= nof_integers_packed, "ERROR: provided array size='{}' is too small", packed_bits.size());

    size_t count = 0;
    if (not LowestInfoBitIsMSB) {
      for (unsigned i = 0; i != nof_words; ++i) {
        unsigned nof_steps = i == nof_words - 1 ? last_word_steps : steps_per_word;
        for (unsigned j = 0; j != nof_steps; ++j) {
          packed_bits[count++] = (buffer[i] >> (j * steps_per_word)) & integer_mask;
        }
      }
    } else {
      for (unsigned i = 0; i != nof_words; ++i) {
        word_t   w         = buffer[i];
        unsigned nof_steps = steps_per_word;
        if (i == nof_words - 1) {
          nof_steps = last_word_steps;
        }
        for (unsigned j = 0; j != nof_steps; ++j) {
          packed_bits[count++] = (w >> (bits_per_word - (j + 1) * bits_per_integer)) & integer_mask;
        }
      }
    }

    return nof_integers_packed;
  }

  /// \brief Converts the bitset to an array of unpacked bits, i.e. an array where each element represents a single
  /// boolean. The order of bits in the resulting array matches the bit information order in the bitset and the template
  /// parameter \c LowInfoBitIsMSB has no effect. That means that \c unpacked_bits[i] will be equal to \c
  /// bitset.test(i).
  ///
  /// \tparam UnsignedInteger Value type of the array where packed bits will be stored. It must be an
  /// unsigned integer. \param[in] unpacked_bits Array where the unpacked bits will be stored. The array size must be
  /// equal or larger than the bitset size (in bits). \return Returns the number of bits packed.
  template <typename UnsignedInteger>
  void to_unpacked_bits(span<UnsignedInteger> unpacked_bits) const noexcept
  {
    static_assert(std::is_unsigned_v<UnsignedInteger>, "Only unsigned integers are supported");
    ocudu_assert(size() == unpacked_bits.size(),
                 "ERROR: provided array size='{}' does not match bitset size='{}'",
                 unpacked_bits.size(),
                 size());

    for (unsigned i = 0, ie = size(); i != ie; ++i) {
      unpacked_bits[i] = test(i);
    }
  }

  /// \brief Generates a list of bit positions corresponding to the information bits set to one or zero.
  ///
  /// The bit positions correspond to the location of each bit within the information bit word stored in the bitset,
  /// regardless of the bit index order in memory given by \ref LowestInfoBitIsMSB.
  ///
  /// \param[in] value Selects the bits whose positions are returned. Set to \c true to find ones, \c false for zeros.
  /// \return A list containing the bit positions.
  static_vector<size_t, N> get_bit_positions(bool value = true) const noexcept
  {
    static_vector<size_t, N> positions;

    for (size_t i_bit = 0, sz = size(); i_bit != sz;) {
      // Find the next bit position of the bit set to value.
      int next_position = find_lowest(i_bit, sz, value);
      if (next_position < 0) {
        break;
      }

      // If a bit was found, add to the list.
      positions.emplace_back(static_cast<size_t>(next_position));

      // Exclude the evaluated bit range from the next search.
      i_bit = next_position + 1;
    }

    return positions;
  }

private:
  template <size_t N2, bool reversed2, typename Tag2>
  friend class bounded_bitset;

  using base_t::assert_range_bounds_;
  using base_t::assert_within_bounds_;
  using base_t::convert_bitpos_;
  using base_t::cur_size;
  using base_t::make_word_mask_range;
  using base_t::maskbit;
  using base_t::nof_words_;

  // Capacity of the underlying array in number of words.
  static constexpr size_t max_nof_words_() noexcept { return (N + bits_per_word - 1) / bits_per_word; }

  std::array<word_t, max_nof_words_()> buffer{};

  constexpr void sanitize_() noexcept
  {
    const size_t n = size() % bits_per_word;
    if (n != 0) {
      const size_t nwords = nof_words_();
      if constexpr (LowestInfoBitIsMSB) {
        buffer[nwords - 1] &= ~((~static_cast<word_t>(0)) >> n);
      } else {
        buffer[nwords - 1] &= ~((~static_cast<word_t>(0)) << n);
      }
    }
  }

  OCUDU_FORCE_INLINE constexpr bool test_(size_t bitpos) const noexcept
  {
    const size_t word_idx = bitpos / bits_per_word;
    ocudu_assume(word_idx < buffer.size());
    const word_t bitmask = maskbit(bitpos);
    return ((buffer[word_idx] & bitmask) != static_cast<word_t>(0));
  }

  constexpr void set_(size_t bitpos, bool val) noexcept
  {
    if (val) {
      set_(bitpos);
    } else {
      reset_(bitpos);
    }
  }
  constexpr void set_(size_t bitpos) noexcept
  {
    const size_t word_idx = bitpos / bits_per_word;
    ocudu_assume(word_idx < buffer.size());
    buffer[word_idx] |= maskbit(bitpos);
  }

  constexpr void reset_(size_t bitpos) noexcept
  {
    const size_t word_idx = bitpos / bits_per_word;
    ocudu_assume(word_idx < buffer.size());
    buffer[word_idx] &= ~maskbit(bitpos);
  }
};

/// \brief Bitwise AND operation result = lhs & rhs.
/// \return new bounded_bitset that results from the Bitwise AND operation.
template <size_t N, bool LowestInfoBitIsMSB>
inline bounded_bitset<N, LowestInfoBitIsMSB> operator&(const bounded_bitset<N, LowestInfoBitIsMSB>& lhs,
                                                       const bounded_bitset<N, LowestInfoBitIsMSB>& rhs) noexcept
{
  bounded_bitset<N, LowestInfoBitIsMSB> res(lhs);
  res &= rhs;
  return res;
}

/// \brief Bitwise AND operation result = lhs | rhs.
/// \return new bounded_bitset that results from the Bitwise OR operation.
template <size_t N, bool LowestInfoBitIsMSB>
inline bounded_bitset<N, LowestInfoBitIsMSB> operator|(const bounded_bitset<N, LowestInfoBitIsMSB>& lhs,
                                                       const bounded_bitset<N, LowestInfoBitIsMSB>& rhs) noexcept
{
  bounded_bitset<N, LowestInfoBitIsMSB> res(lhs);
  res |= rhs;
  return res;
}

/// \brief Flip bits from left to right.
/// \return new bounded_bitset that results from the fliplr operation.
template <size_t N, bool LowestInfoBitIsMSB>
inline bounded_bitset<N, LowestInfoBitIsMSB> fliplr(const bounded_bitset<N, LowestInfoBitIsMSB>& other) noexcept
{
  bounded_bitset<N, LowestInfoBitIsMSB> ret(other.size());
  for (uint32_t i = 0; i < ret.size(); ++i) {
    if (other.test(i)) {
      ret.set(ret.size() - 1 - i);
    }
  }
  return ret;
}

/// Iterate over bits set to 1 in the bitset, from begin() until \c predicate returns true.
/// \return The position of the first bit for which \c predicate returns true, or -1 if no such bit exists.
template <size_t N, bool LowestInfoBitIsMSB, typename Tag, typename FindPred>
int find_first(const bounded_bitset<N, LowestInfoBitIsMSB, Tag>& bitset, FindPred&& predicate)
{
  auto last = bitset.find_highest(0, bitset.size(), true);
  if (last < 0) {
    return -1;
  }
  size_t end = static_cast<size_t>(last) + 1;

  for (size_t start = 0; start != end; ++start) {
    auto pos = bitset.find_lowest(start, end, true);
    if (pos < 0) {
      break;
    }
    start = static_cast<size_t>(pos);
    if (predicate(start)) {
      return pos;
    }
  }
  return -1;
}

/// \brief Divides a bitset of size "S" into "M" smaller bitsets, where each bitset has length "L=S/M". A bitwise-or
/// operation is performed across bitsets. At the end, a slice with an offset "O" and length "K" is taken from the
/// bitset of length "L" that resulted from the bitwise-or operation.
/// This operation is equivalent to reshaping an array of bits of size "S" into a matrix of dimensions "(M, L)" and
/// applying an "or" operation across all bits of each column. The resulting array of "L" bits, is then sliced with
/// an offset "O" and length "K".
/// The operation asserts if "S % L != 0".
/// E.g. Consider the bitset 1000 0100 0000 1001 (S=16), L=4, O=1, K=2. This function performs the following steps:
/// 1. Break the bitset into M=S/L=4 parts: {1000, 0100, 0000, 1001}.
/// 2. Bitwise-or all the M parts: 1101.
/// 3. Slice the bitset obtained in 2. with offset O=1 and slice length K=2: 10.
///
/// \tparam N2 maximum bitset size for returned bitset.
/// \param[in] other original bitset of length "S".
/// \param[in] fold_length length of each folded bitset "L".
/// \param[in] slice_offset offset from where to slice each fold "O".
/// \param[in] slice_length length of the slice taken from each fold "K".
/// \return bitset of size slice_length with the or-accumulated folds.
template <size_t N2, size_t N, bool LowestInfoBitIsMSB>
inline bounded_bitset<N2, LowestInfoBitIsMSB> fold_and_accumulate(const bounded_bitset<N, LowestInfoBitIsMSB>& other,
                                                                  size_t fold_length,
                                                                  size_t slice_offset,
                                                                  size_t slice_length) noexcept
{
  ocudu_assert(
      other.size() % fold_length == 0, "Invalid fold length={} for bitset of size={}", fold_length, other.size());
  bounded_bitset<N2, LowestInfoBitIsMSB> ret(slice_length);
  for (size_t i = 0; i != other.size(); i += fold_length) {
    ret |= other.template slice<N2>(i + slice_offset, i + slice_offset + slice_length);
  }
  return ret;
}

/// \brief Performs the fold and accumulate operation, but without slicing at the end.
///
/// \tparam N2 maximum bitset size for returned bitset.
/// \tparam LowestInfoBitIsMSB internal bit order representation of returned bitset.
/// \param[in] other original bitset from where folds are generated.
/// \param[in] fold_length length of each fold bitset.
/// \return bitset of size fold_length with the accumulated folds.
template <size_t N2, size_t N, bool LowestInfoBitIsMSB>
inline bounded_bitset<N2, LowestInfoBitIsMSB> fold_and_accumulate(const bounded_bitset<N, LowestInfoBitIsMSB>& other,
                                                                  size_t fold_length) noexcept
{
  return fold_and_accumulate<N2, N, LowestInfoBitIsMSB>(other, fold_length, 0, fold_length);
}

/// \brief Executes a function for all \c true (or all \c false) intervals in the given bitset interval.
///
/// The method calls \c function for each interval, passing the first and last bit positions of the interval.
///
/// \param[in] startpos Smallest bit index considered for the function execution (included).
/// \param[in] endpos   Largest bit index considered for the function execution (excluded).
/// \param[in] function Function to execute - the signature should be compatible with <tt>void ()(size_t,size_t)</tt>.
/// \param[in] value    Bit value that triggers the function execution.
template <size_t N, bool LowestInfoBitIsMSB, typename Tag, class Func>
void for_each_interval(const bounded_bitset<N, LowestInfoBitIsMSB, Tag>& bitset,
                       size_t                                            startpos,
                       size_t                                            endpos,
                       Func&&                                            function,
                       bool                                              value = true)
{
  static_assert(std::is_convertible_v<Func, std::function<void(size_t, size_t)>>,
                "The function must have void(size_t) signature.");

  // Iterate for all intervals.
  for (int start_interval = bitset.find_lowest(startpos, endpos, value); start_interval != (-1);
       start_interval     = bitset.find_lowest(start_interval, endpos, value)) {
    // Find the end of the current interval.
    int end_interval = bitset.find_lowest(start_interval, endpos, !value);

    // If no more ending, force the end position.
    if (end_interval == -1) {
      end_interval = endpos;
    }

    // Call function.
    function(start_interval, end_interval);

    // Advance interval.
    start_interval = end_interval;
  }
}

// Executes a function for all \c true (or all \c false) intervals in the given bitset.
template <size_t N, bool LowestInfoBitIsMSB, typename Tag, class Func>
void for_each_interval(const bounded_bitset<N, LowestInfoBitIsMSB, Tag>& bitset, Func&& function, bool value = true)
{
  for_each_interval(bitset, 0, bitset.size(), function, value);
}

/// Converts a list of bit positions to a bounded_bitset.
template <size_t N,
          bool   LowestInfoBitIsMSB = false,
          typename Tag              = detail::default_bounded_bitset_tag,
          typename RangeType,
          typename PosInteger = typename RangeType::value_type>
bounded_bitset<N, LowestInfoBitIsMSB, Tag> bit_positions_to_bitset(const RangeType& bit_positions)
{
  bounded_bitset<N, LowestInfoBitIsMSB, Tag> result(N);
  int                                        max_pos = -1;
  for (PosInteger pos : bit_positions) {
    result.set(static_cast<size_t>(pos));
    max_pos = std::max(max_pos, static_cast<int>(pos));
  }
  result.resize(max_pos + 1);
  return result;
}

} // namespace ocudu
