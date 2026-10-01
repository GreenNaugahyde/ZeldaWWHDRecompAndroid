// Guest threads run on host pthreads. OS synchronization objects live in guest
// memory (the game allocates them) and are backed by host objects keyed by
// their guest address.
#include <mach/mach.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "runtime.h"

// ---------------------------------------------------------------- time
namespace timebase {
static const auto g_boot = std::chrono::steady_clock::now();
uint64_t now() {
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - g_boot).count();
    return (uint64_t)((unsigned __int128)ns * kTicksPerSec / 1000000000ull);
}
}  // namespace timebase

extern "C" uint64_t ppc_timebase(void) { return timebase::now(); }

static std::chrono::nanoseconds ticks_to_ns(uint64_t t) {
    return std::chrono::nanoseconds((int64_t)((unsigned __int128)t * 1000000000ull / timebase::kTicksPerSec));
}

// ---------------------------------------------------------------- OSThread
// Offsets into the guest OSThread structure (see Cemu coreinit_Thread.h)
namespace osthread {
constexpr uint32_t kSize = 0x6A0;
constexpr uint32_t kState = 0x324;
constexpr uint32_t kAttr = 0x325;
constexpr uint32_t kId = 0x326;
constexpr uint32_t kSuspend = 0x328;
constexpr uint32_t kEffPrio = 0x32C;
constexpr uint32_t kBasePrio = 0x330;
constexpr uint32_t kExitValue = 0x334;
constexpr uint32_t kStackBase = 0x394;
constexpr uint32_t kStackEnd = 0x398;
constexpr uint32_t kEntry = 0x39C;
constexpr uint32_t kSpecific = 0x57C;
constexpr uint32_t kName = 0x5C0;
constexpr uint32_t kAffinity = 0x304;
enum State : uint8_t { NONE = 0, READY = 1, RUNNING = 2, WAITING = 4, MORIBUND = 8 };
}  // namespace osthread

struct HostThread {
    uint32_t guest = 0;  // OSThread*
    Cpu cpu{};
    std::mutex m;
    std::condition_variable cv;
    int suspend = 1;
    bool started = false;
    bool exited = false;
    uint32_t exit_value = 0;
    uint32_t entry = 0, argc = 0, argv = 0;
    uint32_t core = 1;  // emulated core this thread is bound to (for OSGetCoreId)
    pthread_t pt{};
    // scheduling
    int prio = 16;            // lower runs first; service threads (alarms, audio) use -1
    bool holds_core = false;
    uint32_t held_core = 0;   // core actually held (affinity may change while holding)
    int irq_off = 0;          // OSDisableInterrupts: no preemption
    std::chrono::steady_clock::time_point ready_since;  // when it started waiting for its core
    bool service = false;
    // statistics
    std::chrono::steady_clock::time_point acquired;
    std::atomic<uint64_t> held_ns{0}, wait_ns{0};
    mach_port_t mach = 0;     // for CPU time statistics
    uint64_t last_cpu_us = 0;
};

// core from an affinity mask (bit0 = core 0, bit1 = core 1, bit2 = core 2); fallback if none set
static uint32_t core_from_affinity(uint32_t mask, uint32_t fallback) {
    mask &= 7;
    if (!mask || mask == 7) return fallback;
    return (uint32_t)__builtin_ctz(mask);
}

bool g_trace_msg = getenv("WWHD_TRACE_MSG") != nullptr;
static std::mutex g_threads_mutex;
static std::unordered_map<uint32_t, HostThread*> g_threads;  // by guest OSThread*
static thread_local HostThread* t_self = nullptr;
static thread_local Cpu* t_cpu = nullptr;
static uint32_t g_sda_base, g_sda2_base;
static uint16_t g_next_id = 1;

namespace threads {
Cpu* current() { return t_cpu; }
uint32_t current_thread() { return t_self ? t_self->guest : 0; }
}  // namespace threads

// ---------------------------------------------------------------- per-core scheduling
// Each emulated core runs one guest thread at a time; a ready thread with a higher priority than
// the running one sets g_core_preempt, and the running thread yields at its next function entry.
// WWHD_NO_SCHED=1 lets all threads run freely (the old behaviour).
volatile int g_core_preempt[3];
static const bool g_sched_on = getenv("WWHD_NO_SCHED") == nullptr;

struct CoreSched {
    std::mutex m;
    std::condition_variable cv;
    HostThread* owner = nullptr;
    std::deque<HostThread*> ready;  // FIFO among equal priorities
};
static CoreSched g_sched[3];

static HostThread* best_ready(CoreSched& k) {  // k.m held
    HostThread* b = nullptr;
    for (HostThread* t : k.ready)  // priority, then FIFO
        if (!b || t->prio < b->prio) b = t;
    return b;
}

static void sched_tick_thread();
static void mem_watch_thread();

