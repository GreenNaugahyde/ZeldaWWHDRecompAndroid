// Wind Waker HD recompiled: boot sequence and the desktop entry point (Android starts from
// android/jni_main.cpp instead).
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <ctime>

#include <cstring>
#include <string>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
namespace gfx {
const char* driver_info();  // vk/vk_window.h
const char* gpu_name();
}  // namespace gfx
#endif

#include "gx2/gx2.h"
#include "platform.h"
#include "recomp_table.h"
#include "runtime.h"
#include "crash_info.h"
#include "release.h"
#include <sys/prctl.h>
#ifdef WWHD_DEVICE_RECOMP
#include "recomp/loader.h"
#endif

void mem_setup_heaps(uint32_t data_end);
void trace_dump(FILE* f, unsigned last);
void mem_init_data_imports(uint32_t alloc_slot, uint32_t alloc_ex_slot, uint32_t free_slot);

static struct sigaction g_prev_action[NSIG];  // handlers before ours (Android: ART/debuggerd)
static int g_crash_fd = -1;  // the crash log file being written (captures/tlozwwhd_*.log)
static timespec g_boot_time;     // for the time since start in a crash log
static char g_crash_name[64] = "captures/tlozwwhd_%Y%m%d-%H%M%S.log";  // strftime format, with the app version
static std::atomic<bool> g_game_loaded{false};  // guest memory holds the game: its state can be read

static void crash_write(const char* buf, int n) {
    write(2, buf, n);
    if (g_crash_fd >= 0) write(g_crash_fd, buf, n);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "wwhd", buf);
#endif
}

// " in libfoo.so+0x1A2A01 [symbol+0x12]" for a host address inside a loaded module (as the official
// project's crash_addr.cpp; dladdr is not async-signal-safe, the same exposure as the backtrace)
static int describe_host(char* buf, size_t cap, uintptr_t addr) {
    Dl_info di{};
    if (!dladdr((const void*)addr, &di) || !di.dli_fname) return 0;
    const char* name = strrchr(di.dli_fname, '/');
    name = name ? name + 1 : di.dli_fname;
    int n = snprintf(buf, cap, " in %s+%#lx", name, (unsigned long)(addr - (uintptr_t)di.dli_fbase));
    if (di.dli_sname && n > 0 && (size_t)n < cap)
        n += snprintf(buf + n, cap - n, " [%s+%#lx]", di.dli_sname, (unsigned long)(addr - (uintptr_t)di.dli_saddr));
    return n < 0 ? 0 : (size_t)n >= cap ? (int)cap - 1 : n;
}

static void crash_handler(int sig, siginfo_t* si, void* uctx) {
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)PPC_MEM_BASE;
    // a crash log to send with a report: captures/tlozwwhd_VERSION_YYYYmmdd-HHMMSS.log (the app's files
    // folder; the app offers it for sharing, CrashLogs.java)
    char path[96];
    {
        mkdir("captures", 0755);
        time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        strftime(path, sizeof path, g_crash_name, &tmv);
        g_crash_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    char buf[512];
    int n;
    if (a >= base && a < base + 0x100000000ull)
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at guest address %08X\n", sig, (unsigned)(a - base));
    else
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at host address %p\n", sig, si->si_addr);
    crash_write(buf, n);
#ifdef __ANDROID__
    // the GPU and its driver: many crashes are in a driver
    n = snprintf(buf, sizeof buf, "  GPU %s, driver %s\n", gfx::gpu_name(), gfx::driver_info());
    crash_write(buf, std::min<int>(n, sizeof buf - 1));
#endif
    {
        // the thread, how long the game ran, and where in the game it was
        char tname[17] = {};
        prctl(PR_GET_NAME, tname);
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        long secs = (long)(now.tv_sec - g_boot_time.tv_sec);
        n = snprintf(buf, sizeof buf, "  thread \"%s\", %ld min %ld s after start", tname, secs / 60, secs % 60);
        if (g_game_loaded) {
            const char* stage = (const char*)mem::ptr(release::data(0x1046F0B0) + 0x5134);  // as savestate.cpp stage_name
            char st[9] = {};
            for (int i = 0; i < 8 && stage[i] >= 0x20 && stage[i] <= 0x7E; i++) st[i] = stage[i];
            n += snprintf(buf + n, sizeof buf - n, ", stage \"%s\", game frame %u", st, ld32(release::data(0x101FF558)));
        }
        if (n > (int)sizeof buf - 2) n = sizeof buf - 2;
        buf[n++] = '\n';
        crash_write(buf, n);
        const char* info = crash_info::text();
        crash_write(info, (int)strlen(info));
    }
    // the faulting instruction and the module holding it (a GPU driver, a Vulkan layer, the game code)
    uintptr_t pc = 0;
#if defined(__aarch64__)
    if (uctx) pc = (uintptr_t)((ucontext_t*)uctx)->uc_mcontext.pc;
#elif defined(__x86_64__) && defined(__linux__)
    if (uctx) pc = (uintptr_t)((ucontext_t*)uctx)->uc_mcontext.gregs[REG_RIP];
#endif
    char where[384];
    if (pc) {
        where[describe_host(where, sizeof where, pc)] = 0;
        n = snprintf(buf, sizeof buf, "  host pc %p%s\n", (void*)pc, where);
        crash_write(buf, std::min<int>(n, sizeof buf - 1));
    }
    if (!(a >= base && a < base + 0x100000000ull) && describe_host(where, sizeof where, a)) {
        n = snprintf(buf, sizeof buf, "  fault address %p%s\n", si->si_addr, where);
        crash_write(buf, std::min<int>(n, sizeof buf - 1));
    }
    Cpu* c = threads::current();
    if (c) {
        n = snprintf(buf, sizeof buf, "  guest lr=%08X ctr=%08X cr=%08X\n", c->lr, c->ctr, ppc_mfcr(c));
        crash_write(buf, n);
        for (int i = 0; i < 32; i += 8) {
            n = snprintf(buf, sizeof buf, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1],
                         c->r[i + 2], c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
            crash_write(buf, n);
        }
        // guest return chain (back-chain words on the guest stack)
        crash_write("  guest call chain:", 19);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 24 && sp >= 0x10000000u && sp < 0xF0000000u; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp || prev - sp > 0x100000u) break;
            n = snprintf(buf, sizeof buf, " %08X", ld32(prev + 4));
            crash_write(buf, n);
            sp = prev;
        }
        crash_write("\n", 1);
    }
    crash_write("  host backtrace:\n", 18);
    platform::print_backtrace(g_crash_fd);
    if (g_crash_fd >= 0) {
        close(g_crash_fd);
        g_crash_fd = -1;
        n = snprintf(buf, sizeof buf, "[crash] wrote %s\n", path);
        crash_write(buf, n);
    }
    if (g_ppc_trace) {
        FILE* f = fopen("trace_dump.txt", "w");
        if (f) { trace_dump(f, 3000); fclose(f); crash_write("[trace] wrote trace_dump.txt\n", 29); }
    }
