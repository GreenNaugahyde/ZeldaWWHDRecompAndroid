#pragma once
#include <cstdint>

// ADPF performance hints (perf_hint.cpp)
namespace perf_hint {
void register_render_thread();         // called on the GX2 render thread
void register_record_thread();         // called on the GX2 record thread
void on_swap(uint32_t swap_interval);  // called by the game thread at each GX2 swap
// CPU time per swap of the game and the render thread, smoothed over the last swaps (ns)
void recent_work(int64_t& game_ns, int64_t& render_ns);
void recent_work(int64_t& game_ns, int64_t& render_ns, int64_t& record_ns);
}  // namespace perf_hint
