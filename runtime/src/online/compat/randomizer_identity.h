// Online co-op compat (../README.md): whether a randomized game runs, and its code file (the game data's id).
#pragma once
#include <string>
namespace randomizer {
bool compiled_seed();                               // a randomized game is played
std::string game_rpx(const std::string& game_dir);  // the code file the game runs (the randomizer's or the game's)
}  // namespace randomizer
