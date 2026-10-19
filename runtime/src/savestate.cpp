// Save states: snapshot the whole running game into one of 5 slots and restore it, also in a new
// process.
//
// The recompiled game runs on host threads with native call stacks, so a snapshot can only be taken
// (and restored) when every guest thread is at a point whose complete state is guest memory, its Cpu
// registers and the HLE objects:
//   - the game's main thread at the top of the per-frame function (0203593C; interp.cpp calls
//     service() there), which requests the save/load;
//   - every other guest thread parked in an HLE wait (OSReceiveMessage, OSWaitEvent, OSLockMutex,
//     OSSleepThread, OSSleepTicks, GX2WaitForVsync, ...: park_wait in threads.cpp). A parked thread
//     re-checks its condition after waking and only then consumes anything, so the HLE objects
//     decide where it continues;
//   - service threads (alarms, AX frame) idle between their callbacks.
// threads::quiesce() freezes the game in that state (other threads keep running until they park).
//
// Restoring into a running process requires each thread to be parked at the same place it was
// saved at (same stack pointer, return address and guest back chain => same host call stack); the
// main thread always is. Worker threads normally idle at the same wait. If not, the load is retried
// on the following frames and finally refused with a message; nothing is changed then.
//
// Saving stops the game only for the small sections; guest memory is captured in the background by
// copy on write (see "background capture"). Auto saves (an interval in the options) take turns in
// two slots of their own (auto1.bin, auto2.bin).
//
// Slot file (version 2): header, the sections from each module (threads + HLE sync objects + alarms
// + guest clock, heaps, open files, AX voices, GX2 registers, runtime allocations) zstd-compressed,
// then guest memory (MEM2, runtime objects, fixed slots, foreground bucket, MEM1) as 64 KiB chunks
// compressed one by one (all-zero chunks are left out). Chunks are compressed straight from guest
// memory while the game is frozen and stay compressed in memory until they're restored, so neither
// saving nor loading holds the raw ~580 MB (that got the game killed for low memory on 6 GB phones).
// Version 1 (read only): zlib blocks (LZ4 on macOS) of one payload holding sections and raw memory.
#include "savestate.h"

#ifdef __APPLE__
#include <compression.h>
#include <mach-o/ldsyms.h>
#include <mach-o/loader.h>
#else
#include <elf.h>
#include <dlfcn.h>
#include <link.h>
#include <zlib.h>  // blocks are deflated instead of LZ4 (states don't move between platforms)
#endif
#include <zstd.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <cstddef>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "packs.h"
#include "runtime.h"
#include "gx2/gx2_cmd.h"
#include "input.h"
#include "mem_writes.h"
#include "release.h"

// module sections
bool threads_ss_save(ss::Writer& w, std::string& why);
bool threads_ss_check(ss::Reader r, std::string& why);
bool threads_ss_rebuild_needed();
void threads_ss_retire();
void threads_ss_load(ss::Reader& r);
void threads_ss_rebuild();
void threads_ss_targets(ss::Reader r);
void mem_ss_save(ss::Writer& w);
bool mem_ss_check(ss::Reader r, std::string& why);
void mem_ss_load(ss::Reader& r);
void fs_ss_save(ss::Writer& w);
void fs_ss_load(ss::Reader& r);
void ax_ss_save(ss::Writer& w);
bool ax_ss_check(ss::Reader r, std::string& why);
void ax_ss_load(ss::Reader& r);
void gx2_ss_drain();
void gx2_ss_save(ss::Writer& w);
bool gx2_ss_check(ss::Reader r, std::string& why);
void gx2_ss_load(ss::Reader& r);
namespace interp { void ss_reset(); }
namespace gfx {
uint64_t frame_count();
void cache_progress(uint64_t out[4]);
void request_tv_dump(const std::string& path, int frames_ahead);
}
namespace dispatch { std::vector<std::pair<uint32_t, std::string>> host_functions(); }
PpcFunc dynload_resolve(const std::string& key);
void dynload_place(uint32_t addr, const std::string& key);

namespace ss {
namespace {

constexpr char kMagic[8] = {'W', 'W', 'H', 'D', 'S', 'T', 'A', 'T'};
constexpr uint32_t kVersion = 2;  // written; 1 is still read
constexpr int kZstdLevel = 1;
constexpr uint32_t kChunk = 0x10000;
constexpr uint32_t kBlock = 8 << 20;  // compression block (raw bytes)

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint64_t created;       // unix time
    char area[32];          // stage name
    uint8_t build[16];      // LC_UUID of the executable that wrote it (informational)
    uint64_t game_id;       // hash of cking.rpx
    uint32_t cpu_size;      // sizeof(Cpu)
    uint32_t blocks;        // compressed blocks that follow
    uint64_t raw_size;      // payload bytes
    // added later (files written before end at header_size == kHeaderV1Size and read these as 0)
    uint32_t controller;    // the controls at the time: 0 unknown, 1 GamePad, 2 Pro Controller
    uint32_t reserved;
};
constexpr uint32_t kHeaderV1Size = offsetof(Header, controller);

enum : uint32_t {
    kSecThreads = 'THRD',
    kSecHeaps = 'HEAP',
    kSecFiles = 'FILE',
    kSecAudio = 'AX  ',
    kSecGx2 = 'GX2 ',
    kSecAllocs = 'RALC',
    kSecDispatch = 'DSPT',
    kSecMemory = 'MEM ',
};

struct Region { uint32_t base, size; };

std::vector<Region> regions() {
    uint32_t top = (mem::runtime_top() + kChunk - 1) & ~(kChunk - 1);
    std::vector<Region> r = {{mem::kMem2Start, mem::kMem2End - mem::kMem2Start},
                             {mem::kRuntimeStart, top - mem::kRuntimeStart},
                             {mem::kFixedStart, mem::kFixedSize},
                             {mem::kFgBucket, mem::kFgBucketSize},
                             {mem::kMem1, mem::kMem1Size}};
    // mod packs' code caves hold their variables (packs.h)
    uint32_t cave = (packs::cave_end() + kChunk - 1) & ~(kChunk - 1);
    if (cave > packs::kCodeCave) r.push_back({packs::kCodeCave, cave - packs::kCodeCave});
    return r;
}

// a memory chunk of a snapshot: 64 KiB as they are (comp == 0) or zstd-compressed (comp bytes)
struct ChunkRef {
    const uint8_t* p = nullptr;
    uint32_t comp = 0;
};
bool chunk_data(const ChunkRef& c, uint8_t* dst) {
    if (!c.comp) {
        memcpy(dst, c.p, kChunk);
        return true;
    }
    return ZSTD_decompress(dst, kChunk, c.p, c.comp) == kChunk;
}

// A snapshot in memory: header + payload (sections; version 1 also the raw memory) + the memory
// chunks (version 2: compressed, in blob).
struct Snapshot {
    Header h{};
    std::vector<uint8_t> payload;
    std::vector<uint8_t> blob;
    std::map<uint32_t, std::pair<size_t, size_t>> sections;  // tag -> (offset, size)
    std::unordered_map<uint32_t, ChunkRef> chunks;            // chunk address -> data (absent = zero)
    std::vector<Region> regs;
    int slot = 0;
    bool converted = false;  // read from a version 1 file (request_load writes it back as version 2)