static void core_acquire(HostThread* t) {
    if (!g_sched_on || t->holds_core) return;
    static std::once_flag tick;
    std::call_once(tick, [] {
        std::thread(sched_tick_thread).detach();
        if (getenv("WWHD_WATCH_MEM")) std::thread(mem_watch_thread).detach();
    });
    uint32_t core = t->core;
    CoreSched& k = g_sched[core];
    std::unique_lock<std::mutex> lk(k.m);
    k.ready.push_back(t);
    if (k.owner && t->prio < k.owner->prio) g_core_preempt[core] = 1;
    auto start = std::chrono::steady_clock::now();
    t->acquired = start;
    t->ready_since = start;
    bool warned = false;
    while (k.owner || best_ready(k) != t) {
        k.cv.wait_for(lk, std::chrono::seconds(2));
        if (!warned && std::chrono::steady_clock::now() - start > std::chrono::seconds(10)) {
            warned = true;
            std::string a = mem::read_cstr(ld32(t->guest + osthread::kName));
            std::string b = k.owner ? mem::read_cstr(ld32(k.owner->guest + osthread::kName)) : "-";
            LOG("[sched] \"%s\" (prio %d) waiting >10s for core %u held by \"%s\" (prio %d, pc %08X)", a.c_str(), t->prio, core,
                b.c_str(), k.owner ? k.owner->prio : 0, k.owner ? k.owner->cpu.lr : 0);
        }
    }
    for (auto it = k.ready.begin(); it != k.ready.end(); ++it)
        if (*it == t) { k.ready.erase(it); break; }
    k.owner = t;
    t->holds_core = true;
    t->held_core = core;
    auto now = std::chrono::steady_clock::now();
    t->wait_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count();
    t->acquired = now;
    t->cpu.core = core;
    HostThread* next = best_ready(k);
    g_core_preempt[core] = next && next->prio < t->prio;
}

static void core_release(HostThread* t) {
    if (!g_sched_on || !t->holds_core) return;
    CoreSched& k = g_sched[t->held_core];
    {
        std::lock_guard<std::mutex> lk(k.m);
        if (k.owner == t) k.owner = nullptr;
        t->holds_core = false;
        t->held_ns += (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t->acquired).count();
        g_core_preempt[t->held_core] = 0;
    }
    k.cv.notify_all();
}

// Like Cafe OS (and Cemu): strict priority, and equal-priority threads round-robin every time slice
// (games busy-wait on other threads of the same core and priority). Lower priorities never preempt.
static constexpr auto kSlice = std::chrono::microseconds(500);

static bool should_yield(CoreSched& k, HostThread* t) {  // k.m held
    HostThread* next = best_ready(k);
    if (!next) return false;
    if (next->prio < t->prio) return true;
    return next->prio == t->prio && std::chrono::steady_clock::now() - t->acquired >= kSlice;
}

extern "C" void ppc_preempt(Cpu* c) {
    HostThread* t = (HostThread*)c->thread;
    if (!t || !t->holds_core || t->irq_off) return;
    {
        CoreSched& k = g_sched[t->held_core];
        std::lock_guard<std::mutex> lk(k.m);
        g_core_preempt[t->held_core] = 0;
        if (!should_yield(k, t)) return;
    }
    core_release(t);
    core_acquire(t);
}

// scheduler tick: requests time-slice / starvation preemption on cores that need it
namespace threads { void report_sched(); }

// debug: WWHD_WATCH_MEM=addr|*ptr+off[,...] polls guest words and logs every change together with
// where each guest thread is (lr), to find who writes a flag
static void mem_watch_thread() {
    pthread_setname_np("mem watch");
    struct W { bool deref; uint32_t a, off; uint32_t last; bool init = false; };
    std::vector<W> ws;
    const char* e = getenv("WWHD_WATCH_MEM");
    while (e && *e) {
        W w{};
        if (*e == '*') { w.deref = true; e++; }
        char* p;
        w.a = (uint32_t)strtoul(e, &p, 16);
        if (*p == '+') w.off = (uint32_t)strtoul(p + 1, &p, 16);
        ws.push_back(w);
        e = *p == ',' ? p + 1 : p;
        if (*p != ',') break;
    }
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        for (auto& w : ws) {
            uint32_t addr = w.deref ? ld32(w.a) + w.off : w.a + w.off;
            if (!addr || (w.deref && ld32(w.a) == 0)) continue;
            uint32_t v = ld32(addr);
            if (w.init && v == w.last) continue;
            std::string who;
            {
                std::lock_guard<std::mutex> lk(g_threads_mutex);
                for (auto& [g, t] : g_threads) {
                    char b[96];
                    snprintf(b, sizeof b, " %s@%08X", mem::read_cstr(ld32(t->guest + osthread::kName)).c_str(), t->cpu.lr);
                    who += b;
                }
            }
            LOG("[memwatch] t=%.2fs %08X: %08X -> %08X |%s", timebase::now() / (double)timebase::kTicksPerSec, addr, w.last, v,
                who.c_str());
            w.last = v;
            w.init = true;
        }
    }
}
static void sched_tick_thread() {
    pthread_setname_np("sched tick");
    static const bool timed_stats = getenv("WWHD_SCHED_STATS") && atoi(getenv("WWHD_SCHED_STATS")) == 2;
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::microseconds(500));
        if (timed_stats && std::chrono::steady_clock::now() >= next_report) {
            next_report += std::chrono::seconds(5);
            threads::report_sched();
        }
        for (int core = 0; core < 3; core++) {
            CoreSched& k = g_sched[core];
            std::lock_guard<std::mutex> lk(k.m);
            if (k.owner && !k.ready.empty() && should_yield(k, k.owner)) g_core_preempt[core] = 1;
        }
    }
}

