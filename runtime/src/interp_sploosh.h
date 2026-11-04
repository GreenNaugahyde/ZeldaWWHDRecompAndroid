// Sploosh Kaboom reuses marker models in column-major cell order, not shot order.
// A new shot can give an existing model a different cell. That is an identity change,
// not movement: its previous matrix must not be blended into its new assignment.
#pragma once
#include <array>
#include <cstdint>

namespace interp {
inline std::array<uint8_t, 52> sploosh_marker_cells(const uint8_t* cells) {
    std::array<uint8_t, 52> result{};  // 20 hit models, then 32 miss models
    unsigned hits = 0, misses = 0;
    for (unsigned cell = 0; cell < 64; ++cell) {
        if (cells[cell] == 3 && hits < 20) result[hits++] = cell + 1;
        else if (cells[cell] == 1 && misses < 32) result[20 + misses++] = cell + 1;
    }
    return result;
}
struct SplooshMarkerCell { uint32_t model = 0; uint8_t cell = 0; };
using SplooshMarkers = std::array<SplooshMarkerCell, 52>;
inline uint8_t sploosh_marker_cell(const SplooshMarkers* markers, uint32_t model) {
    if (markers) for (const auto& marker : *markers)
        if (marker.model == model) return marker.cell;
    return 0;  // every other model keeps its ordinary interpolation history
}
}  // namespace interp
