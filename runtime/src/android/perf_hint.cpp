// ADPF performance hints (Android 13+): each frame reports how much CPU time the game and render
// threads needed against the frame budget, so the governor picks the lowest clocks that hold the
// frame rate instead of boosting blindly and throttling once the device is hot.
// The API is resolved at run time (the app supports Android 11). WWHD_NO_ADPF=1 disables it.
#include "perf_hint.h"

#include <dlfcn.h>
#include <pthread.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <ctime>

#include "../runtime.h"

namespace perf_hint {
namespace {
struct Manager;
struct Session;
Manager* (*p_getManager)();
Session* (*p_createSession)(Manager*, const int32_t*, size_t, int64_t);
int (*p_updateTarget)(Session*, int64_t);
int (*p_report)(Session*, int64_t);
void (*p_close)(Session*);

std::atomic<bool> g_render_set{false};
pthread_t g_render_thread;
std::atomic<int32_t> g_render_tid{0};

bool load() {
    if (getenv("WWHD_NO_ADPF")) return false;
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    p_getManager = (decltype(p_getManager))dlsym(lib, "APerformanceHint_getManager");
    p_createSession = (decltype(p_createSession))dlsym(lib, "APerformanceHint_createSession");
    p_updateTarget = (decltype(p_updateTarget))dlsym(lib, "APerformanceHint_updateTargetWorkDuration");
    p_report = (decltype(p_report))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    p_close = (decltype(p_close))dlsym(lib, "APerformanceHint_closeSession");
    return p_getManager && p_createSession && p_updateTarget && p_report && p_close;
}

int64_t cpu_ns(clockid_t clk) {
    timespec ts{};
    if (clock_gettime(clk, &ts) != 0) return 0;
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}
}  // namespace

void register_render_thread() {
    g_render_thread = pthread_self();
    g_render_tid = (int32_t)syscall(SYS_gettid);
    g_render_set = true;
}

void on_swap(uint32_t swap_interval) {
    static const bool available = load();
    if (!available) return;
    static Manager* mgr = p_getManager();
    static Session* session = nullptr;
    static int32_t session_game = 0, session_render = 0;
    static int64_t target = 0, last_game = 0, last_render = 0;
    static bool failed = false;
    if (!mgr || failed) return;

    const int32_t game = (int32_t)syscall(SYS_gettid);
    const int32_t render = g_render_tid;
    int64_t want = (int64_t)std::max<uint32_t>(swap_interval, 1) * 16683333;  // vsyncs at 59.94 Hz
    if (!session || game != session_game || render != session_render) {
        if (session) p_close(session);
        int32_t tids[2] = {game, render};
        session = p_createSession(mgr, tids, render && render != game ? 2 : 1, want);
        if (!session) {
            failed = true;
            LOG("[adpf] no performance hint session");
            return;
        }
        LOG("[adpf] session for game thread %d, render thread %d, target %.1f ms", game, render, want / 1e6);
        session_game = game;
        session_render = render;
        target = want;
        last_game = last_render = 0;
    }
    if (want != target) {
        p_updateTarget(session, want);
        target = want;
    }

    // the busier of the two threads is the frame's critical path; their CPU time excludes waits
    int64_t g = cpu_ns(CLOCK_THREAD_CPUTIME_ID), r = 0;
    clockid_t rclk;
    if (g_render_set && pthread_getcpuclockid(g_render_thread, &rclk) == 0) r = cpu_ns(rclk);
    if (last_game) {
        int64_t work = std::max(g - last_game, last_render ? r - last_render : 0);
        if (work > 0) p_report(session, work);
    }
    last_game = g;
    last_render = r;
}
}  // namespace perf_hint