// debug: WWHD_LOG_LONGWAIT=1 reports blocking calls that took longer than 2 s (who waited, from where)
static const bool g_log_longwait = getenv("WWHD_LOG_LONGWAIT") != nullptr;
static thread_local std::chrono::steady_clock::time_point t_block_start;

namespace threads {
// share of wall time each thread held / waited for its core since the last report
void report_sched() {
    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    double span = std::chrono::duration<double, std::nano>(now - last).count();
    last = now;
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    std::string out;
    for (auto& [g, t] : g_threads) {
        uint64_t h = t->held_ns.exchange(0), w = t->wait_ns.exchange(0);
        if (h < span * 0.02 && w < span * 0.02) continue;
        std::string name = mem::read_cstr(ld32(t->guest + osthread::kName));
        // actual CPU time the host gave this thread (vs. time it held its emulated core)
        double cpu = 0;
        if (t->mach) {
            thread_basic_info_data_t info;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mach, THREAD_BASIC_INFO, (thread_info_t)&info, &cnt) == KERN_SUCCESS) {
                uint64_t us = (uint64_t)info.user_time.seconds * 1000000 + info.user_time.microseconds +
                              (uint64_t)info.system_time.seconds * 1000000 + info.system_time.microseconds;
                cpu = 100.0 * (us - t->last_cpu_us) * 1000.0 / span;
                t->last_cpu_us = us;
            }
        }
        char buf[192];
        snprintf(buf, sizeof buf, " [%s c%u p%d run %.0f%% cpu %.0f%% wait %.0f%%]", name.empty() ? "main" : name.c_str(), t->core,
                 t->prio, 100.0 * h / span, cpu, 100.0 * w / span);
        out += buf;
    }
    LOG("[sched]%s", out.c_str());
}
void block_begin() {
    if (g_log_longwait) t_block_start = std::chrono::steady_clock::now();
    if (t_self) core_release(t_self);
}
void block_end() {
    if (g_log_longwait && t_self) {
        double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_block_start).count();
        if (sec > 2.0)
            LOG("[longwait] \"%s\" blocked %.1f s (lr %08X)", mem::read_cstr(ld32(t_self->guest + osthread::kName)).c_str(), sec,
                t_self->cpu.lr);
    }
    if (t_self) core_acquire(t_self);
}
bool ensure_core() {
    if (!t_self || t_self->holds_core || !g_sched_on) return false;
    core_acquire(t_self);
    return true;
}
void release_core() { if (t_self) core_release(t_self); }
void set_service_core(uint32_t core) { if (t_self) t_self->core = t_self->cpu.core = core; }
}  // namespace threads

static HostThread* host_thread(uint32_t t) {
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    auto it = g_threads.find(t);
    return it == g_threads.end() ? nullptr : it->second;
}

struct GuestExit {
    uint32_t value;
};

static void* thread_main(void* p) {
    HostThread* ht = (HostThread*)p;
    t_self = ht;
    t_cpu = &ht->cpu;
    std::string name = mem::read_cstr(ld32(ht->guest + osthread::kName));
    pthread_setname_np(name.empty() ? "guest" : name.c_str());
    ht->mach = pthread_mach_thread_np(pthread_self());
    // keep guest threads on performance cores: the default QoS lets macOS park them on efficiency
    // cores, which showed up as the main thread holding its core without getting CPU time
    if (!getenv("WWHD_NO_QOS")) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    LOG("[thread] start \"%s\" core %d prio %d affinity %X", name.c_str(), ht->core, (int)ld32(ht->guest + osthread::kBasePrio),
        ld32(ht->guest + osthread::kAffinity));
    uint32_t rv = 0;
    core_acquire(ht);
    try {
        rv = guest_call(&ht->cpu, ht->entry, {ht->argc, ht->argv});
    } catch (const GuestExit& e) {
        rv = e.value;
    }
    core_release(ht);
    {
        std::lock_guard<std::mutex> lk(ht->m);
        ht->exited = true;
        ht->exit_value = rv;
        st32(ht->guest + osthread::kExitValue, rv);
        st8(ht->guest + osthread::kState, osthread::MORIBUND);
    }
    ht->cv.notify_all();
    TRACE("[thread] %s exited with %08X", name.c_str(), rv);
    return nullptr;
}