    Reader section(uint32_t tag) const {
        auto it = sections.find(tag);
        if (it == sections.end()) return Reader(nullptr, 0);
        return Reader(payload.data() + it->second.first, it->second.second);
    }
    bool parse_sections() {
        Reader r(payload.data(), payload.size());
        while (r.ok && !r.at_end()) {
            uint32_t tag = r.u32();
            uint64_t n = r.u64();
            size_t off = r.p - payload.data();
            if (!r.bytes(nullptr, n)) return false;
            sections[tag] = {off, (size_t)n};
        }
        return r.ok;
    }
    // version 1: the memory is a section of the payload
    bool parse() {
        if (!parse_sections() || !sections.count(kSecMemory)) return false;
        Reader m = section(kSecMemory);
        uint32_t nr = m.u32();
        for (uint32_t i = 0; i < nr && m.ok; i++) {
            Region g{m.u32(), m.u32()};
            regs.push_back(g);
            uint32_t present = m.u32();
            for (uint32_t k = 0; k < present && m.ok; k++) {
                uint32_t idx = m.u32();
                chunks[g.base + idx * kChunk] = ChunkRef{m.p, 0};
                if (!m.bytes(nullptr, kChunk)) return false;
            }
        }
        return m.ok;
    }
};

std::mutex g_mu;  // messages, pending requests
std::mutex g_io;  // one slot file operation at a time
std::string g_message;
std::chrono::steady_clock::time_point g_message_time;
std::atomic<int> g_save_req{0};
std::shared_ptr<Snapshot> g_load_ready;   // decompressed, waiting for the frame boundary
std::atomic<bool> g_loading{false};       // a slot file is being read
// a state saved with the Pro Controller was loaded while the controls act as the GamePad: the game is put
// into GamePad mode on the next frame (service), as the title's controller question does
std::atomic<bool> g_to_gamepad{false};
std::condition_variable g_hold_cv;        // (g_mu) the start hold ends, or a state is ready to load
std::atomic<int> g_hold{0};               // start_hold_state()
std::atomic<int> g_writes{0};             // saves still being compressed / written (writes_pending)
int g_attempts = 0;
std::atomic<bool> g_save_quiet{false};    // the pending save is an auto save (no message, no retries)
std::atomic<int> g_auto_minutes{0};       // auto save interval, 0 off
double g_auto_played = 0;                 // seconds played since the last auto save (game thread)
const Snapshot* g_check = nullptr;        // snapshot whose memory snap_ld32 reads
uint64_t g_check_serial = 0;              // bumped with each g_check (snap_ld32's unpacked chunk)

void message(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void message(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    LOG("[savestate] %s", buf);
    std::lock_guard<std::mutex> lk(g_mu);
    g_message = buf;
    g_message_time = std::chrono::steady_clock::now();
}

void make_dirs(const std::string& d) {
    for (size_t i = 1; i <= d.size(); i++)
        if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
}

std::string state_dir() {
    static const std::string dir = [] {
        std::string d;
        if (const char* e = getenv("WWHD_STATE_DIR")) d = e;
#ifdef __APPLE__
        else d = std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Library/Application Support/wwhd/states";
#else
        else d = config::save_dir.substr(0, config::save_dir.find_last_of('/')) + "/states";  // next to save/
#endif
        make_dirs(d);
        return d;
    }();
    return dir;
}
// slots 1..5 (slotN), then the auto slots (autoN) and the last session (last)
std::string slot_path(int slot, const char* ext = "bin") {
    std::string name = slot == kLastSlot ? "last" : slot > kSlots ? "auto" + std::to_string(slot - kSlots) : "slot" + std::to_string(slot);
    return state_dir() + "/" + name + "." + ext;
}
// "slot 3" / "auto slot 1" (Cap: at the start of a message)
std::string slot_label(int slot, bool cap = false) {
    std::string s = slot == kLastSlot ? "last session slot"
                    : slot > kSlots   ? "auto slot " + std::to_string(slot - kSlots)
                                      : "slot " + std::to_string(slot);
    if (cap) s[0] = (char)toupper(s[0]);
    return s;
}

void build_uuid(uint8_t out[16]) {
    memset(out, 0, 16);
#ifdef __APPLE__
    const auto* h = (const mach_header_64*)&_mh_execute_header;
    const uint8_t* p = (const uint8_t*)(h + 1);
    for (uint32_t i = 0; i < h->ncmds; i++) {
        const auto* lc = (const load_command*)p;
        if (lc->cmd == LC_UUID) { memcpy(out, ((const uuid_command*)lc)->uuid, 16); return; }
        p += lc->cmdsize;
    }
#else
    // the GNU build id of the image containing this function
    dl_iterate_phdr(
        [](dl_phdr_info* info, size_t, void* data) -> int {
            uintptr_t self = (uintptr_t)&build_uuid;
            bool mine = false;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                uintptr_t a = info->dlpi_addr + ph.p_vaddr;
                if (ph.p_type == PT_LOAD && self >= a && self < a + ph.p_memsz) mine = true;
            }
            if (!mine) return 0;
            for (int i = 0; i < info->dlpi_phnum; i++) {
                const auto& ph = info->dlpi_phdr[i];
                if (ph.p_type != PT_NOTE) continue;
                const uint8_t* p = (const uint8_t*)(info->dlpi_addr + ph.p_vaddr);
                const uint8_t* end = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= end) {
                    const auto* n = (const ElfW(Nhdr)*)p;
                    const uint8_t* desc = p + sizeof *n + ((n->n_namesz + 3) & ~3u);
                    if (n->n_type == NT_GNU_BUILD_ID) {
                        memcpy(data, desc, std::min<size_t>(16, n->n_descsz));
                        return 1;
                    }
                    p = desc + ((n->n_descsz + 3) & ~3u);
                }
            }
            return 1;
        },
        out);
#endif
}

// Whether any page of [p, p + n) was ever written: untouched pages are zero and are skipped without
// reading them (reading would map them). Not mincore(): it calls a page that was swapped out "not
// resident", and in the background Android moves much of the game's memory to zram, so states taken
// then lost most of their memory (a "Last session" state of 60 MB instead of 580). /proc/self/pagemap
// tells present and swapped pages apart from untouched ones (bits 63 and 62).
bool ever_touched(const void* p, size_t n) {
#ifdef __APPLE__
    std::vector<char> vec(n / getpagesize() + 1);
    if (mincore((void*)p, n, vec.data()) != 0) return true;
    for (size_t i = 0; i < n / (size_t)getpagesize(); i++)
        if (vec[i]) return true;
    return false;  // (macOS: compressed pages count as resident)
#else
    static const int fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return true;  // unknown: read it
    const size_t ps = getpagesize(), pages = n / ps;
    uint64_t e[64];
    if (pages > 64) return true;
    off_t at = (off_t)((uintptr_t)p / ps * 8);
    if (pread(fd, e, pages * 8, at) != (ssize_t)(pages * 8)) return true;
    for (size_t i = 0; i < pages; i++)
        if (e[i] & (3ull << 62)) return true;  // present or swapped
    return false;
#endif
}

// one block of a version 1 slot file
size_t decompress_block(uint8_t* dst, size_t n, const uint8_t* src, size_t srcSize, void* scratch) {
#ifdef __APPLE__
    return compression_decode_buffer(dst, n, src, srcSize, (uint8_t*)scratch, COMPRESSION_LZ4);
#else
    uLongf out = n;
    return uncompress(dst, &out, src, srcSize) == Z_OK ? out : 0;
#endif
}
size_t scratch_size() {
#ifdef __APPLE__
    return compression_decode_scratch_buffer_size(COMPRESSION_LZ4);
#else
    return 1;
#endif
}

uint64_t game_id() {
    static const uint64_t id = [] {
        uint64_t h = 0xcbf29ce484222325ull;
        FILE* f = fopen((config::game_dir + "/code/cking.rpx").c_str(), "rb");
        if (!f) return h;
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
        fclose(f);
        return h;
    }();
    return id;
}

// current stage (dComIfG_gameInfo.play: the start stage, 8 chars)
const release::Data kStageInfo{0x1046F0B0};
constexpr uint32_t kStageNameOff = 0x5134;  // dStage_startStage_c
constexpr uint32_t kNextStageOff = 0x5140;  // dStage_nextStage_c
std::string stage_name() {
    if (const char* e = getenv("WWHD_STATE_STAGE_ADDR")) {
        uint32_t a = (uint32_t)strtoul(e, nullptr, 16);
        return std::string((const char*)mem::ptr(a), strnlen((const char*)mem::ptr(a), 8));
    }
    const char* p = (const char*)mem::ptr(kStageInfo + kStageNameOff);
    size_t n = strnlen(p, 8);
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return "";
    return std::string(p, n);
}

// ---------------------------------------------------------------- memory
// guest memory, captured while the game is frozen: the chunks that aren't all zero, compressed on
// every core straight from guest memory
struct Captured {
    struct Piece {
        uint32_t thread, offset, comp;  // comp 0: stored as it is
    };
    std::vector<Region> regs;
    std::vector<uint32_t> addrs;  // chunk addresses, in order
    std::vector<Piece> pieces;    // by chunk
    std::vector<std::vector<uint8_t>> out;  // by compressing thread
    size_t bytes() const {
        size_t n = 0;
        for (auto& o : out) n += o.size();
        return n;
    }
};

std::shared_ptr<Captured> capture_memory() {
    auto c = std::make_shared<Captured>();
    c->regs = regions();
    for (auto& g : c->regs) {
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint8_t* p = mem::ptr(g.base + off);
            if (!ever_touched(p, kChunk)) continue;  // zero
            const uint64_t* q = (const uint64_t*)p;
            bool zero = true;
            for (size_t i = 0; i < kChunk / 8 && zero; i++) zero = q[i] == 0;
            if (!zero) c->addrs.push_back(g.base + off);
        }
    }
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    c->out.resize(nt);
    c->pieces.resize(c->addrs.size());
    std::atomic<size_t> next{0};
    auto work = [&](unsigned t) {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk));
        std::vector<uint8_t>& o = c->out[t];
        o.reserve((c->addrs.size() / nt + 16) * (kChunk / 2));  // address space; grows if needed
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < c->addrs.size();) {
            const uint8_t* src = mem::ptr(c->addrs[i]);
            size_t n = cx ? ZSTD_compressCCtx(cx, tmp.data(), tmp.size(), src, kChunk, kZstdLevel) : 0;
            Captured::Piece& pc = c->pieces[i];
            pc.thread = t;
            pc.offset = (uint32_t)o.size();
            if (!cx || ZSTD_isError(n) || n >= kChunk) {
                o.insert(o.end(), src, src + kChunk);
                pc.comp = 0;
            } else {
                o.insert(o.end(), tmp.data(), tmp.data() + n);
                pc.comp = (uint32_t)n;
            }
        }
        ZSTD_freeCCtx(cx);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work, t);
    work(0);
    for (auto& th : pool) th.join();
    return c;
}

