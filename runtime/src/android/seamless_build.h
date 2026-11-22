#pragma once
#include <string>

// Seamless houses: an island's house files from the player's game files (seamless_build.cpp)
namespace seamless_build {
// writes the island `host` (its sea room) into out_dir; "" when done, else why not
std::string build_island(const std::string& game_dir, const std::string& out_dir, int host);
}  // namespace seamless_build