static void start_host_thread(HostThread* ht) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64 << 20);  // deep guest call chains recurse on the host stack
    pthread_create(&ht->pt, &attr, thread_main, ht);
    pthread_attr_destroy(&attr);
}

static void init_cpu(Cpu& c, uint32_t stack_top) {
    memset(&c, 0, sizeof(c));
    c.r[1] = (stack_top - 0x20) & ~0xFu;
    c.r[2] = g_sda2_base;
    c.r[13] = g_sda_base;
    // coreinit defaults for new threads: GQR2-5 quantize as u8, u16, s8, s16; FPSCR.NI set
    c.gqr[2] = 0x40004;
    c.gqr[3] = 0x50005;
    c.gqr[4] = 0x60006;
    c.gqr[5] = 0x70007;
    c.fpscr = 4;
}

static HostThread* create_thread(uint32_t t, uint32_t entry, uint32_t argc, uint32_t argv, uint32_t stack_top,
                                 uint32_t stack_size, int prio, uint32_t attr) {
    auto* ht = new HostThread();
    ht->guest = t;
    ht->entry = entry;
    ht->argc = argc;
    ht->argv = argv;
    memset(mem::ptr(t), 0, osthread::kSize);
    st32(t + 0, 0x4F53436F);  // "OSContxt" tag
    st32(t + 4, 0x6E747874);
    st8(t + osthread::kState, osthread::READY);
    st8(t + osthread::kAttr, attr);
    st16(t + osthread::kId, g_next_id++);
    st32(t + osthread::kSuspend, 1);
    st32(t + osthread::kEffPrio, prio);
    st32(t + osthread::kBasePrio, prio);
    st32(t + osthread::kStackBase, stack_top);
    st32(t + osthread::kStackEnd, stack_top - stack_size);
    st32(t + osthread::kEntry, entry);
    st32(t + osthread::kAffinity, attr & 7);
    init_cpu(ht->cpu, stack_top);
    ht->cpu.thread = ht;
    ht->core = core_from_affinity(attr, t_self ? t_self->core : 1);
    ht->cpu.core = ht->core;
    ht->prio = prio;
    std::lock_guard<std::mutex> lk(g_threads_mutex);
    g_threads[t] = ht;
    return ht;
}

namespace threads {
void init(const LoadedModule& m) {
    g_sda_base = m.sda_base;
    g_sda2_base = m.sda2_base;
}

void run_main(const LoadedModule& m, int argc, uint32_t argv) {
    uint32_t stack_size = std::max<uint32_t>(m.stack_size, 0x100000);
    uint32_t stack = mem::runtime_alloc(stack_size, 0x100);
    uint32_t t = mem::runtime_alloc(osthread::kSize, 8);
    HostThread* ht = create_thread(t, m.entry, argc, argv, stack + stack_size, stack_size, 16, 0x2 /* core 1 */);
    mem::write_cstr(mem::runtime_alloc(16), "{ Main thread }", 16);
    ht->suspend = 0;
    ht->started = true;
    start_host_thread(ht);
    pthread_join(ht->pt, nullptr);
}

Cpu* make_service_cpu(const char* name, uint32_t stack_size) {
    uint32_t stack = mem::runtime_alloc(stack_size, 0x100);
    uint32_t t = mem::runtime_alloc(osthread::kSize, 8);
    HostThread* ht = create_thread(t, 0, 0, 0, stack + stack_size, stack_size, 0, 0x7);
    ht->prio = -1;  // interrupt-like: preempts guest threads
    ht->service = true;
    uint32_t nm = mem::runtime_alloc((uint32_t)strlen(name) + 1);
    mem::write_cstr(nm, name, (uint32_t)strlen(name) + 1);
    st32(t + osthread::kName, nm);
    t_self = ht;
    t_cpu = &ht->cpu;
    if (!getenv("WWHD_NO_QOS")) pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    return &ht->cpu;
}
}  // namespace threads

// ---------------------------------------------------------------- thread API
HLE(coreinit, OSCreateThread) {
    uint32_t t = arg(c, 0), entry = arg(c, 1), argc = arg(c, 2), argv = arg(c, 3), stack = arg(c, 4),
             stack_size = arg(c, 5), prio = arg(c, 6), attr = arg(c, 7);
    TRACE("[thread] OSCreateThread(%08X, entry=%08X, stack=%08X+%X, prio=%d, attr=%X)", t, entry, stack, stack_size,
          prio, attr);
    create_thread(t, entry, argc, argv, stack, stack_size, (int)prio, attr);
    ret(c, 1);
}

