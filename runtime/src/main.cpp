// Wind Waker HD recompiled: boot sequence and the desktop entry point (Android starts from
// android/jni_main.cpp instead).
#include <signal.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#endif

#include "gx2/gx2.h"
#include "platform.h"
#include "recomp_table.h"
#include "runtime.h"
#ifdef WWHD_DEVICE_RECOMP
#include "recomp/loader.h"
#endif

void mem_setup_heaps(uint32_t data_end);
void trace_dump(FILE* f, unsigned last);
void mem_init_data_imports(uint32_t alloc_slot, uint32_t alloc_ex_slot, uint32_t free_slot);

static struct sigaction g_prev_action[NSIG];  // handlers before ours (Android: ART/debuggerd)

static void crash_write(const char* buf, int n) {
    write(2, buf, n);
#ifdef __ANDROID__
    __android_log_write(ANDROID_LOG_ERROR, "wwhd", buf);
#endif
}

static void crash_handler(int sig, siginfo_t* si, void*) {
    uintptr_t a = (uintptr_t)si->si_addr;
    uintptr_t base = (uintptr_t)PPC_MEM_BASE;
    char buf[256];
    int n;
    if (a >= base && a < base + 0x100000000ull)
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at guest address %08X\n", sig, (unsigned)(a - base));
    else
        n = snprintf(buf, sizeof buf, "\nCRASH: signal %d at host address %p\n", sig, si->si_addr);
    crash_write(buf, n);
    Cpu* c = threads::current();
    if (c) {
        n = snprintf(buf, sizeof buf, "  guest lr=%08X ctr=%08X cr=%08X\n", c->lr, c->ctr, ppc_mfcr(c));
        crash_write(buf, n);
        for (int i = 0; i < 32; i += 8) {
            n = snprintf(buf, sizeof buf, "  r%-2d %08X %08X %08X %08X %08X %08X %08X %08X\n", i, c->r[i], c->r[i + 1],
                         c->r[i + 2], c->r[i + 3], c->r[i + 4], c->r[i + 5], c->r[i + 6], c->r[i + 7]);
            crash_write(buf, n);
        }
    }
    platform::print_backtrace();
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
    install_crash_handler();
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

    dispatch::init();
    init_data_imports();
    mem_setup_heaps(m.data_end);
    threads::init(m);

    g_argv = mem::runtime_alloc(16);
    uint32_t arg0 = mem::runtime_alloc(16);
    mem::write_cstr(arg0, "cking.rpx", 16);
    st32(g_argv, arg0);
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
