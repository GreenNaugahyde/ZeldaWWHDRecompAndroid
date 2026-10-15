// Aspect ratio of the TV picture (Graphics menu, WWHD_ASPECT): see aspect.cpp.
#pragma once
#include <cstdint>

namespace aspect {
enum Mode : int { kOriginal = 0, kWindow, k16x10, k21x9, k32x9, kCustom, kModes };
int mode();
void set_mode(int m);
const char* mode_name(int m);
float mode_value(int m);           // fixed aspect of a mode (0 for kWindow)
void set_window_aspect(float a);   // shape of the TV window's drawable (render thread, every present)
float requested();                 // what the setting asks for now
// main thread, GX2SwapScanBuffers: latch the aspect for the next game frame, update the game's
// projections; returns the aspect the render thread should switch to at this swap (drc: the same
// for the GamePad picture)
float on_swap(float& drc);
float game();
uint64_t game_frame();                // swaps so far                      // aspect the game draws with this frame
// HD layouts: the game thread marks uploads of layout projection matrices and the drawing of layout
// roots; the render thread, which knows the target, narrows the projection for TV-shaped targets
// and reports where each root went
bool tagged_projection();                          // gx2: the vertex uniform upload in progress is one
enum Screen : int { kOtherTarget = 0, kTvTarget = 1, kDrcTarget = 2 };
void layout_root_target(uint32_t root, int screen); // render thread (OP_LAYOUT_ROOT)
void ss_reset();  // save state loaded: every layout recomputes its matrices once
// The GamePad picture's shape (a second display that isn't 16:9, e.g. the AYN Thor's lower screen):
// 16:9 (0) or the display's aspect; see aspect.cpp "GamePad picture"
void set_drc_aspect(float a);
// a touch on the GamePad picture (u, v in 0..1 of the picture, whose aspect is given) -> the
// game's 16:9 touch screen in its middle
bool drc_touch(float u, float v, float pictureAspect, float& x, float& y);  // false: nothing there
}  // namespace aspect