HLE(coreinit, OSResumeThread) {
    HostThread* ht = host_thread(arg(c, 0));
    if (!ht) { ret(c, 0); return; }
    int prev;
    bool start = false;
    {
        std::lock_guard<std::mutex> lk(ht->m);
        prev = ht->suspend;
        if (ht->suspend > 0 && --ht->suspend == 0 && !ht->started) {
            ht->started = true;
            start = true;
        }
        st32(ht->guest + osthread::kSuspend, ht->suspend);
    }
    if (start) start_host_thread(ht);
    ht->cv.notify_all();
    ret(c, prev);
}

HLE(coreinit, OSExitThread) {
    throw GuestExit{arg(c, 0)};
}

HLE(coreinit, OSJoinThread) {
    HostThread* ht = host_thread(arg(c, 0));
    if (!ht) { ret(c, 0); return; }
    {
        BlockingScope b;
        std::unique_lock<std::mutex> lk(ht->m);
        ht->cv.wait(lk, [&] { return ht->exited; });
    }
    if (arg(c, 1)) st32(arg(c, 1), ht->exit_value);
    ret(c, 1);
}

HLE(coreinit, OSGetCurrentThread) { ret(c, threads::current_thread()); }
HLE(coreinit, OSGetCoreId) { ret(c, t_self ? t_self->core : 1); }
HLE(coreinit, OSYieldThread) {
    // let other ready threads of the same (or higher) priority on this core run
    BlockingScope b;
    sched_yield();
}
HLE(coreinit, OSSleepTicks) {
    BlockingScope b;
    std::this_thread::sleep_for(ticks_to_ns(arg64(c, 3)));
}
HLE(coreinit, OSSetThreadName) { st32(arg(c, 0) + osthread::kName, arg(c, 1)); }
HLE(coreinit, OSSetThreadAffinity) {
    st32(arg(c, 0) + osthread::kAffinity, arg(c, 1));
    if (HostThread* ht = host_thread(arg(c, 0))) ht->core = core_from_affinity(arg(c, 1), ht->core);  // applies at next schedule
    ret(c, 1);
}
HLE(coreinit, OSGetThreadPriority) { ret(c, ld32(arg(c, 0) + osthread::kBasePrio)); }
HLE(coreinit, OSSetThreadSpecific) { st32(threads::current_thread() + osthread::kSpecific + 4 * arg(c, 0), arg(c, 1)); }
HLE(coreinit, OSGetThreadSpecific) { ret(c, ld32(threads::current_thread() + osthread::kSpecific + 4 * arg(c, 0))); }
HLE(coreinit, OSBlockThreadsOnExit) {}
HLE(coreinit, OSSetExceptionCallback) { ret(c, 0); }
// interrupts off = no rescheduling on this core
HLE(coreinit, OSDisableInterrupts) {
    int prev = t_self ? (t_self->irq_off ? 0 : 1) : 1;
    if (t_self) t_self->irq_off = 1;
    ret(c, prev);
}
HLE(coreinit, OSRestoreInterrupts) {
    int prev = t_self ? (t_self->irq_off ? 0 : 1) : 1;
    if (t_self && arg(c, 0)) {
        t_self->irq_off = 0;
        if (t_self->holds_core && g_core_preempt[t_self->held_core]) ppc_preempt(c);
    }
    ret(c, prev);
}
HLE(coreinit, OSMemoryBarrier) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }

// wait on a host condition with the core given up; never re-takes the core while holding `lk`
template <class Pred>
static void wait_blocking(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, Pred pred) {
    while (!pred()) {
        lk.unlock();
        threads::block_begin();
        lk.lock();
        cv.wait(lk, pred);
        lk.unlock();
        threads::block_end();
        lk.lock();
    }
}

// ---------------------------------------------------------------- host objects
template <typename T>
struct ObjTable {
    std::mutex m;
    std::unordered_map<uint32_t, T*> map;
    T* get(uint32_t addr) {
        std::lock_guard<std::mutex> lk(m);
        T*& p = map[addr];
        if (!p) p = new T();
        return p;
    }
    void reset(uint32_t addr) {
        std::lock_guard<std::mutex> lk(m);
        T*& p = map[addr];
        delete p;
        p = new T();
    }
};

// OSMutex: recursive, owned by a thread
struct HMutex {
    std::mutex m;
    std::condition_variable cv;
    const void* owner = nullptr;
    int count = 0;
};
static ObjTable<HMutex> g_mutexes;

static void mutex_lock(uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    const void* self = t_self ? (const void*)t_self : (const void*)&t_cpu;
    {
        std::lock_guard<std::mutex> lk(mx->m);
        if (mx->owner == self) { mx->count++; return; }
        if (!mx->owner) { mx->owner = self; mx->count = 1; return; }
    }
    BlockingScope b;
    std::unique_lock<std::mutex> lk(mx->m);
    mx->cv.wait(lk, [&] { return mx->owner == nullptr; });
    mx->owner = self;
    mx->count = 1;
}
static bool mutex_trylock(uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    const void* self = t_self ? (const void*)t_self : (const void*)&t_cpu;
    std::lock_guard<std::mutex> lk(mx->m);
    if (mx->owner == self) { mx->count++; return true; }
    if (mx->owner) return false;
    mx->owner = self;
    mx->count = 1;
    return true;
}
static void mutex_unlock(uint32_t addr) {
    HMutex* mx = g_mutexes.get(addr);
    {
        std::lock_guard<std::mutex> lk(mx->m);
        if (--mx->count > 0) return;
        mx->owner = nullptr;
        mx->count = 0;
    }
    mx->cv.notify_one();
}

