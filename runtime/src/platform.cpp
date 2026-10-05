// Host OS services: macOS (Mach) and Android/Linux implementations.
#include "platform.h"

#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <dirent.h>
#include <algorithm>
#include <atomic>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef __APPLE__
#include <execinfo.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread/qos.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unwind.h>
#include <dlfcn.h>
#endif
#ifdef __ANDROID__
#include <android/log.h>

#include <algorithm>
#include <mutex>
#include <vector>
#endif

namespace platform {

void thread_stack_bounds(uintptr_t& lo, uintptr_t& hi) {
    pthread_t self = pthread_self();
#ifdef __APPLE__
    hi = (uintptr_t)pthread_get_stackaddr_np(self);
    lo = hi - pthread_get_stacksize_np(self);
#else
    pthread_attr_t a;
    void* addr = nullptr;
    size_t size = 0;
    if (pthread_getattr_np(self, &a) == 0) {
        pthread_attr_getstack(&a, &addr, &size);
        pthread_attr_destroy(&a);
    }
    lo = (uintptr_t)addr;
    hi = lo + size;
#endif
}

void set_thread_name(const char* name) {
#ifdef __APPLE__
    pthread_setname_np(name);
#else
    char n[16];
    snprintf(n, sizeof n, "%s", name);
    pthread_setname_np(pthread_self(), n);
#endif
}

#ifdef __ANDROID__
// ---- Android Performance Hint API (ADPF, Android 13+; looked up at run time, the app supports 11+).
// The game, render and present threads form one session. Every game frame reports the CPU time of
// the busiest of them against a target below the frame interval, so the system raises CPU clocks
// before a frame runs late and lowers them when there is headroom, instead of reacting to load
// after the fact. WWHD_PERF_HINT=0 disables it, WWHD_PERF_HINT_MS sets the target (default 26).
namespace {
struct APerformanceHintManager;
struct APerformanceHintSession;
struct Adpf {
    APerformanceHintManager* (*getManager)();
    APerformanceHintSession* (*createSession)(APerformanceHintManager*, const int32_t*, size_t, int64_t);
    int (*reportActual)(APerformanceHintSession*, int64_t);
    int (*setThreads)(APerformanceHintSession*, const int32_t*, size_t);  // Android 14+, may be null
    APerformanceHintSession* session = nullptr;
    bool ok = false, failed = false;
    int64_t targetNs = 26000000;
};
Adpf g_adpf;
std::mutex g_hint_mu;
std::vector<int32_t> g_hint_tids;  // threads of the session, and those still to add
std::vector<uint64_t> g_hint_cpu;   // CPU time of each at the last frame, ns
bool g_hint_changed = false;

bool adpf_load() {
    if (g_adpf.ok || g_adpf.failed) return g_adpf.ok;
    g_adpf.failed = true;
    const char* env = getenv("WWHD_PERF_HINT");
    if (env && !strcmp(env, "0")) {
        log_line("[perf] performance hints off (WWHD_PERF_HINT=0)");
        return false;
    }
    if (const char* ms = getenv("WWHD_PERF_HINT_MS")) g_adpf.targetNs = (int64_t)(atof(ms) * 1e6);
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return false;
    g_adpf.getManager = (decltype(g_adpf.getManager))dlsym(lib, "APerformanceHint_getManager");
    g_adpf.createSession = (decltype(g_adpf.createSession))dlsym(lib, "APerformanceHint_createSession");
    g_adpf.reportActual = (decltype(g_adpf.reportActual))dlsym(lib, "APerformanceHint_reportActualWorkDuration");
    g_adpf.setThreads = (decltype(g_adpf.setThreads))dlsym(lib, "APerformanceHint_setThreads");
    if (!g_adpf.getManager || !g_adpf.createSession || !g_adpf.reportActual) {
        log_line("[perf] performance hints unavailable (Android 12 or older)");
        return false;
    }
    g_adpf.failed = false;
    g_adpf.ok = true;
    return true;
}

uint64_t tid_cpu_ns(int32_t tid) {
    // the kernel's per-thread CPU clock (CPUCLOCK_SCHED | per-thread); fails once the thread is gone
    clockid_t cid = (clockid_t)(((unsigned)~tid << 3) | 6);
    timespec ts;
    if (clock_gettime(cid, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
}  // namespace

void perf_hint_frame() {
    std::lock_guard<std::mutex> lk(g_hint_mu);
    if (!adpf_load() || g_hint_tids.empty()) return;
    // CPU time of each thread since the last frame; drop threads that have exited
    int64_t busiest = 0;
    for (size_t i = 0; i < g_hint_tids.size();) {
        uint64_t now = tid_cpu_ns(g_hint_tids[i]);
        if (!now) {
            g_hint_tids.erase(g_hint_tids.begin() + i);
            g_hint_cpu.erase(g_hint_cpu.begin() + i);
            g_hint_changed = true;
            continue;
        }
        if (g_hint_cpu[i]) busiest = std::max<int64_t>(busiest, (int64_t)(now - g_hint_cpu[i]));
        g_hint_cpu[i] = now;
        i++;
    }
    if (!g_adpf.session) {
        APerformanceHintManager* m = g_adpf.getManager();
        g_adpf.session = m ? g_adpf.createSession(m, g_hint_tids.data(), g_hint_tids.size(), g_adpf.targetNs) : nullptr;
        if (!g_adpf.session) {
            log_line("[perf] performance hint session refused");
            g_adpf.ok = false;
            g_adpf.failed = true;
            return;
        }
        char msg[96];
        snprintf(msg, sizeof msg, "[perf] performance hint session: %zu threads, target %.1f ms", g_hint_tids.size(),
                 g_adpf.targetNs / 1e6);
        log_line(msg);
        g_hint_changed = false;
        return;
    }
    if (g_hint_changed && g_adpf.setThreads) g_adpf.setThreads(g_adpf.session, g_hint_tids.data(), g_hint_tids.size());
    g_hint_changed = false;
    if (busiest > 0) g_adpf.reportActual(g_adpf.session, busiest);
    static const bool logIt = getenv("WWHD_PERF_LOG") != nullptr;
    static int64_t sum = 0, n = 0;
    if (!logIt) return;
    sum += busiest;
    if (++n == 300) {
        char msg[80];
        snprintf(msg, sizeof msg, "[perf] hint: busiest thread %.1f ms per frame (300 frames)", sum / 300 / 1e6);
        log_line(msg);
        sum = n = 0;
    }
}

static void perf_hint_add_thread() {
    std::lock_guard<std::mutex> lk(g_hint_mu);
    g_hint_tids.push_back((int32_t)syscall(SYS_gettid));
    g_hint_cpu.push_back(0);
    g_hint_changed = true;
}
#else
void perf_hint_frame() {}
#endif

void set_thread_high_priority() {
#ifdef __ANDROID__
    perf_hint_add_thread();
#endif
    if (getenv("WWHD_NO_QOS")) return;
#ifdef __APPLE__
    // keep guest threads on performance cores: the default QoS lets macOS park them on efficiency
    // cores, which showed up as the main thread holding its core without getting CPU time
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    // Android's THREAD_PRIORITY_URGENT_DISPLAY; the scheduler favours big cores for low nice values.
    // Fails quietly where the app may not raise priorities.
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), -8);
#endif
}

// Where the render and game threads run (the app's setting): 0 automatic (the system decides), 1 the
// performance cores (neither the efficiency cores nor the prime core: the best performance per
// watt, so the least heat), 2 the prime core for the render thread (the fastest single core, and
// the hottest), 3 the prime core for the game thread and the performance cores for the render thread
// (when the game's own logic is the slower of the two). Applied by the threads themselves when they
// call apply_thread_cores.
static std::atomic<int> g_core_mode{getenv("WWHD_CORE_MODE") ? atoi(getenv("WWHD_CORE_MODE")) : 0};
void set_core_mode(int m) { g_core_mode = std::clamp(m, 0, 3); }
int core_mode() { return g_core_mode; }

#if defined(__ANDROID__)
namespace {
struct CoreSets {
    cpu_set_t all, prime, perf;
    int nPrime = 0, nPerf = 0;
    bool clusters = false;  // more than one kind of core
};
// clusters by maximum clock: the highest is the prime core(s), the lowest the efficiency cores
const CoreSets& core_sets() {
    static CoreSets c = [] {
        CoreSets r;
        long freq[64] = {}, best = 0, low = 0;
        int n = 0;
        for (; n < 64; n++) {
            char path[96];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
            FILE* f = fopen(path, "r");
            if (!f) break;
            if (fscanf(f, "%ld", &freq[n]) != 1) freq[n] = 0;
            fclose(f);
            best = std::max(best, freq[n]);
            low = n == 0 ? freq[n] : std::min(low, freq[n]);
        }
        CPU_ZERO(&r.all);
        CPU_ZERO(&r.prime);
        CPU_ZERO(&r.perf);
        for (int i = 0; i < n; i++) {
            CPU_SET(i, &r.all);
            if (freq[i] == best) { CPU_SET(i, &r.prime); r.nPrime++; }
            else if (freq[i] != low) { CPU_SET(i, &r.perf); r.nPerf++; }
        }
        r.clusters = best > 0 && r.nPrime < n;
        if (r.nPerf == 0) { r.perf = r.prime; r.nPerf = r.nPrime; }  // two clusters: the big one
        return r;
    }();
    return c;
}
}  // namespace
#endif

void apply_thread_cores(bool render) {
#if defined(__ANDROID__)
    const CoreSets& c = core_sets();
    if (!c.clusters) return;
    int mode = g_core_mode.load();
    const cpu_set_t* want = &c.all;
    if (mode == 1) want = &c.perf;
    else if (mode == 2 && render) want = &c.prime;
    else if (mode == 3) want = render ? &c.perf : &c.prime;
    static thread_local int applied = -1;  // the mode this thread last applied
    cpu_set_t now;
    bool same = sched_getaffinity(0, sizeof now, &now) == 0 && CPU_EQUAL(&now, want);
    if (same && applied == mode) return;
    if (!same && sched_setaffinity(0, sizeof *want, want) != 0) return;
    if (applied != mode) {
        const char* where = want == &c.prime ? "the prime core" : want == &c.perf ? "the performance cores" : "every core";
        __android_log_print(ANDROID_LOG_INFO, "wwhd", "[platform] %s thread on %s", render ? "render" : "game", where);
    }
    applied = mode;
    if (want == &c.all) return;
    // threads created from the render thread (driver threads) inherit its affinity: every core back
    if (DIR* d = opendir("/proc/self/task")) {
        pid_t tid = (pid_t)syscall(SYS_gettid);
        while (dirent* e = readdir(d)) {
            pid_t t = (pid_t)atoi(e->d_name);
            cpu_set_t a;
            if (t <= 0 || t == tid || sched_getaffinity(t, sizeof a, &a) != 0 || !CPU_EQUAL(&a, want)) continue;
            sched_setaffinity(t, sizeof c.all, &c.all);
        }
        closedir(d);
    }
#endif
}

uint64_t thread_cpu_us(pthread_t t) {
#ifdef __APPLE__
    mach_port_t port = pthread_mach_thread_np(t);
    thread_basic_info_data_t info;
    mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
    if (thread_info(port, THREAD_BASIC_INFO, (thread_info_t)&info, &cnt) != KERN_SUCCESS) return 0;
    return (uint64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds +
           (uint64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
#else
    clockid_t cid;
    timespec ts;
    if (pthread_getcpuclockid(t, &cid) != 0 || clock_gettime(cid, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
#endif
}

bool map_fixed(void* addr, size_t size) {
#ifdef __APPLE__
    mach_vm_address_t a = (mach_vm_address_t)addr;
    return mach_vm_allocate(mach_task_self(), &a, size, VM_FLAGS_FIXED) == KERN_SUCCESS;
#else
    // MAP_FIXED_NOREPLACE fails instead of clobbering an existing mapping; kernels older than 4.17
    // treat it as a hint, so the result is checked as well. NORESERVE: only touched pages count.
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif
    void* p = mmap(addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0);
    if (p == MAP_FAILED) return false;
    if (p != addr) {
        munmap(p, size);
        return false;
    }
    return true;
#endif
}

void log_line(const char* line) {
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_INFO, "wwhd", line);
#endif
    fputs(line, stderr);
    fputc('\n', stderr);
}

#ifndef __APPLE__
namespace {
struct UnwindState {
    void** frames;
    int n, max;
};
_Unwind_Reason_Code unwind_cb(struct _Unwind_Context* ctx, void* arg) {
    auto* s = (UnwindState*)arg;
    uintptr_t pc = _Unwind_GetIP(ctx);
    if (pc && s->n < s->max) s->frames[s->n++] = (void*)pc;
    return s->n < s->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
}  // namespace
#endif

void print_backtrace() {
    void* frames[64];
#ifdef __APPLE__
    int nf = backtrace(frames, 64);
    backtrace_symbols_fd(frames, nf, 2);
#else
    UnwindState s{frames, 0, 64};
    _Unwind_Backtrace(unwind_cb, &s);
    for (int i = 0; i < s.n; i++) {
        Dl_info info{};
        char buf[512];
        int len;
        if (dladdr(frames[i], &info) && info.dli_fname)
            len = snprintf(buf, sizeof buf, "  #%02d %p %s+%#lx (%s)\n", i, frames[i], info.dli_sname ? info.dli_sname : "?",
                           (unsigned long)((uintptr_t)frames[i] - (uintptr_t)(info.dli_saddr ? info.dli_saddr : info.dli_fbase)),
                           info.dli_fname);
        else
            len = snprintf(buf, sizeof buf, "  #%02d %p\n", i, frames[i]);
#ifdef __ANDROID__
        __android_log_write(ANDROID_LOG_ERROR, "wwhd", buf);
#endif
        write(2, buf, len);
    }
#endif
}

}  // namespace platform
