// Display vsync timestamps from AChoreographer, fed to the guest vsync clock (gx2_core.cpp).
// The refresh period is measured from the timestamps: the shortest interval of the last frames
// (frames the thread misses only make intervals longer).
#include "display_vsync.h"

#include <android/choreographer.h>
#include <android/looper.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <thread>

#include "../platform.h"

void gx2_display_vsync(int64_t vsync_ns, int64_t display_period);

namespace display_vsync {
namespace {
int64_t g_last = 0, g_min = 0, g_period = 0;
int g_count = 0;

void on_frame(int64_t frame_ns, void* data) {
    auto* ch = (AChoreographer*)data;
    if (g_last) {
        int64_t d = frame_ns - g_last;
        if (d > 2000000 && (!g_min || d < g_min)) g_min = d;  // ignore bogus intervals under 2 ms
    }
    g_last = frame_ns;
    if (++g_count >= 30) {  // a new measurement every 30 frames follows refresh rate changes
        if (g_min) g_period = g_min;
        g_count = 0;
        g_min = 0;
    }
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
