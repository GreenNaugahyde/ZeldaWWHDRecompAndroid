// Display vsync timestamps from AChoreographer, fed to the guest vsync clock (gx2_core.cpp).
// The refresh period is measured from the timestamps: the median of the last intervals (a missed
// frame or a late callback only adds outliers), snapped to the panel's nominal rate when it is
// within 0.6% of one, so the guest clock runs at exactly 60 Hz instead of following the jitter.
#include "display_vsync.h"

#include <android/choreographer.h>
#include <android/looper.h>

#include <algorithm>
#include <cstdlib>
#include <cstdint>
#include <mutex>
#include <thread>

#include "../platform.h"

void gx2_display_vsync(int64_t vsync_ns, int64_t display_period);

namespace display_vsync {
namespace {
int64_t g_last = 0, g_period = 0;
int64_t g_iv[32];
int g_n = 0, g_count = 0;

int64_t measure() {
    int64_t v[32];
    int n = 0;
    for (int i = 0; i < g_n; i++)
        if (g_iv[i] > 2000000 && g_iv[i] < 50000000) v[n++] = g_iv[i];
    if (n < 8) return 0;
    std::nth_element(v, v + n / 2, v + n);
    int64_t med = v[n / 2];
    for (double hz : {30.0, 60.0, 90.0, 120.0, 144.0}) {
        int64_t nominal = (int64_t)(1e9 / hz + 0.5);
        if (std::abs(med - nominal) * 1000 < nominal * 6) return nominal;
    }
    return med;
}

void on_frame(int64_t frame_ns, void* data) {
    auto* ch = (AChoreographer*)data;
    if (g_last) {
        g_iv[g_count % 32] = frame_ns - g_last;
        g_n = std::min(g_n + 1, 32);
        if (++g_count % 30 == 0)  // a new measurement every 30 frames follows refresh rate changes
            if (int64_t p = measure()) g_period = p;
    }
    g_last = frame_ns;
    if (g_period) gx2_display_vsync(frame_ns, g_period);
    AChoreographer_postFrameCallback64(ch, on_frame, ch);
}
}  // namespace

void start() {
    static std::once_flag once;
    std::call_once(once, [] {
        std::thread([] {
            platform::set_thread_name("display vsync");
            ALooper_prepare(0);
            AChoreographer* ch = AChoreographer_getInstance();
            if (!ch) return;
            AChoreographer_postFrameCallback64(ch, on_frame, ch);
            for (;;) ALooper_pollOnce(-1, nullptr, nullptr, nullptr);
        }).detach();
    });
}
}  // namespace display_vsync
