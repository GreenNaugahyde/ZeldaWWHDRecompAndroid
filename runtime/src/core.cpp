// Guest memory, RPX loading, function dispatch, logging and HLE registry.
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <zlib.h>

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

#include "recomp_table.h"
#include "runtime.h"

bool g_trace_hle = false;
namespace config {
std::string game_dir = "game";
std::string save_dir = "save";
}  // namespace config

static std::mutex g_log_mutex;

void log_msg(const char* fmt, ...) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char* fmt, ...) {
    {
        std::lock_guard<std::mutex> lk(g_log_mutex);
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "FATAL: ");
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }
    abort();
}

// ---------------------------------------------------------------- memory
namespace mem {
static std::atomic<uint32_t> g_runtime_top{kRuntimeStart};

void init() {
    mach_vm_address_t addr = (mach_vm_address_t)PPC_MEM_BASE;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &addr, 0x100000000ull, VM_FLAGS_FIXED);
    if (kr != KERN_SUCCESS) fatal("cannot reserve guest address space at %p (kr=%d)", PPC_MEM_BASE, kr);
    // null page guard: catches guest null-pointer accesses
    mprotect(PPC_MEM_BASE, 0x10000, PROT_NONE);
}

uint32_t runtime_alloc(uint32_t size, uint32_t align) {
    uint32_t cur = g_runtime_top.load();
    uint32_t start;
    do {
        start = (cur + align - 1) & ~(align - 1);
        if (start + size > kRuntimeEnd) fatal("runtime guest region exhausted");
    } while (!g_runtime_top.compare_exchange_weak(cur, start + size));
    memset(ptr(start), 0, size);
    return start;
}

std::string read_cstr(uint32_t ea) {
    if (!ea) return {};
    return std::string((const char*)ptr(ea));
}

void write_cstr(uint32_t ea, const std::string& s, uint32_t max) {
    uint32_t n = (uint32_t)std::min<size_t>(s.size(), max - 1);
    memcpy(ptr(ea), s.data(), n);
    ptr(ea)[n] = 0;
}
}  // namespace mem

// ---------------------------------------------------------------- loader
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool load_rpx(const std::string& path, LoadedModule& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    uint32_t shoff = be32(&d[0x20]);
    uint16_t shentsize = be16(&d[0x2E]), shnum = be16(&d[0x30]);
    out.entry = be32(&d[0x18]);
    out.data_end = 0;
    for (int i = 0; i < shnum; i++) {
        const uint8_t* sh = &d[shoff + i * shentsize];
        uint32_t type = be32(sh + 4), flags = be32(sh + 8), addr = be32(sh + 12), off = be32(sh + 16), size = be32(sh + 20);
        std::vector<uint8_t> data;
        if (type != 8 && size) {
            if (flags & 0x08000000) {
                uLongf dlen = be32(&d[off]);
                data.resize(dlen);
                if (uncompress(data.data(), &dlen, &d[off + 4], size - 4) != Z_OK) fatal("zlib error in section %d", i);
            } else {
                data.assign(d.begin() + off, d.begin() + off + size);
            }
        }
        if (type == 0x80000004 && data.size() >= 0x30) {  // RPL file info
            out.sda_base = be32(&data[0x24]);
            out.sda2_base = be32(&data[0x28]);
            out.stack_size = be32(&data[0x2C]);
        }
        // allocated sections in the text/data regions
        if ((flags & 2) && addr >= 0x02000000 && addr < 0xC0000000) {
            if (type == 8) {
                memset(mem::ptr(addr), 0, size);
                out.data_end = std::max(out.data_end, addr + size);
            } else {
                memcpy(mem::ptr(addr), data.data(), data.size());
                if (addr >= mem::kMem2Start) out.data_end = std::max(out.data_end, addr + (uint32_t)data.size());
            }
        }
    }
    return true;
}