// Init functions reinitialize the host object in place: replacing it would strand threads already
// waiting on the old one (a waiter can get there first, or an object is re-initialized while in use)
HLE(coreinit, OSInitMutex) {
    HMutex* mx = g_mutexes.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(mx->m);
        mx->owner = nullptr;
        mx->count = 0;
    }
    mx->cv.notify_all();
    st32(arg(c, 0), 0x6D557458);  // "mUtX"
}
HLE(coreinit, OSLockMutex) { mutex_lock(arg(c, 0)); }
HLE(coreinit, OSTryLockMutex) { ret(c, mutex_trylock(arg(c, 0))); }
HLE(coreinit, OSUnlockMutex) { mutex_unlock(arg(c, 0)); }

// GHS C library locks
static uint32_t g_ghs_lock = 0xC0FFEE00;
HLE(coreinit, __ghsLock) { mutex_lock(g_ghs_lock); }
HLE(coreinit, __ghsUnlock) { mutex_unlock(g_ghs_lock); }
HLE(coreinit, __ghs_mtx_init) { /* arg: void** handle */ st32(arg(c, 0), mem::runtime_alloc(8)); }
HLE(coreinit, __ghs_mtx_dst) {}
HLE(coreinit, __ghs_mtx_lock) { mutex_lock(ld32(arg(c, 0))); }
HLE(coreinit, __ghs_mtx_unlock) { mutex_unlock(ld32(arg(c, 0))); }
HLE(coreinit, __ghs_flock_file) { mutex_lock(0xC0FFEE10); }
HLE(coreinit, __ghs_funlock_file) { mutex_unlock(0xC0FFEE10); }
HLE(coreinit, __ghs_flock_ptr) { ret(c, mem::runtime_alloc(4)); }
HLE(coreinit, __ghs_flock_destroy) {}

// OSEvent
struct HEvent {
    std::mutex m;
    std::condition_variable cv;
    bool signaled = false;
    bool auto_reset = false;
};
static ObjTable<HEvent> g_events;

HLE(coreinit, OSInitEvent) {
    uint32_t e = arg(c, 0);
    if (g_trace_msg) LOG("[evt] init %08X signaled=%u auto=%u lr=%08X", e, arg(c, 1), arg(c, 2), c->lr);
    HEvent* ev = g_events.get(e);
    {
        std::lock_guard<std::mutex> lk(ev->m);
        ev->signaled = arg(c, 1) != 0;
        ev->auto_reset = arg(c, 2) != 0;  // OS_EVENT_MODE_AUTO = 1
    }
    if (ev->signaled) ev->cv.notify_all();
    st32(e, 0x65566E54);              // "eVnT"
}
HLE(coreinit, OSSignalEvent) {
    if (g_trace_msg) LOG("[evt] signal %08X lr=%08X thread=%08X", arg(c, 0), c->lr, threads::current_thread());
    HEvent* ev = g_events.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(ev->m);
        ev->signaled = true;
    }
    if (ev->auto_reset) ev->cv.notify_one(); else ev->cv.notify_all();
}
HLE(coreinit, OSResetEvent) {
    if (g_trace_msg) LOG("[evt] reset %08X lr=%08X", arg(c, 0), c->lr);
    HEvent* ev = g_events.get(arg(c, 0));
    std::lock_guard<std::mutex> lk(ev->m);
    ev->signaled = false;
}
HLE(coreinit, OSWaitEvent) {
    if (g_trace_msg) LOG("[evt] wait %08X lr=%08X thread=%08X", arg(c, 0), c->lr, threads::current_thread());
    HEvent* ev = g_events.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(ev->m);
        if (ev->signaled) { if (ev->auto_reset) ev->signaled = false; return; }
    }
    BlockingScope b;
    std::unique_lock<std::mutex> lk(ev->m);
    ev->cv.wait(lk, [&] { return ev->signaled; });
    if (ev->auto_reset) ev->signaled = false;
}
HLE(coreinit, OSWaitEventWithTimeout) {
    HEvent* ev = g_events.get(arg(c, 0));
    {
        std::lock_guard<std::mutex> lk(ev->m);
        if (ev->signaled) { if (ev->auto_reset) ev->signaled = false; ret(c, 1); return; }
    }
    bool ok;
    {
        BlockingScope b;
        std::unique_lock<std::mutex> lk(ev->m);
        ok = ev->cv.wait_for(lk, ticks_to_ns(arg64(c, 5)), [&] { return ev->signaled; });
        if (ok && ev->auto_reset) ev->signaled = false;
    }
    ret(c, ok);
}