void restore_memory(const Snapshot& s) {
    // the saved chunks, unpacked on every core straight into guest memory (read_slot checked them)
    std::vector<std::pair<uint32_t, ChunkRef>> list(s.chunks.begin(), s.chunks.end());
    std::atomic<size_t> next{0};
    std::atomic<int> bad{0};
    auto work = [&] {
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < list.size();)
            if (!chunk_data(list[i].second, mem::ptr(list[i].first))) bad++;
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (bad) LOG("[savestate] %d memory chunks could not be unpacked", bad.load());
    for (auto& g : s.regs) {
        for (uint32_t off = 0; off < g.size; off += kChunk) {
            uint32_t a = g.base + off;
            uint8_t* p = mem::ptr(a);
            if (s.chunks.count(a)) continue;
            if (!ever_touched(p, kChunk)) continue;  // zero already
            const uint64_t* q = (const uint64_t*)p;
            for (size_t i = 0; i < kChunk / 8; i++)
                if (q[i]) { memset(p, 0, kChunk); break; }
        }
    }
}

// ---------------------------------------------------------------- background capture
// Copy on write (not macOS): while the game is frozen, guest memory is only write-protected, and the
// game continues at once. A background pass then takes the chunks in address order: copies one,
// makes it writable again and compresses the copy. A write to a chunk the pass hasn't reached yet
// faults: the handler copies the chunk as it is to a side buffer first, unprotects it, and the write
// goes ahead; the pass compresses that copy instead. So the state is the memory at the freeze, while
// the game only pauses for a 64 KiB copy per chunk it writes early. Host code writing guest memory
// faults the same way; the kernel doesn't (read() into protected memory fails with EFAULT), so file
// reads into guest memory call prepare_host_write first.
#ifndef __APPLE__
namespace cow {
enum : uint8_t { kPending, kBusy, kCopied, kDone };

struct Job {
    std::vector<Region> regs;
    std::vector<size_t> first;  // index of each region's first chunk
    size_t n = 0;
    std::unique_ptr<std::atomic<uint8_t>[]> st;  // by chunk
    uint8_t* copies = nullptr;  // n chunks, NORESERVE: only the chunks copied by faults take memory
    uint8_t* pre = nullptr;     // the chunks copied at the stop (prepared memory, pre_size bytes)
    size_t pre_size = 0;
    std::vector<uint32_t> pre_index;  // chunk -> its place in pre + 1 (0: in copies)
    uint8_t* copy_of(size_t i) const { return pre_index[i] ? pre + (size_t)(pre_index[i] - 1) * kChunk : copies + i * kChunk; }
    std::atomic<bool> failed{false};
    std::atomic<uint32_t> faults{0}, copied{0};
    uint32_t precopied = 0;
    std::atomic<uint64_t> fault_ns{0}, max_fault_ns{0};

    long index(uint32_t ea) const {
        for (size_t r = 0; r < regs.size(); r++)
            if (ea - regs[r].base < regs[r].size) return (long)(first[r] + (ea - regs[r].base) / kChunk);
        return -1;
    }
    uint32_t addr(size_t i) const {
        size_t r = regs.size() - 1;
        while (first[r] > i) r--;
        return regs[r].base + (uint32_t)(i - first[r]) * kChunk;
    }
};

std::atomic<Job*> g_job{nullptr};      // the capture running now (read by the fault handler)
struct FaultNote { volatile int entered, passed, job, code; volatile long index; } g_fault_note;
std::shared_ptr<Job> g_last;           // kept until the next capture: a handler may still look at it
std::atomic<bool> g_running{false};    // a background capture hasn't finished
std::vector<uint32_t> g_hot;           // chunks the game wrote during the last capture (start copies them)
// memory for copying g_hot at the stop, made ready by another thread before it (new memory takes a
// page fault per 4 KiB at the first write: ~15 ms for the ~25 MB, which would add to the stop)
std::atomic<int> g_prep_state{0};      // 0 none, 1 being made, 2 ready
uint8_t* g_prep = nullptr;
size_t g_prep_size = 0;

// test aid: WWHD_STATE_COW=0 captures memory while the game is stopped, as before
bool enabled() {
    static const bool on = !getenv("WWHD_STATE_COW") || atoi(getenv("WWHD_STATE_COW")) != 0;
    return on;
}

// game thread, before a save: true when it can start (the copy memory is ready)
bool prepared() {
    if (!enabled()) return true;
    int st = g_prep_state.load(std::memory_order_acquire);
    if (st == 2 || g_hot.empty()) return true;
    if (st == 1) return false;
    g_prep_state = 1;
    size_t size = g_hot.size() * kChunk;
    std::thread([size] {
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED) {
            g_prep = nullptr;
        } else {
            const size_t page = getpagesize();
            for (size_t o = 0; o < size; o += page) ((volatile uint8_t*)p)[o] = 0;
            g_prep = (uint8_t*)p;
            g_prep_size = size;
        }
        g_prep_state.store(2, std::memory_order_release);
    }).detach();
    return false;
}
struct sigaction g_prev;               // the handler before ours (the crash handler)

uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);  // async-signal-safe
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

// everything writable again; the capture is lost (a protection change failed: out of mappings)
void give_up(Job* j) {
    j->failed = true;
    for (auto& g : j->regs) mprotect(mem::ptr(g.base), g.size, PROT_READ | PROT_WRITE);
}

// makes chunk i writable, its content kept for the capture first (fault handler, file reads)
void take(Job* j, size_t i) {
    uint64_t t0 = now_ns();
    for (;;) {
        uint8_t v = kPending;
        if (j->st[i].compare_exchange_strong(v, kBusy, std::memory_order_acq_rel)) {
            uint8_t* p = mem::ptr(j->addr(i));
            memcpy(j->copies + i * kChunk, p, kChunk);
            if (mprotect(p, kChunk, PROT_READ | PROT_WRITE) != 0) give_up(j);
            j->st[i].store(kCopied, std::memory_order_release);
            j->copied.fetch_add(1, std::memory_order_relaxed);
            break;
        }
        if (v != kBusy) break;  // copied or done: writable already
        sched_yield();          // the pass is copying it (a few microseconds)
    }
    uint64_t d = now_ns() - t0;
    j->faults.fetch_add(1, std::memory_order_relaxed);
    j->fault_ns.fetch_add(d, std::memory_order_relaxed);
    uint64_t m = j->max_fault_ns.load(std::memory_order_relaxed);
    while (d > m && !j->max_fault_ns.compare_exchange_weak(m, d, std::memory_order_relaxed)) {}
}

void on_fault(int sig, siginfo_t* si, void* uc) {
    g_fault_note.entered++;
    Job* j = g_job.load(std::memory_order_acquire);
    uintptr_t a = (uintptr_t)si->si_addr, base = (uintptr_t)mem::ptr(0);
    long i = -1;
    // SEGV_ACCERR, or SEGV_MAPERR for a page never touched yet (older kernels, Helio G85 with Android 14):
    // inside the captured regions, which are mapped and readable, a fault can only be the protection
    if (j && (si->si_code == SEGV_ACCERR || si->si_code == SEGV_MAPERR) && a - base < 0x100000000ull) {
        i = j->index((uint32_t)(a - base));
        if (i >= 0) {
            take(j, (size_t)i);
            return;  // the write runs again, now on writable memory
        }
    }
    // (for the crash log: why it wasn't ours)
    g_fault_note.passed++;
    g_fault_note.job = j != nullptr;
    g_fault_note.code = si->si_code;
    g_fault_note.index = i;
    // not ours: as if this handler weren't there
    if (g_prev.sa_flags & SA_SIGINFO) {
        if (g_prev.sa_sigaction) return g_prev.sa_sigaction(sig, si, uc);
    } else if (g_prev.sa_handler != SIG_DFL && g_prev.sa_handler != SIG_IGN) {
        return g_prev.sa_handler(sig);
    }
    signal(sig, SIG_DFL);  // the fault happens again and ends the process
}

// ours first, then the one before (installed again if someone replaced it since)
void install_handler() {
    struct sigaction cur{};
    sigaction(SIGSEGV, nullptr, &cur);
    if ((cur.sa_flags & SA_SIGINFO) && cur.sa_sigaction == on_fault) return;
    struct sigaction sa{};
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    int rc = sigaction(SIGSEGV, &sa, &g_prev);
    struct sigaction now{};
    sigaction(SIGSEGV, nullptr, &now);
    Dl_info di{};
    const char* prev = dladdr((void*)g_prev.sa_sigaction, &di) && di.dli_sname ? di.dli_sname : "?";
    LOG("[savestate] fault handler installed (rc %d, now ours: %d, before: %s flags %X)", rc,
        (now.sa_flags & SA_SIGINFO) && now.sa_sigaction == on_fault, prev, (unsigned)g_prev.sa_flags);
}

