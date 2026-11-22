// Online co-op compat (../README.md): the GX2 surface untiling link_masks uses (upstream's lyt_build::untile, copied
// in compat.cpp).
#pragma once
#include <cstdint>
#include <vector>
namespace lyt_build {
using Bytes = std::vector<uint8_t>;
Bytes untile(const Bytes& surface, uint32_t ew, uint32_t eh, uint32_t bpp, int mode, uint32_t swizzle);
}  // namespace lyt_build
