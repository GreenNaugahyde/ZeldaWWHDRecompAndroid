// Frame rate modes (interp.cpp): the API the menus, the settings overlay, the hosts and the
// renderers use.
#pragma once
#include <cstdint>

namespace interp {
// the camera drawn last (camera_process_class; eye at +0xDC, center at +0xE8), 0 before the first
uint32_t last_camera();
bool frame_is_hold();        // this frame's per-frame function runs a hold pass (no logic): for tasks that calc before it
void record_executed_step(); // game thread; includes built-in turbo passes
uint64_t executed_steps();    // actual actor logic passes, including true60 half steps
int mode();                 // 0 off (30 fps), 1 frame interpolation (at fps()), 2 true 60
void set_mode(int m);
int fps();                  // frame interpolation's chosen frame rate: 60, 120 or 240
void set_fps(int f);
void toggle_fps(int f);     // frame interpolation at f on, or off if it is on at f (menus, the 6 key)
const char* mode_name();    // "30 fps", "60 fps", "120 fps", "240 fps", "true 60"; "240 fps (120 shown)" when capped
int output_fps();           // the rate frame interpolation draws: fps() capped to the display
int frames_per_step();      // frames drawn per 30 Hz logic step: 1, 2, 4 or 8

// "Keep game speed": in-between frames that do not fit are skipped instead of slowing the game down;
// one setting for 60 fps and one for 120/240 fps
bool paced_interpolation();  // at the current rate
void set_paced_interpolation(bool on);
bool paced_interpolation_at(int fps);
void set_paced_interpolation_at(int fps, bool on);
float paced_drawn_share();  // share of in-between frames drawn lately (paced and on), -1 otherwise

// the display (hosts and renderers): refresh rate of the TV window's screen (0: unknown), and
// whether presenting waits for its vsync (Metal, Vulkan FIFO)
void set_display_hz(int hz);
int display_hz();           // (WWHD_DISPLAY_HZ overrides the detected rate)
void set_present_vsync(bool on);
}  // namespace interp