// game frozen: protects guest memory for a capture; false if that isn't possible (nothing changed)
std::shared_ptr<Job> start() {
    auto j = std::make_shared<Job>();
    j->regs = regions();
    for (auto& g : j->regs) {
        j->first.push_back(j->n);
        j->n += g.size / kChunk;
    }
    j->st.reset(new std::atomic<uint8_t>[j->n]);
    for (size_t i = 0; i < j->n; i++) j->st[i].store(kPending, std::memory_order_relaxed);
    void* c = mmap(nullptr, j->n * kChunk, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (c == MAP_FAILED) return nullptr;
    j->copies = (uint8_t*)c;
    install_handler();
    g_last = j;
    // the chunks the game wrote early last time are copied now instead of faulting one by one when the
    // game continues (each fault costs ~70 us, a few hundred of them would land in the first frame)
    j->pre_index.assign(j->n, 0);
    if (g_prep_state.load(std::memory_order_acquire) == 2) {
        j->pre = g_prep;
        j->pre_size = g_prep ? g_prep_size : 0;
        g_prep = nullptr;
        g_prep_state = 0;
    }
    for (uint32_t a : g_hot) {
        long i = j->index(a);
        if (i < 0) continue;
        j->precopied++;
        if (j->pre && (size_t)j->precopied * kChunk <= j->pre_size) j->pre_index[i] = j->precopied;
        memcpy(j->copy_of((size_t)i), mem::ptr(a), kChunk);
        j->st[i].store(kCopied, std::memory_order_relaxed);
    }
    g_job.store(j.get(), std::memory_order_release);  // before protecting: render / audio threads still run
    // protect the rest (runs of chunks that weren't copied)
    for (size_t r = 0; r < j->regs.size(); r++) {
        size_t end = j->first[r] + j->regs[r].size / kChunk;
        for (size_t i = j->first[r]; i < end;) {
            if (j->st[i].load(std::memory_order_relaxed) != kPending) { i++; continue; }
            size_t k = i;
            while (k < end && j->st[k].load(std::memory_order_relaxed) == kPending) k++;
            if (mprotect(mem::ptr(j->addr(i)), (k - i) * kChunk, PROT_READ) != 0) {
                give_up(j.get());
                g_job.store(nullptr, std::memory_order_release);
                munmap(j->copies, j->n * kChunk);
                if (j->pre) munmap(j->pre, j->pre_size);
                return nullptr;
            }
            i = k;
        }
    }
    g_running = true;
    return j;
}

// background: the chunks compressed (in address order, on `threads` threads); null if it failed
std::shared_ptr<Captured> finish(const std::shared_ptr<Job>& j, unsigned threads) {
    auto c = std::make_shared<Captured>();
    c->regs = j->regs;
    std::vector<Captured::Piece> pieces(j->n);
    std::vector<uint8_t> present(j->n), written(j->n);  // written: copied, and changed since
    c->out.resize(threads);
    std::atomic<size_t> next{0};
    auto work = [&](unsigned t) {
        ZSTD_CCtx* cx = ZSTD_createCCtx();
        std::vector<uint8_t> tmp(ZSTD_compressBound(kChunk)), local(kChunk);
        std::vector<uint8_t>& o = c->out[t];
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < j->n;) {
            uint8_t* p = mem::ptr(j->addr(i));
            const uint8_t* src = nullptr;  // null: untouched (zero)
            bool fromCopy = false;
            uint8_t v = kPending;
            if (j->st[i].compare_exchange_strong(v, kBusy, std::memory_order_acq_rel)) {
                if (ever_touched(p, kChunk)) {  // (else zero)
                    memcpy(local.data(), p, kChunk);
                    src = local.data();
                }
                if (mprotect(p, kChunk, PROT_READ | PROT_WRITE) != 0) give_up(j.get());
                j->st[i].store(kDone, std::memory_order_release);
            } else {
                while (j->st[i].load(std::memory_order_acquire) == kBusy) sched_yield();
                src = j->copy_of(i);  // copied by a write before it, or at the stop
                fromCopy = true;
            }
            if (src) {
                const uint64_t* q = (const uint64_t*)src;
                bool zero = true;
                for (size_t k = 0; k < kChunk / 8 && zero; k++) zero = q[k] == 0;
                if (!zero) {
                    size_t n = cx ? ZSTD_compressCCtx(cx, tmp.data(), tmp.size(), src, kChunk, kZstdLevel) : 0;
                    Captured::Piece& pc = pieces[i];
                    pc.thread = t;
                    pc.offset = (uint32_t)o.size();
                    if (!cx || ZSTD_isError(n) || n >= kChunk) {
                        o.insert(o.end(), src, src + kChunk);
                        pc.comp = 0;
                    } else {
                        o.insert(o.end(), tmp.data(), tmp.data() + n);
                        pc.comp = (uint32_t)n;
                    }
                    present[i] = 1;
                }
            }
            if (fromCopy) {
                // still hot? (the live chunk may change while compared: only a hint for the next capture)
                written[i] = memcmp(src, p, kChunk) != 0;
                if (!j->pre_index[i]) madvise(j->copies + i * kChunk, kChunk, MADV_DONTNEED);  // its memory back
            }
        }
        ZSTD_freeCCtx(cx);
    };
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < threads; t++)
        pool.emplace_back([&, t] {
            setpriority(PRIO_PROCESS, 0, 10);  // this thread only (Linux): the game's threads go first
            work(t);
        });
    work(0);
    for (auto& th : pool) th.join();
    g_job.store(nullptr, std::memory_order_release);
    munmap(j->copies, j->n * kChunk);  // nothing is pending: no handler copies into it any more
    j->copies = nullptr;
    if (j->pre) munmap(j->pre, j->pre_size);
    j->pre = nullptr;
    g_hot.clear();
    for (size_t i = 0; i < j->n; i++)
        if (written[i]) g_hot.push_back(j->addr(i));
    if (j->failed) return nullptr;
    for (size_t i = 0; i < j->n; i++)
        if (present[i]) {
            c->addrs.push_back(j->addr(i));
            c->pieces.push_back(pieces[i]);
        }
    return c;
}
}  // namespace cow
#endif

// ---------------------------------------------------------------- slot files
// version 2: header, u64 size + zstd sections, u32 count + regions, then per chunk u32 address,
// u32 compressed size (0: 64 KiB as they are) and its bytes
bool write_slot(int slot, const Header& h0, const std::vector<uint8_t>& sections, const Captured& c) {
    Header h = h0;
    h.blocks = (uint32_t)c.addrs.size();
    h.raw_size = sections.size();
    std::vector<uint8_t> sc(ZSTD_compressBound(sections.size()));
    size_t sn = ZSTD_compress(sc.data(), sc.size(), sections.data(), sections.size(), kZstdLevel);
    if (ZSTD_isError(sn)) return false;
    std::string tmp = slot_path(slot, "tmp");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {  // the folder was deleted while the game ran: made again
        make_dirs(state_dir());
        f = fopen(tmp.c_str(), "wb");
    }
    if (!f) return false;
    uint64_t sn64 = sn;
    uint32_t nr = (uint32_t)c.regs.size();
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn64, 8, 1, f) == 1 && fwrite(sc.data(), 1, sn, f) == sn &&
              fwrite(&nr, 4, 1, f) == 1 && fwrite(c.regs.data(), sizeof(Region), nr, f) == nr;
    for (size_t i = 0; i < c.addrs.size() && ok; i++) {
        const Captured::Piece& pc = c.pieces[i];
        uint32_t rec[2] = {c.addrs[i], pc.comp};
        size_t n = pc.comp ? pc.comp : kChunk;
        ok = fwrite(rec, 8, 1, f) == 1 && fwrite(c.out[pc.thread].data() + pc.offset, 1, n, f) == n;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
    else unlink(tmp.c_str());
    return ok;
}

bool read_header(FILE* f, Header& h, std::string& why) {
    memset(&h, 0, sizeof h);
    if (fread(&h, kHeaderV1Size, 1, f) != 1 || memcmp(h.magic, kMagic, 8) != 0) { why = "not a save state"; return false; }
    if (h.version < 1 || h.version > kVersion || h.header_size < kHeaderV1Size || h.header_size > 4096) { why = "saved by another version"; return false; }
    // the fields added since (a newer header's unknown rest is skipped)
    size_t known = std::min<size_t>(h.header_size, sizeof h) - kHeaderV1Size;
    uint32_t headerSize = h.header_size;
    if (known && fread((uint8_t*)&h + kHeaderV1Size, known, 1, f) != 1) { why = "file is truncated"; return false; }
    if (headerSize > sizeof h && fseek(f, headerSize - sizeof h, SEEK_CUR) != 0) { why = "file is truncated"; return false; }
    if (h.cpu_size != sizeof(Cpu)) { why = "saved by an incompatible build"; return false; }
    if (h.game_id != game_id()) { why = "saved with a different game executable"; return false; }
    return true;
}

