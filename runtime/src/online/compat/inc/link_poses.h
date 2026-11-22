// Online co-op compat (../README.md): Link's files from the game's 3D pack (coop_emotes: his skeleton and clips).
#pragma once
#include <cstdint>
#include <string>
#include <vector>
namespace link_poses {
using Bytes = std::vector<uint8_t>;
// Link.bfres and LkAnm.bfres of the 3D pack the game opened (permanent_3d.pack); false with `why`
bool game_files(Bytes& link_bfres, Bytes& anim_bfres, std::string& why);
void note_open(const std::string& guest_path, const std::string& host_path);  // (the fs: which 3D pack)
}  // namespace link_poses
