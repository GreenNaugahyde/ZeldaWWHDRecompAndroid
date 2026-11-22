// Online co-op compat (README.md): the fork's side of the compat layer (the app's bridge, the game loop).
#pragma once
struct Cpu;
#include <string>
#include <vector>

namespace online_compat {
constexpr int kShapeFloats = 15;  // a shape of the overlay: kind, r, g, b, a, 4 points, size, text index
void set_dir(const std::string& dir);
std::string dir();                      // that folder   // the app's internal folder for online (identity, settings, blocks)
void tick(Cpu* c);                      // game thread, each full step: the co-op code's warps answered, portable loads
std::vector<std::string> take_notices();  // what the co-op code told the player since the last call
}  // namespace online_compat