// the rest of a version 2 file (after the header): sections unpacked, memory chunks kept compressed
// (each one checked here, so restoring can't fail halfway)
bool read_slot_v2(FILE* f, Snapshot& s, std::string& why) {
    why = "file is truncated";
    uint64_t sn = 0;
    if (fread(&sn, 8, 1, f) != 1 || sn > (1u << 30) || s.h.raw_size > (1u << 30)) return false;
    std::vector<uint8_t> sc(sn);
    if (fread(sc.data(), 1, sn, f) != sn) return false;
    s.payload.resize(s.h.raw_size);
    if (ZSTD_decompress(s.payload.data(), s.payload.size(), sc.data(), sc.size()) != s.payload.size()) { why = "file is corrupt"; return false; }
    uint32_t nr = 0;
    if (fread(&nr, 4, 1, f) != 1 || nr > 64) return false;
    s.regs.resize(nr);
    if (fread(s.regs.data(), sizeof(Region), nr, f) != nr) return false;
    long at = ftell(f);
    if (at < 0 || fseek(f, 0, SEEK_END) != 0) return false;
    long end = ftell(f);
    if (end < at || fseek(f, at, SEEK_SET) != 0) return false;
    s.blob.resize((size_t)(end - at));
    if (fread(s.blob.data(), 1, s.blob.size(), f) != s.blob.size()) return false;
    Reader r(s.blob.data(), s.blob.size());
    std::vector<std::pair<uint32_t, ChunkRef>> list;
    list.reserve(s.h.blocks);
    for (uint32_t i = 0; i < s.h.blocks && r.ok; i++) {
        uint32_t addr = r.u32(), comp = r.u32();
        const uint8_t* p = r.p;
        if (comp > kChunk || !r.bytes(nullptr, comp ? comp : kChunk)) return false;
        list.push_back({addr, ChunkRef{p, comp}});
    }
    // every chunk must unpack (into a scratch buffer; the game keeps running meanwhile)
    std::atomic<size_t> next{0};
    std::atomic<bool> bad{false};
    auto work = [&] {
        std::vector<uint8_t> tmp(kChunk);
        for (size_t i; (i = next.fetch_add(1, std::memory_order_relaxed)) < list.size() && !bad;)
            if (!chunk_data(list[i].second, tmp.data())) bad = true;
    };
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 1; t < nt; t++) pool.emplace_back(work);
    work();
    for (auto& th : pool) th.join();
    if (bad || !r.ok || !s.parse_sections()) { why = "file is corrupt"; return false; }
    s.chunks.reserve(list.size());
    for (auto& [a, c] : list) s.chunks[a] = c;
    why.clear();
    return true;
}

// Version 1 file, read into the version 2 form: its blocks are unpacked one by one into a stream, the
// sections go to the payload, and each memory chunk is zstd-compressed as it comes (in batches on
// every core). The raw ~580 MB are never held at once (holding them got the game killed for low
// memory on a 6 GB phone after a few loads of an old state).
struct V1Stream {
    FILE* f;
    uint32_t blocks, next = 0;
    std::vector<uint8_t> raw, comp, scratch = std::vector<uint8_t>(scratch_size());
    size_t pos = 0;
    bool bad = false;
    bool fill() {
        if (next >= blocks) return false;
        uint32_t hdr[2];
        if (fread(hdr, 8, 1, f) != 1 || hdr[0] > kBlock) return bad = true, false;
        comp.resize(hdr[1] ? hdr[1] : hdr[0]);
        if (fread(comp.data(), 1, comp.size(), f) != comp.size()) return bad = true, false;
        raw.resize(hdr[0]);
        if (!hdr[1]) memcpy(raw.data(), comp.data(), hdr[0]);
        else if (decompress_block(raw.data(), hdr[0], comp.data(), hdr[1], scratch.data()) != hdr[0]) return bad = true, false;
        pos = 0;
        next++;
        return true;
    }
    bool read(void* out, size_t n) {
        uint8_t* o = (uint8_t*)out;
        while (n) {
            if (pos == raw.size() && !fill()) return false;
            size_t k = std::min(n, raw.size() - pos);
            if (o) memcpy(o, raw.data() + pos, k), o += k;
            pos += k;
            n -= k;
        }
        return true;
    }
    template <class T> bool get(T& v) { return read(&v, sizeof v); }
};

bool read_slot_v1(FILE* f, Snapshot& s, std::string& why) {
    why = "file is corrupt";
    V1Stream in{f, s.h.blocks};
    const unsigned nt = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
    constexpr size_t kBatch = 128;  // chunks per round (8 MiB)
    std::vector<uint8_t> batch(kBatch * kChunk);
    std::vector<uint32_t> addrs;
    struct Ref { uint32_t addr; size_t off; uint32_t comp; };
    std::vector<Ref> refs;
    std::vector<std::vector<uint8_t>> outs(nt);
    // compresses the batch's chunks on every core and appends them to the blob
    auto flush = [&](size_t n) {
        std::vector<std::vector<uint8_t>> res(n);
        std::atomic<size_t> next{0};
        auto work = [&] {
            ZSTD_CCtx* cx = ZSTD_createCCtx();
            for (size_t i; (i = next.fetch_add(1)) < n;) {
                std::vector<uint8_t>& o = res[i];
                o.resize(ZSTD_compressBound(kChunk));
                size_t k = cx ? ZSTD_compressCCtx(cx, o.data(), o.size(), batch.data() + i * kChunk, kChunk, kZstdLevel) : 0;
                if (!cx || ZSTD_isError(k) || k >= kChunk) o.clear();  // stored as it is
                else o.resize(k);
            }
            ZSTD_freeCCtx(cx);
        };
        std::vector<std::thread> pool;
        for (unsigned t = 1; t < nt; t++) pool.emplace_back(work);
        work();
        for (auto& th : pool) th.join();
        for (size_t i = 0; i < n; i++) {
            const bool stored = res[i].empty();
            refs.push_back({addrs[i], s.blob.size(), stored ? 0u : (uint32_t)res[i].size()});
            if (stored) s.blob.insert(s.blob.end(), batch.data() + i * kChunk, batch.data() + (i + 1) * kChunk);
            else s.blob.insert(s.blob.end(), res[i].begin(), res[i].end());
        }
        addrs.clear();
    };
    s.blob.reserve((size_t)s.h.raw_size / 2);  // address space; grows if needed
    bool mem = false;
    while (!mem) {
        uint32_t tag;
        uint64_t n;
        if (!in.get(tag) || !in.get(n)) break;
        if (tag != kSecMemory) {  // a section: kept as it is
            size_t at = s.payload.size();
            s.payload.resize(at + 12 + n);
            memcpy(&s.payload[at], &tag, 4);
            memcpy(&s.payload[at + 4], &n, 8);
            if (!in.read(&s.payload[at + 12], n)) return false;
            continue;
        }
        mem = true;
        uint32_t nr = 0;
        if (!in.get(nr) || nr > 64) return false;
        for (uint32_t r = 0; r < nr; r++) {
            Region g{};
            uint32_t present = 0;
            if (!in.get(g.base) || !in.get(g.size) || !in.get(present)) return false;
            s.regs.push_back(g);
            for (uint32_t k = 0; k < present; k++) {
                uint32_t idx;
                if (!in.get(idx) || !in.read(batch.data() + addrs.size() * kChunk, kChunk)) return false;
                addrs.push_back(g.base + idx * kChunk);
                if (addrs.size() == kBatch) flush(kBatch);
            }
        }
        if (!addrs.empty()) flush(addrs.size());
    }
    if (in.bad || !mem || !s.parse_sections()) return false;
    s.blob.shrink_to_fit();
    s.chunks.reserve(refs.size());
    for (auto& r : refs) s.chunks[r.addr] = ChunkRef{s.blob.data() + r.off, r.comp};
    s.h.blocks = (uint32_t)refs.size();
    s.converted = true;
    why.clear();
    return true;
}

