#pragma once
#include <string>

// The developer Warp menu's places from the player's game files (warp_catalog.cpp)
namespace warp_catalog {
// writes out_path (JSON: categories of places with their spawn points); "" when done, else why not
std::string build(const std::string& game_dir, const std::string& out_path, int* stages = nullptr, int* spawns = nullptr);
}  // namespace warp_catalog
