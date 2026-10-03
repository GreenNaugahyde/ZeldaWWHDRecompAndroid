#pragma once
#include <array>
#include <cstdint>

namespace gfxvk {
// Metadata only: a matching key is an opportunity, never proof of equal bytes.
struct VertexHistoryProbe {
  struct Key { uint32_t address = 0, size = 0; };
  struct History { std::array<Key, 8> keys{}; uint32_t count = 0; };
  std::array<History, 16> bindings{};
  uintptr_t device = 0;
  uint64_t generation = 0;
  bool initialized = false;
  // Returns an MRU distance 1..7 for a nonconsecutive match; zero otherwise.
  uint32_t observe(uintptr_t nextDevice, uint64_t nextGeneration,
                   uint32_t binding, uint32_t address, uint32_t size, bool bounded) {
    if (!initialized || device != nextDevice || generation != nextGeneration) {
      bindings = {}; device = nextDevice; generation = nextGeneration; initialized = true;
    }
    if (binding >= bindings.size()) return 0;
    auto& history = bindings[binding];
    if (!bounded) { history = {}; return 0; }
    uint32_t found = history.count;
    for (uint32_t i = 0; i < history.count; ++i)
      if (history.keys[i].address == address && history.keys[i].size == size) { found = i; break; }
    const uint32_t distance = found < history.count ? found : 0;
    const uint32_t end = found < history.count ? found :
        (history.count < history.keys.size() ? history.count++ : uint32_t(history.keys.size() - 1));
    for (uint32_t i = end; i > 0; --i) history.keys[i] = history.keys[i - 1];
    history.keys[0] = {address, size};
    return distance;
  }
};
} // namespace gfxvk
