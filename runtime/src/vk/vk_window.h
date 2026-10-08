// Android window <-> renderer interface (called from the UI thread through JNI).
#pragma once

struct ANativeWindow;

namespace gfx {

// A new window surface (the renderer takes its own reference), or null when the surface is destroyed.
// Returns once the renderer no longer uses the previous window.
void set_window(ANativeWindow* w);

// Where the TV and GamePad images go, in window pixels. A zero-sized TV rect means "letterbox to
// the whole window".
struct ScreenRect {
    float x = 0, y = 0, w = 0, h = 0;
};
// tv.w < 0: the TV picture is not drawn (the app's "GamePad on demand" shows only the GamePad)
void set_layout(ScreenRect tv, ScreenRect drc, bool drcVisible);

// The GamePad picture on its own display (the second screen of dual-screen devices): its window
// surface, or null. The main window's layout then usually leaves the GamePad picture out.
void set_drc_window(ANativeWindow* w);

// write the Vulkan pipeline cache to disk (the app is going to the background)
void save_caches();

// performance overlay: {game fps, game frame time avg ms, worst ms, presented fps, frame generation GPU ms}
// over the last second (any thread)
void perf_stats(float out[7]);
// the running GPU driver's name and version ("" before the renderer started)
const char* driver_info();
// fast forward (mods/turbo.cpp) runs: present the game's frames only, without frame generation
void set_fast_forward(bool on);
const char* gpu_name();
// an installed GPU driver was chosen but couldn't be loaded: the system's runs
bool driver_fallback();

// rendering resolution scale (the app's setting); applied from the next frame on (any thread)
void set_resolution_scale(float scale);

// frame generation settings (the app's menu); applied from the next frame on (any thread)
void request_frame_generation(bool on, const char* dll, bool quality, float flowScale, int multiplier, bool uiDetection);

// how the TV picture fills its area: 0 original aspect ratio (bars), 1 stretched, 2 filled (cut)
void set_tv_aspect(int mode);
int tv_aspect();

}  // namespace gfx
