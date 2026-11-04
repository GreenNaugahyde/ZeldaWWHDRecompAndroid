// Dynamic resolution (Graphics > Dynamic resolution, WWHD_DYN_RES_MIN): when the GPU holds the game
// below its frame rate, the resolution scale goes down in steps of 0.05 towards the chosen minimum;
// when the GPU has room again it goes back up towards the Rendering resolution setting (the upper
// limit). The game's logic is tied to its frames, so a GPU-bound game runs slower than normal; a
// lower resolution trades sharpness for speed.
//
// "GPU-bound" comes from GPU timestamps: two per command buffer (vk_profile.cpp), summed per wall
// time (busy share). A game held back by the CPU (game code, shader compiles) leaves the GPU idle,
// so the resolution stays. Measured in half-second windows, judged over a span of them: a step down
// once the GPU held the game back for 1 s in a row (far below the frame rate: at once), a step up only
// after 4 s in a row at the full frame rate with GPU room (one stutter starts the 4 s again: it rose on
// a single good half second while the game showed 24 fps, Tegra X1, user 2026-11-03); going up also
// waits until the last step down is a while ago, and that wait doubles when a step up had to be undone
// (no swinging back and forth; every change reallocates the screen-sized render targets).
//
// Memory: when Android's room above its low-memory line (MainActivity every 2 s, and MemAvailable read
// here every 250 ms) falls under WWHD_DYN_RES_MEM_MB (384), the scale goes down two steps at a time (every
// second at most; under half the line straight to the minimum) and that becomes the upper limit; devices
// under 4 GB use a line of 64 MB and never go above 1x (higher scales ran them into the line, and the
// pipelines freed there cost the CPU they were short of); no step up while the room is under twice the line. The limit goes back
// up one step per 10 s once the room stays above twice the line for a minute. (Tegra X1, 3 GB, at 2x:
// the low-memory killer ended the game after 13 min while dynres swung 1.45x <-> 1.50x.)
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "interp.h"
#include "platform.h"
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
constexpr int kUpWindows = 8;    // windows in a row with room before a step up (4 s)
constexpr int kDownWindows = 2;  // windows in a row held back by the GPU before a step down (1 s)

float g_cur = 0;  // the dynamic scale; 0 = not started (the setting)
double g_busy_ns = 0;
int g_frames = 0;
clk::time_point g_window{}, g_last_down{}, g_last_up{};
double g_hold = kHoldBase;  // seconds after a step down before going up
int g_room = 0;             // windows in a row with GPU room
int g_tight = 0;            // windows in a row held back by the GPU
bool g_settle = false;      // the window after a change: its frames were partly at the old scale
std::atomic<float> g_busy_shown{-1};
float g_mem_cap = 0;  // upper limit set by low memory; 0 = none
clk::time_point g_mem_down{}, g_mem_up{}, g_mem_room{};

float snap(float v) { return std::round(v / kStep) * kStep; }
double since(clk::time_point now, clk::time_point t) {
    return t == clk::time_point{} ? 1e9 : std::chrono::duration<double>(now - t).count();
}

}  // namespace

void set_dynamic_resolution_min(float m) { g_min = m >= 0.25f && m <= 4.0f ? m : 0.0f; }
float dynamic_resolution_min() { return g_min; }
bool dynamic_resolution_on() { return g_min > 0; }
float dynamic_resolution_busy() { return g_busy_shown; }

namespace {
std::atomic<bool> g_bench{false};
std::mutex g_bench_mu;
double g_bench_gpu_ns = 0, g_bench_cpu_ns = 0, g_bench_worst = 0;
int g_bench_frames = 0;
std::atomic<int> g_bench_total{0};  // frames shown since the benchmark started (not reset by a sample)
clk::time_point g_bench_t0{}, g_bench_last{};
}  // namespace

// a command buffer's GPU time (vk_profile.cpp, render thread)
void dynres_gpu_busy(double ns) {
    g_busy_ns += ns;
    if (g_bench.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_bench_mu);
        g_bench_gpu_ns += ns;
    }
}

void set_benchmark(bool on) {
    g_bench = on;
    LOG("[bench] %s", on ? "on" : "off");
}
bool benchmark_on() { return g_bench.load(std::memory_order_relaxed); }
int benchmark_frames() { return g_bench_total.load(std::memory_order_relaxed); }

