#pragma once
#include <cstdint>
#include <vector>

// The game's own GamePad screens drawn on a GamePad device (the album, the warp map, Sploosh Kaboom, the wind's
// choice, the save question ...): the GamePad layouts' panes as the game draws them (nw::lyt pictures, windows and
// text), each as textured quads in the GamePad's 1280 x 720 layout space with the material's colours, sent to the
// device (PadHost) and drawn there (MirrorView). Textures are the game's own GX2 surfaces, decoded once each.
namespace drc_mirror {
void set_on(bool on);  // a GamePad device wants the quads (else nothing is recorded)
bool on();

// game threads (aspect.cpp, Pane::Draw): a layout root's tree begins (drawn to the GamePad?), a pane about to be
// drawn (pre-order), the root's tree done
void root_begin(uint32_t root, bool drc);
void pane(uint32_t pane);
void root_done(uint32_t root);
// render thread (OP_LAYOUT_ROOT): the order the roots go to the GamePad
void root_drawn(uint32_t root, bool drc);

// the GamePad screen as last drawn, serialized (little-endian; see drc_mirror.cpp), if it changed since the
// last call: false if not
bool frame(std::vector<uint8_t>& out);

// a texture a frame uses (its id): the GX2 surface to decode (still the same content: else false)
struct TexInfo {
    uint32_t image, size, w, h, depth, format, tile, swizzle, slice, compSel;
};
bool texture(uint32_t id, TexInfo& info);
}  // namespace drc_mirror
