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

void set_thread_high_priority() {
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
