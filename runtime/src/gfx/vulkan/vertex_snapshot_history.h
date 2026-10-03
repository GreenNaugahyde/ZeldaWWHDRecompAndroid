#pragma once
#include <cstdint>
#include <utility>

namespace gfxvk {
template<class Slice>
struct VertexSnapshotHistory {
  struct Entry { uint32_t address = 0, size = 0; Slice slice{}; };
  Entry last{}, previous{};
  static bool matches(const Entry& entry, uint32_t address, uint32_t size) {
    return entry.slice.buffer && entry.address == address && entry.size == size;
  }
  Entry* secondary(uint32_t address, uint32_t size) {
    return matches(previous, address, size) ? &previous : nullptr;
  }
  void promote() { std::swap(last, previous); }
  void remember(uint32_t address, uint32_t size, Slice slice, bool keepHistory) {
    if (!keepHistory) { last = {address, size, slice}; previous = {}; return; }
    // Replace the same key's changed payload rather than retaining versions.
    if (!matches(last, address, size)) previous = last;
    last = {address, size, slice};
  }
};
} // namespace gfxvk
