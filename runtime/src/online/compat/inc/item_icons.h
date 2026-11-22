// Online co-op compat (../README.md): the archive readers link_masks uses (the fork's gcmodel ones).
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
namespace item_icons {
using Bytes = std::vector<uint8_t>;
std::map<std::string, Bytes> read_sarc(const Bytes& sarc);  // throws std::runtime_error when it isn't one
Bytes yaz0_decode(const Bytes& in);
}  // namespace item_icons
