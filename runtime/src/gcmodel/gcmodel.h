#pragma once
// GameCube player models in Wind Waker HD: a custom model pack for the GameCube randomizer (Link.arc, J3D models,
// e.g. github.com/Sage-of-Mirrors/Custom-Wind-Waker-Player-Models) converted into the game's own
// content/Common/Pack/permanent_3d.pack (Link.szs > Link.bfres). What the pack changed is found against the game's
// original (gcm_baseline.inc); those parts get the pack's meshes on HD's skeleton (HD's materials, shaders and
// animations stay), their textures upscaled to HD's sizes and formats. The app's mod pack import calls it
// (ModPacks.importArchive); tools/gcmodels has the PC tools this was worked out with.
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gcmodel {

struct Result {
    std::string error;   // empty when done
    std::string report;  // a line per part and texture, for the pack's details
};

// arc: Link.arc (RARC, Yaz0 or not); pack: the game's permanent_3d.pack; linkSzs: the converted Link.szs (Yaz0),
// the one file of the pack that changes. progress(percent, step) now and then.
Result convert(const std::vector<uint8_t>& arc, const std::vector<uint8_t>& pack, std::vector<uint8_t>& linkSzs,
               const std::function<void(int, const std::string&)>& progress = {});
// the game's permanent_3d.pack with Link.szs replaced; empty and `error` set when it can't be done
std::vector<uint8_t> assemble(const std::vector<uint8_t>& pack, const std::vector<uint8_t>& linkSzs, std::string& error);
// Link.szs out of a permanent_3d.pack (empty if none)
std::vector<uint8_t> link_szs(const std::vector<uint8_t>& pack);

}  // namespace gcmodel