// a snapshot read from a version 1 file, written back as version 2 (the next loads need little memory)
bool write_snapshot(int slot, const Snapshot& s) {
    Header h = s.h;
    h.version = kVersion;
    h.header_size = sizeof(Header);
    h.blocks = (uint32_t)s.chunks.size();
    h.raw_size = s.payload.size();
    std::vector<uint8_t> sc(ZSTD_compressBound(s.payload.size()));
    size_t sn = ZSTD_compress(sc.data(), sc.size(), s.payload.data(), s.payload.size(), kZstdLevel);
    if (ZSTD_isError(sn)) return false;
    std::vector<uint32_t> addrs;
    addrs.reserve(s.chunks.size());
    for (auto& [a, c] : s.chunks) addrs.push_back(a);
    std::sort(addrs.begin(), addrs.end());
    std::string tmp = slot_path(slot, "tmp");
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint64_t sn64 = sn;
    uint32_t nr = (uint32_t)s.regs.size();
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite(&sn64, 8, 1, f) == 1 && fwrite(sc.data(), 1, sn, f) == sn &&
              fwrite(&nr, 4, 1, f) == 1 && fwrite(s.regs.data(), sizeof(Region), nr, f) == nr;
    for (size_t i = 0; i < addrs.size() && ok; i++) {
        const ChunkRef& c = s.chunks.at(addrs[i]);
        uint32_t rec[2] = {addrs[i], c.comp};
        size_t n = c.comp ? c.comp : kChunk;
        ok = fwrite(rec, 8, 1, f) == 1 && fwrite(c.p, 1, n, f) == n;
    }
    ok = fclose(f) == 0 && ok;
    if (ok) ok = rename(tmp.c_str(), slot_path(slot).c_str()) == 0;
    else unlink(tmp.c_str());
    return ok;
}

std::shared_ptr<Snapshot> read_slot(int slot, std::string& why) {
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) { why = "empty"; return nullptr; }
    auto s = std::make_shared<Snapshot>();
    s->slot = slot;
    if (!read_header(f, s->h, why)) { fclose(f); return nullptr; }
    if (s->h.version >= 2) {
        bool ok = read_slot_v2(f, *s, why);
        fclose(f);
        return ok ? s : nullptr;
    }
    bool ok = read_slot_v1(f, *s, why);
    fclose(f);
    return ok ? s : nullptr;
}

// ---------------------------------------------------------------- save / load at the frame boundary
void put_section(Writer& w, uint32_t tag, const Writer& sec) {
    w.u32(tag);
    w.u64(sec.b.size());
    w.bytes(sec.b.data(), sec.b.size());
}

void save_allocs(Writer& w) {
    auto log = mem::runtime_alloc_log();
    w.u32(mem::runtime_top());
    w.u32((uint32_t)log.size());
    for (auto& a : log) w.pod(a);
}

bool check_allocs(Reader r, std::string& why) {
    uint32_t top = r.u32();
    (void)top;
    uint32_t n = r.u32();
    auto cur = mem::runtime_alloc_log();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        mem::AllocRec a = r.pod<mem::AllocRec>();
        if (i >= cur.size()) break;  // allocated later in the saved session: beyond our top
        const mem::AllocRec& b = cur[i];
        // (the allocating code address, a.tag, changes with every rebuild: only logged)
        if (a.tag != b.tag && i < 4) LOG("[savestate] runtime object #%u allocated by different code (rebuilt?)", i);
        if (a.addr != b.addr || a.size != b.size) {
            char buf[160];
            snprintf(buf, sizeof buf, "runtime objects are laid out differently (#%u: %08X+%X vs %08X+%X)", i, a.addr, a.size, b.addr,
                     b.size);
            why = buf;
            return false;
        }
    }
    return r.ok;
}

void save_dispatch(Writer& w) {
    auto v = dispatch::host_functions();
    w.u32((uint32_t)v.size());
    for (auto& [a, n] : v) {
        w.u32(a);
        w.str(n);
    }
}

// the host functions of the state: at the same addresses, or found by name and put there on loading
std::vector<std::pair<uint32_t, std::string>> g_dispatch_moves;
bool check_dispatch(Reader r, std::string& why) {
    std::unordered_map<uint32_t, std::string> cur;
    for (auto& [a, n] : dispatch::host_functions()) cur[a] = n;
    g_dispatch_moves.clear();
    uint32_t n = r.u32();
    for (uint32_t i = 0; i < n && r.ok; i++) {
        uint32_t a = r.u32();
        std::string name = r.str();
        auto it = cur.find(a);
        if (it != cur.end() && it->second == name) continue;
        if (!dispatch::host_by_name(name) && !dynload_resolve(name)) {
            why = "host function " + name + " is not known";
            return false;
        }
        g_dispatch_moves.push_back({a, name});
    }
    return r.ok;
}

void load_dispatch() {
    for (auto& [a, name] : g_dispatch_moves) {
        PpcFunc fn = dispatch::host_by_name(name);
        if (!fn) fn = dynload_resolve(name);
        dispatch::place_host(a, fn, name);
        dynload_place(a, name);
    }
    if (!g_dispatch_moves.empty()) LOG("[savestate] %zu host functions put at the state's addresses", g_dispatch_moves.size());
    g_dispatch_moves.clear();
}

std::string area_label(const char* stage) {
    static const std::map<std::string, std::string> names = {
        {"sea", "Great Sea"}, {"LinkRM", "Link's House"}, {"MajyuE", "Forsaken Fortress"}, {"M_NewD2", "Dragon Roost Cavern"},
        {"kindan", "Forbidden Woods"}, {"Siren", "Tower of the Gods"}, {"Asoko", "Tetra's Ship"},
    };
    auto it = names.find(stage);
    return it == names.end() ? std::string(stage) : it->second;
}

// background part of a save: compress (copy-on-write captures) and write the slot file
void finish_save(int slot, bool quiet, Header h, std::shared_ptr<Writer> payload, std::shared_ptr<Captured> memory,
                 std::shared_ptr<void> job, std::string stage) {
    struct Done { ~Done() { g_writes--; } } done;  // on every way out
    auto t1 = std::chrono::steady_clock::now();
    std::string what;
#ifndef __APPLE__
    if (job) {
        setpriority(PRIO_PROCESS, 0, 10);  // this thread only (Linux): the game's threads go first
        auto j = std::static_pointer_cast<cow::Job>(job);
        static const unsigned nt = [] {
            const char* e = getenv("WWHD_STATE_THREADS");
            return std::max(1u, std::min(8u, e ? (unsigned)atoi(e) : 2u));
        }();
        memory = cow::finish(j, nt);
        char buf[256];
        snprintf(buf, sizeof buf, "; %u copied at the stop, %u faults, %u chunks copied (%.1f MB), %.1f ms in faults, longest %.2f ms; %zu hot next time",
                 j->precopied, j->faults.load(), j->copied.load(), j->copied.load() * (double)kChunk / 1048576.0, j->fault_ns.load() / 1e6,
                 j->max_fault_ns.load() / 1e6, cow::g_hot.size());
        what = buf;
        cow::g_running = false;
        if (!memory) {
            LOG("[savestate] %s: capture failed (protection change failed)%s", slot_label(slot).c_str(), what.c_str());
            if (!quiet) message("%s: not saved (out of memory mappings)", slot_label(slot, true).c_str());
            return;
        }
        LOG("[savestate] %s: captured %.1f MB of memory as %.1f MB in the background in %.0f ms%s", slot_label(slot).c_str(),
            memory->addrs.size() * (double)kChunk / 1048576.0, memory->bytes() / 1048576.0,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(), what.c_str());
    }
#else
    (void)job;
#endif
    bool ok;
    {
        std::lock_guard<std::mutex> io(g_io);
        ok = write_slot(slot, h, payload->b, *memory);
    }
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
    struct stat st{};
    stat(slot_path(slot).c_str(), &st);
    LOG("[savestate] %s: %s (%.1f MB on disk, %.0f ms)", slot_label(slot).c_str(), ok ? "written" : "WRITE FAILED", st.st_size / 1048576.0, ms);
    if (quiet && ok) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_message = ok ? "Saved to " + slot_label(slot) + (stage.empty() ? "" : " (" + area_label(stage.c_str()) + ")")
                   : slot_label(slot, true) + ": write failed";
    g_message_time = std::chrono::steady_clock::now();
}

