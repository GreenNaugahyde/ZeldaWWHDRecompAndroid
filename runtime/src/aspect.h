// Aspect ratio of the TV picture (Graphics menu, WWHD_ASPECT): see aspect.cpp.
#pragma once
#include <cstdint>

#include <string>

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
// the GamePad screen's touch targets (the game's hit boxes, panes "B_*", drawn this frame) as
// "name parent x y w h;" (16:9 screen, 0..1, y down), and whether the game shows its own button
// cursor there (W_Cursor_00: it takes the buttons itself, e.g. Options)
std::string drc_targets();
bool drc_game_cursor();
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
// the Picto Box's viewfinder at twice its size (touch controls): its window, black frame and shutter
// in the PictographBox_00 layout are scaled up around the screen centre, the 3D view zoomed to match
void set_picto_big(bool on);
// around camera_draw (interp.cpp's hook): the 3D view zoomed as for a photo while the bigger finder
// is up (aspect.cpp); end() gets what begin() returned
bool picto_zoom_begin();
void picto_zoom_end(bool set);
// The 3D view zoomed out (z: the vertical field of view's tangent times z, 1 = the game's; Graphics
// menu and a pinch) and its picture's centre moved down (layout units of 720; the singing panel), see
// aspect.cpp. Around camera_draw: begin() returns what end() needs. For projections of our own: this
// frame's tangent factor and shift (NDC, down).
void set_view_zoom(float z);
void set_view_shift(float units);
float view_zoom_begin(uint32_t cam);
void view_zoom_end(uint32_t cam, float fovy);
float view_tan_scale();
float view_shift_ndc();
// The GamePad inventory's pages (MenuItem_00: the items at x 0, the page right of them at -1280, the app's options
// page left of them at +1280) moved by the app: mode 1 follows a finger (v layout units to the right of where the
// drag began), 2 glides to x = v, 0 leaves them where they are. A tap on the game's arrow where the pages already
// are switches the game's page without a move (its slide's speed comes from what is left).
void item_page_drag(int mode, float v);
bool item_page_gliding();     // mode 2 still running
float item_page_x();          // the pages' x as of the last frame drawn
void set_item_sheet_left(bool on);  // the paper sheet reaches left of the items (under the options page)
}  // namespace aspect
