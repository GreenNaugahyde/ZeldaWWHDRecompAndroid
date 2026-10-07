// Guest CPU writes the GPU may read, as the game announces them: on the Wii U the CPU caches aren't
// coherent with the GPU, so data the GPU reads must be flushed first (DCFlushRange / DCStoreRange,
// GX2Invalidate with the CPU bit); file reads land in memory by DMA. Each 4 KiB page keeps the
// counter value of its last write, so the renderer can tell whether data it copied earlier is still
// current without comparing it byte by byte (vk_draw.cpp, vertex buffers).
#pragma once
#include <cstdint>

namespace memw {
void mark(uint32_t addr, uint32_t size);
void mark_all();  // everything may have changed (a save state was loaded)
uint32_t now();   // take before copying; compare with unchanged_since afterwards
bool unchanged_since(uint32_t addr, uint32_t size, uint32_t gen);
}  // namespace memw