// true when done (saved or failed for good); false to retry on the next frame. An auto save (quiet)
// gives the game threads less time to reach their waits and doesn't retry: it comes again later.
bool do_save(int slot, bool quiet) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    auto ms_since = [](std::chrono::steady_clock::time_point t) {
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
    };
    bool parked = quiet ? threads::quiesce(40, busy, 1, 10) : threads::quiesce(250, busy, 1, 30);
    if (!parked) {
        threads::thaw();
        if (quiet) {
            LOG("[savestate] %s: not saved (game busy:%s), %.1f ms", slot_label(slot).c_str(), busy.c_str(), ms_since(t0));
            return true;
        }
        if (++g_attempts < 30) return false;
        message("%s: not saved (game busy:%s)", slot_label(slot, true).c_str(), busy.c_str());
        return true;
    }
    double tQuiesce = ms_since(t0);
    auto t = std::chrono::steady_clock::now();
    gx2_ss_drain();
    double tDrain = ms_since(t);
    t = std::chrono::steady_clock::now();
    Writer threads_w;
    if (!threads_ss_save(threads_w, why)) {
        threads::thaw();
        if (quiet) {
            LOG("[savestate] %s: not saved (%s)", slot_label(slot).c_str(), why.c_str());
            return true;
        }
        if (++g_attempts < 30) return false;
        message("%s: not saved (%s)", slot_label(slot, true).c_str(), why.c_str());
        return true;
    }
    auto payload = std::make_shared<Writer>();  // the sections (memory: below)
    put_section(*payload, kSecThreads, threads_w);
    Writer w;
    mem_ss_save(w);
    put_section(*payload, kSecHeaps, w);
    w = Writer();
    fs_ss_save(w);
    put_section(*payload, kSecFiles, w);
    w = Writer();
    ax_ss_save(w);
    put_section(*payload, kSecAudio, w);
    w = Writer();
    gx2_ss_save(w);
    put_section(*payload, kSecGx2, w);
    w = Writer();
    save_allocs(w);
    put_section(*payload, kSecAllocs, w);
    w = Writer();
    save_dispatch(w);
    put_section(*payload, kSecDispatch, w);
    double tSections = ms_since(t);
    t = std::chrono::steady_clock::now();
    // memory: write-protected and captured in the background, or (macOS, WWHD_STATE_COW=0, or if
    // protecting fails) compressed now
    std::shared_ptr<void> job;
    std::shared_ptr<Captured> memory;
#ifndef __APPLE__
    if (cow::enabled()) job = cow::start();
#endif
    if (!job) memory = capture_memory();
    double tMemory = ms_since(t);
    Header h{};
    memcpy(h.magic, kMagic, 8);
    h.version = kVersion;
    h.header_size = sizeof(Header);
    h.created = (uint64_t)time(nullptr);
    std::string stage = stage_name();
    snprintf(h.area, sizeof h.area, "%s", stage.c_str());
    build_uuid(h.build);
    h.game_id = game_id();
    h.cpu_size = sizeof(Cpu);
    h.controller = input::pro_controller() ? 2 : 1;
    threads::thaw();
    LOG("[savestate] %s: game stopped %.1f ms (threads parked %.1f, renderer drained %.1f, sections %.1f, memory %s %.1f), stage %s",
        slot_label(slot).c_str(), ms_since(t0), tQuiesce, tDrain, tSections, job ? "protected" : "compressed", tMemory, stage.c_str());
    if (memory)
        LOG("[savestate] %s: captured %.1f MB of memory as %.1f MB", slot_label(slot).c_str(),
            memory->addrs.size() * (double)kChunk / 1048576.0, memory->bytes() / 1048576.0);
    gfx::request_tv_dump(slot_path(slot, "png"), 0);
    // compress (copy on write) and write in the background; the game continues
    g_writes++;
    std::thread(finish_save, slot, quiet, h, payload, memory, job, stage).detach();
    return true;
}

bool do_load(const std::shared_ptr<Snapshot>& s) {
    std::string busy, why;
    auto t0 = std::chrono::steady_clock::now();
    threads_ss_targets(s->section(kSecThreads));
    // the first frames: threads park only where they were saved (the fast way); then anywhere (a
    // thread parked elsewhere gets its saved call chain rebuilt)
    if (!threads::quiesce(250, busy, g_attempts < 3 ? 2 : 1, 0)) {
        threads::thaw();
        if (++g_attempts < 30) return false;
        message("%s: not loaded (game busy:%s)", slot_label(s->slot, true).c_str(), busy.c_str());
        return true;
    }
    gx2_ss_drain();
    g_check = s.get();
    g_check_serial++;
    bool ok = check_allocs(s->section(kSecAllocs), why) && check_dispatch(s->section(kSecDispatch), why) &&
              mem_ss_check(s->section(kSecHeaps), why) && ax_ss_check(s->section(kSecAudio), why) &&
              gx2_ss_check(s->section(kSecGx2), why);
    bool layout_ok = ok;
    ok = ok && threads_ss_check(s->section(kSecThreads), why);
    g_check = nullptr;
    bool slow = ok && threads_ss_rebuild_needed();
    if (slow && g_attempts < 3) {  // a few frames for the threads to get where they were saved
        threads::thaw();
        if (++g_attempts == 1) LOG("[savestate] %s: waiting for the game threads (%s)", slot_label(s->slot).c_str(), why.c_str());
        return false;
    }
    if (!ok) {
        threads::thaw();
        // threads at other waits right now may be where they were saved on a later frame
        if (layout_ok && ++g_attempts < 30) {
            if (g_attempts == 1) LOG("[savestate] %s: waiting for the game threads (%s)", slot_label(s->slot).c_str(), why.c_str());
            return false;
        }
        message("%s: cannot load here (%s)", slot_label(s->slot, true).c_str(), why.c_str());
        return true;
    }
    if (slow) {
        LOG("[savestate] %s: loading with rebuilt threads (%s)", slot_label(s->slot).c_str(), why.c_str());
        threads_ss_retire();  // before anything changes: they leave their host stacks
    }
    restore_memory(*s);
    load_dispatch();
    Reader r = s->section(kSecAllocs);
    mem::raise_runtime_top(r.u32());
    r = s->section(kSecHeaps);
    mem_ss_load(r);
    r = s->section(kSecFiles);
    fs_ss_load(r);
    r = s->section(kSecAudio);
    ax_ss_load(r);
    r = s->section(kSecGx2);
    gx2_ss_load(r);
    interp::ss_reset();
    r = s->section(kSecThreads);
    threads_ss_load(r);  // last: wakes the parked threads (they continue after the thaw)
    threads_ss_rebuild();
    threads::thaw();
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::string area = s->h.area[0] ? " (" + area_label(s->h.area) + ")" : "";
    // a state saved while the controls acted as a Pro Controller (older builds): the controls stay
    // the GamePad, and the game switches to it (service; else it waits for a Pro Controller)
    if (s->h.controller == 2 && !input::pro_controller()) {
        g_to_gamepad = true;
        message("Loaded %s%s (saved with the Pro Controller: switched to the GamePad)", slot_label(s->slot).c_str(), area.c_str());
    } else {
        message("Loaded %s%s", slot_label(s->slot).c_str(), area.c_str());
    }
    memw::mark_all();  // guest memory replaced: the renderer copies everything again
    LOG("[savestate] %s: restored in %.1f ms", slot_label(s->slot).c_str(), ms);
    return true;
}

// test aid: WWHD_STATE_SAVE_AT=frame:slot,...  WWHD_STATE_LOAD_AT=frame:slot,... (TV frames)
struct Timed { uint64_t frame; int slot; };
std::vector<Timed> parse_timed(const char* var) {
    std::vector<Timed> v;
    const char* e = getenv(var);
    unsigned long long f;
    int s, n;
    while (e && sscanf(e, "%llu:%d%n", &f, &s, &n) == 2) {
        v.push_back({f, s});
        e += n;
        if (*e != ',') break;
        e++;
    }
    return v;
}

// a stage is loaded that isn't the title screen or the file select (auto and leave saves only there)
bool in_play() {
    std::string st = stage_name();
    return !st.empty() && st != "sea_T" && st != "Name";
}

// the auto slot to write next: an empty one, else the one saved longest ago
int older_auto_slot() {
    int best = kSlots + 1;
    time_t bestTime = 0;
    for (int slot = kSlots + 1; slot <= kSlots + kAutoSlots; slot++) {
        struct stat st{};
        if (stat(slot_path(slot).c_str(), &st) != 0) return slot;
        if (slot == kSlots + 1 || st.st_mtime < bestTime) best = slot, bestTime = st.st_mtime;
    }
    return best;
}

}  // namespace

int front_screen() {
    if (gfx::frame_count() == 0) return 4;  // (the game has drawn: its memory is set up)
    std::string st = stage_name();
    if (st.empty()) return 4;  // the logos (the title is the first stage)
    if (st == "sea_T") return 1;
    if (st != "Name") return 0;
    // the file select sets the next stage to itself; leaving it (dComIfGs_gameStart) sets the save's:
    // then the new game's opening (the start stage stays "Name" until the game's first stage loads)
    const char* next = (const char*)mem::ptr(kStageInfo + kNextStageOff);
    return strncmp(next, "Name", 8) == 0 ? 2 : 3;
}

uint32_t snap_ld32(uint32_t ea) {
    if (!g_check) return ld32(ea);
    uint32_t base = ea & ~(kChunk - 1);
    auto it = g_check->chunks.find(base);
    if (it == g_check->chunks.end()) {
        for (auto& g : g_check->regs)
            if (ea - g.base < g.size) return 0;
        return ld32(ea);  // not part of the snapshot (code, host-only memory)
    }
    // the chunk unpacked once while it's read (the checks read a few words from each of a few chunks)
    static uint64_t cachedSerial = 0;
    static uint32_t cachedBase = 0;
    static std::vector<uint8_t> buf(kChunk);
    if (cachedSerial != g_check_serial || cachedBase != base) {
        if (!chunk_data(it->second, buf.data())) memset(buf.data(), 0, kChunk);
        cachedSerial = g_check_serial;
        cachedBase = base;
    }
    uint32_t v;
    memcpy(&v, buf.data() + (ea - base), 4);
    return __builtin_bswap32(v);
}