// OSMessageQueue: messages are 16 bytes
struct HQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::array<uint32_t, 4>> msgs;
    uint32_t capacity = 0;
};
static ObjTable<HQueue> g_queues;

HLE(coreinit, OSInitMessageQueue) {
    uint32_t q = arg(c, 0);
    if (g_trace_msg) LOG("[msg] init q=%08X count=%u", q, arg(c, 2));
    HQueue* hq = g_queues.get(q);
    {
        std::lock_guard<std::mutex> lk(hq->m);
        hq->msgs.clear();
        hq->capacity = arg(c, 2);
    }
    hq->cv.notify_all();
    st32(q, 0x6D536751);  // "mSgQ"
}
HLE(coreinit, OSSendMessage) {
    HQueue* q = g_queues.get(arg(c, 0));
    uint32_t m = arg(c, 1), flags = arg(c, 2);
    std::array<uint32_t, 4> msg{ld32(m), ld32(m + 4), ld32(m + 8), ld32(m + 12)};
    if (g_trace_msg) LOG("[msg] send q=%08X msg=%08X %08X flags=%X lr=%08X", arg(c, 0), msg[0], msg[1], flags, c->lr);
    {
        std::unique_lock<std::mutex> lk(q->m);
        if (q->msgs.size() >= q->capacity) {
            if (!(flags & 1)) { ret(c, 0); return; }
            wait_blocking(lk, q->cv, [&] { return q->msgs.size() < q->capacity; });
        }
        if (flags & 2) q->msgs.push_front(msg); else q->msgs.push_back(msg);  // OS_MESSAGE_FLAG_HIGH_PRIORITY
    }
    q->cv.notify_all();
    ret(c, 1);
}
HLE(coreinit, OSReceiveMessage) {
    HQueue* q = g_queues.get(arg(c, 0));
    uint32_t m = arg(c, 1), flags = arg(c, 2);
    std::array<uint32_t, 4> msg;
    if (g_trace_msg) LOG("[msg] recv q=%08X flags=%X lr=%08X thread=%08X", arg(c, 0), flags, c->lr, threads::current_thread());
    {
        std::unique_lock<std::mutex> lk(q->m);
        if (q->msgs.empty()) {
            if (!(flags & 1)) { ret(c, 0); return; }
            wait_blocking(lk, q->cv, [&] { return !q->msgs.empty(); });
        }
        msg = q->msgs.front();
        q->msgs.pop_front();
    }
    q->cv.notify_all();
    for (int i = 0; i < 4; i++) st32(m + 4 * i, msg[i]);
    ret(c, 1);
}

// OSThreadQueue / OSSleepThread: sleepers wait on a condition keyed by the queue
struct HSleep {
    std::mutex m;
    std::condition_variable cv;
    uint64_t gen = 0;
};
static ObjTable<HSleep> g_sleepq;
HLE(coreinit, OSInitThreadQueue) { g_sleepq.get(arg(c, 0)); }  // sleepers (if any) keep waiting for a wakeup
HLE(coreinit, OSSleepThread) {
    HSleep* q = g_sleepq.get(arg(c, 0));
    std::unique_lock<std::mutex> lk(q->m);
    uint64_t g = q->gen;
    wait_blocking(lk, q->cv, [&] { return q->gen != g; });
}
void os_wakeup_thread_queue(uint32_t queue) {
    HSleep* q = g_sleepq.get(queue);
    {
        std::lock_guard<std::mutex> lk(q->m);
        q->gen++;
    }
    q->cv.notify_all();
}

// OSRendezvous
struct HRendezvous {
    std::mutex m;
    std::condition_variable cv;
    uint32_t arrived = 0;
};
static ObjTable<HRendezvous> g_rdv;
HLE(coreinit, OSInitRendezvous) {
    HRendezvous* r = g_rdv.get(arg(c, 0));
    std::lock_guard<std::mutex> lk(r->m);
    r->arrived = 0;
}
HLE(coreinit, OSWaitRendezvous) {
    // arg1 is a core mask; we treat each set bit as one participant
    HRendezvous* r = g_rdv.get(arg(c, 0));
    uint32_t need = __builtin_popcount(arg(c, 1) & 7);
    std::unique_lock<std::mutex> lk(r->m);
    r->arrived++;
    r->cv.notify_all();
    wait_blocking(lk, r->cv, [&] { return r->arrived >= need; });
    ret(c, 1);
}