void benchmark_frame() {
    if (!g_bench.load(std::memory_order_relaxed)) return;
    clk::time_point now = clk::now();
    std::lock_guard<std::mutex> lk(g_bench_mu);
    if (g_bench_last != clk::time_point{})
        g_bench_worst = std::max(g_bench_worst, std::chrono::duration<double, std::milli>(now - g_bench_last).count());
    g_bench_last = now;
    if (g_bench_t0 == clk::time_point{}) g_bench_t0 = now;
    g_bench_frames++;
    g_bench_total++;
    g_bench_cpu_ns += (double)platform::last_busiest_ns();
}

void benchmark_sample(float out[6]) {
    std::lock_guard<std::mutex> lk(g_bench_mu);
    clk::time_point now = clk::now();
    int n = g_bench_frames;
    out[0] = (float)n;
    out[1] = g_bench_t0 == clk::time_point{} ? 0.0f : (float)std::chrono::duration<double>(now - g_bench_t0).count();
    out[2] = n ? (float)(g_bench_gpu_ns / n / 1e6) : 0.0f;
    out[3] = n ? (float)(g_bench_cpu_ns / n / 1e6) : 0.0f;
    out[4] = (float)g_bench_worst;
    static uint64_t skipped_before = 0;
    uint64_t skipped = skipped_compiling();
    out[5] = (float)(skipped - skipped_before);
    skipped_before = skipped;
    g_bench_gpu_ns = g_bench_cpu_ns = g_bench_worst = 0;
    g_bench_frames = 0;
    g_bench_t0 = now;
}

std::atomic<int> g_mem_threshold{-1};  // Android's low-memory line in MB (MainActivity), -1 not known
void set_memory_threshold(int mb) { g_mem_threshold = mb; }

namespace {
// /proc/meminfo's field in MB, -1 when not readable
int meminfo_mb(const char* field) {
    FILE* f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    char line[128];
    int mb = -1;
    const size_t n = strlen(field);
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, field, n) && line[n] == ':') {
            mb = (int)(atol(line + n + 1) >> 10);
            break;
        }
    fclose(f);
    return mb;
}
// devices with little memory (under 4 GB; Tegra X1 3 GB) start at 1x at most (2x went over the line in the
// first area load, 1x played fine) and have a lower line: they play with ~150 MB of room at the title (Tegra X1
// TV), so 384 held them at the minimum (user 2026-11-02); the cap goes up as below once there is room
constexpr float kLowRamStart = 1.0f;
bool low_ram() {
    static const bool v = [] {
        int t = meminfo_mb("MemTotal");
        return t > 0 && t < 3800;
    }();
    return v;
}
int mem_line() {
    static const int v = getenv("WWHD_DYN_RES_MEM_MB") ? atoi(getenv("WWHD_DYN_RES_MEM_MB")) : low_ram() ? 64 : 384;
    return v;
}
// the room above the low-memory line: Java's reading (every 2 s) or, fresher, MemAvailable read here every
// 250 ms against Java's line, the lower of both (a load can eat 300 MB in 4 s: Tegra X1, 2026-11-02)
int g_fast_h = -1;
clk::time_point g_fast_at{};
int headroom(clk::time_point now) {
    int h = memory_headroom();
    const int thr = g_mem_threshold.load(std::memory_order_relaxed);
    if (thr < 0) return h;
    if (since(now, g_fast_at) >= 0.25) {
        g_fast_at = now;
        const int avail = meminfo_mb("MemAvailable");
        g_fast_h = avail >= 0 ? std::max(0, avail - thr) : -1;
    }
    if (g_fast_h < 0) return h;
    return h < 0 ? g_fast_h : std::min(h, g_fast_h);
}
bool memory_tight() {
    int h = headroom(clk::now());
    return h >= 0 && h < 2 * mem_line();
}
float upper(float setting) {
    float u = g_mem_cap > 0 ? std::min(setting, g_mem_cap) : setting;
    return low_ram() ? std::min(u, kLowRamStart) : u;  // (under 4 GB: 1x at most)
}