#ifdef __ANDROID__
    // hand the fault to the previous handler (debuggerd writes its tombstone): returning re-executes
    // the faulting instruction
    sigaction(sig, &g_prev_action[sig], nullptr);
    return;
#else
    _exit(128 + sig);
#endif
}

static void install_crash_handler() {
    // the app version in the log's name (WWHD_APP_VERSION from the app; letters, digits, '.', '-')
    if (const char* v = getenv("WWHD_APP_VERSION")) {
        std::string s;
        for (const char* p = v; *p && s.size() < 16; p++)
            if (isalnum((unsigned char)*p) || *p == '.' || *p == '-') s += *p;
        if (!s.empty()) snprintf(g_crash_name, sizeof g_crash_name, "captures/tlozwwhd_%s_%%Y%%m%%d-%%H%%M%%S.log", s.c_str());
    }
    static char altstack[1 << 16];
    stack_t ss{};
    ss.ss_sp = altstack;
    ss.ss_size = sizeof altstack;
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, &g_prev_action[sig]);
}

static void init_data_imports() {
    uint32_t alloc = 0, alloc_ex = 0, free_ = 0;
    for (unsigned i = 0; i < g_recomp_import_count; i++) {
        const RecompImport& im = g_recomp_imports[i];
        if (im.is_func) continue;
        std::string n = im.name;
        if (n == "MEMAllocFromDefaultHeap") alloc = im.addr;
        else if (n == "MEMAllocFromDefaultHeapEx") alloc_ex = im.addr;
        else if (n == "MEMFreeToDefaultHeap") free_ = im.addr;
        else if (n == "__gh_FOPEN_MAX") st32(im.addr, 20);
        else if (n == "environ") st32(im.addr, im.addr + 0x10);  // empty environment list
    }
    mem_init_data_imports(alloc, alloc_ex, free_);
}

static LoadedModule g_module;
static uint32_t g_argv;

void boot_runtime() {
    clock_gettime(CLOCK_MONOTONIC, &g_boot_time);
    install_crash_handler();
    crash_info::capture_env();
    mem::init();

    LoadedModule& m = g_module;
    std::string rpx = config::game_dir + "/code/cking.rpx";
    if (!load_rpx(rpx, m)) fatal("cannot load %s", rpx.c_str());
#ifdef WWHD_DEVICE_RECOMP
    std::string err;
    if (!recomp::load_game_code(rpx, config::code_dir, err)) fatal("game code: %s", err.c_str());
#endif
    if (m.entry != g_recomp_entry_point) fatal("%s does not match the recompiled code", rpx.c_str());
    LOG("[boot] loaded %s: entry %08X sda %08X sda2 %08X data end %08X", rpx.c_str(), m.entry, m.sda_base, m.sda2_base,
        m.data_end);
    {
        char t[96];
        snprintf(t, sizeof t, "release %s (entry %08X)", release::name(), m.entry);
        crash_info::set("game", t);
    }

    dispatch::init();
    init_data_imports();
    mem_setup_heaps(m.data_end);
    threads::init(m);

    g_argv = mem::runtime_alloc(16);
    uint32_t arg0 = mem::runtime_alloc(16);
    mem::write_cstr(arg0, "cking.rpx", 16);
    st32(g_argv, arg0);
    g_game_loaded = true;
}

void start_game_thread() {
    std::thread([] {
        threads::run_main(g_module, 1, g_argv);
        LOG("[boot] game main thread returned");
        std::exit(0);
    }).detach();
}

#ifndef __ANDROID__
int main(int argc, char** argv) {
    bool warm_shaders = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) config::game_dir = argv[++i];
        else if (!strcmp(argv[i], "--save") && i + 1 < argc) config::save_dir = argv[++i];
        else if (!strcmp(argv[i], "--trace")) g_trace_hle = true;
        else if (!strcmp(argv[i], "--warm-shaders")) warm_shaders = true;
    }
    boot_runtime();
    // the game runs on its own threads; the process main thread belongs to the window system
    gfx::init();
    if (warm_shaders) {
        // compile the shader head start once (fills the macOS Metal shader cache), then quit
        int gfx_headstart_warm();
        return gfx_headstart_warm();
    }
    start_game_thread();
    gfx::run_main_loop();
    return 0;
}
#endif
