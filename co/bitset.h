// Copyright 2023-2026 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef __BITSET_H
#define __BITSET_H

#include <strings.h>

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <vector>

namespace co {

inline constexpr size_t BitsToWords(size_t bits) {
  return bits == 0 ? 0 : (bits - 1) / 64 + 1;
}

class BitSet {
public:
  BitSet() = default;
  BitSet(int num_bits) { Resize(num_bits); }

  void Resize(int num_bits) {
    if (num_bits <= 0) {
      bits_.clear();
      return;
    }
    bits_.resize(BitsToWords(static_cast<size_t>(num_bits)));
  }

  // Allocate the first free bit.
  std::uint32_t Allocate();

  // Free a bit.
  void Free(std::uint32_t bit);

  // Set a bit.
  void Set(std::uint32_t bit);

  // Is the bitset empty (all bits clear)?
  bool IsEmpty() const;

  // Is the given bit set?
  bool Contains(std::uint32_t bit) const;

  void Clear() {
    for (auto &b : bits_) {
      b = 0;
    }
  }

  int SizeInBits() const {
    constexpr size_t kBitsPerWord = 64;
    if (bits_.size() >
        static_cast<size_t>(std::numeric_limits<int>::max()) / kBitsPerWord) {
      return std::numeric_limits<int>::max();
    }
    return static_cast<int>(bits_.size() * kBitsPerWord);
  }

private:
  // Note the use of explicit long long type here because
  // we use ffsll to look for the set bits and that is
  // explicit in its use of long long.
  std::vector<long long> bits_;
};

inline std::uint32_t BitSet::Allocate() {
  size_t start = 0;
  for (;;) {
    for (size_t i = start; i < bits_.size(); i++) {
      const int bit_index = ffsll(~bits_[i]);
      if (bit_index == 0) {
        continue;
      }
      const size_t bit = static_cast<size_t>(bit_index);
      const size_t allocated = i * 64 + (bit - 1);
      if (allocated > std::numeric_limits<std::uint32_t>::max()) {
        throw std::overflow_error("BitSet index exceeds uint32_t");
      }
      bits_[i] |= (1ULL << (bit - 1));
      return static_cast<std::uint32_t>(allocated);
    }
    // Expand bit set and allocate again.  There's no point in
    // searching the whole bitset again because we know it won't
    // have any zero bits in it, so start at the newly added
    // word of zeroes.
    constexpr size_t kMaxWords =
        static_cast<size_t>(std::numeric_limits<std::uint32_t>::max()) / 64 + 1;
    if (bits_.size() >= kMaxWords) {
      throw std::overflow_error("BitSet exhausted uint32_t index space");
    }
    start = bits_.size();
    bits_.push_back(0);
  }
}

inline void BitSet::Free(std::uint32_t bit) {
  size_t word = bit / 64;
  if (word >= bits_.size()) {
    return;
  }
  std::uint32_t b = bit % 64;
  bits_[word] &= ~(1LL << b);
}

inline void BitSet::Set(std::uint32_t bit) {
  size_t word = bit / 64;
  if (word >= bits_.size()) {
    return;
  }
  std::uint32_t b = bit % 64;
  bits_[word] |= (1LL << b);
}

inline bool BitSet::IsEmpty() const {
  for (size_t i = 0; i < bits_.size(); i++) {
    if (bits_[i] != 0) {
      return false;
    }
  }
  return true;
}

inline bool BitSet::Contains(std::uint32_t bit) const {
  size_t word = bit / 64;
  if (word >= bits_.size()) {
    return false;
  }
  size_t b = bit % 64;
  return (bits_[word] & (1LL << b)) != 0;
}
} // namespace co
#endif // __BITSET_H
