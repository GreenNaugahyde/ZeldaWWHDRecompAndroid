#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Official artwork for the touch controls from the player's game files (ui_icons.cpp)
namespace ui_icons {
// mip 0 of a GX2 surface (GX2SurfaceFormat gx2Format; R8, R8G8, RGBA8, BC1, BC3, BC4, BC5; sRGB or not, as stored)
// of dataSize bytes at b, untiled: w * h texels of ch channels each (BC1 / BC3 give 4, BC4 1, BC5 2)
bool decode_surface(const uint8_t* b, uint32_t dataSize, uint32_t w, uint32_t h, uint32_t gx2Format, uint32_t tileMode,
                    uint32_t swizzle, std::vector<uint8_t>& out, uint32_t& ch);
struct Request {
    std::string layout;   // layout archive in the 2D pack, without ".szs" (e.g. "BtnItemIcon_00")
    std::string texture;  // texture in its timg/ folder, without ".bflim" (e.g. "Icon128_21^l")
};
// writes outDir/<texture>.rgba for each texture found; returns how many
int extract(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests);
// every texture of the pack as outDir/<layout>__<texture>.rgba (to look at them); how many
int dump_all(const std::string& gameDir, const std::string& outDir);
// a font of the pack (e.g. "CKingMain"): outDir/<font>_<n>.rgba glyph sheets and outDir/<font>.txt metrics
bool extract_font(const std::string& gameDir, const std::string& outDir, const std::string& font);
// the texts of a layout's message file (e.g. "CommandGuide_00": the buttons' action names) as outFile, a
// "label<TAB>text" line each (UTF-8)
bool extract_texts(const std::string& gameDir, const std::string& outFile, const std::string& layout);
}  // namespace ui_icons
