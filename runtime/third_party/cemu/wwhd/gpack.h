// Cemu graphic packs (rules.txt + patch_*.asm) for the recompiled game. Adapted from Cemu's
// Cafe/GraphicPack/GraphicPack2*.cpp (MPL-2.0, see ../LICENSE-MPL-2.0.txt): the same rules.txt preset
// logic and the same patch syntax, assembled with Cemu's own PPC assembler. Instead of writing into a
// running emulator's memory, the result is a list of byte writes (game code, data, code caves) that the
// runtime applies before the game code is recompiled and loaded.
#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace gpack {

struct Preset {
    std::string category, name, condition;
    bool isDefault = false;
    std::vector<std::pair<std::string, std::string>> vars;  // "$name" (type suffix removed) -> expression
};

struct Rules {
    std::string name, path, description;
    int version = 0;
    std::vector<uint64_t> titleIds;
    std::vector<std::pair<std::string, std::string>> defaults;  // [Default] vars
    std::vector<Preset> presets;                                // in file order
    std::vector<std::string> sections;                          // all section names (to see what a pack uses)
    std::vector<std::string> intVars;                           // variables declared "$name:int"
};

// false and `err` if the file has no [Definition] or a malformed line
bool parse_rules(const std::string& text, Rules& out, std::string& err);

// the categories in file order ("" for presets without one)
std::vector<std::string> categories(const Rules& r);

// the active preset of each category for a selection (category -> preset name; a missing or unknown
// name selects the category's default: default = 1, else the first visible one) and the variables
// they give ($name -> value), as Cemu: active visible presets first, then hidden ones, then [Default]
struct Resolved {
    std::map<std::string, std::string> active;  // category -> preset name
    std::map<std::string, double> vars;
    std::vector<std::pair<std::string, std::vector<std::string>>> visible;  // category -> visible preset names
};
Resolved resolve(const Rules& r, const std::map<std::string, std::string>& selection);

// a [TextureRedefine] section (GraphicPack2.cpp, Activate): which textures, and what to change. -1: not set
struct TextureRule {
    int width = -1, height = -1, depth = -1;
    int inMem1 = 0;  // 0 any, 1 only textures in MEM1, 2 only outside
    std::vector<int> formats, formatsExcluded, tileModes, tileModesExcluded;
    int overwriteWidth = -1, overwriteHeight = -1, overwriteDepth = -1, overwriteFormat = -1;
    int lodBias = -1, relativeLodBias = -1;  // in 1/64 (Cemu's units)
    int anisotropy = -1;                     // 0..4: 1x, 2x, 4x, 8x, 16x
};
// the rules.txt's texture rules, their expressions evaluated with the preset variables
std::vector<TextureRule> texture_rules(const std::string& rulesText, const std::map<std::string, double>& vars);

struct Module {
    uint32_t crc = 0;                                     // Cemu's module checksum (moduleMatches)
    std::vector<std::pair<uint32_t, uint32_t>> sections;  // address, size
};
uint32_t module_crc(const std::vector<uint8_t>& rpxFile);  // as Cemu's RPLLoader_BeginCemuhookCRC
Module module_info(const std::vector<uint8_t>& rpxFile);

struct Write {
    uint32_t addr;
    std::vector<uint8_t> bytes;
};
struct PatchResult {
    std::vector<Write> writes;
    uint32_t caveBase = 0, caveEnd = 0;  // the code caves used, [caveBase, caveEnd)
    std::vector<uint32_t> entryCallbacks;  // .callback entry: called before the game's entry point
    std::vector<uint32_t> codePointers;    // code whose address is taken (function entries for the recompiler)
    int groups = 0, groupsOther = 0;       // groups applied / for other executables (moduleMatches)
    std::vector<std::string> errors;
    std::map<std::string, uint32_t> symbols;  // labels and variables (debugging)
};

// one pack's patch files (file name, text), its variables (resolve), the executable; code caves are
// placed from caveBase on (256-byte aligned, one per group as Cemu). false: errors in out.errors
bool apply_patches(const std::vector<std::pair<std::string, std::string>>& files, const std::map<std::string, double>& vars,
                   const Module& m, uint32_t caveBase, PatchResult& out);

}  // namespace gpack
