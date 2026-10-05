#pragma once
#include <cstdint>

// Game state for the touch controls' icons (touch_hud.cpp); layout as ControlsView reads it:
// {flags, A action, B action, ZR action, X item, Y item, R item}, codes of TouchIcons' tables
namespace touch_hud {
constexpr int kSize = 7;
enum Flags : int32_t { kKnown = 1, kOnBoat = 2, kSwordOut = 4, kTargeting = 8, kFirstPerson = 16 };
void state(int32_t out[kSize]);
}  // namespace touch_hud