// ---------------------------------------------------------------- dispatch
namespace dispatch {
static constexpr uint32_t kTextBase = 0x02000000;
static constexpr uint32_t kTextSize = 0x01000000;  // 16 MiB covers cking.rpx .text
static PpcFunc* g_text_table;                        // indexed by (addr - kTextBase) / 4
// lock-free direct tables for import slots and host functions (called through pointers a lot)
static constexpr uint32_t kSlotBase = 0xC0000000, kSlotSize = 0x40000;     // import stubs, 4-byte steps
static constexpr uint32_t kHostSize = 0x80000;                              // host functions, 8-byte steps
static std::atomic<PpcFunc> g_slot_table[kSlotSize / 4];
static std::atomic<PpcFunc> g_host_table[kHostSize / 8];
static std::unordered_map<uint32_t, PpcFunc> g_other;
static std::shared_mutex g_other_mutex;
static std::atomic<uint32_t> g_next_host{mem::kHleFuncBase};
static std::unordered_map<uint32_t, std::string> g_host_names;

void init() {
    g_text_table = (PpcFunc*)calloc(kTextSize / 4, sizeof(PpcFunc));
    for (unsigned i = 0; i < g_recomp_func_count; i++) set(g_recomp_funcs[i].addr, g_recomp_funcs[i].fn);
    // imported functions are reachable through their import slot addresses
    for (unsigned i = 0; i < g_recomp_import_count; i++)
        if (g_recomp_imports[i].is_func) set(g_recomp_imports[i].slot, g_recomp_imports[i].fn);
}

void set(uint32_t addr, PpcFunc fn) {
    if (addr - kTextBase < kTextSize) {
        g_text_table[(addr - kTextBase) >> 2] = fn;
        return;
    }
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) { g_slot_table[(addr - kSlotBase) >> 2] = fn; return; }
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7)) { g_host_table[(addr - mem::kHleFuncBase) >> 3] = fn; return; }
    std::unique_lock lk(g_other_mutex);
    g_other[addr] = fn;
}

PpcFunc lookup(uint32_t addr) {
    if (addr - kTextBase < kTextSize) return g_text_table[(addr - kTextBase) >> 2];
    if (addr - kSlotBase < kSlotSize && !(addr & 3)) return g_slot_table[(addr - kSlotBase) >> 2].load(std::memory_order_relaxed);
    if (addr - mem::kHleFuncBase < kHostSize && !(addr & 7))
        return g_host_table[(addr - mem::kHleFuncBase) >> 3].load(std::memory_order_relaxed);
    std::shared_lock lk(g_other_mutex);
    auto it = g_other.find(addr);
    return it == g_other.end() ? nullptr : it->second;
}

uint32_t register_host(PpcFunc fn, const char* name) {
    uint32_t a = g_next_host.fetch_add(8);
    set(a, fn);
    std::unique_lock lk(g_other_mutex);
    g_host_names[a] = name;
    return a;
}
}  // namespace dispatch

extern "C" void ppc_dispatch(Cpu* c) {
    PpcFunc f = dispatch::lookup(c->pc);
    if (!f) fatal("indirect branch to unknown address %08X (lr=%08X ctr=%08X)", c->pc, c->lr, c->ctr);
    MUSTTAIL return f(c);
}

uint32_t guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> args) {
    uint32_t save_lr = c->lr, save_ctr = c->ctr, save_sp = c->r[1];
    // open a minimal frame so the callee's LR save slot doesn't clobber our caller's frame
    c->r[1] -= 0x40;
    st32(c->r[1], save_sp);
    int i = 3;
    for (uint32_t a : args) c->r[i++] = a;
    c->lr = 0;
    c->pc = fn;
    ppc_dispatch(c);
    c->r[1] = save_sp;
    c->lr = save_lr;
    c->ctr = save_ctr;
    return c->r[3];
}

extern "C" void ppc_unimplemented(Cpu* c, uint32_t addr, uint32_t insn) {
    fatal("unimplemented instruction %08X at %08X (lr=%08X)", insn, addr, c->lr);
}

extern "C" void ppc_trap(Cpu* c, uint32_t addr) {
    fatal("guest trap at %08X (lr=%08X r3=%08X)", addr, c->lr, c->r[3]);
}

// ---------------------------------------------------------------- HLE registry
static std::vector<HleReg*>& hle_registry() {
    static std::vector<HleReg*> r;
    return r;
}
HleReg::HleReg(const char* l, const char* n, PpcFunc f) : lib(l), name(n), fn(f) { hle_registry().push_back(this); }

PpcFunc hle_find(const char* lib, const char* name) {
    std::string l = lib;
    if (l.size() > 4 && l.substr(l.size() - 4) == ".rpl") l.resize(l.size() - 4);
    for (HleReg* r : hle_registry())
        if (l == r->lib && strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

PpcFunc hle_find_any(const char* name) {
    for (HleReg* r : hle_registry())
        if (strcmp(name, r->name) == 0) return r->fn;
    return nullptr;
}

extern "C" void hle_unimplemented(Cpu* c, const char* lib, const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    {
        std::lock_guard<std::mutex> lk(m);
        int& n = seen[std::string(lib) + "." + name];
        if (n++ < 3)
            log_msg("[hle] unimplemented %s.%s(%08X, %08X, %08X, %08X) lr=%08X", lib, name, c->r[3], c->r[4], c->r[5],
                    c->r[6], c->lr);
    }
    c->r[3] = 0;
}