SlotInfo slot_info(int slot) {
    SlotInfo info;
    if (slot < 1 || slot > kAllSlots) return info;
    FILE* f = fopen(slot_path(slot).c_str(), "rb");
    if (!f) return info;
    Header h;
    std::string why;
    info.used = true;
    info.compatible = read_header(f, h, why);
    fclose(f);
    if (info.compatible || memcmp(h.magic, kMagic, 8) == 0) {
        time_t t = (time_t)h.created;
        struct tm tmv;
        localtime_r(&t, &tmv);
        char buf[64];
        strftime(buf, sizeof buf, "%b %d %H:%M:%S", &tmv);
        info.when = buf;
        h.area[sizeof h.area - 1] = 0;
        if (h.area[0]) info.area = area_label(h.area);
        info.controller = (int)h.controller;
    }
    return info;
}

void request_save(int slot) {
    if (slot < 1 || slot > kSlots) return;
    g_save_quiet = false;
    g_save_req = slot;
}

void request_load(int slot) {
    if (slot < 1 || slot > kAllSlots) return;
    if (g_loading.exchange(true)) return;
    std::thread([slot] {
        auto t0 = std::chrono::steady_clock::now();
        std::string why;
        std::shared_ptr<Snapshot> s;
        {
            std::lock_guard<std::mutex> io(g_io);
            s = read_slot(slot, why);
        }
        if (!s) {
            message("%s: %s", slot_label(slot, true).c_str(), why.c_str());
            g_loading = false;
            set_start_hold(false);  // the game starts as usual
            return;
        }
        if (s->converted) {  // an old state: from now on in the new form
            std::lock_guard<std::mutex> io(g_io);
            bool ok = write_snapshot(slot, *s);
            LOG("[savestate] slot %d: converted to version %u %s", slot, kVersion, ok ? "and written" : "but NOT written");
        }
        LOG("[savestate] slot %d: read %.1f MB (%zu memory chunks, version %u) in %.0f ms", slot,
            (s->payload.size() + s->blob.size()) / 1048576.0, s->chunks.size(), s->h.version,
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
        std::lock_guard<std::mutex> lk(g_mu);
        g_load_ready = s;
        g_hold_cv.notify_all();
    }).detach();
}

void set_start_hold(bool on) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hold = on ? 1 : 0;
    g_hold_cv.notify_all();
}
int start_hold_state() { return g_hold.load(); }
int writes_pending() { return g_writes.load(); }

void set_auto_interval(int minutes) {
    g_auto_minutes = std::max(0, minutes);
    LOG("[savestate] auto save %s", minutes > 0 ? ("every " + std::to_string(minutes) + " min").c_str() : "off");
}
int auto_interval() { return g_auto_minutes.load(); }

void save_on_leave() {
    if (g_loading || g_save_req.load() || !in_play()) return;  // the previous last session stays
    auto t0 = std::chrono::steady_clock::now();
    g_save_quiet = true;
    g_save_req = kLastSlot;
    while (g_save_req.load() == kLastSlot && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1))
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    int slot = kLastSlot;
    bool cancelled = g_save_req.compare_exchange_strong(slot, 0);
    LOG("[savestate] leaving the app: %s after %.0f ms", cancelled ? "NOT saved (the game didn't get there)" : "state taken",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

int fault_note(char* buf, size_t n) {
#ifndef __APPLE__
    const auto& f = cow::g_fault_note;
    if (!f.entered && !cow::g_job.load()) return 0;
    return snprintf(buf, n, "  save state fault handler: entered %d, passed on %d (capture running %d, si_code %d, chunk %ld), capture now %d\n",
                    f.entered, f.passed, f.job, f.code, (long)f.index, cow::g_job.load() != nullptr);
#else
    (void)buf;
    (void)n;
    return 0;
#endif
}

void prepare_host_write(uint32_t addr, uint32_t size) {
#ifndef __APPLE__
    cow::Job* j = cow::g_job.load(std::memory_order_acquire);
    if (!j || !size) return;
    for (uint64_t a = addr & ~(uint64_t)(kChunk - 1); a < (uint64_t)addr + size; a += kChunk) {
        long i = j->index((uint32_t)a);
        if (i >= 0) cow::take(j, (size_t)i);
    }
#else
    (void)addr;
    (void)size;
#endif
}

std::string last_message() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_message.empty() || std::chrono::steady_clock::now() - g_message_time > std::chrono::seconds(4)) return "";
    return g_message;
}

// the game's controller manager (101F5088): +0x1D0 its mode (0 Pro Controller, 1 GamePad; 3 / 4 while
// starting); 02618094(manager, mode) switches it (the title's controller question calls it with 1)
const release::Data kControllerMgr{0x101F5088};
const release::Code kSetControllerMode{0x02618094};

void service(Cpu* c) {
    if (g_to_gamepad && !g_loading) {
        g_to_gamepad = false;
        uint32_t mgr = ld32(kControllerMgr);
        if (mgr && ld32(mgr + 0x1D0) == 0) {
            uint32_t ok = guest_call(c, kSetControllerMode, {mgr, 1});
            LOG("[savestate] Pro Controller state: game switched to the GamePad (%u, mode now %u)", ok, ld32(mgr + 0x1D0));
        }
    }
    static const std::vector<Timed> save_at = parse_timed("WWHD_STATE_SAVE_AT"), load_at = parse_timed("WWHD_STATE_LOAD_AT");
    if (!save_at.empty() || !load_at.empty()) {
        static uint64_t last = 0;
        uint64_t f = gfx::frame_count();
        for (auto& t : save_at)
            if (t.frame > last && t.frame <= f) request_save(t.slot);
        for (auto& t : load_at)
            if (t.frame > last && t.frame <= f) request_load(t.slot);
        last = f;
    }
    // test aid: WWHD_STATE_DUMP=n dumps the n TV frames after each save/load (state_<save|load><slot>_<k>.png)
    static const int dump = getenv("WWHD_STATE_DUMP") ? atoi(getenv("WWHD_STATE_DUMP")) : 0;
#ifndef __APPLE__
    if (cow::g_running) return;  // the last save's memory is still being captured: saves and loads wait
#endif
    // the app's picker at start: wait here (before the intro) for its choice, once the shader cache
    // replay is done (the game's first draw starts it, so it can't wait before that)
    if (g_hold.load()) {
        uint64_t p[4];
        gfx::cache_progress(p);
        if (p[0] == 2) {
            BlockingScope b;  // other threads keep their cores
            std::unique_lock<std::mutex> lk(g_mu);
            if (g_hold == 1 && !g_load_ready) {
                g_hold = 2;
                LOG("[savestate] the game waits at the start for the app's choice");
                // meanwhile the renderer warms its shader cache (there are no frames to do it in)
                while (!g_hold_cv.wait_for(lk, std::chrono::milliseconds(20), [] { return g_hold == 0 || g_load_ready; })) {
                    lk.unlock();
                    gx2::warm_while_held();
                    lk.lock();
                }
            }
        }
    }
    // auto save: every g_auto_minutes of play (time in the menu or in the background doesn't count),
    // into the older of the auto slots, while a stage is loaded (not the title screen or file select)
    {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double dt = std::min(std::chrono::duration<double>(now - last).count(), 0.25);
        last = now;
        int minutes = g_auto_minutes.load();
        g_auto_played = minutes ? g_auto_played + dt : 0;
        static const double scale = getenv("WWHD_STATE_AUTO_SECONDS") ? 1.0 : 60.0;  // test aid: the interval in seconds
        if (minutes && g_auto_played >= minutes * scale && !g_save_req && !g_loading) {
            g_auto_played = 0;
            if (in_play()) {
                g_save_quiet = true;
                g_save_req = older_auto_slot();
            }
        }
    }
    if (int slot = g_save_req.load()) {
#ifndef __APPLE__
        if (!cow::prepared()) return;  // a frame or so
#endif
        if (do_save(slot, g_save_quiet)) {
            g_save_req = 0;
            g_attempts = 0;
            for (int k = 1; k <= dump; k++)
                gfx::request_tv_dump("state_save" + std::to_string(slot) + "_" + std::to_string(k) + ".png", k);
        }
        return;
    }
    std::shared_ptr<Snapshot> s;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        s = g_load_ready;
    }
    if (s && do_load(s)) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            g_load_ready.reset();
        }
        g_loading = false;
        g_attempts = 0;
        g_auto_played = 0;  // the next auto save a full interval after a load
        set_start_hold(false);  // loaded, or refused: the game goes on from here
        for (int k = 1; k <= dump; k++)
            gfx::request_tv_dump("state_load" + std::to_string(s->slot) + "_" + std::to_string(k) + ".png", k);
    }
}

}  // namespace ss
