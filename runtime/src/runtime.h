// Internal runtime API shared by the loader, dispatcher, threads and HLE libraries.
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "ppc.h"

// ---- guest memory layout ----
namespace mem {
constexpr uint32_t kMem2Start = 0x10000000;   // app data + heap (MEM2)
constexpr uint32_t kMem2End = 0x50000000;
constexpr uint32_t kRuntimeStart = 0x60000000; // runtime-owned guest objects (stacks, OS structs)
constexpr uint32_t kRuntimeEnd = 0x70000000;
constexpr uint32_t kFgBucket = 0xE0000000;     // foreground bucket
constexpr uint32_t kFgBucketSize = 0x02800000;
constexpr uint32_t kMem1 = 0xF4000000;         // MEM1, 32 MiB
constexpr uint32_t kMem1Size = 0x02000000;
constexpr uint32_t kHleFuncBase = 0xC2000000;  // synthetic addresses of host functions

void init();
// bump allocator for runtime-owned guest memory (never freed)
uint32_t runtime_alloc(uint32_t size, uint32_t align = 16);
inline uint8_t* ptr(uint32_t ea) { return PPC_MEM_BASE + ea; }
inline uint32_t guest(const void* p) { return (uint32_t)((const uint8_t*)p - PPC_MEM_BASE); }
std::string read_cstr(uint32_t ea);
void write_cstr(uint32_t ea, const std::string& s, uint32_t max);
}  // namespace mem

// ---- loader ----
struct LoadedModule {
    uint32_t entry;
    uint32_t sda_base;   // r13
    uint32_t sda2_base;  // r2
    uint32_t stack_size;
    uint32_t data_end;   // end of .bss: first free MEM2 address
};
bool load_rpx(const std::string& path, LoadedModule& out);

// ---- dispatch ----
namespace dispatch {
void init();
// register a host function callable from guest code; returns its guest address
uint32_t register_host(PpcFunc fn, const char* name);
void set(uint32_t addr, PpcFunc fn);
PpcFunc lookup(uint32_t addr);
}  // namespace dispatch

// call a guest function from host code (on the current guest thread)
uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args = {});

// ---- threads ----
// Per-core scheduling: like the hardware, only one guest thread runs on each emulated core at a
// time, chosen by priority. Code that blocks the host thread (waits, sleeps, I/O) runs inside a
// BlockingScope so other threads on the core can run meanwhile.
namespace threads {
void block_begin();   // give up the core
void block_end();     // take it back (waits for a turn)
bool ensure_core();   // service threads entering guest code; true if the core was taken
void release_core();
void set_service_core(uint32_t core);
void report_sched();  // log per-thread core usage
}  // namespace threads
struct BlockingScope {
    BlockingScope() { threads::block_begin(); }
    ~BlockingScope() { threads::block_end(); }
    BlockingScope(const BlockingScope&) = delete;
};

namespace threads {
void init(const LoadedModule& m);
void run_main(const LoadedModule& m, int argc, uint32_t argv);  // does not return until the game exits
Cpu* current();          // Cpu of the calling host thread (null if not a guest thread)
uint32_t current_thread();  // guest OSThread* of the calling thread
// a Cpu + guest stack for host-created threads that need to call guest code (alarms, audio)
Cpu* make_service_cpu(const char* name, uint32_t stack_size = 0x10000);
}  // namespace threads

// ---- time ----
namespace timebase {
constexpr uint64_t kTicksPerSec = 62156250ull;  // Espresso bus clock / 4
uint64_t now();  // guest ticks since boot
}

// ---- HLE registration ----
struct HleReg {
    const char* lib;
    const char* name;
    PpcFunc fn;
    HleReg(const char* l, const char* n, PpcFunc f);
};
PpcFunc hle_find(const char* lib, const char* name);  // null if not implemented
PpcFunc hle_find_any(const char* name);

#define HLE(lib, name)                                                        \
    extern "C" void imp_##lib##_##name(Cpu* c);                                \
    static HleReg hle_reg_##lib##_##name(#lib, #name, imp_##lib##_##name);   \
    extern "C" void imp_##lib##_##name(Cpu* c)

// argument helpers (PPC SysV ABI)
inline uint32_t arg(Cpu* c, int i) { return c->r[3 + i]; }
inline uint64_t arg64(Cpu* c, int reg) { return ((uint64_t)c->r[reg] << 32) | c->r[reg + 1]; }  // reg = first of pair
inline void ret(Cpu* c, uint32_t v) { c->r[3] = v; }
inline void ret64(Cpu* c, uint64_t v) { c->r[3] = (uint32_t)(v >> 32); c->r[4] = (uint32_t)v; }

// ---- logging ----
extern bool g_trace_hle;
void log_msg(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) log_msg(__VA_ARGS__)
#define TRACE(...) do { if (g_trace_hle) log_msg(__VA_ARGS__); } while (0)

// ---- configuration ----
namespace config {
extern std::string game_dir;   // extracted game root (contains code/, content/, meta/)
extern std::string save_dir;   // host directory for save data
}
