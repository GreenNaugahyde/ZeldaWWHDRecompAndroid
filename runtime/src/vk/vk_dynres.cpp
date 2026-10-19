// Dynamic resolution (Graphics > Dynamic resolution, WWHD_DYN_RES_MIN): when the GPU holds the game
// below its frame rate, the resolution scale goes down in steps of 0.05 towards the chosen minimum;
// when the GPU has room again it goes back up towards the Rendering resolution setting (the upper
// limit). The game's logic is tied to its frames, so a GPU-bound game runs slower than normal; a
// lower resolution trades sharpness for speed.
//
// "GPU-bound" comes from GPU timestamps: two per command buffer (vk_profile.cpp), summed per wall
// time (busy share). A game held back by the CPU (game code, shader compiles) leaves the GPU idle,
// so the resolution stays. Decisions every half second; going up waits until the last step down is
// a while ago, and that wait doubles when a step up had to be undone (no swinging back and forth;
// every change reallocates the screen-sized render targets).
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "interp.h"
#include "runtime.h"
#include "vk.h"

namespace gfx {
namespace {

using clk = std::chrono::steady_clock;

std::atomic<float> g_min{[] {
    const char* e = getenv("WWHD_DYN_RES_MIN");
    float v = e ? (float)atof(e) : 0.0f;
    return v >= 0.25f && v <= 4.0f ? v : 0.0f;
}()};

constexpr float kStep = 0.05f;
constexpr double kWindow = 0.5;  // seconds per decision
constexpr double kHoldBase = 2.0, kHoldMax = 32.0;

float g_cur = 0;  // the dynamic scale; 0 = not started (the setting)
double g_busy_ns = 0;
int g_frames = 0;
clk::time_point g_window{}, g_last_down{}, g_last_up{};
double g_hold = kHoldBase;  // seconds after a step down before going up
int g_room = 0;             // windows in a row with GPU room
bool g_settle = false;      // the window after a change: its frames were partly at the old scale
std::atomic<float> g_busy_shown{-1};

float snap(float v) { return std::round(v / kStep) * kStep; }
double since(clk::time_point now, clk::time_point t) {
    return t == clk::time_point{} ? 1e9 : std::chrono::duration<double>(now - t).count();
}

}  // namespace

void set_dynamic_resolution_min(float m) { g_min = m >= 0.25f && m <= 4.0f ? m : 0.0f; }
float dynamic_resolution_min() { return g_min; }
bool dynamic_resolution_on() { return g_min > 0; }
float dynamic_resolution_busy() { return g_busy_shown; }

// a command buffer's GPU time (vk_profile.cpp, render thread)
void dynres_gpu_busy(double ns) { g_busy_ns += ns; }

// once per frame at the swap (render thread): the scale for the next frame, `setting` or lower
float dynres_scale(float setting) {
    float lo = std::min<float>(g_min, setting);
    if (lo <= 0 || !gpu_timestamps()) {
        g_cur = 0;
        g_busy_ns = 0;
        g_frames = 0;
        g_window = {};
        g_busy_shown = -1;
        return setting;
    }
    if (g_cur <= 0) g_cur = setting;
    g_cur = std::max(lo, std::min(setting, g_cur));
    // debug (WWHD_DYN_RES_FLIP=N): one step down and back every N frames, to check the changes for glitches
    static const int flip = getenv("WWHD_DYN_RES_FLIP") ? atoi(getenv("WWHD_DYN_RES_FLIP")) : 0;
    if (flip > 0) {
        static int n = 0;
        if (++n % flip == 0) {
            g_cur = g_cur < setting ? setting : std::max(lo, snap(setting - kStep));
            LOG("[dynres] flip to %.2fx", g_cur);
        }
        return g_cur;
    }
    clk::time_point now = clk::now();
    if (g_window == clk::time_point{}) {
        g_window = now;
        g_busy_ns = 0;
        g_frames = 0;
        return g_cur;
    }
    g_frames++;
    double el = std::chrono::duration<double>(now - g_window).count();
    if (el < kWindow) return g_cur;
    // (frame generation's network is recorded into the frame's command buffer: already in it; adding
    // fg::gpu_ms() on top counted it twice, and at x4 the 14 ms network alone held the scale at the minimum)
    double busy = g_busy_ns / 1e6;  // ms
    double share = busy / (el * 1000.0), fps = g_frames / el;
    double target = 30.0 * interp::frames_per_step();
    g_window = now;
    g_busy_ns = 0;
    g_frames = 0;
    g_busy_shown = (float)std::min(share, 1.0);
    if (el > 2.0) return g_cur;  // paused, in the background or loading: no decision
    if (g_settle) {  // reallocation and old-scale frames: no decision on this window
        g_settle = false;
        return g_cur;
    }
    float before = g_cur;
    if (fps < target * 0.95 && share > 0.85) {
        // GPU-bound: one step, or far below the frame rate at once to where the pixel count fits
        // (GPU time taken as proportional to the pixels: scale * sqrt(fps / target))
        float next = g_cur - kStep;
        if (fps < target * 0.8) next = std::min(next - kStep, std::floor(g_cur * (float)std::sqrt(fps / target) / kStep) * kStep);
        g_cur = std::max(lo, snap(next));
        if (since(now, g_last_up) < 4.0) g_hold = std::min(kHoldMax, g_hold * 2);  // the step up didn't fit
        if (g_cur != before) g_last_down = now;
        g_room = 0;
    } else if (fps >= target * 0.97 && share < 0.70 && g_cur < setting) {
        if (++g_room >= 2 && since(now, g_last_down) >= g_hold) {
            g_cur = std::min(setting, snap(g_cur + kStep));
            g_last_up = now;
            g_room = 0;
        }
    } else {
        g_room = 0;
    }
    if (since(now, g_last_down) > 30.0) g_hold = kHoldBase;
    if (g_cur != before) g_settle = true;
    if (g_cur != before)
        LOG("[dynres] %.2fx -> %.2fx (game %.1f fps of %.0f, GPU busy %.0f%%, next step up after %.0f s)", before, g_cur, fps, target,
            share * 100, g_hold);
    return g_cur;
}

}  // namespace gfx
