// The AppKit display (display.mm) as seen by a renderer: window layers, and how the finished TV and
// GamePad pictures are laid out in the windows each frame. Plain C++ so the Vulkan renderer can use it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gfx {

// pixel rectangle in the target (drawable), top-left origin
struct Box { float x = 0, y = 0, w = 0, h = 0; };

// What to draw this frame (render thread, once per swap)
struct PresentPlan {
    float dw = 0, dh = 0;      // size the TV layout is for: the TV drawable, or WWHD_SIM_SCREEN
    bool sim = false;          // WWHD_SIM_SCREEN: the window itself still gets a layout for its drawable
    Box tv, pip;               // TV picture; GamePad overlay (pip_on)
    bool pip_on = false;
    bool pip_wanted = false;   // the overlay is up this frame (pip_on also needs a GamePad picture)
    float scale = 1;           // TV picture scale (target pixels per source pixel)
    float pip_opacity = 1;
    bool drc_window = false;   // the GamePad window is shown: present the GamePad picture there
    int filter = 0;            // picture scaling: 0 smooth, 1 sharp, 2 integer
    bool sample_auto = false;  // automatic overlay mode wants this frame's 32x18 signatures
};
constexpr uint32_t kSignatureW = 32, kSignatureH = 18;

// start of a present: GamePad aspect for touch, "Match window" aspect, scripted touches, layout
PresentPlan display_plan(bool haveTv, float tvW, float tvH, bool haveDrc, float drcW, float drcH, float layerW, float layerH,
                         uint64_t frame);
// the same layout for another target size (sim screen: the window's real drawable)
PresentPlan display_plan_for(const PresentPlan& p, float dw, float dh, float tvW, float tvH, float drcW, float drcH);
// GamePad window: its picture scaled to fit
Box display_layout(float dw, float dh, float tw, float th);
// automatic overlay: display-encoded luma (0..1, kSignatureW x kSignatureH) of the GamePad and TV pictures
void display_auto_signature(const std::vector<float>& drc, const std::vector<float>* tv, uint64_t frame);
// WWHD_DUMP_PRESENT / capture: composed window pictures requested since the last present
std::vector<std::string> display_take_present_dumps();
void request_present_dump(const std::string& path);
void display_log_present_dump(const std::string& path, const PresentPlan& p, float tvW, float tvH);

}  // namespace gfx
