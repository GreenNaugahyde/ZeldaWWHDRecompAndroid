// Host OS services: macOS (Mach) and Android/Linux implementations.
#include "platform.h"

#include <sys/mman.h>
#include <unistd.h>
#include <sched.h>
#include <dirent.h>
#include <algorithm>

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

#if defined(__ANDROID__)
// the cores whose maximum clock is the highest of all, and all cores; false if there is one cluster
static bool core_sets(cpu_set_t& fastest, cpu_set_t& all, int& count) {
    long best = 0, freq[64] = {};
    int n = 0;
    for (; n < 64; n++) {
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", n);
        FILE* f = fopen(path, "r");
        if (!f) break;
        if (fscanf(f, "%ld", &freq[n]) != 1) freq[n] = 0;
        fclose(f);
        best = std::max(best, freq[n]);
    }
    CPU_ZERO(&fastest);
    CPU_ZERO(&all);
    count = 0;
    for (int i = 0; i < n; i++) {
        CPU_SET(i, &all);
        if (freq[i] == best) {
            CPU_SET(i, &fastest);
            count++;
        }
    }
    return best > 0 && count < n;
}
#endif

void set_thread_fastest_cores() {
#if defined(__ANDROID__)
    if (getenv("WWHD_NO_PRIME_CORE")) return;
    static cpu_set_t fastest, all;
    static int count = 0;
    static const bool multi = core_sets(fastest, all, count);
    if (!multi) return;
    static pid_t self = 0;
    pid_t tid = (pid_t)syscall(SYS_gettid);
    cpu_set_t now;
    bool pinned = sched_getaffinity(0, sizeof now, &now) == 0 && CPU_EQUAL(&now, &fastest);
    if (!pinned) {
        if (sched_setaffinity(0, sizeof fastest, &fastest) != 0) return;
        if (self != tid)
            __android_log_print(ANDROID_LOG_INFO, "wwhd", "[platform] render thread on the %d fastest core(s)", count);
        else
            __android_log_print(ANDROID_LOG_INFO, "wwhd", "[platform] render thread affinity restored");
    }
    self = tid;
    // threads created from this one (driver threads) inherit the affinity: give them every core back
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            pid_t t = (pid_t)atoi(e->d_name);
            cpu_set_t a;
            if (t <= 0 || t == tid || sched_getaffinity(t, sizeof a, &a) != 0 || !CPU_EQUAL(&a, &fastest)) continue;
            sched_setaffinity(t, sizeof all, &all);
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
