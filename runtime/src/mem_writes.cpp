#include "mem_writes.h"

#include <atomic>

namespace memw {
namespace {
constexpr uint32_t kPageBits = 12;
std::atomic<uint32_t> g_pages[1u << (32 - kPageBits)];  // 4 MiB
std::atomic<uint32_t> g_counter{1}, g_all{0};
}  // namespace

void mark(uint32_t addr, uint32_t size) {
    if (!size) return;
    if (size >= (16u << 20)) {  // a flush of a whole heap: cheaper as one global mark
        mark_all();
        return;
    }
    uint32_t c = g_counter.fetch_add(1, std::memory_order_relaxed) + 1;
    uint32_t last = (uint32_t)(((uint64_t)addr + size - 1) >> kPageBits);
    for (uint32_t p = addr >> kPageBits; p <= last; p++) g_pages[p].store(c, std::memory_order_release);
}

void mark_all() { g_all.store(g_counter.fetch_add(1, std::memory_order_relaxed) + 1, std::memory_order_release); }

uint32_t now() { return g_counter.load(std::memory_order_acquire); }

bool unchanged_since(uint32_t addr, uint32_t size, uint32_t gen) {
    if (g_all.load(std::memory_order_acquire) > gen) return false;
    uint32_t last = (uint32_t)(((uint64_t)addr + (size ? size : 1) - 1) >> kPageBits);
    for (uint32_t p = addr >> kPageBits; p <= last; p++)
        if (g_pages[p].load(std::memory_order_acquire) > gen) return false;
    return true;
}
}  // namespace memw
