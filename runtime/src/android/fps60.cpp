// The app's 60 fps setting: frame interpolation (interp.cpp; game logic stays at 30 steps per
// second, every second frame is drawn halfway between two steps).
// Adaptive mode: interpolation only pays off at a steady 60. It falls back to 30 (where it paces
// evenly) as soon as frames are lost: a second below 50 fps, or two seconds in a row below 57.
// It goes back to 60 only when the measurements show room for it: the render and game threads'
// CPU time per frame at 30 fps (perf_hint) low enough for twice the frames, for 3 s in a row, and
// not too soon after a failed try (5 s, doubling up to 2 min after tries that fail quickly).
#include "fps60.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "../runtime.h"
#include "perf_hint.h"

namespace interp {
bool interp_on();
void set_enabled(bool v);
void set_mode(int m);  // 0 off, 1 interpolation, 2 true 60
}  // namespace interp

namespace fps60 {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<int> g_mode{0};
std::atomic<bool> g_reset{false};

constexpr auto kWindow = std::chrono::seconds(1);
constexpr auto kMinWait = std::chrono::seconds(5), kMaxWait = std::chrono::seconds(120);
constexpr double kSteady = 57.0, kBad = 50.0;
// room for 60: CPU time per frame at 30 fps (60 fps renders each frame in half the time)
constexpr int64_t kRenderRoom = 11000000, kGameRoom = 22000000;
}  // namespace

void set_mode(int m) {
    m = std::clamp(m, 0, 3);
    g_mode = m;
    g_reset = true;
    if (m == 3) interp::set_mode(2);  // true 60 (experimental): Link and the camera at 60 logic steps
    else interp::set_enabled(m != 0);
}

int mode() { return g_mode; }

void on_swap() {
    static Clock::time_point window_start, fallback_at, tried_at, room_since;
    static Clock::duration wait = kMinWait;
    static uint32_t swaps = 0;
    static int low_windows = 0;
    static bool warmup = true, has_room = false;
    if (g_mode.load(std::memory_order_relaxed) != 2) return;
    auto now = Clock::now();
    if (g_reset.exchange(false)) {
        window_start = tried_at = now;
        swaps = 0;
        low_windows = 0;
        warmup = true;
        wait = kMinWait;
        return;
    }
    if (!interp::interp_on()) {  // at 30 after a fallback: back to 60 once there is room for it
        int64_t game, render;
        perf_hint::recent_work(game, render);
        bool room = render > 0 && render < kRenderRoom && game < kGameRoom;
        if (room && !has_room) room_since = now;
        has_room = room;
        if (room && now - room_since >= std::chrono::seconds(3) && now - fallback_at >= wait) {
            interp::set_enabled(true);
            LOG("[fps60] room for 60 (render %.1f ms, game %.1f ms per frame): trying 60 fps", render / 1e6, game / 1e6);
            window_start = tried_at = now;
            swaps = 0;
            low_windows = 0;
            warmup = true;
        }
        return;
    }
    swaps++;
    auto elapsed = now - window_start;
    if (elapsed < kWindow) return;
    double fps = swaps / std::chrono::duration<double>(elapsed).count();
    bool stalled = elapsed > kWindow * 3;  // paused, loading or in the background: not a measurement
    window_start = now;
    swaps = 0;
    if (warmup || stalled) {  // the first window after a switch includes the transition
        warmup = false;
        return;
    }
    low_windows = fps < kSteady ? low_windows + 1 : 0;
    if (fps < kBad || low_windows >= 2) {
        interp::set_enabled(false);
        // a try that fails quickly waits longer before the next one
        wait = now - tried_at < std::chrono::seconds(15) ? std::min<Clock::duration>(wait * 2, kMaxWait) : kMinWait;
        fallback_at = now;
        has_room = false;
        LOG("[fps60] %.1f fps: back to 30 fps (next try after %lld s with room for 60)", fps,
            (long long)std::chrono::duration_cast<std::chrono::seconds>(wait).count());
    }
}
}  // namespace fps60
