#pragma once
// Reading the game's own archive files (Wii U): Yaz0 compression and SARC archives (big-endian).
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace game_archive {
using Bytes = std::vector<uint8_t>;

bool read_file(const std::string& path, Bytes& out);
inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
inline uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
// Yaz0 data unpacked into out; anything else is copied as it is
bool yaz0(const Bytes& in, Bytes& out);
// each named file of a SARC archive: (name, data, size); false if b isn't a SARC
bool sarc_each(const Bytes& b, const std::function<void(const char* name, const uint8_t* data, size_t size)>& fn);
// a file of a SARC archive by name
bool sarc_file(const Bytes& b, const char* name, Bytes& out);
}  // namespace game_archive
