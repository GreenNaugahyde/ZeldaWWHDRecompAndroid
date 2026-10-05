#pragma once
#include <cstdint>

// ADPF performance hints (perf_hint.cpp)
namespace perf_hint {
void register_render_thread();         // called on the GX2 render thread
void on_swap(uint32_t swap_interval);  // called by the game thread at each GX2 swap
}  // namespace perf_hint