// the low-memory part of dynres_scale: a new scale when memory made one (0 otherwise)
float memory_step(clk::time_point now, float lo, float setting) {
    const int h = headroom(now);
    if (h < 0) return 0;
    if (h < mem_line()) {
        g_mem_room = {};
        // far under the line: straight to the minimum; else two steps, at most every second (the readings
        // are fresh now, so freed render targets show in the next ones)
        const bool far = h < mem_line() / 2;
        if (!far && since(now, g_mem_down) < 1.0) return 0;
        float before = g_cur;
        g_cur = far ? lo : std::max(lo, snap(g_cur - 2 * kStep));
        g_mem_cap = g_cur;
        g_mem_down = now;
        if (g_cur == before) return 0;
        g_last_down = now;
        g_room = 0;
        g_settle = true;
        LOG("[dynres] %.2fx -> %.2fx (memory low: %d MB above the low-memory line, under %d%s; upper limit %.2fx)", before, g_cur, h,
            mem_line(), far ? ", far under: minimum" : "", g_mem_cap);
        return g_cur;
    }
    if (g_mem_cap <= 0) return 0;
    if (h < 2 * mem_line()) {
        g_mem_room = {};
        return 0;
    }
    if (g_mem_room == clk::time_point{}) g_mem_room = now;
    if (since(now, g_mem_room) < 60.0 || since(now, g_mem_down) < 60.0 || since(now, g_mem_up) < 10.0) return 0;
    g_mem_up = now;
    g_mem_cap = snap(g_mem_cap + kStep);
    if (g_mem_cap >= (low_ram() ? std::min(setting, kLowRamStart) : setting)) {
        g_mem_cap = 0;
        LOG("[dynres] memory back (%d MB above the low-memory line): upper limit lifted, %.2fx", h, setting);
    } else {
        LOG("[dynres] memory back (%d MB above the low-memory line): upper limit %.2fx", h, g_mem_cap);
    }
    return 0;  // (going up is left to the GPU part)
}
}  // namespace

// once per frame at the swap (render thread): the scale for the next frame, `setting` or lower
float dynres_scale(float setting) {
    float lo = std::min<float>(g_min, setting);
    if (lo <= 0) g_mem_cap = 0;
    if (lo <= 0 || !gpu_timestamps()) {
        g_cur = 0;
        g_busy_ns = 0;
        g_frames = 0;
        g_window = {};
        g_busy_shown = -1;
        return setting;
    }
    if (g_cur <= 0) {
        g_cur = setting;
        if (low_ram() && setting > kLowRamStart && g_mem_cap <= 0) {
            g_mem_cap = std::max(lo, kLowRamStart);
            g_mem_down = clk::now();  // (the cap goes up after a minute with room)
            LOG("[dynres] under 4 GB of memory: starts at %.2fx at most (setting %.2fx)", g_mem_cap, setting);
        }
    }
    g_cur = std::max(lo, std::min(upper(setting), g_cur));
    clk::time_point now = clk::now();
    if (float m = memory_step(now, lo, setting)) return m;
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
    if (el > 2.0) {  // paused, in the background or loading: no decision, the spans start again
        g_room = g_tight = 0;
        return g_cur;
    }
    if (g_settle) {  // reallocation and old-scale frames: no decision on this window
        g_settle = false;
        return g_cur;
    }
    float before = g_cur;
    const bool gpuBound = fps < target * 0.95 && share > 0.85;
    g_tight = gpuBound ? g_tight + 1 : 0;
    if (gpuBound && (g_tight >= kDownWindows || fps < target * 0.8)) {
        g_tight = 0;
        // GPU-bound: one step, or far below the frame rate at once to where the pixel count fits
        // (GPU time taken as proportional to the pixels: scale * sqrt(fps / target))
        float next = g_cur - kStep;
        if (fps < target * 0.8) next = std::min(next - kStep, std::floor(g_cur * (float)std::sqrt(fps / target) / kStep) * kStep);
        g_cur = std::max(lo, snap(next));
        if (since(now, g_last_up) < 4.0) g_hold = std::min(kHoldMax, g_hold * 2);  // the step up didn't fit
        if (g_cur != before) g_last_down = now;
        g_room = 0;
    } else if (fps >= target * 0.97 && share < 0.70 && g_cur < upper(setting) && !memory_tight()) {
        if (++g_room >= kUpWindows && since(now, g_last_down) >= g_hold) {
            g_cur = std::min(upper(setting), snap(g_cur + kStep));
            g_last_up = now;
            g_room = 0;
        }
    } else {
        g_room = 0;  // (a stutter, or held back by the GPU but not yet for long enough to step down)
    }
    if (since(now, g_last_down) > 30.0) g_hold = kHoldBase;
    if (g_cur != before) g_settle = true;
    if (g_cur != before)
        LOG("[dynres] %.2fx -> %.2fx (game %.1f fps of %.0f, GPU busy %.0f%%, next step up after %.0f s)", before, g_cur, fps, target,
            share * 100, g_hold);
    return g_cur;
}

}  // namespace gfx