// ---------------------------------------------------------------- alarms
// One host thread fires all alarms and runs their guest callbacks.
struct Alarm {
    uint32_t core;
    uint32_t guest;
    uint64_t when;
    uint64_t period;
    uint32_t callback;
    uint64_t serial;
};
static std::mutex g_alarm_mutex;
static std::condition_variable g_alarm_cv;
static std::multimap<uint64_t, Alarm> g_alarm_queue;
static std::unordered_map<uint32_t, uint64_t> g_alarm_serial;  // alarm -> active serial (0 = cancelled)
static uint64_t g_next_serial = 1;
static bool g_alarm_thread_started = false;

static void alarm_thread() {
    Cpu* c = threads::make_service_cpu("alarm", 0x20000);
    pthread_setname_np("alarm");
    std::unique_lock<std::mutex> lk(g_alarm_mutex);
    for (;;) {
        if (g_alarm_queue.empty()) { g_alarm_cv.wait(lk); continue; }
        auto it = g_alarm_queue.begin();
        uint64_t now = timebase::now();
        if (it->first > now) {
            g_alarm_cv.wait_for(lk, ticks_to_ns(it->first - now));
            continue;
        }
        Alarm a = it->second;
        g_alarm_queue.erase(it);
        if (g_alarm_serial[a.guest] != a.serial) continue;  // cancelled or re-armed
        if (a.period) {
            a.when += a.period;
            if (a.when < now) a.when = now + a.period;
            g_alarm_queue.emplace(a.when, a);
        } else {
            g_alarm_serial[a.guest] = 0;
        }
        lk.unlock();
        threads::set_service_core(a.core);
        bool took = threads::ensure_core();
        guest_call(c, a.callback, {a.guest, 0});
        if (took) threads::release_core();
        lk.lock();
    }
}

static void arm_alarm(uint32_t alarm, uint64_t when, uint64_t period, uint32_t cb) {
    std::lock_guard<std::mutex> lk(g_alarm_mutex);
    if (!g_alarm_thread_started) {
        g_alarm_thread_started = true;
        std::thread(alarm_thread).detach();
    }
    uint64_t s = g_next_serial++;
    g_alarm_serial[alarm] = s;
    g_alarm_queue.emplace(when, Alarm{t_self ? t_self->core : 1u, alarm, when, period, cb, s});
    g_alarm_cv.notify_all();
}

// OSAlarm layout (Cemu): +0x04 name, +0x0C callback, +0x10 tag, +0x18 nextFire, +0x28 period, +0x30 tick, +0x38 userData
HLE(coreinit, OSCreateAlarm) {
    memset(mem::ptr(arg(c, 0)), 0, 0x58);
    st32(arg(c, 0), 0x614C724D);  // "aLrM"
}
HLE(coreinit, OSSetAlarm) {
    uint32_t alarm = arg(c, 0), cb = c->r[7];
    uint64_t delay = arg64(c, 5);
    st32(alarm + 0x0C, cb);
    arm_alarm(alarm, timebase::now() + delay, 0, cb);
    ret(c, 1);
}
HLE(coreinit, OSSetPeriodicAlarm) {
    uint32_t alarm = arg(c, 0), cb = c->r[9];
    uint64_t start = arg64(c, 5), period = arg64(c, 7);
    st32(alarm + 0x0C, cb);
    uint64_t now = timebase::now();
    uint64_t when = start;
    if (when < now && period) when += ((now - start) / period + 1) * period;
    arm_alarm(alarm, when, period, cb);
    ret(c, 1);
}
HLE(coreinit, OSCancelAlarm) {
    std::lock_guard<std::mutex> lk(g_alarm_mutex);
    g_alarm_serial[arg(c, 0)] = 0;
    ret(c, 1);
}
HLE(coreinit, OSSetAlarmUserData) { st32(arg(c, 0) + 0x38, arg(c, 1)); }
HLE(coreinit, OSGetAlarmUserData) { ret(c, ld32(arg(c, 0) + 0x38)); }

// ---------------------------------------------------------------- time API
HLE(coreinit, OSGetTime) { ret64(c, timebase::now()); }
HLE(coreinit, OSGetSystemTime) { ret64(c, timebase::now()); }
HLE(coreinit, OSGetTick) { ret(c, (uint32_t)timebase::now()); }

HLE(coreinit, OSTicksToCalendarTime) {
    // OSCalendarTime: sec, min, hour, mday, mon, year, wday, yday, msec, usec (int32 each)
    uint64_t ticks = arg64(c, 3);
    uint32_t out = arg(c, 2);
    // guest epoch is 2000-01-01; report the host's wall clock instead of time since boot
    (void)ticks;
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);
    uint32_t v[10] = {(uint32_t)tmv.tm_sec, (uint32_t)tmv.tm_min, (uint32_t)tmv.tm_hour, (uint32_t)tmv.tm_mday,
                      (uint32_t)tmv.tm_mon, (uint32_t)(tmv.tm_year + 1900), (uint32_t)tmv.tm_wday,
                      (uint32_t)tmv.tm_yday, 0, 0};
    for (int i = 0; i < 10; i++) st32(out + 4 * i, v[i]);
}
