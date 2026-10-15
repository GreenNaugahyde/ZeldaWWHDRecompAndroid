// Mod packs: Cemu graphic packs (rules.txt + patch_*.asm) applied to the recompiled game.
//
// Packs live in <files>/packs/<folder>/ (next to the game folder); <files>/packs/enabled.txt, written by
// the app, lists the enabled ones and their preset choices:
//   pack<TAB><folder>
//   preset<TAB><folder><TAB><category><TAB><preset name>
// The code patches of the enabled packs that match this executable (moduleMatches) change the game code
// before it is recompiled (recomp::CodePatch, part of the code cache key: a change recompiles), new code
// goes to code caves from 0x01800000 on (as Cemu), and all patched bytes are written into guest memory
// at boot. A pack's content/ folder replaces or adds game files under /vol/content (as Cemu mounts it;
// file names ignore case, a later pack in the list wins). Pixel and vertex shader replacements
// (<base hash>_<aux hash>_ps|vs.txt, Cemu's GLSL for Vulkan; packs of rules version 4 and up) stand in for
// the translation of the shader with those hashes (vk_draw.cpp; the first pack in the list wins, as
// Cemu). Other parts of a graphic pack (geometry shaders, texture resolution rules, aoc/) are reported
// as not supported yet.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace packs {

constexpr uint32_t kCodeCave = 0x01800000, kCodeCaveEnd = 0x01C00000;  // Cemu's code cave area (4 MiB)

// reads enabled.txt and builds the patch for the game in game_dir (once; later calls with the same
// folder return at once) and hands it to the recompiler (recomp::set_code_patch)
void init(const std::string& gameDir);

void apply_memory();  // boot, after the executable is in guest memory: the patched bytes

// content replacement (hle/fs.cpp): `rel` is the path below /vol/content ('/'-separated, any case)
std::string content_file(const std::string& rel);  // the pack's file for it, or ""
std::string content_dir(const std::string& rel);   // a pack's folder for it, or "" (folders only packs have)
struct DirEntry {
    std::string name, host;  // the name as the pack spells it, its host path
};
std::vector<DirEntry> content_entries(const std::string& relDir);  // what packs put into that folder

// a pack's replacement for a shader (type 'p', 'v', 'g'; Cemu's hashes, vk_draw.cpp), its $variables
// filled in; null if none
const std::string* custom_shader(uint64_t baseHash, uint64_t auxHash, char type);

// texture rules ([TextureRedefine]): what the enabled packs change for sampling a texture (vk_draw.cpp,
// get_sampler); null when no rule matches. Rules apply in order, a later one overwrites what an earlier
// one set (as Cemu's LatteTexture constructor).
struct TextureOverride {
    int anisotropy = -1;  // 0..4 (1x..16x), -1 the game's
    bool hasLodBias = false, hasRelativeLodBias = false;
    int lodBias = 0, relativeLodBias = 0;  // 1/64
};
const TextureOverride* texture_override(uint32_t format, uint32_t tileMode, uint32_t width, uint32_t height, uint32_t depth,
                                        uint32_t addr);

struct Status {
    std::string folder, name;
    int groups = 0, groupsOther = 0;  // patch groups applied / for other game versions
    std::vector<std::string> notes;   // errors and unsupported parts
};
const std::vector<Status>& status();
void note_color_buffer(uint32_t w, uint32_t h);  // GX2SetColorBuffer, target 0 (the compat in packs.cpp)
uint32_t cave_end();  // end of the code caves in use (kCodeCave if none): save states keep [kCodeCave, end)

// the app's pack list entry: a description of the pack in <packsDir>/<folder> as lines
//   name<TAB>..., path<TAB>..., description<TAB>..., version<TAB>n,
//   category<TAB><name><TAB><active preset><TAB><preset 1><TAB><preset 2>... (visible presets),
//   patches<TAB><groups for this game version><TAB><groups for others>, note<TAB><text>
// selection: "category<TAB>preset" lines; gameDir: to check the patches against the executable
std::string describe(const std::string& packDir, const std::string& selection, const std::string& gameDir);

}  // namespace packs
