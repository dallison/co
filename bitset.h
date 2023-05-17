// Copyright 2023 David Allison
// All Rights Reserved
// See LICENSE file for licensing information.

#ifndef __BITSET_H
#define __BITSET_H

#include <strings.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

namespace co {

class BitSet {
 public:
  // Allocate the first free bit.
  int Allocate();

  // Free a bit.
  void Free(size_t bit);

  // Set a bit.
  void Set(size_t bit);

  // Is the bitset empty (all bits clear)?
  bool IsEmpty() const;

  // Is the given bit set?
  bool Contains(size_t bit) const;

 private:
  // Note the use of explicit long long type here because
  // we use ffsll to look for the set bits and that is
  // explicit in its use of long long.
  std::vector<long long> bits_;
};

inline int BitSet::Allocate() {
  size_t start = 0;
  for (;;) {
    for (size_t i = start; i < bits_.size(); i++) {
      int bit = ffsll(~bits_[i]);
      if (bit != 0) {
        bits_[i] |= (1 << (bit - 1));
        return i * 64 + (bit - 1);
      }
    }
    // Expand bit set and allocate again.  There's no point in
    // searching the whole bitset again because we know it won't
    // have any zero bits in it, so start at the newly added
    // word of zeroes.
    start = bits_.size();
    bits_.push_back(0);
  }
}

inline void BitSet::Free(size_t bit) {
  size_t word = bit / 64;
  if (word < 0 || word >= bits_.size()) {
    return;
  }
  int b = bit % 64;
  bits_[word] &= ~(1 << b);
}

inline void BitSet::Set(size_t bit) {
  size_t word = bit / 64;
  if (word < 0 || word >= bits_.size()) {
    return;
  }
  int b = bit % 64;
  bits_[word] |= (1 << b);
}

inline bool BitSet::IsEmpty() const {
  for (size_t i = 0; i < bits_.size(); i++) {
    if (bits_[i] != 0) {
      return false;
    }
  }
  return true;
}

inline bool BitSet::Contains(size_t bit) const {
  size_t word = bit / 64;
  if (word < 0 || word >= bits_.size()) {
    return false;
  }
  int b = bit % 64;
  return (bits_[word] & (1 << b)) != 0;
}
}  // namespace co
#endif  // __BITSET_H
