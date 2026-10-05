// The app's 60 fps setting: frame interpolation (interp.cpp; game logic stays at 30 steps per
// second, every second frame is drawn halfway between two steps).
// Adaptive mode: interpolation only pays off at a steady 60. When the game can't hold it (the
// frame rate drops below 54 for a 2 s window), it falls back to 30, where it paces evenly, and tries
// 60 again later; each failed try doubles the wait (20 s up to 160 s), a steady minute resets it.
#include "fps60.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include "../runtime.h"

namespace interp {
bool interp_on();
void set_enabled(bool v);
}  // namespace interp

namespace fps60 {
namespace {
using Clock = std::chrono::steady_clock;
std::atomic<int> g_mode{0};
std::atomic<bool> g_reset{false};

constexpr auto kWindow = std::chrono::seconds(2);
constexpr auto kMinBackoff = std::chrono::seconds(20), kMaxBackoff = std::chrono::seconds(160);
constexpr double kDropBelow = 54.0;
}  // namespace

void set_mode(int m) {
    m = std::clamp(m, 0, 2);
    g_mode = m;
    g_reset = true;
    interp::set_enabled(m != 0);
}

int mode() { return g_mode; }

void on_swap() {
    static Clock::time_point window_start, retry_at, steady_since;
    static Clock::duration backoff = kMinBackoff;
    static uint32_t swaps = 0;
    static bool warmup = true;
    if (g_mode.load(std::memory_order_relaxed) != 2) return;
    auto now = Clock::now();
    if (g_reset.exchange(false)) {
        window_start = steady_since = now;
        swaps = 0;
        warmup = true;
        backoff = kMinBackoff;
        return;
    }
    if (!interp::interp_on()) {  // at 30 after a fallback: try 60 again once the wait is over
        if (now >= retry_at) {
            interp::set_enabled(true);
            LOG("[fps60] trying 60 fps again");
            window_start = steady_since = now;
            swaps = 0;
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
    if (fps < kDropBelow) {
        interp::set_enabled(false);
        retry_at = now + backoff;
        LOG("[fps60] %.1f fps: back to 30 fps, next try in %lld s", fps,
            (long long)std::chrono::duration_cast<std::chrono::seconds>(backoff).count());
        backoff = std::min<Clock::duration>(backoff * 2, kMaxBackoff);
        return;
    }
    if (now - steady_since > std::chrono::seconds(60)) backoff = kMinBackoff;
}
}  // namespace fps60
