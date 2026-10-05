#pragma once
#include <string>
#include <vector>

// Official artwork for the touch controls from the player's game files (ui_icons.cpp)
namespace ui_icons {
struct Request {
    std::string layout;   // layout archive in the 2D pack, without ".szs" (e.g. "BtnItemIcon_00")
    std::string texture;  // texture in its timg/ folder, without ".bflim" (e.g. "Icon128_21^l")
};
// writes outDir/<texture>.rgba for each texture found; returns how many
int extract(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests);
}  // namespace ui_icons
