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
float drc_picture_aspect();  // width / height of the GamePad picture the game sent last

// write the Vulkan pipeline cache to disk (the app is going to the background)
void save_caches();

// performance overlay: {game fps, game frame time avg ms, worst ms, presented fps, frame generation GPU ms}
// over the last second, {game frames so far, their seconds}, then live {resolution scale, 3D view width,
// height, dynamic resolution's minimum (0 = off), its GPU busy share (-1 = off)} (any thread)
constexpr int kPerfStats = 12;
void perf_stats(float out[kPerfStats]);
// the running GPU driver's name and version ("" before the renderer started)
const char* driver_info();
// fast forward (mods/turbo.cpp) runs: present the game's frames only, without frame generation
void set_fast_forward(bool on);
const char* gpu_name();
// the shader cache's replay at start (vk_draw.cpp): {state 0 not started / 1 loading / 2 done, bytes
// read, file size, shader variants prepared}, for the app's loading screen
void cache_progress(uint64_t out[4]);
// an installed GPU driver was chosen but couldn't be loaded: the system's runs
bool driver_fallback();

// rendering resolution scale (the app's setting); applied from the next frame on (any thread)
void set_resolution_scale(float scale);
// dynamic resolution's lowest scale, 0 = off (vk_dynres.cpp)
void set_dynamic_resolution_min(float min);
float dynamic_resolution_min();
// ---- graphics benchmark (vk_dynres.cpp, the app's preset finder): GPU timestamps stay on while it
// runs; per frame shown: the GPU's busy time and the busiest game / render thread's CPU time
void set_benchmark(bool on);
bool benchmark_on();
// since the last call (then starts again): {frames shown, seconds, GPU ms per frame, busiest thread ms
// per frame (0: unknown), worst frame interval ms, draws left out while their pipeline compiled} (any thread)
constexpr int kBenchStats = 6;
void benchmark_sample(float out[kBenchStats]);
int benchmark_frames();  // frames shown since it started (a live count)
int work_done();         // shaders compiled and pipelines built since the start (the benchmark's boot wait)
void trim_memory();      // Android runs low on memory: pipelines unused lately are freed (any thread; vk_draw.cpp)
void set_background_compiles_paused(bool on);  // its measuring windows: no new background compiles

// frame generation settings (the app's menu); applied from the next frame on (any thread)
void request_frame_generation(bool on, const char* dll, bool quality, float flowScale, int multiplier, bool uiDetection);

// how the TV picture fills its area: 0 original aspect ratio (bars), 1 stretched, 2 filled (cut)
void set_tv_aspect(int mode);
int tv_aspect();

}  // namespace gfx
