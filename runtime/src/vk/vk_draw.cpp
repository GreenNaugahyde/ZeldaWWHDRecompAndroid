#include <bitset>
#include <chrono>
extern "C" uint64_t g_shader_state_gen;  // gx2_core.cpp: bumped by shader-relevant register changes
extern "C" uint64_t g_draw_state_gen;    // gx2_core.cpp: bumped by any non-data register change (draw fast path)
// Draw calls on Vulkan: shader translation (Cemu decompiler, GLSL -> SPIR-V), pipelines, resource
// binding and primitive submission. Follows gfx/metal_draw.mm; binding conventions are those of
// the Vulkan GLSL the decompiler emits (one descriptor set per stage, as in Cemu's Vulkan renderer).
#include "../packs.h"
#include "Cafe/HW/Latte/Core/FetchShader.h"
#include "Cafe/HW/Latte/Core/LatteCachedFBO.h"
#include "Cafe/HW/Latte/Core/LatteShader.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LegacyShaderDecompiler/LatteDecompiler.h"
#include "gx2/gx2.h"
#include "gx2/gx2_cmd.h"
#include "platform.h"
#include "runtime.h"
#include "util/helpers/StringBuf.h"
#include "vk.h"
#include "vk_record.h"
#include "../mem_writes.h"
#include "flat_table.h"

#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <ctime>
#include <deque>
#include <map>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
#include <unistd.h>
#include <zlib.h>
#if defined(__aarch64__) && defined(__linux__)
#include <arm_acle.h>
#include <sys/auxv.h>
#endif

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& decompilerOutput);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash fsHash, uint32* contextRegister,
                                                           uint32* fsProgramCode, uint32 fsProgramSize);
void LatteShader_calculateFSKey(LatteFetchShader* fetchShader);

using namespace Latte;

namespace gfx {
static float bitsf_(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

// debug: WWHD_LOG_FRAME=n or n-m logs every draw of those frames
static uint64_t g_log_frame = ~0ull, g_log_frame_end = 0;
static bool g_log_init = [] {
    if (const char* e = getenv("WWHD_LOG_FRAME")) {
        g_log_frame = strtoull(e, (char**)&e, 10);
        g_log_frame_end = *e == '-' ? strtoull(e + 1, nullptr, 10) : g_log_frame;
    }
    return true;
}();
// capture: the next frame's draw log goes to captures/<time>/draws.log and every full-screen pass
// (post-processing / deferred lighting) has its target dumped there
static std::atomic<bool> g_capture_requested{false};
static uint64_t g_capture_frame = ~0ull;
static std::string g_capture_dir;
static FILE* g_capture_log = nullptr;
void request_capture() { g_capture_requested = true; }
static bool capturing() { return R.frame == g_capture_frame; }
bool log_this_frame() { return capturing() || (R.frame >= g_log_frame && R.frame <= g_log_frame_end); }
static void dlog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void dlog(const char* fmt, ...) {
    char line[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (capturing() && g_capture_log) fprintf(g_capture_log, "%s\n", line);
    else LOG("%s", line);
}
#define DLOG(...) do { if (log_this_frame()) dlog(__VA_ARGS__); } while (0)

// called from swap() after R.frame advanced; returns the capture directory when this frame is captured
const char* capture_begin_frame() {
    if (g_capture_log) { fclose(g_capture_log); g_capture_log = nullptr; }
    static uint64_t envFrame = getenv("WWHD_CAPTURE") ? strtoull(getenv("WWHD_CAPTURE"), nullptr, 10) : ~0ull;  // scripted capture
    if (!g_capture_requested.exchange(false) && R.frame != envFrame) return nullptr;
    char dir[64];
    time_t t = time(nullptr);
    strftime(dir, sizeof dir, "captures/%Y%m%d-%H%M%S", localtime(&t));
    mkdir("captures", 0755);
    mkdir(dir, 0755);
    g_capture_dir = dir;
    g_capture_frame = R.frame;
    g_capture_log = fopen((g_capture_dir + "/draws.log").c_str(), "w");
    LOG("[gfx] capturing frame %llu to %s", (unsigned long long)R.frame, dir);
    return g_capture_dir.c_str();
}

// fast 64-bit hash over 8-byte words (tail handled bytewise)
static uint64_t hash_bytes(const void* data, size_t n, uint64_t h = 0x9E3779B97F4A7C15ull) {
    const uint8_t* p = (const uint8_t*)data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h = (h ^ v) * 0xFF51AFD7ED558CCDull;
        h ^= h >> 32;
    }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001B3ull;
    return h ^ (h >> 29);
}

// bytes copied to the GPU per category since the last report (vertex data, uniform blocks, uniform variables)
static uint64_t g_bytes_vtx, g_bytes_vtx_shared, g_bytes_ubo, g_bytes_vars, g_bytes_vtx_reused, g_bytes_ubo_reused;
static uint64_t g_stale_reuses;  // tracked reuses a comparison found outdated (copy_tracked)
static uint64_t g_keep_bytes = 0, g_keep_count = 0, g_keep_reused = 0;  // kept vertex copies (kept_vertex_buffer)
static uint64_t g_fast_draws, g_slow_draws;  // draws through the fast path (state reused) and the full one

// Guest data copied for draws, per submission by address and size: a draw whose data is
// byte-identical to an earlier copy in the same submission reuses it (the same mesh or uniform block
// drawn many times). The comparison reads cached upload memory, much cheaper than another copy.
struct SubmissionCopies {
    struct Copy { Upload u; uint32_t gen; };
    FlatTable<Copy> map;
    uint64_t serial = ~0ull;
};
static Upload copy_deduped(SubmissionCopies& c, const void* src, uint32_t addr, uint32_t size, VkDeviceSize align,
                           uint64_t& copied, uint64_t& reused) {
    command_buffer();
    if (R.cmdSerial != c.serial) {  // transient memory is only valid within one submission
        c.map.clear();
        c.serial = R.cmdSerial;
    }
    uint64_t key = (uint64_t)addr << 32 | size;
    if (R.uploadCached) {
        SubmissionCopies::Copy* it = c.map.find(key);
        if (it && memcmp(it->u.ptr, src, size) == 0) {
            reused += size;
            return it->u;
        }
    }
    Upload u = upload_alloc(size, align);
    memcpy(u.ptr, src, size);
    copied += size;
    if (R.uploadCached) c.map[key] = {u, 0};
    return u;
}

// The same, but a copy is reused while the game hasn't written its pages since it was made
// (mem_writes.h: the flushes the console needs) instead of comparing the bytes, which cost the
// render thread ~10% in busy views (tens of MB a frame). Each copy is still compared on one frame
// in 16 (spread by address), counting what the tracking missed. WWHD_TRACK_WRITES=0: compare always.
static const bool g_track_writes = [] { const char* e = getenv("WWHD_TRACK_WRITES"); return !e || atoi(e) != 0; }();
static const bool g_track_ubo_writes = g_track_writes && getenv("WWHD_TRACK_UBO_WRITES") != nullptr;
static Upload copy_tracked(SubmissionCopies& c, uint32_t addr, uint32_t size, VkDeviceSize align, uint64_t& copied, uint64_t& reused) {
    command_buffer();
    if (R.cmdSerial != c.serial) {  // transient memory is only valid within one submission
        c.map.clear();
        c.serial = R.cmdSerial;
    }
    const uint8_t* src = mem::ptr(addr);
    uint64_t key = (uint64_t)addr << 32 | size;
    SubmissionCopies::Copy* it = c.map.find(key);
    if (it && memw::unchanged_since(addr, size, it->gen)) {
        // the check reads the copy back: only from cached upload memory (uncached reads are ~100x
        // slower; original project 3563e7f)
        bool verify = R.uploadCached && ((addr >> 6) + R.frame) % 16 == 0;
        if (!verify || memcmp(it->u.ptr, src, size) == 0) {
            reused += size;
            return it->u;
        }
        g_stale_reuses++;
        static int logged = 0;
        if (logged < 40) {
            logged++;
            uint32_t at = 0;
            while (at < size && ((const uint8_t*)it->u.ptr)[at] == src[at]) at++;
            LOG("[gfx] write tracking: %s %08X+%X outdated (first difference at +%X, frame %llu)", &copied == &g_bytes_ubo ? "uniform block" : "vertices",
                addr, size, at, (unsigned long long)R.frame);
        }
    }
    uint32_t gen = memw::now();  // before the copy: a write during it makes the next use copy again
    Upload u = upload_alloc(size, align);
    memcpy(u.ptr, src, size);
    copied += size;
    c.map[key] = {u, gen};
    return u;
}

// shader programs rarely change in place: cache their hash per address, revalidated once per frame
// with a sample of the program (its start, middle and end), fully every 32 frames or when the
// sample changed (as textures are checked)
struct ProgramHash {
    uint32_t size;
    uint64_t hash, sample;
    uint64_t frame, fullFrame;
};
static std::unordered_map<uint32_t, ProgramHash> g_program_hashes;
static uint64_t program_sample(const uint8_t* p, uint32_t size) {
    if (size <= 192) return hash_bytes(p, size);
    uint64_t h = hash_bytes(p, 64);
    h = hash_bytes(p + (size / 2 & ~7u), 64, h);
    return hash_bytes(p + size - 64, 64, h);
}
// The game uses ~1600 programs a frame: revalidating each through the map every frame cost ~7% of
// the render thread (lookups that miss the CPU caches). A hash stays valid here while the game hasn't
// written the program's pages since (mem_writes.h, as vertex buffers); each program still goes
// through the map's check on one frame in 16 (spread by address), counting what the tracking missed.
struct ProgramHot { uint32_t addr = 0, size = 0, gen = 0; uint64_t frame = ~0ull, hash = 0; };
static ProgramHot g_program_hot[4096];
// two slots per address (~1600 programs a frame collide often in one): its own, else the less recently used
static ProgramHot& program_hot(uint32_t addr) {
    ProgramHot* p = &g_program_hot[((addr >> 8) * 0x9E3779B1u >> 20) & ~1u];
    if (p[0].addr == addr) return p[0];
    if (p[1].addr == addr) return p[1];
    return p[0].frame + 1 < p[1].frame + 1 ? p[0] : p[1];  // (never used: ~0 + 1 = 0)
}
static uint64_t g_program_map_lookups = 0, g_program_lookups = 0, g_program_stale = 0;
static uint64_t program_hash(uint32_t addr, uint32_t size) {
    ProgramHot& hot = program_hot(addr);
    g_program_lookups++;
    const bool same = hot.addr == addr && hot.size == size;
    if (same && hot.frame == R.frame) return hot.hash;
    const bool verify = ((addr >> 8) + R.frame) % 16 == 0;
    if (same && g_track_writes && !verify && memw::unchanged_since(addr, size, hot.gen)) {
        hot.frame = R.frame;
        return hot.hash;
    }
    g_program_map_lookups++;
    const uint32_t gen = memw::now();  // before reading: a write during it makes the next use check again
    auto& e = g_program_hashes[addr];
    if (!(e.size == size && e.frame == R.frame)) {
        const uint8_t* p = (const uint8_t*)mem::ptr(addr);
        uint64_t sample = program_sample(p, size);
        if (e.size != size || sample != e.sample || R.frame - e.fullFrame >= 32) {
            e.size = size;
            e.hash = hash_bytes(p, size);
            e.sample = sample;
            e.fullFrame = R.frame;
        }
        e.frame = R.frame;
    }
    if (same && e.hash != hot.hash && memw::unchanged_since(addr, size, hot.gen)) g_program_stale++;
    hot = {addr, size, gen, R.frame, e.hash};
    return e.hash;
}

// ---------------------------------------------------------------- compile completion
// Compiles publish their result and signal this; wait_compiled sleeps on it instead of polling
// (from the original project): it wakes as soon as the compile finishes.
static std::mutex g_compile_m;
static std::condition_variable g_compile_cv;
static void compile_done() {
    { std::lock_guard<std::mutex> lk(g_compile_m); }
    g_compile_cv.notify_all();
}

// ---------------------------------------------------------------- background compiles
// Shader modules and pipelines are created on worker threads (WWHD_SYNC_SHADERS=1 creates them
// inline instead); draws that need one still compiling wait a bounded time, then are skipped.
// The game's own compiles (a draw needs them now) go to the front of the queue, and the background
// work (cache replay, head start) leaves one thread free for them (cache_warm_step): before, it could
// fill every thread and a new shader of the game waited for a background compile to end.
static bool g_cache_replaying = false;  // translating cache or head-start records, not the game's draws
static unsigned compile_threads() {
    static const unsigned n = [] {
        unsigned v = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 4u);
        if (const char* e = getenv("WWHD_COMPILE_THREADS")) v = std::clamp(atoi(e), 1, 8);  // debug
        return v;
    }();
    return n;
}
// the game's compiles over the report window (report_skips): count, time in the queue, time compiling
static std::atomic<uint64_t> g_game_jobs{0}, g_game_wait_us{0}, g_game_wait_max_us{0}, g_game_run_us{0}, g_game_run_max_us{0};
static void atomic_max(std::atomic<uint64_t>& a, uint64_t v) {
    for (uint64_t cur = a.load(std::memory_order_relaxed); v > cur && !a.compare_exchange_weak(cur, v);) {}
}
namespace {
class Workers {
public:
    // urgent: the game's own compile (front of the queue, timed); else background work
    void push(std::function<void()> fn, bool urgent = false) {
        static std::once_flag once;
        std::call_once(once, [this] {
            unsigned n = compile_threads();
            for (unsigned i = 0; i < n; i++)
                std::thread([this] {
                    platform::set_thread_name("shader compile");
                    for (;;) {
                        std::function<void()> job;
                        {
                            std::unique_lock<std::mutex> lk(m_);
                            cv_.wait(lk, [this] { return !q_.empty(); });
                            job = std::move(q_.front());
                            q_.pop_front();
                        }
                        job();
                    }
                }).detach();
        });
        if (urgent) {
            auto queued = std::chrono::steady_clock::now();
            fn = [fn = std::move(fn), queued] {
                using namespace std::chrono;
                auto t0 = steady_clock::now();
                fn();
                uint64_t wait = (uint64_t)duration_cast<microseconds>(t0 - queued).count();
                uint64_t run = (uint64_t)duration_cast<microseconds>(steady_clock::now() - t0).count();
                g_game_jobs++;
                g_game_wait_us += wait;
                g_game_run_us += run;
                atomic_max(g_game_wait_max_us, wait);
                atomic_max(g_game_run_max_us, run);
            };
        }
        std::lock_guard<std::mutex> lk(m_);
        if (urgent) q_.push_front(std::move(fn));
        else q_.push_back(std::move(fn));
        cv_.notify_one();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
};
Workers g_workers;
}  // namespace

// ---------------------------------------------------------------- fetch shaders
static std::unordered_map<uint64_t, LatteFetchShader*> g_fetch;

// Fetch "programs" besides the game's microcode: ours from GX2InitFetchShaderEx ("WWFS", see
// gx2_resources.cpp) and the code-free form of a parsed microcode fetch shader ("WWFP", for shader
// head-start templates): the attributes as parsed, plus the microcode's hash, so it gets the key of
// the program it stands for. Big-endian words like guest memory: magic, size in bytes, hash (high,
// low), valid groups, invalid groups, attributeBufferMask, 0; per group (valid ones first): buffer |
// attribute count << 8 | vertex access << 16 | instance access << 17, minOffset,
// totalAttribRangeSize; per attribute: semantic | format << 8 | fetchType << 16 | nfa << 24,
// isSigned | endianSwap << 8 | buffer << 16, ds[0..3] (a byte each), aluDivisor, offset.
constexpr uint32_t kFetchOurs = 0x57574653, kFetchCodeFree = 0x57574650;

static std::vector<uint8_t> fetch_code_free(const LatteFetchShader* fs, uint64_t programHash) {
    std::vector<uint32_t> w = {kFetchCodeFree, 0, (uint32_t)(programHash >> 32), (uint32_t)programHash,
                               (uint32_t)fs->bufferGroups.size(), (uint32_t)fs->bufferGroupsInvalid.size(),
                               fs->attributeBufferMask, 0};
    for (auto* groups : {&fs->bufferGroups, &fs->bufferGroupsInvalid})
        for (auto& g : *groups) {
            w.push_back(g.attributeBufferIndex | (uint32_t)(uint8_t)g.attribCount << 8 | (uint32_t)g.hasVtxIndexAccess << 16 |
                        (uint32_t)g.hasInstanceIndexAccess << 17);
            w.push_back(g.minOffset);
            w.push_back(g.totalAttribRangeSize);
            for (int i = 0; i < g.attribCount; i++) {
                const LatteParsedFetchShaderAttribute& a = g.attrib[i];
                w.push_back(a.semanticId | ((uint32_t)a.format & 0xFF) << 8 | ((uint32_t)a.fetchType & 0xFF) << 16 | (uint32_t)a.nfa << 24);
                w.push_back(a.isSigned | ((uint32_t)a.endianSwap & 0xFF) << 8 | (uint32_t)a.attributeBufferIndex << 16);
                w.push_back(a.ds[0] | a.ds[1] << 8 | a.ds[2] << 16 | (uint32_t)a.ds[3] << 24);
                w.push_back((uint32_t)a.aluDivisor);
                w.push_back(a.offset);
            }
        }
    w[1] = (uint32_t)w.size() * 4;
    std::vector<uint8_t> out(w.size() * 4);
    for (size_t i = 0; i < w.size(); i++)
        for (int b = 0; b < 4; b++) out[i * 4 + b] = (uint8_t)(w[i] >> (24 - 8 * b));
    return out;
}

// cacheHash: the key get_fetch_shader gives it (as LatteShaderRecompiler_createFetchShader gets it)
static LatteFetchShader* fetch_from_code_free(uint32_t prog, uint64_t cacheHash) {
    const uint32_t size = ld32(prog + 4), valid = ld32(prog + 16), invalid = ld32(prog + 20);
    if (size < 32 || size > 0x1000 || valid > 16 || invalid > 256) return nullptr;
    auto* fs = new LatteFetchShader();
    fs->m_cacheHash = cacheHash;
    fs->attributeBufferMask = ld32(prog + 24);
    uint32_t at = prog + 32;
    const uint32_t end = prog + size;
    for (uint32_t gi = 0; gi < valid + invalid; gi++) {
        if (at + 12 > end) { delete fs; return nullptr; }
        const uint32_t w0 = ld32(at);
        LatteParsedFetchShaderBufferGroup g{};
        g.attributeBufferIndex = (uint8)(w0 & 0xFF);
        g.attribCount = (sint8)((w0 >> 8) & 0x7F);
        g.hasVtxIndexAccess = (w0 >> 16) & 1;
        g.hasInstanceIndexAccess = (w0 >> 17) & 1;
        g.minOffset = ld32(at + 4);
        g.totalAttribRangeSize = ld32(at + 8);
        at += 12;
        if (at + g.attribCount * 20u > end) { delete fs; return nullptr; }
        g.attrib = (LatteParsedFetchShaderAttribute*)calloc(std::max<int>(g.attribCount, 1), sizeof(LatteParsedFetchShaderAttribute));
        for (int i = 0; i < g.attribCount; i++, at += 20) {
            LatteParsedFetchShaderAttribute& a = g.attrib[i];
            const uint32_t a0 = ld32(at), a1 = ld32(at + 4), a2 = ld32(at + 8);
            a.semanticId = (uint8)(a0 & 0xFF);
            a.format = (Latte::E_HWFMT)((a0 >> 8) & 0xFF);
            a.fetchType = (LatteConst::VertexFetchType2)((a0 >> 16) & 0xFF);
            a.nfa = (uint8)(a0 >> 24);
            a.isSigned = (uint8)(a1 & 0xFF);
            a.endianSwap = (LatteConst::VertexFetchEndianMode)((a1 >> 8) & 0xFF);
            a.attributeBufferIndex = (uint8)((a1 >> 16) & 0xFF);
            for (int k = 0; k < 4; k++) a.ds[k] = (uint8)(a2 >> (8 * k));
            a.aluDivisor = (sint32)ld32(at + 12);
            a.offset = ld32(at + 16);
        }
        (gi < valid ? fs->bufferGroups : fs->bufferGroupsInvalid).push_back(g);
    }
    LatteShader_calculateFSKey(fs);
    fs->CalculateFetchShaderVkHash();
    return fs;
}

// the previous draw's fetch shader (draws in a row mostly share it): within a frame, for the same
// program address, size word and strides (the cache replay's scratch programs reset it)
struct FetchLast {
    uint64_t frame = ~0ull, key = 0;
    uint32_t prog = 0, sizeWord = 0, strides[16] = {};
    LatteFetchShader* fs = nullptr;
};
static FetchLast g_fetch_last;

static LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut) {
    uint32_t prog = regs[mmSQ_PGM_START_FS] << 8;
    if (!prog) return nullptr;
    uint32_t strides[16];
    for (uint32_t b = 0; b < 16; b++) strides[b] = (regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + b * 7 + 2] >> 11) & 0xFFFF;
    FetchLast& L = g_fetch_last;
    if (L.frame == R.frame && L.prog == prog && L.sizeWord == regs[mmSQ_PGM_START_FS + 1] && !memcmp(L.strides, strides, sizeof strides)) {
        *keyOut = L.key;
        return L.fs;
    }
    // our compact encoding (from GX2InitFetchShaderEx), a code-free stand-in, or real fetch shader microcode
    const uint32_t magic = ld32(prog);
    const bool ours = magic == kFetchOurs, codeFree = magic == kFetchCodeFree;
    uint32_t size = ours ? 16 + ld32(prog + 4) * 16 : codeFree ? ld32(prog + 4) : regs[mmSQ_PGM_START_FS + 1] << 3;
    if (size > 0x1000) return nullptr;
    uint64_t h = codeFree ? (uint64_t)ld32(prog + 8) << 32 | ld32(prog + 12) : program_hash(prog, size);
    // strides are part of the pipeline's vertex input state
    for (uint32_t b = 0; b < 16; b++) h = (h ^ strides[b]) * 1099511628211ull;
    *keyOut = h;
    auto it = g_fetch.find(h);
    LatteFetchShader* fs;
    if (it != g_fetch.end()) {
        fs = it->second;
    } else {
        fs = ours       ? gx2::build_fetch_shader(prog)
             : codeFree ? fetch_from_code_free(prog, h)
                        : LatteShaderRecompiler_createFetchShader(h, (uint32*)regs, (uint32*)mem::ptr(prog), size);
        g_fetch[h] = fs;
    }
    L.frame = R.frame;
    L.key = h;
    L.prog = prog;
    L.sizeWord = regs[mmSQ_PGM_START_FS + 1];
    memcpy(L.strides, strides, sizeof strides);
    L.fs = fs;
    return fs;
}

// ---------------------------------------------------------------- shaders
static const bool g_sync_shaders = getenv("WWHD_SYNC_SHADERS") != nullptr;
enum CompileState { CS_PENDING, CS_READY, CS_FAILED, CS_DEFERRED };  // deferred: translated from the cache, not compiled yet

// A compiled shader module, shared by every shader variant whose translation came out identical.
// Shader keys include cross-stage state (a vertex shader is keyed by the pixel shader inputs it
// feeds and vice versa), so one program has many keys but few distinct translations.
struct Module {
    VkShaderModule module = VK_NULL_HANDLE;   // valid once state == CS_READY
    std::atomic<int> state{CS_PENDING};       // CS_DEFERRED: from the cache replay, compiled later (start_module)
    std::string pending;                      // ... with its source (the only copy kept)
    uint64_t hash = 0, key = 0;
    bool vertex = false;
};

struct Shader {
    uint64_t key = 0;
    bool vertex = false;
    LatteDecompilerShader* dec = nullptr;
    Module* mod = nullptr;                    // set when compiling is requested
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    // uniform buffers are dynamic (offset given at bind time), so descriptor sets can be cached;
    // false when the shader has more of them than the device allows
    bool dynamicUbos = false;
    bool used = false;                            // a draw asked for it this session (mark_used)
    uint32_t varsRange = 0;                       // size of the uniform-variable block
    uint32_t blockRange[LATTE_NUM_MAX_UNIFORM_BUFFERS] = {};  // size bound for each uniform block
    std::atomic<int> state{CS_PENDING};       // translation: CS_FAILED, CS_DEFERRED, else see mod
    // depth clamp emulated (devices without the feature, compile_shader): translated for a draw that
    // wants depth clamp, the shader also gets this variant
    bool clampWanted = false;
    Module* clampMod = nullptr;
    uint64_t srcHash = 0;  // the translation's source (WWHD_NARROW_KEYS=2 compares it)
};
// compile state of a shader (translation failure, or its module's compile)
static const std::atomic<int>& shader_state(const Shader* s) { return s->mod ? s->mod->state : s->state; }
static std::unordered_map<uint64_t, Module*> g_modules;  // by hash of the translated source; render thread
static size_t g_module_cache_hits;
static std::unordered_map<uint64_t, Shader*> g_shaders;
// The shader cache fills g_shaders with ~200k keys: a lookup there misses the CPU caches two or three
// times (~8% of the render thread with ~2000 lookups a frame). The frame's keys are found here
// first, one cache line each. Shaders are never freed, so an entry stays valid.
// Two slots per key: its own, else the one not used last (alternating).
struct ShaderHot { uint64_t key = 0; Shader* s = nullptr; };
static ShaderHot g_shader_hot[4096];
static uint8_t g_shader_hot_last[2048];  // per pair: the slot used last
static uint64_t g_shader_lookups = 0, g_shader_map_lookups = 0;
static ShaderHot& shader_hot(uint64_t key) {
    uint32_t set = key >> 53;
    ShaderHot* p = &g_shader_hot[set * 2];
    uint32_t i = p[0].s && p[0].key == key ? 0 : p[1].s && p[1].key == key ? 1 : g_shader_hot_last[set] ^ 1;
    g_shader_hot_last[set] = i;
    return p[i];
}
// compiles (shaders and pipelines) started and not finished yet; background work holds back while it's high
static std::atomic<int> g_compiles_in_flight{0};
static std::atomic<size_t> g_pipelines_created{0};
size_t pipelines_created() { return g_pipelines_created; }
// time spent per stage, reported with the skip statistics
static double g_t_decompile;
static double g_t_warm;  // the render thread's background warm-up steps (cache_warm_step)
static std::atomic<uint64_t> g_t_spirv_us{0}, g_t_pipeline_us{0};
static double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void report_compile_error(const std::string& src, uint64_t key, const std::string& log) {
    LOG("[gfx] shader %016llx failed to compile: %s", (unsigned long long)key, log.c_str());
    static std::atomic<int> dumped{0};
    if (dumped++ < 3) {
        char name[64];
        snprintf(name, sizeof name, "failed_shader_%016llx.glsl", (unsigned long long)key);
        if (FILE* f = fopen(name, "w")) { fputs(src.c_str(), f); fclose(f); }
    }
}

// ---- SPIR-V cache on disk (spirv3.bin next to shaders.bin): translations seen in earlier sessions
// skip glslang. Records: uint64 source hash, uint32 word count, words. The file only grows: when
// every translation changes (strictMul, mul_nonIEEE's form), a new name starts it over and the old file is deleted.
static std::mutex g_spv_mutex;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_spv_disk;
static FILE* g_spv_out = nullptr;
static size_t g_spv_disk_hits;

static void spirv_cache_open(const std::string& dir) {
    remove((dir + "/spirv.bin").c_str());   // translations before strictMul
    remove((dir + "/spirv2.bin").c_str());  // before the select form of mul_nonIEEE
    std::string path = dir + "/spirv3.bin";
    if (FILE* f = fopen(path.c_str(), "rb")) {
        uint64_t h;
        uint32_t n;
        while (fread(&h, 8, 1, f) == 1 && fread(&n, 4, 1, f) == 1 && n < (1u << 24)) {
            std::vector<uint32_t> w(n);
            if (fread(w.data(), 4, n, f) != n) break;
            g_spv_disk[h] = std::move(w);
        }
        fclose(f);
    }
    g_spv_out = fopen(path.c_str(), "ab");
    if (!g_spv_disk.empty()) LOG("[gfx] SPIR-V cache: %zu modules from %s", g_spv_disk.size(), path.c_str());
}

static void compile_now(Module* m, uint64_t hash, uint64_t key, bool vertex, const std::string& src) {
    double t0 = now_ms();
    std::vector<uint32_t> spirv;
    {
        std::lock_guard<std::mutex> lk(g_spv_mutex);
        auto it = g_spv_disk.find(hash);
        if (it != g_spv_disk.end()) {
            spirv = it->second;
            g_spv_disk_hits++;
        }
    }
    if (spirv.empty()) {
        std::string log;
        if (!compile_glsl(src.c_str(), vertex, spirv, log)) {
            report_compile_error(src, key, log);
            m->state.store(CS_FAILED, std::memory_order_release);
            compile_done();
            return;
        }
        std::lock_guard<std::mutex> lk(g_spv_mutex);
        if (g_spv_out) {
            uint32_t n = (uint32_t)spirv.size();
            fwrite(&hash, 8, 1, g_spv_out);
            fwrite(&n, 4, 1, g_spv_out);
            fwrite(spirv.data(), 4, n, g_spv_out);
            fflush(g_spv_out);
        }
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    if (vkCreateShaderModule(R.device, &ci, nullptr, &m->module) != VK_SUCCESS) {
        LOG("[gfx] shader %016llx: vkCreateShaderModule failed", (unsigned long long)key);
        m->state.store(CS_FAILED, std::memory_order_release);
        compile_done();
        return;
    }
    g_t_spirv_us += (uint64_t)((now_ms() - t0) * 1000);
    m->state.store(CS_READY, std::memory_order_release);
    compile_done();
}

// The translation's source is only needed until the shader has its module (hash, module lookup, the
// compile's own copy): freed then. The cache replay translates tens of thousands of variants sharing
// ~1000 modules, and each kept its tens of KB of GLSL: about 2 GB of native heap on a Snapdragon 855, which
// Android ended for low memory (in Dragon Roost, and after save state loads).
static void free_source(Shader* sh) {
    delete sh->dec->strBuf_shaderSource;
    sh->dec->strBuf_shaderSource = nullptr;
}

static std::vector<Module*> g_deferred_modules;  // the cache replay's new modules, compiled later
static void compile_module(Module* m, uint64_t hash, uint64_t key, bool vertex, std::string src);

// a deferred module's compile, now (first use, or gradually in the background)
static void start_module(Module* m) {
    if (m->state != CS_DEFERRED) return;
    m->state = CS_PENDING;
    compile_module(m, m->hash, m->key, m->vertex, std::move(m->pending));
    m->pending = std::string();
}

static Module* module_for(std::string src, uint64_t key, bool vertex, bool defer);
static bool depth_clamp_variant(const std::string& src, bool vertex, std::string& out);

// attach the shader to the module for its translation, compiling that module if it is new; with
// defer (the cache replay) a new module is only created, keeping the source, and compiled later
static void compile_shader(Shader* sh, bool defer = false) {
    std::string src = sh->dec->strBuf_shaderSource->c_str();
    // Depth-only and shaded variants of the same geometry must rasterize identical positions:
    // later passes depth-test EQUAL/LEQUAL against the first, and without invariance the driver
    // may compute gl_Position differently per shader (camera-dependent z-fighting: shadow flicker,
    // moire on the ground). As the original project's Vulkan renderer (and Metal's [[invariant]]).
    static const bool noInvariant = getenv("WWHD_NO_INVARIANT") != nullptr;  // debug: as before
    if (sh->vertex && !noInvariant) {
        if (size_t main = src.find("void main("); main != std::string::npos) src.insert(main, "invariant gl_Position;\n");
    }
    // debug: WWHD_DUMP_SHADER=<key>[,<key>…] (hex, as the GPU breadcrumbs name them) writes the
    // translation to shader_<key>.glsl in the files folder
    static const char* dumpKeys = getenv("WWHD_DUMP_SHADER");
    if (dumpKeys) {
        char k[20];
        snprintf(k, sizeof k, "%016llx", (unsigned long long)sh->key);
        if (strstr(dumpKeys, k)) {
            char name[48];
            snprintf(name, sizeof name, "shader_%s.glsl", k);
            if (FILE* f = fopen(name, "w")) { fputs(src.c_str(), f); fclose(f); }
            LOG("[gfx] shader %s written to %s", k, name);
        }
    }
    free_source(sh);
    std::string clampSrc;
    if (sh->clampWanted && !R.features.depthClamp && depth_clamp_variant(src, sh->vertex, clampSrc))
        sh->clampMod = module_for(std::move(clampSrc), sh->key, sh->vertex, defer);
    sh->mod = module_for(std::move(src), sh->key, sh->vertex, defer);
}

// the module of a translation: shared by identical sources; compiled now, or with defer only created
// (the cache replay) and compiled on first use
static Module* module_for(std::string src, uint64_t key, bool vertex, bool defer) {
    uint64_t hash = hash_bytes(src.data(), src.size(), vertex ? 0x5653ull : 0x5053ull) ^ src.size();
    auto it = g_modules.find(hash);
    if (it != g_modules.end()) {
        g_module_cache_hits++;
        if (!defer) start_module(it->second);
        return it->second;
    }
    auto* m = new Module();
    g_modules[hash] = m;
    if (defer) {
        m->state = CS_DEFERRED;
        m->pending = std::move(src);
        m->hash = hash;
        m->key = key;
        m->vertex = vertex;
        g_deferred_modules.push_back(m);
        return m;
    }
    compile_module(m, hash, key, vertex, std::move(src));
    return m;
}

// Depth clamp where the device has none (Mali-G52): the game draws its shadow cascades with depth
// clipping off (PA_CL_CLIP_CNTL), so casters in front of a cascade's near plane (palm crowns, rocks)
// still land in the shadow map, at its nearest depth. Without the feature they were clipped and those
// shadows came and went with the camera (issue #45). The vertex shader variant clamps the position's
// depth at the near plane, per vertex: exact for casters wholly in front; a triangle crossing the plane
// gets a little too deep (at worst a little less shadow). The far side is still clipped: nothing there
// can shadow anything in range, and clamped per vertex big ground triangles reaching past it came out
// too near (wrong shadows on the grass). Per pixel (gl_FragDepth) it was exact but cost the Mali-G52's
// shadow pass 2.4 -> 3.9 ms (no early depth test). Only for pipelines that want depth clamp (get_pipeline).
static bool depth_clamp_variant(const std::string& src, bool vertex, std::string& out) {
    if (!vertex) return false;
    const std::string def = "#define SET_POSITION(_v) ";
    size_t d = src.find(def), eol = d == std::string::npos ? d : src.find_first_of("\r\n", d);
    if (eol == std::string::npos) return false;
    out = src;
    out.insert(eol, "; if (gl_Position.w > 0.0) gl_Position.z = max(gl_Position.z, 0.0)");
    return true;
}

static void compile_module(Module* m, uint64_t hash, uint64_t key, bool vertex, std::string src) {
    if (g_sync_shaders) {
        compile_now(m, hash, key, vertex, src);
        return;
    }
    g_compiles_in_flight++;
    g_workers.push([m, hash, key, vertex, src = std::move(src)] {
        compile_now(m, hash, key, vertex, src);
        g_compiles_in_flight--;
    }, !g_cache_replaying);
}

// descriptor set layout from the decompiler's Vulkan binding assignment
static void create_set_layout(Shader* sh) {
    auto& rm = sh->dec->resourceMapping;
    LatteDecompilerShader* dec = sh->dec;
    VkShaderStageFlags stage = sh->vertex ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT;
    // uniform ranges: the uniform-variable block's size, and each block's declared array size (all
    // the shader can read)
    const uint32_t maxRange = R.props.limits.maxUniformBufferRange;
    uint32_t ubos = 0;
    if (rm.uniformVarsBufferBindingPoint >= 0) {
        sh->varsRange = (std::max<uint32_t>(dec->uniform.uniformRangeSize, 16) + 15) & ~15u;
        ubos++;
    }
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++) {
        if (rm.uniformBuffersBindingPoint[i] < 0) continue;
        uint32_t size = 0x10000;
        for (auto& q : dec->list_quickBufferList)
            if (q.index == (uint32_t)i && q.size) size = q.size;
        sh->blockRange[i] = std::min<uint32_t>((size + 15) & ~15u, maxRange);
        ubos++;
    }
    // the limit counts both stages of a pipeline layout
    sh->dynamicUbos = ubos <= R.props.limits.maxDescriptorSetUniformBuffersDynamic / 2;
    const VkDescriptorType uboType = sh->dynamicUbos ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    for (sint32 i = 0; i < rm.getTextureCount(); i++)
        sh->bindings.push_back({(uint32_t)(rm.getTextureBaseBindingPoint() + i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stage, nullptr});
    if (rm.uniformVarsBufferBindingPoint >= 0)
        sh->bindings.push_back({(uint32_t)rm.uniformVarsBufferBindingPoint, uboType, 1, stage, nullptr});
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++)
        if (rm.uniformBuffersBindingPoint[i] >= 0)
            sh->bindings.push_back({(uint32_t)rm.uniformBuffersBindingPoint[i], uboType, 1, stage, nullptr});
    // identical binding lists share a layout
    static std::unordered_map<uint64_t, VkDescriptorSetLayout> cache;
    uint64_t h = hash_bytes(sh->bindings.data(), sh->bindings.size() * sizeof(VkDescriptorSetLayoutBinding)) ^ stage;
    for (auto& b : sh->bindings) h = hash_bytes(&b.binding, 8, h);
    auto it = cache.find(h);
    if (it != cache.end()) {
        sh->dsl = it->second;
        return;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = (uint32_t)sh->bindings.size();
    ci.pBindings = sh->bindings.data();
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &sh->dsl));
    cache[h] = sh->dsl;
}

// registers that influence how a shader stage is translated (gathered, then hashed in one pass)
// The registers the shader key takes whole (stage_state_hash); shader_reg_relevant below derives
// from this table and the masked ones in stage_state_hash, so keep both in step.
static const struct { uint32_t first, count; } kKeyRegs[] = {
    {mmSQ_VTX_SEMANTIC_0, 32}, {mmSPI_VS_OUT_ID_0, 10}, {mmSPI_VS_OUT_CONFIG, 1}, {mmPA_CL_VS_OUT_CNTL, 1},
    {mmSPI_PS_IN_CONTROL_0, 2}, {mmSPI_PS_INPUT_CNTL_0, 32}, {REGADDR::SQ_CONFIG, 1}, {mmCB_SHADER_MASK, 1},
    {mmCB_SHADER_CONTROL, 1}, {mmDB_SHADER_CONTROL, 1}, {mmSPI_INPUT_Z, 1}, {REGADDR::SX_ALPHA_TEST_CONTROL, 1},
    {REGADDR::PA_CL_VTE_CNTL, 1}, {REGADDR::PA_CL_CLIP_CNTL, 1}, {REGADDR::CB_COLOR_CONTROL, 1}, {REGADDR::CB_TARGET_MASK, 1},
    {mmCB_COLOR0_INFO, 8}};

static uint32_t stage_state_words(const uint32_t* regs, uint32_t texBase, uint32_t* buf) {
    uint32_t n = 0;
    for (auto& k : kKeyRegs) {
        memcpy(&buf[n], &regs[k.first], k.count * 4);
        n += k.count;
    }
    uint32_t cbBase[8];
    for (int i = 0; i < 8; i++) {
        cbBase[i] = regs[mmCB_COLOR0_BASE + i] & ~0xFFu;
        buf[n++] = cbBase[i] != 0;
    }
    buf[n++] = regs[REGADDR::DB_DEPTH_CONTROL] & 0x83;
    // texture types and formats
    for (int t = 0; t < 18; t++) {
        const uint32_t* w = &regs[texBase + t * 7];
        buf[n++] = (w[0] & 7) | (w[4] & 0x300);
        buf[n++] = w[1] & 0x3F00000;
        uint32_t fb = 0, base = w[2] << 8;
        if (base)
            for (int i = 0; i < 8; i++)
                if (base == cbBase[i]) fb = 1 + i;
        buf[n++] = fb;
    }
    // depth-compare samplers
    for (int i = 0; i < 18 * 3; i++) buf[n++] = regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + i * 3] & 0xF8000000;
    return n;
}
// The same value, remembered by a digest of the state: hash_bytes is one serial multiply chain over
// ~850 bytes, for both stages of every draw whose shader state changed (~10% of the GX2 render thread
// at Windfall on a Helio G85). The digest runs four independent lanes; a state seen before gets its
// key from the table. (The key's value must not change: pipeline records and the head start name
// shaders by it.)
static uint64_t state_digest_mul(const uint32_t* buf, uint32_t n, uint64_t h, uint32_t texBase) {
    uint64_t a = h ^ 0x243F6A8885A308D3ull, b = texBase ^ 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w[4];
        memcpy(w, &buf[i], 32);
        a = (a ^ w[0]) * 0xFF51AFD7ED558CCDull; a ^= a >> 32;
        b = (b ^ w[1]) * 0xC4CEB9FE1A85EC53ull; b ^= b >> 32;
        c = (c ^ w[2]) * 0x9E3779B97F4A7C15ull; c ^= c >> 32;
        d = (d ^ w[3]) * 0xD6E8FEB86659FD93ull; d ^= d >> 32;
    }
    for (; i < n; i++) a = (a ^ buf[i]) * 0x100000001B3ull;
    uint64_t digest = (a ^ (b << 21 | b >> 43)) * 0x9E3779B97F4A7C15ull ^ (c ^ (d << 42 | d >> 22)) * 0xFF51AFD7ED558CCDull ^ n;
    return digest ^ digest >> 31;
}
#if defined(__aarch64__) && defined(__linux__)
// the same with the CRC32 instructions (four lanes, 128 bits folded to 64): about a third of the
// multiply chain's time (the digest only finds the memo entry; it is never stored anywhere)
__attribute__((target("crc"))) static uint64_t state_digest_crc(const uint32_t* buf, uint32_t n, uint64_t h, uint32_t texBase) {
    uint32_t a = (uint32_t)h, b = (uint32_t)(h >> 32), c = texBase, d = 0x9E3779B9u ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w[4];
        memcpy(w, &buf[i], 32);
        a = __crc32cd(a, w[0]);
        b = __crc32cd(b, w[1]);
        c = __crc32cd(c, w[2]);
        d = __crc32cd(d, w[3]);
    }
    for (; i < n; i++) a = __crc32cw(a, buf[i]);
    uint64_t lo = (uint64_t)a << 32 | b, hi = (uint64_t)c << 32 | d;
    uint64_t digest = lo * 0x9E3779B97F4A7C15ull ^ (hi << 29 | hi >> 35) * 0xFF51AFD7ED558CCDull;
    return digest ^ digest >> 31;
}
static const bool g_has_crc = (getauxval(AT_HWCAP) & (1UL << 7)) != 0;  // HWCAP_CRC32
#endif
static uint64_t stage_state_hash(const uint32_t* regs, uint64_t h, uint32_t texBase) {
    uint32_t buf[400];
    const uint32_t n = stage_state_words(regs, texBase, buf);
#if defined(__aarch64__) && defined(__linux__)
    const uint64_t digest = g_has_crc ? state_digest_crc(buf, n, h, texBase) : state_digest_mul(buf, n, h, texBase);
#else
    const uint64_t digest = state_digest_mul(buf, n, h, texBase);
#endif
    struct Memo { uint64_t h; uint32_t texBase; uint64_t key; };
    static FlatTable<Memo> memo;
    static size_t entries = 0;
    if (Memo* m = memo.find(digest); m && m->h == h && m->texBase == texBase) return m->key;
    if (entries >= 65536) {
        memo.clear();
        entries = 0;
    }
    uint64_t key = hash_bytes(buf, n * 4, h);
    memo[digest] = Memo{h, texBase, key};
    entries++;
    return key;
}

// Does a register change alter the shader key? gx2_core.cpp counts only those in g_shader_state_gen
// (plugged in below). The programs' addresses aren't counted: get_shader compares them itself.
extern "C" bool (*g_shader_reg_filter)(uint32_t reg, uint32_t oldv, uint32_t newv);
// (filled at load: a function-local static's guard is checked on every call)
static uint8_t g_whole_key_reg[0x10000];  // (bytes: no bit extraction per call)
static const bool g_whole_key_reg_filled = [] {
    for (auto& k : kKeyRegs)
        for (uint32_t i = 0; i < k.count; i++) g_whole_key_reg[k.first + i] = 1;
    return true;
}();
static bool shader_reg_relevant(uint32_t reg, uint32_t oldv, uint32_t newv) {
    const uint8_t* whole = g_whole_key_reg;
    constexpr size_t kWhole = sizeof g_whole_key_reg;
    if (reg < kWhole && whole[reg]) return true;
    const uint32_t* regs = gx2::regs();
    auto isColorBuffer = [&](uint32_t base) {
        if (!base) return false;
        for (int i = 0; i < 8; i++)
            if ((regs[mmCB_COLOR0_BASE + i] & ~0xFFu) == base) return true;
        return false;
    };
    uint32_t d = oldv ^ newv;
    for (uint32_t tb : {(uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS,
                        (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS}) {
        if (reg < tb || reg >= tb + 18 * 7) continue;
        switch ((reg - tb) % 7) {
        case 0: return (d & 7) != 0;
        case 1: return (d & 0x3F00000) != 0;
        case 2: return isColorBuffer(oldv << 8) || isColorBuffer(newv << 8);  // a texture is (no longer) a color buffer
        case 4: return (d & 0x300) != 0;
        default: return false;
        }
    }
    const uint32_t sb = REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    if (reg >= sb && reg < sb + 18 * 3 * 3) return (reg - sb) % 3 == 0 && (d & 0xF8000000) != 0;
    if (reg >= mmCB_COLOR0_BASE && reg < mmCB_COLOR0_BASE + 8) {
        // the key holds whether a color buffer is set, and which textures are color buffers
        if (((oldv & ~0xFFu) != 0) != ((newv & ~0xFFu) != 0)) return true;
        for (uint32_t tb : {(uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS})
            for (int t = 0; t < 18; t++) {
                uint32_t base = regs[tb + t * 7 + 2] << 8;
                if (base && (base == (oldv & ~0xFFu) || base == (newv & ~0xFFu))) return true;
            }
        return false;
    }
    if (reg == REGADDR::DB_DEPTH_CONTROL) return (d & 0x83) != 0;
    return false;
}
// the registers shader_reg_relevant looks at: always (the whole ones), or depending on the change
extern "C" const uint8_t* g_shader_reg_kind;
static const uint8_t* shader_reg_kinds() {
    static uint8_t kind[0x10000];
    for (auto& k : kKeyRegs)
        for (uint32_t i = 0; i < k.count; i++) kind[k.first + i] = kShaderRegAlways;
    auto check = [](uint32_t first, uint32_t count) {
        for (uint32_t r = first; r < first + count; r++)
            if (kind[r] == kShaderRegNever) kind[r] = kShaderRegCheck;
    };
    for (uint32_t tb : {(uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS,
                        (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_GS})
        check(tb, 18 * 7);
    check(REGADDR::SQ_TEX_SAMPLER_WORD0_0, 18 * 3 * 3);
    check(mmCB_COLOR0_BASE, 8);
    check(REGADDR::DB_DEPTH_CONTROL, 1);
    return kind;
}
static bool g_reg_filter_set = (g_shader_reg_filter = &shader_reg_relevant, g_shader_reg_kind = shader_reg_kinds(), true);

// ---------------------------------------------------------------- shader-state digest
// What stage_state_words gathers, kept up to date as registers change instead of gathered and hashed
// for every lookup: a slot per gathered value (masked as there), the digest the sum of the slots'
// terms. gx2_core reports each register command that changed a shader-relevant register
// (g_shader_regs_changed) and bumps g_shader_bulk_gen when registers changed otherwise (contexts,
// save states). Draws alternate between a few states (alpha test on and off, ...), each change
// invalidating the shader lookup: get_shader finds the shader of a digest seen this frame without
// the gather, the program hash and the big tables behind them (~2000 cycles a lookup on a Helio G85,
// most of it cache misses). WWHD_KD_CHECK=1: every digest checked against one computed from scratch.
extern "C" void (*g_shader_regs_changed)(uint32_t first, uint32_t n);
extern "C" uint64_t g_shader_bulk_gen;
namespace kd {
constexpr uint32_t kTexSlots = 18 * 3;  // per stage: per texture unit (w0 & 7 | w4 & 0x300), w1 & 0x3F00000, color buffer index
struct Digest {
    uint64_t bulk = 0;                   // g_shader_bulk_gen it was built at (0: never)
    uint64_t common = 0, tex[2] = {};    // tex: 0 vertex, 1 pixel
    uint32_t keyVal[0x100] = {};         // values of the kKeyRegs words (by their order)
    uint32_t cbVal[8] = {}, depthVal = 0, samplerVal[54] = {}, texVal[2][kTexSlots] = {};
};
Digest g;
uint16_t g_key_slot[0x10000];  // kKeyRegs word -> its index + 1
const bool g_key_slots = [] {
    uint32_t n = 0;
    for (auto& k : kKeyRegs)
        for (uint32_t i = 0; i < k.count; i++) g_key_slot[k.first + i] = (uint16_t)++n;
    return true;
}();
inline uint64_t term(uint32_t slot, uint32_t v) {
    uint64_t x = ((uint64_t)slot << 32 | v) ^ 0x2545F4914F6CDD1Dull;
    x *= 0x9E3779B97F4A7C15ull;
    x ^= x >> 31;
    x *= 0xBF58476D1CE4E5B9ull;
    return x ^ (x >> 29);
}
inline void set(uint64_t& sum, uint32_t& stored, uint32_t slot, uint32_t v) {
    if (v == stored) return;
    sum += term(slot, v) - term(slot, stored);
    stored = v;
}
inline uint32_t tex_base_reg(int stage) {
    return stage ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS;
}
// the color buffer a texture is (stage_state_words' fb: the last matching one + 1, 0 for none)
inline uint32_t fb_index(const uint32_t* regs, uint32_t word2) {
    uint32_t fb = 0, base = word2 << 8;
    if (base)
        for (int i = 0; i < 8; i++)
            if (base == (regs[mmCB_COLOR0_BASE + i] & ~0xFFu)) fb = 1 + i;
    return fb;
}
void update_unit(const uint32_t* regs, int stage, uint32_t t) {
    const uint32_t* w = &regs[tex_base_reg(stage) + t * 7];
    const uint32_t slot = 0x20000 + stage * 0x100 + t * 3;
    set(g.tex[stage], g.texVal[stage][t * 3 + 0], slot + 0, (w[0] & 7) | (w[4] & 0x300));
    set(g.tex[stage], g.texVal[stage][t * 3 + 1], slot + 1, w[1] & 0x3F00000);
    set(g.tex[stage], g.texVal[stage][t * 3 + 2], slot + 2, fb_index(regs, w[2]));
}
void update_reg(const uint32_t* regs, uint32_t r) {
    if (uint16_t k = g_key_slot[r]) {
        set(g.common, g.keyVal[k - 1], r, regs[r]);
        return;
    }
    if (r >= mmCB_COLOR0_BASE && r < mmCB_COLOR0_BASE + 8) {  // its flag, and which textures are color buffers
        set(g.common, g.cbVal[r - mmCB_COLOR0_BASE], 0x10000 + (r - mmCB_COLOR0_BASE), (regs[r] & ~0xFFu) != 0);
        for (int stage = 0; stage < 2; stage++)
            for (uint32_t t = 0; t < 18; t++) update_unit(regs, stage, t);
        return;
    }
    if (r == REGADDR::DB_DEPTH_CONTROL) {
        set(g.common, g.depthVal, 0x10010, regs[r] & 0x83);
        return;
    }
    const uint32_t sb = REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    if (r >= sb && r < sb + 54 * 3) {
        if ((r - sb) % 3 == 0) set(g.common, g.samplerVal[(r - sb) / 3], r, regs[r] & 0xF8000000);
        return;
    }
    for (int stage = 0; stage < 2; stage++) {
        const uint32_t tb = tex_base_reg(stage);
        if (r >= tb && r < tb + 18 * 7) {
            uint32_t k = (r - tb) % 7;
            if (k == 0 || k == 1 || k == 2 || k == 4) update_unit(regs, stage, (r - tb) / 7);
            return;
        }
    }
}
void rebuild(const uint32_t* regs) {
    g = Digest{};
    // every slot starts at value 0 with its term counted, then moves to the register's value
    auto zero = [](uint64_t& sum, uint32_t slot) { sum += term(slot, 0); };
    uint32_t n = 0;
    for (auto& k : kKeyRegs)
        for (uint32_t i = 0; i < k.count; i++, n++) {
            zero(g.common, k.first + i);
            set(g.common, g.keyVal[n], k.first + i, regs[k.first + i]);
        }
    for (int i = 0; i < 8; i++) {
        zero(g.common, 0x10000 + i);
        set(g.common, g.cbVal[i], 0x10000 + i, (regs[mmCB_COLOR0_BASE + i] & ~0xFFu) != 0);
    }
    zero(g.common, 0x10010);
    set(g.common, g.depthVal, 0x10010, regs[REGADDR::DB_DEPTH_CONTROL] & 0x83);
    const uint32_t sb = REGADDR::SQ_TEX_SAMPLER_WORD0_0;
    for (uint32_t i = 0; i < 54; i++) {
        zero(g.common, sb + i * 3);
        set(g.common, g.samplerVal[i], sb + i * 3, regs[sb + i * 3] & 0xF8000000);
    }
    for (int stage = 0; stage < 2; stage++)
        for (uint32_t t = 0; t < 18; t++) {
            for (uint32_t p = 0; p < 3; p++) zero(g.tex[stage], 0x20000 + stage * 0x100 + t * 3 + p);
            update_unit(regs, stage, t);
        }
    g.bulk = g_shader_bulk_gen;
}
void regs_changed(uint32_t first, uint32_t n) {
    if (g.bulk != g_shader_bulk_gen) return;  // rebuilt at the next lookup anyway
    const uint32_t* regs = gx2::regs();
    for (uint32_t r = first; r < first + n; r++)
        if (g_shader_reg_kind[r] != kShaderRegNever) update_reg(regs, r);
}
const bool g_hooked = (g_shader_regs_changed = &regs_changed, true);
// the digest of a stage's state (current registers)
const bool g_check = getenv("WWHD_KD_CHECK") != nullptr;
uint64_t stage_digest(bool vertex) {
    const uint32_t* regs = gx2::regs();
    if (g.bulk != g_shader_bulk_gen) rebuild(regs);
    const bool check = g_check;
    if (check) {
        static int bad = 0;
        Digest saved = g;
        rebuild(regs);
        if ((saved.common != g.common || saved.tex[0] != g.tex[0] || saved.tex[1] != g.tex[1]) && bad++ < 20)
            LOG("[gfx] shader-state digest out of date (common %d, vertex textures %d, pixel textures %d)", saved.common != g.common,
                saved.tex[0] != g.tex[0], saved.tex[1] != g.tex[1]);
    }
    uint64_t d = g.common * 0x9E3779B97F4A7C15ull + g.tex[vertex ? 0 : 1];
    return d ^ (vertex ? 0x5653ull : 0x5053ull);
}
}  // namespace kd

// A pixel shader translated for the draw's vertex shader: the semantic ids that vertex shader
// exports (its output parameters through SPI_VS_OUT_ID) and the PS inputs none of them feeds. Those
// inputs are constants in the translation, the GPU's default value for them (SPI_PS_INPUT_CNTL
// DEFAULT_VAL; LatteDecompilerOptions::linkPSInputsToVS) instead of inputs no output writes, whose
// values are undefined and which some drivers refuse (original project 73c5a3d).
struct PsLink {
    bool linked = false;
    std::bitset<256> exports;
    uint32_t unfed = 0;  // bit f: PS input f has no vertex shader output
    uint64_t key = 0;    // unfed inputs and their DEFAULT_VAL, for the shader key
    uint32_t vsMask = 0; // the vertex shader's output parameters (recorded with the shader)
};
// mask: the vertex shader's outputParameterMask
static PsLink ps_link_mask(const uint32_t* regs, uint32_t mask) {
    PsLink link;
    link.linked = true;
    link.vsMask = mask;
    for (uint32_t i = 0; i < 32; ++i)
        if (mask & (1u << i)) link.exports.set((regs[mmSPI_VS_OUT_ID_0 + i / 4] >> (8 * (i % 4))) & 0xFF);
    const uint32_t control0 = regs[mmSPI_PS_IN_CONTROL_0];
    const uint32_t inputs = std::min<uint32_t>(control0 & 0x3F, GPU7_PS_MAX_INPUTS);
    const uint32_t position = (control0 >> 8) & 1 ? (control0 >> 10) & 0x1F : 0xFFFFFFFFu;
    uint32_t words[1 + GPU7_PS_MAX_INPUTS];
    uint32_t n = 1;
    for (uint32_t f = 0; f < inputs; ++f)
        if (f != position && !link.exports.test(regs[mmSPI_PS_INPUT_CNTL_0 + f] & 0xFF)) {
            link.unfed |= 1u << f;
            words[n++] = (regs[mmSPI_PS_INPUT_CNTL_0 + f] >> 8) & 3;
        }
    words[0] = link.unfed;
    link.key = link.unfed ? hash_bytes(words, n * 4, 0x5053'4C49'4E4Bull) : 0;
    return link;
}
static PsLink ps_link(const uint32_t* regs, const Shader* vs) {
    if (!vs || !vs->dec) return PsLink{};
    return ps_link_mask(regs, vs->dec->outputParameterMask);
}

static Shader* get_shader_uncached(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey,
                                   const PsLink& link = PsLink{});

// vs: for a pixel shader, the draw's vertex shader (its inputs are linked to that one's outputs)
static const bool g_use_key_digest = !getenv("WWHD_KEY_DIGEST") || atoi(getenv("WWHD_KEY_DIGEST")) != 0;
static Shader* get_shader(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey, const Shader* vs = nullptr) {
    const uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    const uint32_t addr = regs[start], size = regs[start + 1];
    // nothing shader-relevant changed since the previous draw: same shader
    struct Last { uint64_t gen = 0, frame = ~0ull, fsKey = 0; uint32_t addr = 0, size = 0; const Shader* vs = nullptr; Shader* s = nullptr; };
    static Last last[2];
    Last& L = last[vertex ? 0 : 1];
    if (L.gen == g_shader_state_gen && L.frame == R.frame && L.addr == addr && L.size == size && (!vertex || L.fsKey == fsKey) &&
        (vertex || L.vs == vs))
        return L.s;
    // a state seen before (by its digest, kd above) with this program: its shader without the gather.
    // In a later frame the program's hash (program_hash: per frame, as the shader key takes it) must
    // still be the one it had, so a program rewritten in place is translated again as before.
    struct DigestHit { Shader* s; uint64_t prog; uint64_t frame; };
    struct DigestMemo { FlatTable<DigestHit> m; size_t entries = 0; };
    static DigestMemo dmemo[2];
    const bool useDigest = g_use_key_digest;
    DigestMemo& D = dmemo[vertex ? 0 : 1];
    uint64_t dk = 0;
    if (useDigest && regs == gx2::regs()) {
        dk = kd::stage_digest(vertex);
        dk = FlatTable<DigestHit>::mix(dk ^ ((uint64_t)addr << 32 | size)) ^
             (vertex ? fsKey * 0xC2B2AE3D27D4EB4Full : (uint64_t)(uintptr_t)vs * 0x165667B19E3779F9ull);
        if (DigestHit* hit = D.m.find(dk)) {
            if (hit->frame != R.frame && (!addr || !size || program_hash(addr << 8, size << 3) != hit->prog)) {
                hit = nullptr;  // (looked up again below)
            } else {
                hit->frame = R.frame;
                L = Last{g_shader_state_gen, R.frame, fsKey, addr, size, vs, hit->s};
                return hit->s;
            }
        }
    }
    // the same state with another program (the game switches between a few): remembered per program
    struct Memo { uint64_t gen = 0, frame = ~0ull; FlatTable<Shader*> m; };
    static Memo memo[2];
    Memo& M = memo[vertex ? 0 : 1];
    if (M.gen != g_shader_state_gen || M.frame != R.frame) {
        M.m.clear();
        M.gen = g_shader_state_gen;
        M.frame = R.frame;
    }
    uint64_t mk = ((uint64_t)addr << 32 | size) ^ (vertex ? fsKey * 0x9E3779B97F4A7C15ull : (uint64_t)(uintptr_t)vs * 0xC2B2AE3D27D4EB4Full);
    Shader** it = M.m.find(mk);
    Shader* s = it ? *it
                   : (M.m[mk] = get_shader_uncached(regs, vertex, fs, fsKey, vertex ? PsLink{} : ps_link(regs, vs)));
    L = Last{g_shader_state_gen, R.frame, fsKey, addr, size, vs, s};
    if (dk && s && addr && size) {
        if (D.entries >= 16384) {  // (states and programs of other places: start over)
            D.m.clear();
            D.entries = 0;
        }
        DigestHit& h = D.m[dk];
        if (!h.s) D.entries++;
        h = DigestHit{s, program_hash(addr << 8, size << 3), R.frame};
    }
    return s;
}

static void cache_record_shader(const uint32_t* regs, bool vertex, const PsLink& link);
static bool g_defer_compiles = false;

static void compile_deferred(Shader* s) {
    if (s && s->mod) start_module(s->mod);
    if (s && s->clampMod) start_module(s->clampMod);
}

// the shader key of a state; base gets the program's part (the decompiler's shader id). 0: no program
static uint64_t shader_key(const uint32_t* regs, bool vertex, uint64_t fsKey, const PsLink& link, uint64_t* base = nullptr) {
    uint32_t addr = regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] << 8;
    uint32_t size = regs[(vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS) + 1] << 3;
    if (!addr || !size) return 0;
    uint64_t key = program_hash(addr, size) ^ (vertex ? 0x1111 : 0x2222);
    if (base) *base = key;
    key = stage_state_hash(regs, key, vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS);
    if (vertex) key ^= fsKey * 31;
    else key ^= link.key * 37;  // 0 when every input is fed: the same translation as unlinked
    return key;
}

// Shader variants the game used this session (not the cache's or the head start's replays): the
// cache keeps the records of those used in its last few sessions (shader_use_save, cache_load)
static std::vector<uint64_t> g_used_keys;
static inline void mark_used(Shader* s) {
    if (s->used || g_cache_replaying) return;
    s->used = true;
    g_used_keys.push_back(s->key);
}

// Mod packs' shader replacements (packs.h): Cemu names a pixel shader by a hash of its program plus
// the PS input table's key (LatteShader.cpp, LatteSHRC_UpdatePSBaseHash) and an aux hash of the state
// it was translated for (LatteSHRC_CalcPSAuxHash: CB_SHADER_MASK, alpha test, texture dimensions). A
// pack's GLSL for those hashes stands in for the translation.
static void use_pack_shader(Shader* s, uint64_t base, uint64_t aux, char type, uint32_t addr) {
    static const bool log = getenv("WWHD_SHADER_HASH_LOG") != nullptr;  // debug: Cemu's names of the translated shaders
    if (log) LOG("[gfx] %s shader %08X: %016llx_%016llx_%cs", type == 'v' ? "vertex" : "pixel", addr, (unsigned long long)base,
                 (unsigned long long)aux, type);
    const std::string* src = packs::custom_shader(base, aux, type);
    if (!src) return;
    delete s->dec->strBuf_shaderSource;
    s->dec->strBuf_shaderSource = new StringBuf((uint32)src->size() + 16);
    s->dec->strBuf_shaderSource->add(*src);
}

// Cemu's program hash (_calcShaderHashGeneric): the words as Cemu reads them (host order)
static uint64_t cemu_program_hash(uint32_t addr, uint32_t size) {
    uint64_t h1 = 0, h2 = 0;
    const uint32_t* code = (const uint32_t*)mem::ptr(addr);
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t t = code[i];
        h1 += t;
        h2 ^= t;
        h1 = (h1 << 3) | (h1 >> 61);
        h2 = (h2 >> 7) | (h2 << 57);
    }
    return h1 + h2;
}

// a vertex shader (LatteSHRC_UpdateVSBaseHash, LatteSHRC_CalcVSAuxHash): the program, the fetch shader,
// the PS input table, PA_CL_VTE_CNTL, rects / points, stream out, the clip space; aux: stream-out
// strides and integer-format textures
static void pack_vertex_shader(Shader* s, const uint32_t* regs, uint32_t addr, uint32_t size, LatteFetchShader* fs) {
    if (!fs) return;
    uint64_t base = cemu_program_hash(addr, size) + fs->key + LatteSHRC_GetPSInputTable()->key;  // (no geometry shaders)
    base += regs[REGADDR::PA_CL_VTE_CNTL] ^ 0x43F;
    const uint32_t prim = regs[REGADDR::VGT_PRIMITIVE_TYPE];
    if (prim == 0x11) base += 13;       // rects
    else if (prim == 0x01) base += 71;  // points
    base += regs[mmVGT_STRMOUT_EN] ? 21 : 0;
    if ((regs[REGADDR::PA_CL_CLIP_CNTL] >> 19) & 1) base += 0x1537;  // DX_CLIP_SPACE_DEF (halfZ)
    uint64_t aux = 0, auxTex = 0;
    if (s->dec->hasStreamoutBufferWrite)
        for (uint32_t i = 0; i < LATTE_NUM_STREAMOUT_BUFFER; i++) {
            if (!s->dec->streamoutBufferWriteMask[i]) continue;
            aux = (aux << 7) | (aux >> 57);
            aux += regs[mmVGT_STRMOUT_VTX_STRIDE_0 + i * 4];
        }
    for (uint8_t i = 0; i < s->dec->textureUnitListCount; i++) {
        const uint32_t word4 = regs[REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS + s->dec->textureUnitList[i] * 7 + 4];
        if ((word4 & 0x300) == 0x100) {  // integer format
            auxTex = (auxTex << 7) | (auxTex >> 57);
            auxTex += 0x333;
        }
    }
    static const bool log = getenv("WWHD_SHADER_HASH_LOG") != nullptr;  // debug: the hash's parts
    if (log)
        LOG("[gfx] vertex shader %08X parts: prog %016llx fs %016llx psin %016llx vte %08X prim %X strmout %X clip %08X",
            addr, (unsigned long long)cemu_program_hash(addr, size), (unsigned long long)fs->key,
            (unsigned long long)LatteSHRC_GetPSInputTable()->key, regs[REGADDR::PA_CL_VTE_CNTL], prim, regs[mmVGT_STRMOUT_EN],
            regs[REGADDR::PA_CL_CLIP_CNTL]);
    use_pack_shader(s, base, aux + auxTex, 'v', addr);
}

static void pack_pixel_shader(Shader* s, const uint32_t* regs, uint32_t addr, uint32_t size) {
    const uint64_t base = cemu_program_hash(addr, size) + LatteSHRC_GetPSInputTable()->key;  // (no geometry shaders in this game)
    uint64_t aux = 0;
    aux += regs[mmCB_SHADER_MASK];
    const uint32_t alpha = regs[REGADDR::SX_ALPHA_TEST_CONTROL];
    if ((alpha >> 3) & 1) {
        aux += alpha & 7;
        aux = (aux >> 3) | (aux << 61);
        aux += 1;
    }
    for (uint8_t i = 0; i < s->dec->textureUnitListCount; i++) {
        const uint32_t word0 = regs[REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + s->dec->textureUnitList[i] * 7];
        aux = (aux << 3) | (aux >> 61);
        aux += word0 & 7;
    }
    use_pack_shader(s, base, aux, 'p', addr);
}

// Narrow keys: the shader key takes the state both stages could read, for both, and much of it
// stale: all 18 texture units and 54 samplers, all 32 vertex semantics and PS input controls, the
// other stage's registers. Thousands of keys a minute (the game leaves old values in what a draw
// doesn't use) whose translation (~1 ms each on the render thread: the new-shader stutter) came out
// the same. The narrow key takes what the translation reads (Cemu's decompiler):
//   vertex: the fetch shader, the slot each fetched attribute's semantic resolves to (first match in
//     SQ_VTX_SEMANTIC), each exported parameter's semantic if a PS input takes it, with that input's
//     index (the output's location) and interpolation (SPI_VS_OUT_ID, the PS input table), its own
//     output and clip registers
//   pixel: the PS input table's registers (SPI_PS_IN_CONTROL, the first n SPI_PS_INPUT_CNTL), the
//     link to the vertex shader, the target, alpha and depth registers
//   both: only the program's own texture units and their samplers
// What a program uses (units, samplers, exported parameters) comes from its instructions: learnt from
// its first translation. A new full key whose narrow key is known gets a shader of its own that shares
// that translation (and so its modules and pipelines); keys and records stay as they were (cache,
// pipeline records, template exports and the head start name shaders by them).
// WWHD_NARROW_KEYS=0 off, =2 check: translate anyway, count translations that differ, and report
// which parts still tell same-translation variants apart.
static const int g_narrow_mode = getenv("WWHD_NARROW_KEYS") ? atoi(getenv("WWHD_NARROW_KEYS")) : 1;
struct ProgramUnits {
    uint32_t count = 0;
    uint8_t unit[LATTE_NUM_MAX_TEX_UNITS];
    uint16_t sampler[LATTE_NUM_MAX_TEX_UNITS];
    uint32_t outputs = 0;  // vertex: exported parameters (outputParameterMask)
};
static std::unordered_map<uint64_t, ProgramUnits> g_program_units;  // by the key's program part
static std::unordered_map<uint64_t, Shader*> g_narrow;               // by narrow key: the first translation
static uint64_t g_narrow_reused = 0, g_narrow_checked = 0, g_narrow_differ = 0, g_translations = 0;

struct NarrowParts {
    uint32_t n = 0, count = 0;
    uint32_t end[24];
    const char* name[24];
    void mark(const char* nm) { name[count] = nm; end[count++] = n; }
};
static uint32_t narrow_words(const uint32_t* regs, bool vertex, const ProgramUnits& u, const LatteFetchShader* fs, uint32_t* buf,
                             NarrowParts& P) {
    uint32_t& n = P.n;
    n = 0;
    auto put = [&](uint32_t reg, uint32_t count) {
        memcpy(&buf[n], &regs[reg], count * 4);
        n += count;
    };
    // the PS input table (both stages: the vertex shader exports what it takes)
    const uint32_t numInputs = std::min<uint32_t>(regs[mmSPI_PS_IN_CONTROL_0] & 0x3F, 32);
    const uint32_t posInput = (regs[mmSPI_PS_IN_CONTROL_0] >> 8) & 1 ? (regs[mmSPI_PS_IN_CONTROL_0] >> 10) & 0x1F : ~0u;  // gl_FragCoord
    if (vertex) {
        buf[n++] = fs ? (uint32_t)fs->key : 0;
        buf[n++] = fs ? (uint32_t)(fs->key >> 32) : 0;
        P.mark("fetch shader");
        if (fs)
            for (const auto* groups : {&fs->bufferGroups, &fs->bufferGroupsInvalid})
                for (const auto& g : *groups)
                    for (sint32 a = 0; a < g.attribCount; a++) {
                        uint32_t slot = 0xFF;
                        for (uint32_t f = 0; f < 32; f++)
                            if (regs[mmSQ_VTX_SEMANTIC_0 + f] == g.attrib[a].semanticId) { slot = f; break; }
                        buf[n++] = slot;
                    }
        P.mark("attribute slots");
        for (uint32_t i = 0; i < 32; i++) {
            if (!(u.outputs & (1u << i))) continue;
            uint32_t sid = (regs[mmSPI_VS_OUT_ID_0 + i / 4] >> (8 * (i % 4))) & 0xFF, taken = 0xFF;
            for (uint32_t f = 0; f < numInputs; f++)
                if (f != posInput && (regs[mmSPI_PS_INPUT_CNTL_0 + f] & 0xFF) == sid) {
                    // with its location (the PS input's index) and flat / no-perspective
                    taken = sid | (regs[mmSPI_PS_INPUT_CNTL_0 + f] & 0x1400) << 8 | f << 24;
                    break;
                }
            buf[n++] = taken;
        }
        buf[n++] = regs[mmSPI_PS_IN_CONTROL_0] & 0x7FFF00;  // position input, parameter generation
        P.mark("exported semantics");
        put(mmSPI_VS_OUT_CONFIG, 1);
        put(mmPA_CL_VS_OUT_CNTL, 1);
        put(REGADDR::PA_CL_VTE_CNTL, 1);
        put(REGADDR::PA_CL_CLIP_CNTL, 1);
        put(REGADDR::SQ_CONFIG, 1);
        P.mark("vs registers");
    } else {
        put(mmSPI_PS_IN_CONTROL_0, 2);
        put(mmSPI_PS_INPUT_CNTL_0, numInputs);
        P.mark("PS input table");
        put(mmCB_SHADER_MASK, 1);
        put(mmCB_SHADER_CONTROL, 1);
        put(REGADDR::CB_COLOR_CONTROL, 1);
        P.mark("CB shader/color control");
        put(REGADDR::CB_TARGET_MASK, 1);
        P.mark("CB_TARGET_MASK");
        put(mmCB_COLOR0_INFO, 8);
        P.mark("CB_COLOR_INFO");
        for (int i = 0; i < 8; i++) buf[n++] = (regs[mmCB_COLOR0_BASE + i] & ~0xFFu) != 0;
        P.mark("CB bases set");
        put(mmDB_SHADER_CONTROL, 1);
        put(mmSPI_INPUT_Z, 1);
        buf[n++] = regs[REGADDR::DB_DEPTH_CONTROL] & 0x83;
        P.mark("depth registers");
        put(REGADDR::SX_ALPHA_TEST_CONTROL, 1);
        P.mark("alpha test");
        put(REGADDR::PA_CL_VTE_CNTL, 1);
        put(REGADDR::PA_CL_CLIP_CNTL, 1);
        put(REGADDR::SQ_CONFIG, 1);
        P.mark("ps other registers");
    }
    uint32_t cbBase[8];
    for (int i = 0; i < 8; i++) cbBase[i] = regs[mmCB_COLOR0_BASE + i] & ~0xFFu;
    const uint32_t texBase = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    const uint32_t samplerBase = vertex ? SAMPLER_BASE_INDEX_VERTEX : SAMPLER_BASE_INDEX_PIXEL;
    for (uint32_t j = 0; j < u.count; j++) {  // as stage_state_words, for the program's units only
        const uint32_t t = u.unit[j];
        const uint32_t* w = &regs[texBase + t * 7];
        buf[n++] = t;
        buf[n++] = (w[0] & 7) | (w[4] & 0x300);
        buf[n++] = w[1] & 0x3F00000;
        uint32_t fb = 0, tb = w[2] << 8;
        if (tb)
            for (int i = 0; i < 8; i++)
                if (tb == cbBase[i]) fb = 1 + i;
        buf[n++] = fb;
        if (u.sampler[j] < LATTE_NUM_MAX_TEX_UNITS)
            buf[n++] = regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (u.sampler[j] + samplerBase) * 3] & 0xF8000000;
    }
    P.mark("used textures/samplers");
    return n;
}
static uint64_t narrow_key(const uint32_t* regs, bool vertex, const ProgramUnits& u, const LatteFetchShader* fs, uint64_t base,
                           const PsLink& link) {
    uint32_t buf[512];
    NarrowParts P;
    const uint32_t n = narrow_words(regs, vertex, u, fs, buf, P);
    uint64_t key = hash_bytes(buf, n * 4, base ^ 0x6E6172726F77ull);
    return vertex ? key : key ^ link.key * 37;
}

// WWHD_NARROW_KEYS=2: per program its first variant's narrow words; a later one with the same
// translation counts each part that still differs (reported with the skip statistics)
struct NarrowRef { std::vector<uint32_t> words; uint64_t linkKey, srcHash; };
static std::unordered_map<uint64_t, NarrowRef> g_narrow_ref;
static std::map<std::string, uint64_t> g_narrow_diff;
static void narrow_diagnose(const uint32_t* regs, bool vertex, const ProgramUnits& u, const LatteFetchShader* fs, uint64_t base,
                            uint64_t linkKey, uint64_t srcHash) {
    uint32_t buf[512];
    NarrowParts P;
    const uint32_t n = narrow_words(regs, vertex, u, fs, buf, P);
    auto it = g_narrow_ref.find(base);
    if (it == g_narrow_ref.end()) {
        g_narrow_ref.emplace(base, NarrowRef{std::vector<uint32_t>(buf, buf + n), linkKey, srcHash});
        return;
    }
    const NarrowRef& r = it->second;
    if (r.srcHash != srcHash) return;  // a different translation: rightly a different variant
    const std::string stage = vertex ? "vs " : "ps ";
    if (!vertex && r.linkKey != linkKey) g_narrow_diff[stage + "PS link"]++;
    if (r.words.size() != n) {
        g_narrow_diff[stage + "length"]++;
        return;
    }
    for (uint32_t p = 0, i = 0; p < P.count; p++) {
        bool d = false;
        for (; i < P.end[p]; i++) d |= r.words[i] != buf[i];
        if (d) g_narrow_diff[stage + P.name[p]]++;
    }
}

static Shader* get_shader_uncached(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey, const PsLink& link) {
    uint32_t addr = regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] << 8;
    uint32_t size = regs[(vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS) + 1] << 3;
    uint64_t base = 0;
    uint64_t key = shader_key(regs, vertex, fsKey, link, &base);
    if (!key) return nullptr;
    ShaderHot& hot = shader_hot(key);
    g_shader_lookups++;
    if (hot.s && hot.key == key) { mark_used(hot.s); return hot.s; }
    g_shader_map_lookups++;
    auto it = g_shaders.find(key);
    if (it != g_shaders.end()) {
        hot = {key, it->second};
        mark_used(it->second);
        return it->second;
    }
    uint64_t nkey = 0;
    Shader* twin = nullptr;  // the translation for the same narrow key
    if (g_narrow_mode) {
        if (auto pu = g_program_units.find(base); pu != g_program_units.end()) {
            nkey = narrow_key(regs, vertex, pu->second, fs, base, link);
            if (auto ni = g_narrow.find(nkey); ni != g_narrow.end()) twin = ni->second;
        }
        if (twin && g_narrow_mode == 1) {
            // a shader of its own (its key in records, pipelines and exports as before) sharing the
            // twin's translation, modules and layout (pipelines are keyed by modules: shared too)
            double t0 = now_ms();
            auto* a = new Shader();
            a->key = key;
            a->vertex = vertex;
            a->dec = twin->dec;
            a->mod = twin->mod;
            a->dsl = twin->dsl;
            a->bindings = twin->bindings;
            a->dynamicUbos = twin->dynamicUbos;
            a->varsRange = twin->varsRange;
            memcpy(a->blockRange, twin->blockRange, sizeof a->blockRange);
            a->state.store(twin->state.load());
            a->clampWanted = twin->clampWanted;
            a->clampMod = twin->clampMod;
            g_shaders[key] = a;
            hot = {key, a};
            mark_used(a);
            cache_record_shader(regs, vertex, link);
            g_narrow_reused++;
            g_t_decompile += now_ms() - t0;
            return a;
        }
    }

    auto* s = new Shader();
    s->key = key;
    s->vertex = vertex;
    g_shaders[key] = s;
    hot = {key, s};
    mark_used(s);
    double t0 = now_ms();
    LatteShader_UpdatePSInputs((uint32*)regs);
    LatteDecompilerOptions opt;
    // the GPU's MUL/MULADD give 0*anything=0 (rsqrt(0)*0 is NaN otherwise: the black letter in the
    // Rito mail sorting game); Cemu's default too (original project 0dfd3c3). WWHD_STRICT_MUL=0: off
    static const bool strictMul = !getenv("WWHD_STRICT_MUL") || strcmp(getenv("WWHD_STRICT_MUL"), "0");
    opt.strictMul = strictMul;
    if (!vertex && link.linked) {
        opt.linkPSInputsToVS = true;
        opt.vsOutputSemantics = link.exports;
    }
    LatteDecompilerOutput_t out{};
    if (vertex)
        LatteDecompiler_DecompileVertexShader(base, (uint32*)regs, mem::ptr(addr), size, fs, opt, &out);
    else
        LatteDecompiler_DecompilePixelShader(base, (uint32*)regs, mem::ptr(addr), size, opt, &out);
    if (!out.shader || out.shader->hasError || !out.shader->strBuf_shaderSource) {
        LOG("[gfx] %s shader %08X failed to translate", vertex ? "vertex" : "pixel", addr);
        s->state = CS_FAILED;
        return s;
    }
    s->dec = FinishDecompiledShader(out);
    {
        LATTE_PA_CL_CLIP_CNTL clip;
        uint32_t raw = regs[REGADDR::PA_CL_CLIP_CNTL];
        memcpy(&clip, &raw, 4);
        s->clampWanted = clip.get_ZCLIP_FAR_DISABLE() && !R.features.depthClamp;
    }
    if (vertex) pack_vertex_shader(s, regs, addr, size, fs);
    else pack_pixel_shader(s, regs, addr, size);
    g_translations++;
    if (g_narrow_mode) {
        auto pu = g_program_units.find(base);
        if (pu == g_program_units.end()) {
            ProgramUnits u;
            u.count = s->dec->textureUnitListCount;
            for (uint32_t j = 0; j < u.count; j++) {
                u.unit[j] = s->dec->textureUnitList[j];
                u.sampler[j] = s->dec->textureUnitSamplerAssignment[u.unit[j]];
            }
            u.outputs = s->dec->outputParameterMask;
            pu = g_program_units.emplace(base, u).first;
        }
        if (!nkey) nkey = narrow_key(regs, vertex, pu->second, fs, base, link);
        if (g_narrow_mode == 2) {
            const StringBuf* src = s->dec->strBuf_shaderSource;
            s->srcHash = hash_bytes(src->c_str(), src->getLen());
            narrow_diagnose(regs, vertex, pu->second, fs, base, link.key, s->srcHash);
            if (twin) {
                g_narrow_checked++;
                if (twin->srcHash != s->srcHash || twin->clampWanted != s->clampWanted) {
                    if (g_narrow_differ++ < 10)
                        LOG("[gfx] narrow key check: %s shader %08X key %016llx translates differently from %016llx",
                            vertex ? "vertex" : "pixel", addr, (unsigned long long)key, (unsigned long long)twin->key);
                }
            }
        }
        g_narrow.emplace(nkey, s);  // (the first translation stays)
    }
    create_set_layout(s);
    cache_record_shader(regs, vertex, link);
    g_t_decompile += now_ms() - t0;
    // debug: WWHD_DUMP_SHADERS=1 writes the translated GLSL to ./shaders, =/some/dir there
    if (const char* dump = getenv("WWHD_DUMP_SHADERS")) {
        std::string dir = dump[0] == '/' ? dump : "shaders";
        mkdir(dir.c_str(), 0755);
        char name[96];
        snprintf(name, sizeof name, "/%s_%08X_%016llx.glsl", vertex ? "vs" : "ps", addr, (unsigned long long)key);
        if (FILE* f = fopen((dir + name).c_str(), "w")) { fputs(s->dec->strBuf_shaderSource->c_str(), f); fclose(f); }
    }
    // cache replay: new modules are compiled on first use or gradually in the background
    compile_shader(s, g_defer_compiles);
    static uint32_t count = 0;
    if (++count % 100 == 0) LOG("[gfx] %u shaders translated", count);
    return s;
}

// ---------------------------------------------------------------- conversions
static VkBlendFactor blend_factor(uint32_t f) {
    const bool dual = R.features.dualSrcBlend;
    switch (f) {
    case 0x00: return VK_BLEND_FACTOR_ZERO;
    case 0x01: return VK_BLEND_FACTOR_ONE;
    case 0x02: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x03: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x04: case 0x0B: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x05: case 0x0C: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x06: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x07: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x08: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x09: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0A: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x0D: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x0E: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x0F: return dual ? VK_BLEND_FACTOR_SRC1_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
    case 0x10: return dual ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x11: return dual ? VK_BLEND_FACTOR_SRC1_ALPHA : VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x12: return dual ? VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x13: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x14: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(uint32_t f) {
    switch (f) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
    }
}
static VkCompareOp compare_op(uint32_t f) {
    static const VkCompareOp t[8] = {VK_COMPARE_OP_NEVER, VK_COMPARE_OP_LESS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_LESS_OR_EQUAL,
                                     VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL, VK_COMPARE_OP_GREATER_OR_EQUAL,
                                     VK_COMPARE_OP_ALWAYS};
    return t[f & 7];
}
static VkStencilOp stencil_op(uint32_t f) {
    static const VkStencilOp t[8] = {VK_STENCIL_OP_KEEP, VK_STENCIL_OP_ZERO, VK_STENCIL_OP_REPLACE,
                                     VK_STENCIL_OP_INCREMENT_AND_CLAMP, VK_STENCIL_OP_DECREMENT_AND_CLAMP, VK_STENCIL_OP_INVERT,
                                     VK_STENCIL_OP_INCREMENT_AND_WRAP, VK_STENCIL_OP_DECREMENT_AND_WRAP};
    return t[f & 7];
}

// ---------------------------------------------------------------- render passes and framebuffers
// Everything that makes a render pass compatible: attachment formats (VK_FORMAT_UNDEFINED = none)
struct PassFormats {  // only uint32 members: hashed as raw bytes
    uint32_t color[8] = {};
    uint32_t depth = 0;
    uint32_t stencil = 0;
};

// clears: bit i = color attachment i, kClearDepth / kClearStencil = depth buffer (the load op
// clears instead of loading; render passes that differ only in that stay compatible)
constexpr uint32_t kClearDepth = 1u << 8, kClearStencil = 1u << 9;
// debug: WWHD_DISCARD_LOADS=1 (color) / 2 (depth) / 3: no attachment loads at all (wrong picture: what loads cost)
static const int g_discard_loads = getenv("WWHD_DISCARD_LOADS") ? atoi(getenv("WWHD_DISCARD_LOADS")) : 0;
static VkRenderPass get_render_pass(const PassFormats& pf, uint32_t clears = 0) {
    static std::unordered_map<uint64_t, VkRenderPass> cache;
    uint64_t h = hash_bytes(&pf, sizeof pf, clears);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    std::vector<VkAttachmentDescription> atts;
    VkAttachmentReference colorRefs[8];
    uint32_t colorCount = 0;
    for (int i = 0; i < 8; i++)
        if (pf.color[i]) colorCount = i + 1;
    for (uint32_t i = 0; i < colorCount; i++) {
        if (!pf.color[i]) { colorRefs[i] = {VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_UNDEFINED}; continue; }
        VkAttachmentDescription a{};
        a.format = (VkFormat)pf.color[i];
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = clears & (1u << i) ? VK_ATTACHMENT_LOAD_OP_CLEAR : (g_discard_loads & 1) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colorRefs[i] = {(uint32_t)atts.size(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        atts.push_back(a);
    }
    VkAttachmentReference depthRef{};
    if (pf.depth) {
        VkAttachmentDescription a{};
        a.format = (VkFormat)pf.depth;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = clears & kClearDepth ? VK_ATTACHMENT_LOAD_OP_CLEAR : (g_discard_loads & 2) ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = !pf.stencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE
                          : clears & kClearStencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        a.stencilStoreOp = pf.stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depthRef = {(uint32_t)atts.size(), VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
        atts.push_back(a);
    }
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = colorCount;
    sp.pColorAttachments = colorRefs;
    sp.pDepthStencilAttachment = pf.depth ? &depthRef : nullptr;
    // Order attachment accesses against earlier and later passes on the same images. Other uses
    // (sampling, copies, clears) change the image's layout first, and that barrier orders them
    // (prepare). Precise stages let the next pass's vertex work overlap this one's pixels;
    // WWHD_BROAD_BARRIERS=1: everything waits for everything (as before).
    static const bool broad = getenv("WWHD_BROAD_BARRIERS") != nullptr;
    const VkPipelineStageFlags att = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags attWrite = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    const VkAccessFlags attAll = attWrite | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    VkSubpassDependency deps[2] = {{VK_SUBPASS_EXTERNAL, 0, att, att, attWrite, attAll, 0}, {0, VK_SUBPASS_EXTERNAL, att, att, attWrite, attAll, 0}};
    if (broad) {
        deps[0] = {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
        deps[1] = {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_WRITE_BIT,
                   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0};
    }
    VkRenderPassCreateInfo ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    ci.attachmentCount = (uint32_t)atts.size();
    ci.pAttachments = atts.data();
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    VkRenderPass rp;
    VK_CHECK(vkCreateRenderPass(R.device, &ci, nullptr, &rp));
    cache[h] = rp;
    return rp;
}

static std::unordered_map<uint64_t, VkFramebuffer> g_framebuffers;

static VkFramebuffer get_framebuffer(VkRenderPass rp, const VkImageView* views, uint32_t n, uint32_t w, uint32_t h) {
    auto& cache = g_framebuffers;
    uint64_t key = hash_bytes(views, n * sizeof(VkImageView), (uint64_t)(uintptr_t)rp);
    key = hash_bytes(&w, 4, key);
    key = hash_bytes(&h, 4, key);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = rp;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w;
    ci.height = h;
    ci.layers = 1;
    VkFramebuffer fb;
    VK_CHECK(vkCreateFramebuffer(R.device, &ci, nullptr, &fb));
    cache[key] = fb;
    return fb;
}

// ---------------------------------------------------------------- pipelines
// Pipeline state that Metal sets on the encoder but Vulkan bakes into the pipeline
struct PipelineState {
    PassFormats pass;
    uint32_t blend[8] = {}, colorControl = 0, targetMask = 0;
    uint32_t topology = 0;
    uint32_t cull = 0;          // bit 0 front, bit 1 back
    uint32_t frontCCW = 0;
    uint32_t depthControl = 0;  // DB_DEPTH_CONTROL (0 without depth buffer)
    uint32_t depthClamp = 0;
    uint32_t intTargets = 0;    // bit per color attachment with an integer format
};

struct Pipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;   // valid once status == CS_READY
    VkPipelineLayout layout = VK_NULL_HANDLE;
    std::atomic<int> status{CS_PENDING};
};
static std::unordered_map<uint64_t, Pipeline*> g_pipelines;

static VkPipelineLayout get_pipeline_layout(Shader* vs, Shader* ps) {
    static std::unordered_map<uint64_t, VkPipelineLayout> cache;
    static VkDescriptorSetLayout empty = VK_NULL_HANDLE;
    if (!empty) {
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        VK_CHECK(vkCreateDescriptorSetLayout(R.device, &ci, nullptr, &empty));
    }
    VkDescriptorSetLayout sets[2] = {vs->dsl ? vs->dsl : empty, ps->dsl ? ps->dsl : empty};
    uint64_t h = hash_bytes(sets, sizeof sets);
    auto it = cache.find(h);
    if (it != cache.end()) return it->second;
    VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ci.setLayoutCount = 2;
    ci.pSetLayouts = sets;
    VkPipelineLayout l;
    VK_CHECK(vkCreatePipelineLayout(R.device, &ci, nullptr, &l));
    cache[h] = l;
    return l;
}

// Ambient-occlusion quirks, switchable in game (settings menu; WWHD_AO_MODE=0..2 sets the start):
//   0 = as the hardware renders it
//   1 = centre depth sampled bilinear (removes the every-third-row lines)
//   2 = 1 + noise tiled per 960x540 pixel instead of per 640x360 pixel (removes the remaining
//       uneven noise bands the game's blur can't average out)
static std::atomic<int> g_ao_mode{[] {
    if (const char* e = getenv("WWHD_AO_MODE")) return atoi(e) % 3;
    return getenv("WWHD_NO_AO_QUIRK") ? 0 : 2;
}()};
int ao_mode() { return g_ao_mode.load(std::memory_order_relaxed); }
void set_ao_mode(int m) { g_ao_mode = m % 3; LOG("[gfx] ambient occlusion mode %d", m % 3); }

// Draws whose shaders or pipeline are still compiling wait for the compile, up to a per-frame budget
// (WWHD_COMPILE_WAIT_MS, default 8; the game's frame is 33 ms), and are skipped after it: new things
// may be missing for a frame or two instead of the game stuttering (25 ms stuttered noticeably). Draws
// into a target drawn for the first time (textures the game renders once, e.g. a scene's lighting
// when it loads) wait until the compile is done (g_wait_whole), or their result would stay wrong
// until the scene is loaded again; on a cold cache that is about 250 draws, 1.5 s in all, mostly
// while booting. Pipelines built ahead of use (cache replay, head start) never wait.
static bool g_building_ahead = false;
static bool g_wait_whole = false;
// which draws wait (WWHD_WAIT_POLICY): 0 none (all skip after the budget), 1 draws into targets not
// drawn in each of the last 3 frames (more waiting, same result in tests), 2 (default) only draws
// into targets drawn for the first time
static const int g_wait_policy = getenv("WWHD_WAIT_POLICY") ? atoi(getenv("WWHD_WAIT_POLICY")) : 2;
// time waited, by target kind: 0 drawn in the last frame (streak < 3), 1 drawn before with a gap,
// 2 drawn for the first time (reported with the skipped draws)
static int g_wait_kind = 0;
static double g_waited_ms[3];
static uint64_t g_waited_n[3];
static bool wait_compiled(const std::atomic<int>& st) {
    static const double budgetMs = getenv("WWHD_COMPILE_WAIT_MS") ? atof(getenv("WWHD_COMPILE_WAIT_MS")) : 8.0;
    if (st.load(std::memory_order_acquire) == CS_READY) return true;
    if (g_building_ahead) return false;
    auto pending = [&] { return st.load(std::memory_order_acquire) == CS_PENDING; };
    if (g_wait_whole) {  // a safety limit only: compiles take milliseconds to a few hundred
        double t0 = now_ms();
        {
            std::unique_lock<std::mutex> lk(g_compile_m);
            g_compile_cv.wait_for(lk, std::chrono::seconds(2), [&] { return !pending(); });
        }
        g_waited_ms[g_wait_kind] += now_ms() - t0;
        g_waited_n[g_wait_kind]++;
        return st.load(std::memory_order_acquire) == CS_READY;
    }
    static uint64_t frame = ~0ull;
    static double spent = 0;
    if (frame != R.frame) { frame = R.frame; spent = 0; }
    double t0 = now_ms();
    if (pending() && spent < budgetMs) {
        std::unique_lock<std::mutex> lk(g_compile_m);
        g_compile_cv.wait_for(lk, std::chrono::microseconds((int64_t)((budgetMs - spent) * 1000)), [&] { return !pending(); });
    }
    spent += now_ms() - t0;
    return st.load(std::memory_order_acquire) == CS_READY;
}

static void cache_record_pipeline(const uint32_t* regs, Shader* vs, Shader* ps, uint64_t fsKey, const PipelineState& st);

// everything vkCreateGraphicsPipelines points to, kept alive for a background compile
struct PipelineDesc {
    VkPipelineShaderStageCreateInfo stages[2];
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attribs;
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState blend[8]{};
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    VkDynamicState dyn[7];
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
};

static VkPrimitiveTopology topology_of(uint32_t t) { return (VkPrimitiveTopology)t; }

static Pipeline* get_pipeline(const uint32_t* regs, Shader* vs, Shader* ps, LatteFetchShader* fs, uint64_t fsKey, const PipelineState& st) {
    // depth clamp emulated (depth_clamp_variant): the variant pair, once compiled; else as before
    Module* vm = vs->mod;
    Module* pm = ps->mod;
    if (st.depthClamp && !R.features.depthClamp && vs->clampMod) {
        start_module(vs->clampMod);
        if (vs->clampMod->state != CS_FAILED) {
            if (!wait_compiled(vs->clampMod->state)) return nullptr;
            vm = vs->clampMod;
        }
    }
    // shaders with identical translations share pipelines
    Module* mods[2] = {vm, pm};
    uint64_t h = hash_bytes(mods, sizeof mods);
    h = hash_bytes(&vs->dsl, sizeof(vs->dsl), h);
    h = hash_bytes(&ps->dsl, sizeof(ps->dsl), h);
    h ^= fsKey;
    h = hash_bytes(&st, sizeof st, h);
    // ~1750 pipelines: the frame's are found in a small table first (pipelines are never freed)
    static HotCache<Pipeline*, 1024> hot;
    if (Pipeline** p = hot.find(h)) return wait_compiled((*p)->status) ? *p : nullptr;
    auto it = g_pipelines.find(h);
    if (it != g_pipelines.end()) {
        hot.put(h, it->second);
        return wait_compiled(it->second->status) ? it->second : nullptr;
    }
    auto* pl = new Pipeline();
    g_pipelines[h] = pl;
    hot.put(h, pl);
    cache_record_pipeline(regs, vs, ps, fsKey, st);
    pl->layout = get_pipeline_layout(vs, ps);

    auto d = std::make_shared<PipelineDesc>();
    d->stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm->module, "main", nullptr};
    d->stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pm->module, "main", nullptr};
    // vertex input: attributes fetched as uint and decoded (byte-swapped) in the shader
    for (auto& g : fs->bufferGroups) {
        bool instanced = false, any = false;
        for (sint32 j = 0; j < g.attribCount; j++) {
            auto& a = g.attrib[j];
            sint32 loc = vs->dec->resourceMapping.attributeMapping[a.semanticId];
            if (loc < 0) continue;
            VkFormat vf = vertex_format((uint32_t)a.format);
            if (vf == VK_FORMAT_UNDEFINED) continue;
            d->attribs.push_back({(uint32_t)loc, (uint32_t)g.attributeBufferIndex, vf, (uint32_t)a.offset});
            if (a.fetchType == LatteConst::VertexFetchType2::INSTANCE_DATA) instanced = true;
            any = true;
        }
        if (!any) continue;
        uint32_t stride = (regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 2] >> 11) & 0xFFFF;
        d->bindings.push_back({(uint32_t)g.attributeBufferIndex, stride,
                               instanced && stride ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
    }
    d->vi.vertexBindingDescriptionCount = (uint32_t)d->bindings.size();
    d->vi.pVertexBindingDescriptions = d->bindings.data();
    d->vi.vertexAttributeDescriptionCount = (uint32_t)d->attribs.size();
    d->vi.pVertexAttributeDescriptions = d->attribs.data();
    d->ia.topology = topology_of(st.topology);
    d->vp.viewportCount = d->vp.scissorCount = 1;
    d->rs.depthClampEnable = st.depthClamp && R.features.depthClamp;
    d->rs.polygonMode = VK_POLYGON_MODE_FILL;
    d->rs.cullMode = ((st.cull & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((st.cull & 2) ? VK_CULL_MODE_BACK_BIT : 0);
    d->rs.frontFace = st.frontCCW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    d->rs.depthBiasEnable = VK_TRUE;  // values are dynamic (zero when the game disables the offset)
    d->rs.lineWidth = 1.0f;
    d->ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    if (st.pass.depth) {
        LATTE_DB_DEPTH_CONTROL dc;
        memcpy(&dc, &st.depthControl, 4);
        if (dc.get_Z_ENABLE()) {
            d->ds.depthTestEnable = VK_TRUE;
            d->ds.depthWriteEnable = dc.get_Z_WRITE_ENABLE();
            d->ds.depthCompareOp = compare_op((uint32_t)dc.get_Z_FUNC());
        }
        if (dc.get_STENCIL_ENABLE() && st.pass.stencil) {
            d->ds.stencilTestEnable = VK_TRUE;
            d->ds.front.compareOp = compare_op((uint32_t)dc.get_STENCIL_FUNC_F());
            d->ds.front.failOp = stencil_op((uint32_t)dc.get_STENCIL_FAIL_F());
            d->ds.front.depthFailOp = stencil_op((uint32_t)dc.get_STENCIL_ZFAIL_F());
            d->ds.front.passOp = stencil_op((uint32_t)dc.get_STENCIL_ZPASS_F());
            if (dc.get_BACK_STENCIL_ENABLE()) {
                d->ds.back.compareOp = compare_op((uint32_t)dc.get_STENCIL_FUNC_B());
                d->ds.back.failOp = stencil_op((uint32_t)dc.get_STENCIL_FAIL_B());
                d->ds.back.depthFailOp = stencil_op((uint32_t)dc.get_STENCIL_ZFAIL_B());
                d->ds.back.passOp = stencil_op((uint32_t)dc.get_STENCIL_ZPASS_B());
            } else {
                d->ds.back = d->ds.front;
            }
        }
    }

    uint32_t colorCount = 0;
    for (int i = 0; i < 8; i++)
        if (st.pass.color[i]) colorCount = i + 1;
    uint32_t blendMask = (st.colorControl >> 8) & 0xFF;
    int firstUsed = -1;
    for (uint32_t i = 0; i < colorCount; i++) {
        VkPipelineColorBlendAttachmentState& ca = d->blend[i];
        if (!st.pass.color[i]) continue;
        if (firstUsed < 0) firstUsed = (int)i;
        uint32_t m = (st.targetMask >> (4 * i)) & 0xF;
        ca.colorWriteMask = m;  // R=1 G=2 B=4 A=8, as in Vulkan
        // integer targets can't blend
        bool isInt = (st.intTargets >> i) & 1;
        if ((blendMask & (1 << i)) && !isInt) {
            LATTE_CB_BLENDN_CONTROL b;
            memcpy(&b, &st.blend[i], 4);
            ca.blendEnable = VK_TRUE;
            ca.colorBlendOp = blend_op((uint32_t)b.get_COLOR_COMB_FCN());
            ca.srcColorBlendFactor = blend_factor((uint32_t)b.get_COLOR_SRCBLEND());
            ca.dstColorBlendFactor = blend_factor((uint32_t)b.get_COLOR_DSTBLEND());
            if (b.get_SEPARATE_ALPHA_BLEND()) {
                ca.alphaBlendOp = blend_op((uint32_t)b.get_ALPHA_COMB_FCN());
                ca.srcAlphaBlendFactor = blend_factor((uint32_t)b.get_ALPHA_SRCBLEND());
                ca.dstAlphaBlendFactor = blend_factor((uint32_t)b.get_ALPHA_DSTBLEND());
            } else {
                ca.alphaBlendOp = ca.colorBlendOp;
                ca.srcAlphaBlendFactor = ca.srcColorBlendFactor;
                ca.dstAlphaBlendFactor = ca.dstColorBlendFactor;
            }
        }
    }
    // without independent blending every attachment must use the same state
    if (!R.features.independentBlend && firstUsed >= 0)
        for (uint32_t i = 0; i < colorCount; i++) d->blend[i] = d->blend[firstUsed];
    d->cb.attachmentCount = colorCount;
    d->cb.pAttachments = d->blend;
    d->dyn[0] = VK_DYNAMIC_STATE_VIEWPORT;
    d->dyn[1] = VK_DYNAMIC_STATE_SCISSOR;
    d->dyn[2] = VK_DYNAMIC_STATE_DEPTH_BIAS;
    d->dyn[3] = VK_DYNAMIC_STATE_BLEND_CONSTANTS;
    d->dyn[4] = VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK;
    d->dyn[5] = VK_DYNAMIC_STATE_STENCIL_WRITE_MASK;
    d->dyn[6] = VK_DYNAMIC_STATE_STENCIL_REFERENCE;
    d->dy.dynamicStateCount = 7;
    d->dy.pDynamicStates = d->dyn;
    d->pi.stageCount = 2;
    d->pi.pStages = d->stages;
    d->pi.pVertexInputState = &d->vi;
    d->pi.pInputAssemblyState = &d->ia;
    d->pi.pViewportState = &d->vp;
    d->pi.pRasterizationState = &d->rs;
    d->pi.pMultisampleState = &d->ms;
    d->pi.pDepthStencilState = &d->ds;
    d->pi.pColorBlendState = &d->cb;
    d->pi.pDynamicState = &d->dy;
    d->pi.layout = pl->layout;
    d->pi.renderPass = get_render_pass(st.pass);

    auto build = [pl, d, vsKey = vs->key, psKey = ps->key] {
        auto t0 = std::chrono::steady_clock::now();
        VkResult r = create_graphics_pipeline(&d->pi, &pl->pipeline);
        g_t_pipeline_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (r != VK_SUCCESS) LOG("[gfx] pipeline creation failed: VkResult %d", (int)r);
        else {
            g_pipelines_created++;
            rec::crumb_pipeline(pl->pipeline, vsKey, psKey);
        }
        pl->status.store(r == VK_SUCCESS ? CS_READY : CS_FAILED, std::memory_order_release);
        compile_done();
    };
    if (g_sync_shaders) {
        build();
        return pl->status == CS_READY ? pl : nullptr;
    }
    g_compiles_in_flight++;
    g_workers.push([build] {
        build();
        g_compiles_in_flight--;
    }, !g_cache_replaying);
    return wait_compiled(pl->status) ? pl : nullptr;
}

// ---------------------------------------------------------------- samplers and textures
static std::unordered_map<uint64_t, VkSampler> g_samplers;

static VkSamplerAddressMode address_mode(uint32_t c) {
    switch (c) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case 3: case 5: case 7:
        return R.mirrorClampToEdge ? VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    }
}

// enhancement, toggled in game (settings menu; off by default, WWHD_ANISO=1 starts with it on): 16x
// anisotropic filtering on mipmapped, linearly filtered textures
static std::atomic<bool> g_aniso{[] { const char* e = getenv("WWHD_ANISO"); return e && atoi(e) != 0; }()};
bool aniso_enabled() { return g_aniso.load(std::memory_order_relaxed); }
void set_aniso(bool v) { g_aniso = v; LOG("[gfx] anisotropic filtering %s", v ? "on" : "off"); }

// allowAniso: the bound texture is a real mipmapped asset (not a buffer the GPU rendered); the game also
// reads its screen-sized lighting/occlusion buffers through mip-filtering samplers, and anisotropy there
// blurs the screen-space effects
// pack: the mod packs' texture rules for the bound texture (packs.h; anisotropy, LOD bias), or null
static VkSampler get_sampler(const uint32_t* w, bool allowAniso = false, bool nearestOnly = false,
                             const packs::TextureOverride* pack = nullptr) {
    const bool anisoForce = allowAniso && aniso_enabled() && !nearestOnly && !(pack && pack->anisotropy >= 0);
    uint64_t h = hash_bytes(w, 12) ^ (anisoForce ? 0x5A5A5A5A5A5A5A5Aull : 0) ^ (nearestOnly ? 0x4E4E4E4E00000000ull : 0);
    if (pack) {
        const int32_t f[5] = {pack->anisotropy, pack->hasLodBias, pack->hasRelativeLodBias, pack->lodBias, pack->relativeLodBias};
        h = hash_bytes(f, sizeof f, h);
    }
    auto it = g_samplers.find(h);
    if (it != g_samplers.end()) return it->second;
    LATTE_SQ_TEX_SAMPLER_WORD0_0 w0;
    LATTE_SQ_TEX_SAMPLER_WORD1_0 w1;
    memcpy(&w0, &w[0], 4);
    memcpy(&w1, &w[1], 4);
    VkSamplerCreateInfo d{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    d.addressModeU = address_mode((uint32_t)w0.get_CLAMP_X());
    d.addressModeV = address_mode((uint32_t)w0.get_CLAMP_Y());
    d.addressModeW = address_mode((uint32_t)w0.get_CLAMP_Z());
    // E_XY_FILTER: only POINT (0) and ANISO_POINT (4) are nearest; bilinear/bicubic/aniso-bilinear filter
    auto xy = [](uint32_t f) { return (f == 0 || f == 4) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR; };
    d.magFilter = xy((uint32_t)w0.get_XY_MAG_FILTER());
    d.minFilter = xy((uint32_t)w0.get_XY_MIN_FILTER());
    uint32_t mip = (uint32_t)w0.get_MIP_FILTER();
    d.mipmapMode = mip >= 2 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (nearestOnly) {  // the format can't be filtered (filters_linear)
        d.magFilter = d.minFilter = VK_FILTER_NEAREST;
        d.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    }
    float maxAniso = 1.0f;
    uint32_t anisoRatio = (uint32_t)w0.get_MAX_ANISO_RATIO();
    if (pack && pack->anisotropy >= 0) anisoRatio = (uint32_t)pack->anisotropy;  // a pack's level instead of the game's
    if (anisoRatio) maxAniso = (float)std::min(1u << anisoRatio, 16u);
    if (anisoForce && mip != 0 && d.minFilter == VK_FILTER_LINEAR && (uint32_t)w0.get_DEPTH_COMPARE_FUNCTION() == 0) maxAniso = 16;
    if (maxAniso > 1.0f && R.features.samplerAnisotropy) {
        d.anisotropyEnable = VK_TRUE;
        d.maxAnisotropy = std::min(maxAniso, R.props.limits.maxSamplerAnisotropy);
    }
    if (anisoForce) {  // shows the option reaches the GPU (the first few samplers it changes)
        static std::atomic<int> forced{0};
        int n = ++forced;
        if (n == 1 || n == 10 || n == 100)
            LOG("[gfx] anisotropic filtering: %d texture samplers at %gx", n, d.anisotropyEnable ? d.maxAnisotropy : 1.0f);
    }
    if (mip == 0) {  // not mipmapped: base level only
        d.minLod = 0;
        d.maxLod = 0.25f;
    } else {
        d.minLod = w1.get_MIN_LOD() / 64.0f;
        d.maxLod = std::max(d.minLod, w1.get_MAX_LOD() / 64.0f);
    }
    // a pack's LOD bias (as Cemu: the game's own bias plus the relative one, or the absolute one; without a
    // pack rule the game's bias stays unused, as before)
    if (pack && (pack->hasLodBias || pack->hasRelativeLodBias)) {
        int bias = (int)w1.get_LOD_BIAS();
        if (pack->hasRelativeLodBias) bias += pack->relativeLodBias;
        if (pack->hasLodBias) bias = pack->lodBias;
        const float lim = R.props.limits.maxSamplerLodBias;
        d.mipLodBias = std::clamp(bias / 64.0f, -lim, lim);
    }
    if (pack) {  // shows the rules reach the GPU (the first few samplers they change)
        static std::atomic<int> changed{0};
        int n = ++changed;
        if (n <= 3 || n == 100)
            LOG("[gfx] mod pack texture rule: sampler %d at %gx anisotropy, LOD bias %+.2f", n, d.anisotropyEnable ? d.maxAnisotropy : 1.0f,
                d.mipLodBias);
    }
    uint32_t border = (uint32_t)w0.get_BORDER_COLOR_TYPE();
    d.borderColor = border == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                  : border == 2 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (uint32_t cmp = (uint32_t)w0.get_DEPTH_COMPARE_FUNCTION()) {
        d.compareEnable = VK_TRUE;
        d.compareOp = compare_op(cmp);
    }
    VkSampler s;
    VK_CHECK(vkCreateSampler(R.device, &d, nullptr, &s));
    g_samplers[h] = s;
    return s;
}

// the view type a decompiled shader declares for a texture unit (see LatteDecompilerEmitGLSLHeader.hpp)
static VkImageViewType shader_view_type(Latte::E_DIM dim) {
    switch (dim) {
    case E_DIM::DIM_1D: return VK_IMAGE_VIEW_TYPE_1D;
    case E_DIM::DIM_1D_ARRAY: return VK_IMAGE_VIEW_TYPE_1D_ARRAY;
    case E_DIM::DIM_3D: return VK_IMAGE_VIEW_TYPE_3D;
    case E_DIM::DIM_CUBEMAP: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;  // samplerCubeArray
    case E_DIM::DIM_2D_ARRAY: case E_DIM::DIM_2D_ARRAY_MSAA: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    default: return VK_IMAGE_VIEW_TYPE_2D;
    }
}

static VkImageView null_texture(VkImageViewType type) {
    static std::unordered_map<int, VkImageView> cache;
    static std::vector<Image*> images;
    auto it = cache.find((int)type);
    if (it != cache.end()) return it->second;
    auto* img = new Image();
    images.push_back(img);
    VkImageType it_ = type == VK_IMAGE_VIEW_TYPE_1D || type == VK_IMAGE_VIEW_TYPE_1D_ARRAY ? VK_IMAGE_TYPE_1D
                    : type == VK_IMAGE_VIEW_TYPE_3D                                       ? VK_IMAGE_TYPE_3D
                                                                                          : VK_IMAGE_TYPE_2D;
    bool cube = type == VK_IMAGE_VIEW_TYPE_CUBE || type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY;
    uint32_t layers = cube ? 6 : 1;
    if (!create_image(*img, it_, type, VK_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, layers, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, cube, false, false))
        fatal("cannot create a placeholder texture");
    prepare(*img, Use::COPY_DST);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
    vkCmdClearColorImage(command_buffer(), img->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    prepare(*img, Use::SAMPLED);
    VkImageView v = make_view(*img, type, 0, layers, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    if (!v) fatal("cannot create a placeholder texture view");
    cache[(int)type] = v;
    return v;
}

// image view with the shader's expected type and the resource's component swizzle; null if the
// image can't be viewed that way
struct CachedView {
    VkImage image;
    VkImageView view;
};
static std::unordered_map<uint64_t, CachedView> g_views;
static HotCache<VkImageView, 1024> g_views_hot;  // the frame's views first; cleared with g_views' erasures

static VkImageView texture_view(Surface* s, VkImageViewType type, uint32_t word4) {
    LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    memcpy(&w4, &word4, 4);
    uint32_t sel[4] = {(uint32_t)w4.get_DST_SEL_X(), (uint32_t)w4.get_DST_SEL_Y(), (uint32_t)w4.get_DST_SEL_Z(), (uint32_t)w4.get_DST_SEL_W()};
    if (s->fmt.depth) sel[0] = 0, sel[1] = 1, sel[2] = 2, sel[3] = 3;  // depth: shaders read .x
    uint64_t key = (uint64_t)(uintptr_t)s->img.image ^ ((uint64_t)type << 56) ^ ((uint64_t)(sel[0] | sel[1] << 4 | sel[2] << 8 | sel[3] << 12) << 40);
    if (VkImageView* hv = g_views_hot.find(key)) return *hv;
    auto it = g_views.find(key);
    if (it != g_views.end()) {
        g_views_hot.put(key, it->second.view);
        return it->second.view;
    }
    const Image& img = s->img;
    uint32_t layers = 0;
    switch (type) {
    case VK_IMAGE_VIEW_TYPE_1D: if (img.type == VK_IMAGE_TYPE_1D) layers = 1; break;
    case VK_IMAGE_VIEW_TYPE_1D_ARRAY: if (img.type == VK_IMAGE_TYPE_1D) layers = img.layers; break;
    case VK_IMAGE_VIEW_TYPE_3D: if (img.type == VK_IMAGE_TYPE_3D) layers = 1; break;
    case VK_IMAGE_VIEW_TYPE_2D: if (img.type == VK_IMAGE_TYPE_2D) layers = 1; break;
    case VK_IMAGE_VIEW_TYPE_2D_ARRAY: if (img.type == VK_IMAGE_TYPE_2D) layers = img.layers; break;
    case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
        if (img.type == VK_IMAGE_TYPE_2D && img.cube && img.layers >= 6) {
            layers = img.layers / 6 * 6;
            if (!R.features.imageCubeArray) { type = VK_IMAGE_VIEW_TYPE_CUBE; layers = 6; }
        }
        break;
    default: break;
    }
    VkImageView v = VK_NULL_HANDLE;
    if (layers) {
        static const VkComponentSwizzle sw[8] = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
                                                 VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE,
                                                 VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO};
        VkComponentMapping cm{sw[sel[0] & 7], sw[sel[1] & 7], sw[sel[2] & 7], sw[sel[3] & 7]};
        if (s->fmt.depth) cm = {};
        v = make_view(s->img, type, 0, layers, cm, s->fmt.depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!v) {
        static int warned = 0;
        if (warned++ < 10)
            LOG("[gfx] texture view failed: %ux%u vk format %d image type %d -> view type %d", s->width, s->height, (int)img.format,
                (int)img.type, (int)type);
    }
    g_views[key] = {s->img.image, v};
    g_views_hot.put(key, v);
    return v;
}

// ---------------------------------------------------------------- per-stage resources
// enhancement, toggled in game (settings menu; WWHD_AO_HIRES=0 starts with it off): the game
// downsamples depth to 640x360 (PS 3BB9DE00) and computes ambient occlusion from it at 960x540
// (PS 44BDFD00). The size mismatch is what leaves lines in shadowed grass. With this on, the
// downsample is drawn a second time into a private 960x540 copy and the occlusion pass reads that
// copy, one depth texel per pixel. Every other reader keeps the game's 640x360 buffer.
static std::atomic<bool> g_ao_hires{[] { const char* e = getenv("WWHD_AO_HIRES"); return !e || atoi(e) != 0; }()};
bool ao_hires_enabled() { return g_ao_hires.load(std::memory_order_relaxed); }
void set_ao_hires(bool v) { g_ao_hires = v; LOG("[gfx] full-size occlusion depth %s", v ? "on" : "off"); }
constexpr uint32_t kDepthDownsamplePS = 0x3BB9DE00, kOcclusionPS = 0x44BDFD00;
static bool g_hires_redraw = false;          // inside the second draw of the downsample
static uint32_t g_hires_src = 0;             // guest address of the game's 640x360 buffer
static uint64_t g_hires_frame = ~0ull;       // frame the private copy was last drawn
static Surface g_hires_color, g_hires_depth;

static Surface* hires_surface(Surface& dst, const Surface* like) {
    uint32_t w = like->width * 3 / 2, h = like->height * 3 / 2;
    if (!dst.img.image || dst.width != w || dst.height != h || dst.fmt.format != like->fmt.format) {
        // the previous image stays alive: framebuffers and views may still refer to it
        Surface s;
        s.width = w;
        s.height = h;
        s.slices = 1;
        s.mips = 1;
        s.format = like->format;
        s.dim = 1;
        s.isDepth = like->isDepth;
        s.fmt = like->fmt;
        s.addr = 0;  // private: never found by address lookups
        if (!create_surface_image(&s, true)) return nullptr;
        dst = std::move(s);
    }
    return &dst;
}

// a copy of `s` taken now, for sampling while `s` is a bound attachment (Vulkan forbids reading an
// attachment of the current render pass as a texture; Metal used framebuffer fetch instead)
static Surface* feedback_copy(Surface* s) {
    if (!s->feedbackCopy) {
        auto* c = new Surface();
        c->width = s->width;
        c->height = s->height;
        c->slices = s->slices;
        c->mips = s->img.mips;  // (a picture with its mip chain in place: all of it)
        c->format = s->format;
        c->dim = s->dim;
        c->isDepth = s->isDepth;
        c->fmt = s->fmt;
        c->gpuWritten = true;
        if (!create_surface_image(c, true)) { delete c; return nullptr; }
        s->feedbackCopy = c;
    }
    Surface* c = s->feedbackCopy;
    prepare(s->img, Use::COPY_SRC);
    prepare(c->img, Use::COPY_DST);
    VkImageCopy r[16]{};
    const uint32_t levels = std::min({s->img.mips, c->img.mips, 16u});
    for (uint32_t l = 0; l < levels; l++) {
        r[l].srcSubresource = {s->img.aspect, l, 0, std::min(s->img.layers, c->img.layers)};
        r[l].dstSubresource = {c->img.aspect, l, 0, std::min(s->img.layers, c->img.layers)};
        r[l].extent = {std::max(std::min(s->img.width, c->img.width) >> l, 1u), std::max(std::min(s->img.height, c->img.height) >> l, 1u), 1};
    }
    vkCmdCopyImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, r);
    return c;
}

// textures of one stage, resolved before the render pass begins
struct StageTextures {
    VkImageView view[LATTE_NUM_MAX_TEX_UNITS];
    VkSampler sampler[LATTE_NUM_MAX_TEX_UNITS];
    float unitScale[LATTE_NUM_MAX_TEX_UNITS];  // by texture unit: resolution scale of the bound surface (x)
    float unitScaleY[LATTE_NUM_MAX_TEX_UNITS]; // ... (y; differs from x for aspect-widened targets)
    Image* sampled[LATTE_NUM_MAX_TEX_UNITS];   // by binding: the image to prepare for sampling (prepare_sampled)
    int count = 0;
    bool tvPicture = false;  // samples a rendered buffer of the TV picture's shape (and size)
};

// Every sampled image goes to its shader-read layout once all textures of the draw are resolved:
// resolving one texture can copy from another one already resolved (a surface at the same address
// in another format, a feedback copy), which leaves that one in TRANSFER_SRC while its descriptor
// says SHADER_READ_ONLY (found by the validation layer).
static void prepare_sampled(StageTextures& t, bool vertex) {
    for (int i = 0; i < t.count; i++)
        if (t.sampled[i]) prepare_read(*t.sampled[i], vertex ? VK_PIPELINE_STAGE_VERTEX_SHADER_BIT : VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
}

// Linear filtering where the format supports it (depth formats often don't: D32_SFLOAT on the Adreno
// 640 with Qualcomm's driver v502; found by the validation layer)
static bool filters_linear(VkFormat f) {
    static std::unordered_map<uint32_t, bool> known;  // render thread
    auto it = known.find((uint32_t)f);
    if (it != known.end()) return it->second;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R.pd, f, &fp);
    bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
    if (!ok) LOG("[gfx] format %d can't be filtered linearly: its textures are point sampled", (int)f);
    return known.emplace((uint32_t)f, ok).first->second;
}

static void resolve_textures(const uint32_t* regs, Shader* sh, bool vertex, Surface* const* colors, Surface* depth, StageTextures& out) {
    LatteDecompilerShader* dec = sh->dec;
    auto& rm = dec->resourceMapping;
    uint32_t texBase = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    uint32_t samplerBase = vertex ? SAMPLER_BASE_INDEX_VERTEX : SAMPLER_BASE_INDEX_PIXEL;
    for (int u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++) out.unitScale[u] = out.unitScaleY[u] = 1.0f;
    out.count = rm.getTextureCount();
    out.tvPicture = false;
    for (sint32 i = 0; i < rm.getTextureCount(); i++) {
        out.view[i] = VK_NULL_HANDLE;
        out.sampler[i] = VK_NULL_HANDLE;
        out.sampled[i] = nullptr;
        sint32 unit = rm.getRelativeTextureUnitFromRelativeBindingPoint(i);
        if (unit < 0) continue;
        VkImageViewType type = shader_view_type(dec->textureUnitDim[unit]);
        const uint32_t* tw = &regs[texBase + unit * 7];
        // the levels its sampler reaches (get_sampler: no mips -> the base level; else up to MAX_LOD)
        uint32_t reach = 16;
        if (uint32_t si = dec->textureUnitSamplerAssignment[unit]; si < LATTE_NUM_MAX_TEX_UNITS) {
            const uint32_t* smp = &regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (si + samplerBase) * 3];
            LATTE_SQ_TEX_SAMPLER_WORD0_0 s0;
            LATTE_SQ_TEX_SAMPLER_WORD1_0 s1;
            memcpy(&s0, &smp[0], 4);
            memcpy(&s1, &smp[1], 4);
            if ((uint32_t)s0.get_MIP_FILTER() == 0) reach = 1;
            else reach = std::max((uint32_t)s1.get_MIN_LOD(), (uint32_t)s1.get_MAX_LOD()) / 64 + 2;
        }
        Surface* s = sampled_texture(tw, dec->textureUsesDepthCompare[unit], reach);
        if (!vertex && s && g_hires_src && s->addr == g_hires_src && g_hires_frame == R.frame && ao_hires_enabled() &&
            (regs[mmSQ_PGM_START_PS] << 8) == kOcclusionPS && g_hires_color.img.image)
            s = &g_hires_color;
        if (s) {
            bool attached = s == depth;
            for (int c = 0; c < 8 && !attached; c++) attached = colors[c] == s;
            if (attached) s = feedback_copy(s);
        }
        VkImageView v = s && s->img.image ? texture_view(s, type, tw[4]) : VK_NULL_HANDLE;
        uint32_t samplerIdx = dec->textureUnitSamplerAssignment[unit];
        if (samplerIdx >= LATTE_NUM_MAX_TEX_UNITS) samplerIdx = LATTE_NUM_MAX_TEX_UNITS;  // none assigned
        DLOG("[draw]   %s tex%u %08X %ux%u fmt %X gpu=%d view=%d smp %08X %08X %08X", vertex ? "VS" : "PS", unit, tw[2] << 8,
             s ? s->width : 0, s ? s->height : 0, s ? s->format : 0, s ? s->gpuWritten : -1, v != VK_NULL_HANDLE,
             regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (std::min<uint32_t>(samplerIdx, 17) + samplerBase) * 3],
             regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (std::min<uint32_t>(samplerIdx, 17) + samplerBase) * 3 + 1],
             regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (std::min<uint32_t>(samplerIdx, 17) + samplerBase) * 3 + 2]);
        if (v) {
            out.sampled[i] = &s->img;  // prepared once the draw's textures are all resolved (prepare_sampled)
            out.unitScale[unit] = s->rscale * s->ax;
            out.unitScaleY[unit] = s->rscale * s->ay;
            // a rendered picture (layouts also draw with 16:9 textures from the game files, 800x450)
            if (s->gpuWritten && s->width >= 640 && target_is_tv(s->width, s->height)) out.tvPicture = true;
        }
        else v = null_texture(type == VK_IMAGE_VIEW_TYPE_CUBE_ARRAY && !R.features.imageCubeArray ? VK_IMAGE_VIEW_TYPE_CUBE : type);
        out.view[i] = v;
        static const uint32_t kDefaultSampler[3] = {};
        const uint32_t* sw = samplerIdx < LATTE_NUM_MAX_TEX_UNITS ? &regs[REGADDR::SQ_TEX_SAMPLER_WORD0_0 + (samplerIdx + samplerBase) * 3]
                                                                 : kDefaultSampler;
        // quirk: the ambient-occlusion pass (PS 44BDFD00) point-samples its centre depth from a 640x360
        // buffer while drawing 960x540; every third row lands half a texel off and shows as screen-fixed
        // lines on sloped ground in shadow. Bilinear for that one fetch matches its neighbour fetches.
        uint32_t patched[3];
        if (ao_mode() >= 1 && !vertex && unit == 0 && (regs[mmSQ_PGM_START_PS] << 8) == 0x44BDFD00) {
            memcpy(patched, sw, sizeof patched);
            patched[0] = (patched[0] & ~0x7E00u) | (1u << 9) | (1u << 12);  // XY mag/min filter: bilinear
            sw = patched;
        }
        // compare samplers (shadow maps) filter depth by the compare feature, not this one
        LATTE_SQ_TEX_SAMPLER_WORD0_0 sw0;
        memcpy(&sw0, &sw[0], 4);
        const bool nearestOnly = v && s && (uint32_t)sw0.get_DEPTH_COMPARE_FUNCTION() == 0 && !filters_linear(s->img.format);
        const packs::TextureOverride* pack = s ? packs::texture_override(s->format, s->tileMode, s->width, s->height, s->slices, s->addr) : nullptr;
        out.sampler[i] = get_sampler(sw, s && !s->gpuWritten && s->mips > 1, nearestOnly, pack);
    }
}

// Descriptor sets for shaders with dynamic uniform buffers, kept across draws and frames. Everything
// they reference (image views, samplers, transient buffers) lives as long as the renderer.
static std::unordered_map<uint64_t, VkDescriptorSet> g_set_cache;
static HotCache<VkDescriptorSet, 1024> g_set_hot;  // the frame's sets first; cleared with g_set_cache
static uint64_t g_set_cache_epoch = 0;  // bumped when cached sets are dropped (bind_stage's last-set check)
static std::vector<VkDescriptorPool> g_set_pools;

static VkDescriptorSet cached_descriptor_set(uint64_t key, VkDescriptorSetLayout dsl, VkWriteDescriptorSet* writes, uint32_t nw) {
    if (VkDescriptorSet* hs = g_set_hot.find(key)) return *hs;
    auto it = g_set_cache.find(key);
    if (it != g_set_cache.end()) {
        g_set_hot.put(key, it->second);
        return it->second;
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    for (int attempt = 0; attempt < 2 && !set; attempt++) {
        if (!g_set_pools.empty()) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = g_set_pools.back();
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &dsl;
            if (vkAllocateDescriptorSets(R.device, &ai, &set) == VK_SUCCESS) break;
            set = VK_NULL_HANDLE;
        }
        VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8192},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8192}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 2048;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        VkDescriptorPool pool;
        VK_CHECK(vkCreateDescriptorPool(R.device, &pi, nullptr, &pool));
        g_set_pools.push_back(pool);
    }
    if (!set) fatal("cannot allocate a cached descriptor set");
    for (uint32_t i = 0; i < nw; i++) writes[i].dstSet = set;
    vkUpdateDescriptorSets(R.device, nw, writes, 0, nullptr);
    g_set_cache[key] = set;
    g_set_hot.put(key, set);
    return set;
}

// A render target is being replaced by an image of another size (the resolution scale changed):
// forget everything that refers to the old image and free it once the GPU is done with it.
// Framebuffers and cached descriptor sets are dropped wholesale (rare); the sets stay allocated
// in their pool until the next trim.
void retire_image(const Image& old) {
    end_pass();
    std::vector<VkImageView> views;
    for (VkImageView v : old.attachViews)
        if (v) views.push_back(v);
    for (auto it = g_views.begin(); it != g_views.end();) {
        if (it->second.image == old.image) {
            if (it->second.view) views.push_back(it->second.view);
            it = g_views.erase(it);
        } else {
            ++it;
        }
    }
    g_views_hot.clear();
    std::vector<VkFramebuffer> fbs;
    for (auto& [k, fb] : g_framebuffers) fbs.push_back(fb);
    g_framebuffers.clear();
    g_set_cache.clear();
    g_set_hot.clear();
    g_set_cache_epoch++;
    Image img = old;
    on_complete([img, views, fbs] {
        for (VkFramebuffer fb : fbs) vkDestroyFramebuffer(R.device, fb, nullptr);
        for (VkImageView v : views) vkDestroyImageView(R.device, v, nullptr);
        vmaDestroyImage(R.vma, img.image, img.alloc);
    });
}

// frame boundary (swap): bound the cache's memory by starting over when it gets large (rare)
void descriptor_cache_trim() {
    if (g_set_cache.size() < 32768) return;
    wait_idle();
    for (VkDescriptorPool p : g_set_pools) vkResetDescriptorPool(R.device, p, 0);
    g_set_cache.clear();
    g_set_hot.clear();
    g_set_cache_epoch++;
    LOG("[gfx] descriptor set cache reset");
}

// binds one stage's descriptor set: textures, support buffer (uniform registers, remapped
// uniforms, helper values) and uniform blocks
static void bind_stage(VkCommandBuffer cmd, VkPipelineLayout layout, const uint32_t* regs, Shader* sh, bool vertex,
                       const StageTextures& tex, float targetScale, float targetScaleY) {
    if (!sh->dsl || sh->bindings.empty()) return;
    LatteDecompilerShader* dec = sh->dec;
    auto& rm = dec->resourceMapping;
    const bool dyn = sh->dynamicUbos;
    VkDescriptorSet set = dyn ? VK_NULL_HANDLE : alloc_descriptor_set(sh->dsl);
    const VkDescriptorType uboType = dyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    struct Dyn { uint32_t binding, offset; } dynOffsets[LATTE_NUM_MAX_UNIFORM_BUFFERS + 1];
    uint32_t nd = 0;
    VkWriteDescriptorSet writes[LATTE_NUM_MAX_TEX_UNITS + LATTE_NUM_MAX_UNIFORM_BUFFERS + 1];
    VkDescriptorImageInfo images[LATTE_NUM_MAX_TEX_UNITS];
    VkDescriptorBufferInfo buffers[LATTE_NUM_MAX_UNIFORM_BUFFERS + 1];
    uint32_t nw = 0, nb = 0;
    auto write = [&](uint32_t binding, VkDescriptorType type, VkDescriptorImageInfo* ii, VkDescriptorBufferInfo* bi) {
        VkWriteDescriptorSet& w = writes[nw++];
        w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set;
        w.dstBinding = binding;
        w.descriptorCount = 1;
        w.descriptorType = type;
        w.pImageInfo = ii;
        w.pBufferInfo = bi;
    };
    for (sint32 i = 0; i < rm.getTextureCount(); i++) {
        images[i] = {tex.sampler[i], tex.view[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        write((uint32_t)(rm.getTextureBaseBindingPoint() + i), VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &images[i], nullptr);
    }
    const VkDeviceSize uboAlign = std::max<VkDeviceSize>(R.props.limits.minUniformBufferOffsetAlignment, 16);
    const uint32_t maxRange = R.props.limits.maxUniformBufferRange;

    if (rm.uniformVarsBufferBindingPoint >= 0) {
        uint32_t size = sh->varsRange;
        Upload u = upload_alloc(size, uboAlign);
        g_bytes_vars += size;
        // assembled in cached memory, then copied in one go: clearing the upload memory (uncached on
        // some GPUs, e.g. Adreno) and writing it in scattered 16-byte pieces cost ~4% of the render thread
        static std::vector<uint8_t> vars;  // (draws run one at a time, under gx2's execution lock: no TLS lookup)
        if (vars.size() < size) vars.resize(size);
        // cleared except the uniform registers' range, copied whole below
        uint32_t regAt = size, regEnd = size;
        if (dec->uniform.loc_uniformRegister >= 0) {
            regAt = std::min<uint32_t>((uint32_t)dec->uniform.loc_uniformRegister & ~3u, size);
            regEnd = std::min<uint32_t>(regAt + dec->uniform.count_uniformRegister * 16, size);
        }
        memset(vars.data(), 0, regAt);
        memset(vars.data() + regEnd, 0, size - regEnd);
        float* f = (float*)vars.data();
        auto at = [&](sint32 loc) { return f + loc / 4; };
        if (dec->uniform.loc_alphaTestRef >= 0) {
            LATTE_SX_ALPHA_REF ref;
            uint32_t raw = regs[REGADDR::SX_ALPHA_REF];
            memcpy(&ref, &raw, 4);
            *at(dec->uniform.loc_alphaTestRef) = ref.get_ALPHA_TEST_REF();
        }
        if (dec->uniform.loc_pointSize >= 0) {
            float pw = (float)(regs[REGADDR::PA_SU_POINT_SIZE] & 0xFFFF) / 8.0f;
            *at(dec->uniform.loc_pointSize) = pw == 0 ? 1.0f / 8.0f : pw;
        }
        uint32_t aluBase = mmSQ_ALU_CONSTANT0_0 + (vertex ? 0x400 : 0);
        uint32_t blockBase = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
        if (dec->uniform.loc_remapped >= 0) {
            uint8_t* dst = (uint8_t*)at(dec->uniform.loc_remapped);
            // the decompiler fills only the list matching the shader's uniform mode
            for (auto& e : dec->list_remappedUniformEntries_register)
                memcpy(dst + e.mappedIndexOffset, &regs[aluBase + e.indexOffset / 4], 16);
            for (auto& g : dec->list_remappedUniformEntries_bufferGroups) {
                uint32_t addr = regs[blockBase + g.kcacheBankIdOffset / 4];
                if (!addr) continue;
                for (auto& e : g.entries) memcpy(dst + e.mappedIndexOffset, mem::ptr(addr + e.indexOffset), 16);
            }
            // AO mode 2: the occlusion pass's VS (44BDF900) scales its noise coordinates by remapped[0].w
            // for a 640x360 grid; the pass draws 960x540, so tile the 4x4 noise per output pixel instead
            if (vertex && ao_mode() == 2 && (regs[mmSQ_PGM_START_VS] << 8) == 0x44BDF900)
                ((float*)dst)[3] *= 1.5f;
        }
        if (dec->uniform.loc_uniformRegister >= 0) memcpy(vars.data() + regAt, &regs[aluBase], regEnd - regAt);
        if (dec->uniform.loc_windowSpaceToClipSpaceTransform >= 0) {
            float vw = 2.0f * gx2::bitsf(regs[REGADDR::PA_CL_VPORT_XSCALE]);
            // same clip coordinates as the Metal renderer: both APIs map clip to window space identically
            // with the viewports set below and in metal_draw.mm
            float vh = -2.0f * gx2::bitsf(regs[REGADDR::PA_CL_VPORT_YSCALE]);
            float* v = at(dec->uniform.loc_windowSpaceToClipSpaceTransform);
            v[0] = vw != 0 ? 2.0f / vw : 0;
            v[1] = vh != 0 ? 2.0f / vh : 0;
        }
        if (dec->uniform.loc_fragCoordScale >= 0) {
            float* v = at(dec->uniform.loc_fragCoordScale);
            v[0] = 1.0f / targetScale;  // xy: render target pixels -> guest pixels; zw origin (Vulkan layout)
            v[1] = 1.0f / targetScaleY;
            v[2] = v[3] = 0.0f;
        }
        for (auto& e : dec->uniform.list_ufTexRescale) {  // texel coordinates: guest texels -> image texels
            bool known = e.texUnit < LATTE_NUM_MAX_TEX_UNITS;
            at(e.uniformLocation)[0] = known ? tex.unitScale[e.texUnit] : 1.0f;
            at(e.uniformLocation)[1] = known ? tex.unitScaleY[e.texUnit] : 1.0f;
        }
        memcpy(u.ptr, vars.data(), size);
        buffers[nb] = {u.buf, dyn ? 0 : u.offset, size};
        if (dyn) dynOffsets[nd++] = {(uint32_t)rm.uniformVarsBufferBindingPoint, (uint32_t)u.offset};
        write((uint32_t)rm.uniformVarsBufferBindingPoint, uboType, nullptr, &buffers[nb++]);
    }

    // uniform blocks: snapshot at draw time. The GPU runs after the CPU built the frame, and games
    // rewrite uniform memory between draws.
    uint32_t blockBase = vertex ? mmSQ_VTX_UNIFORM_BLOCK_START : mmSQ_PS_UNIFORM_BLOCK_START;
    for (int i = 0; i < LATTE_NUM_MAX_UNIFORM_BUFFERS; i++) {
        sint32 binding = rm.uniformBuffersBindingPoint[i];
        if (binding < 0) continue;
        uint32_t addr = regs[blockBase + i * 7];
        // the shader's declared array size (reading past the game's block in guest memory is harmless)
        uint32_t size = sh->blockRange[i];
        // shaders that index a block dynamically (skinning palettes) declare 64 KiB: the game's own
        // block size bounds what they can read (GX2Set*UniformBlock, word 1 = size - 1). Rounded to
        // a power of two so the cached descriptor sets keep matching.
        if (uint32_t game = regs[blockBase + i * 7 + 1] + 1; game > 16 && game < size) {
            uint32_t p = 256;
            while (p < game) p <<= 1;
            size = std::min(size, p);
        }
        static SubmissionCopies blocks;
        Upload u;
        if (addr && addr + (uint64_t)size <= 0x100000000ull) {
            // Uniform blocks are compared, not reused by write tracking (as the original project's
            // uniform snapshots do): the game rewrites some (0x800-byte light/shadow blocks) without the
            // flush the tracking relies on, and a stale copy drew the shadows with another frame's
            // matrices (fix from the PR's commit 4832d83). WWHD_TRACK_UBO_WRITES=1: the tracking again.
            const bool trackUbo = g_track_ubo_writes;
            u = trackUbo ? copy_tracked(blocks, addr, size, uboAlign, g_bytes_ubo, g_bytes_ubo_reused)
                               : copy_deduped(blocks, mem::ptr(addr), addr, size, uboAlign, g_bytes_ubo, g_bytes_ubo_reused);
        } else {
            u = upload_alloc(size, uboAlign);
            memset(u.ptr, 0, size);
        }
        buffers[nb] = {u.buf, dyn ? 0 : u.offset, size};
        if (dyn) dynOffsets[nd++] = {(uint32_t)binding, (uint32_t)u.offset};
        write((uint32_t)binding, uboType, nullptr, &buffers[nb++]);
    }
    if (!dyn) {
        vkUpdateDescriptorSets(R.device, nw, writes, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, vertex ? 0 : 1, 1, &set, 0, nullptr);
        return;
    }
    // cached set: identified by its layout and contents (offsets are dynamic). Consecutive draws of a
    // stage mostly use the same textures and buffers: compare with the previous one before hashing
    struct LastSet {
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        uint32_t nt = 0, nb = 0;
        VkDescriptorImageInfo images[LATTE_NUM_MAX_TEX_UNITS];
        VkDescriptorBufferInfo buffers[LATTE_NUM_MAX_UNIFORM_BUFFERS + 1];
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t epoch = ~0ull;
    };
    static LastSet lastSet[2];
    LastSet& L = lastSet[vertex ? 0 : 1];
    const uint32_t nt = (uint32_t)rm.getTextureCount();
    if (L.epoch == g_set_cache_epoch && L.dsl == sh->dsl && L.nt == nt && L.nb == nb &&
        memcmp(L.images, images, nt * sizeof(VkDescriptorImageInfo)) == 0 &&
        memcmp(L.buffers, buffers, nb * sizeof(VkDescriptorBufferInfo)) == 0) {
        set = L.set;
    } else {
        uint64_t key = hash_bytes(&sh->dsl, sizeof(sh->dsl));
        for (uint32_t i = 0; i < nt; i++) key = hash_bytes(&images[i], 2 * sizeof(void*), key);
        for (uint32_t i = 0; i < nb; i++) key = hash_bytes(&buffers[i], sizeof(VkBuffer) + sizeof(VkDeviceSize) * 2, key);
        set = cached_descriptor_set(key, sh->dsl, writes, nw);
        L.dsl = sh->dsl;
        L.nt = nt;
        L.nb = nb;
        memcpy(L.images, images, nt * sizeof(VkDescriptorImageInfo));
        memcpy(L.buffers, buffers, nb * sizeof(VkDescriptorBufferInfo));
        L.set = set;
        L.epoch = g_set_cache_epoch;
    }
    // dynamic offsets go in binding order
    std::sort(dynOffsets, dynOffsets + nd, [](const Dyn& a, const Dyn& b) { return a.binding < b.binding; });
    uint32_t offs[LATTE_NUM_MAX_UNIFORM_BUFFERS + 1];
    for (uint32_t i = 0; i < nd; i++) offs[i] = dynOffsets[i].offset;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, vertex ? 0 : 1, 1, &set, nd, offs);
}

// ---------------------------------------------------------------- render pass
// ---------------------------------------------------------------- redundant state filter
// State recorded by the draws of the current render pass: commands that would set it to what it
// already is are skipped (each one costs the driver at the next draw). Only draws record state inside
// their render passes (presentation and the other passes begin their own), so the cache starts over
// whenever a draw render pass begins.
struct DrawState {
    VkPipeline pipeline = VK_NULL_HANDLE;
    uint32_t stencil[6] = {};
    float blend[4] = {}, bias[3] = {};
    VkViewport viewport{};
    VkRect2D scissor{};
    VkBuffer vb[16] = {};
    VkDeviceSize vbOffset[16] = {};
    VkBuffer ib = VK_NULL_HANDLE;
    bool valid = false;
};
static DrawState g_ds;
static uint64_t g_pass_serial = 0;  // render passes begun by draws (the fast path stays within one)

static bool ensure_pass(Surface* const* colors, const uint32_t* colorSlices, Surface* depth, uint32_t depthSlice, uint32_t w,
                        uint32_t h, const PassFormats& pf) {
    if (R.pass) {
        bool same = R.passDepth == depth && (!depth || R.passDepthSlice == depthSlice);
        for (int i = 0; i < 8 && same; i++)
            same = R.passColor[i] == colors[i] && (!colors[i] || R.passColorSlice[i] == colorSlices[i]);
        if (same) return true;
    }
    end_pass();
    // clears the game asked for that wait for this pass (Image::pendingClear) become its load ops
    VkClearValue clearValues[9] = {};
    uint32_t clears = 0;
    {
        uint32_t k = 0;
        VkImageAspectFlags aspects;
        for (int i = 0; i < 8; i++)
            if (colors[i]) {
                if (take_pending_clear(colors[i]->img, colorSlices[i], w, h, aspects, clearValues[k])) clears |= 1u << i;
                k++;
            }
        if (depth && take_pending_clear(depth->img, depthSlice, w, h, aspects, clearValues[k])) {
            if (aspects & VK_IMAGE_ASPECT_DEPTH_BIT) clears |= kClearDepth;
            if (aspects & VK_IMAGE_ASPECT_STENCIL_BIT) clears |= kClearStencil;
        }
    }
    for (int i = 0; i < 8; i++)
        if (colors[i]) prepare(colors[i]->img, Use::COLOR);
    if (depth) prepare(depth->img, Use::DEPTH);
    VkImageView views[9];
    uint32_t n = 0;
    for (int i = 0; i < 8; i++)
        if (colors[i]) {
            views[n] = attachment_view(colors[i]->img, colorSlices[i]);
            if (!views[n]) return false;
            n++;
            mark_gpu_written(colors[i]);
        }
    if (depth) {
        views[n] = attachment_view(depth->img, depthSlice);
        if (!views[n]) return false;
        n++;
        mark_gpu_written(depth);
    }
    if (!n) return false;
    if (log_this_frame()) {  // pass boundaries in the capture log: what each pass loads, and whether it continues the previous one
        static const Surface* lastColor0 = nullptr;
        static const Surface* lastDepth = nullptr;
        DLOG("[pass] begin %ux%u color0 %08X depth %08X (slice %u) clears %03X%s", w, h, colors[0] ? colors[0]->addr : 0,
             depth ? depth->addr : 0, depthSlice, clears, (colors[0] == lastColor0 && depth == lastDepth) ? " CONTINUES the previous pass" : "");
        lastColor0 = colors[0];
        lastDepth = depth;
    }
    VkRenderPass rp = get_render_pass(pf, clears);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = get_framebuffer(rp, views, n, w, h);
    bi.renderArea = {{0, 0}, {w, h}};
    if (clears) {
        bi.clearValueCount = n;
        bi.pClearValues = clearValues;
    }
    VkCommandBuffer cb = command_buffer();
    {
        VkFormat cf[8];
        uint32_t nc = 0;
        for (int i = 0; i < 8; i++)
            if (pf.color[i]) cf[nc++] = (VkFormat)pf.color[i];
        prof_pass_begin(w, h, cf, nc, (VkFormat)pf.depth);
    }
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
    // Dynamic state and bindings recorded from here on are tracked again. Not valid until the first
    // draw has set all of it: Vulkan keeps the previous pass's values within a command buffer (and
    // leaves them undefined in a new one), so a first draw that wants a zero depth bias, stencil
    // reference or blend constant must still set it rather than match the zeroed cache.
    g_ds = DrawState{};
    g_pass_serial++;
    R.pass = rp;
    for (int i = 0; i < 8; i++) {
        R.passColor[i] = colors[i];
        R.passColorSlice[i] = colorSlices[i];
    }
    R.passDepth = depth;
    R.passDepthSlice = depthSlice;
    if (depth && depth->width == 1280 && depth->height == 720) R.mainDepthAddr = depth->addr;
    R.passWidth = w;
    R.passHeight = h;
    return true;
}

// ---------------------------------------------------------------- indices
// Converts guest indices (possibly big-endian, possibly a primitive type Vulkan lacks or that
// mobile drivers handle poorly) into a 32-bit little-endian index list. Returns the topology.
static bool build_indices(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, std::vector<uint32_t>& out,
                          VkPrimitiveTopology& type) {
    auto idx = [&](uint32_t i) -> uint32_t {
        if (!indexAddr) return i;
        switch (indexType) {
        case 0: return ((const uint16_t*)mem::ptr(indexAddr))[i];
        case 1: return ((const uint32_t*)mem::ptr(indexAddr))[i];
        case 4: return ld16(indexAddr + 2 * i);
        case 9: return ld32(indexAddr + 4 * i);
        default: return ld16(indexAddr + 2 * i);
        }
    };
    out.clear();
    switch (prim) {
    case 1: type = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: type = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: type = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 4: type = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    case 6: type = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    case 5:  // triangle fan -> list
        type = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        for (uint32_t i = 2; i < count; i++) out.insert(out.end(), {idx(0), idx(i - 1), idx(i)});
        return true;
    case 0x13:  // quads -> list
        type = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        for (uint32_t q = 0; q + 3 < count; q += 4)
            out.insert(out.end(), {idx(q), idx(q + 1), idx(q + 2), idx(q), idx(q + 2), idx(q + 3)});
        return true;
    case 0x14:  // quad strip -> list
        type = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        for (uint32_t q = 0; q + 3 < count; q += 2)
            out.insert(out.end(), {idx(q), idx(q + 1), idx(q + 3), idx(q), idx(q + 3), idx(q + 2)});
        return true;
    case 0x12:  // line loop
        type = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        for (uint32_t i = 0; i < count; i++) out.push_back(idx(i));
        if (count) out.push_back(idx(0));
        return true;
    default:
        return false;  // rects and adjacency primitives: not supported yet
    }
    if (indexAddr) {  // the common case: lists and strips, converted in tight loops
        out.resize(count);
        uint32_t* o = out.data();
        const void* src = mem::ptr(indexAddr);
        switch (indexType) {
        case 0: { auto* s16 = (const uint16_t*)src; for (uint32_t i = 0; i < count; i++) o[i] = s16[i]; break; }
        case 1: memcpy(o, src, (size_t)count * 4); break;
        case 9: { auto* s32 = (const uint32_t*)src; for (uint32_t i = 0; i < count; i++) o[i] = __builtin_bswap32(s32[i]); break; }
        default: { auto* s16 = (const uint16_t*)src; for (uint32_t i = 0; i < count; i++) o[i] = __builtin_bswap16(s16[i]); break; }
        }
    }
    return true;
}

// Indices converted and uploaded once per submission for each (address, count, type, primitive),
// reused while the game hasn't written their pages (as copy_tracked): the same mesh drawn many
// times converts its indices once. n == 0: a non-indexed draw.
struct DrawIndices {
    Upload u;
    uint32_t n = 0;
    VkPrimitiveTopology type = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
};
static uint64_t g_index_reused, g_index_built, g_index_kept;

// Kept indices: indices the game left unchanged from one submission to the next (static meshes; ~430
// conversions a frame at Windfall on a Helio G85, each copied into uncached upload memory) stay
// converted across submissions and frames while the game hasn't written their pages, as kept vertex
// buffers, and are compared again on one frame in 16. They share one persistent buffer (power-of-two
// blocks), so draws keep binding the same index buffer instead of one per mesh (a bind costs the
// driver). A rewritten one is freed once the GPU is done with it; one rewritten twice after being
// kept stays transient. Unused ones go after ~20 s. WWHD_KEEP_INDICES=0: as before.
namespace keepidx {
constexpr uint32_t kArenaBytes = 16u << 20, kMinClass = 8;  // 256-byte blocks at least
struct Arena {
    VkBuffer buf = VK_NULL_HANDLE;
    VmaAllocation alloc = nullptr;
    uint8_t* ptr = nullptr;
    uint32_t top = 0;
    std::vector<uint32_t> free[32];  // free blocks by size class (log2 of their size)
    bool tried = false;
};
Arena A;
struct Kept { uint32_t off, cls, n, gen, prim, indexType; VkPrimitiveTopology type; uint64_t used; };
struct Seen { uint32_t gen; uint64_t frame; uint8_t drops; };
std::unordered_map<uint64_t, Kept> kept;
std::unordered_map<uint64_t, Seen> seen;
uint64_t lastSweep = 0;
const bool g_on = [] { const char* e = getenv("WWHD_KEEP_INDICES"); return !e || atoi(e) != 0; }();
bool arena() {
    if (A.buf) return true;
    if (A.tried) return false;
    A.tried = true;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = kArenaBytes;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &A.buf, &A.alloc, &info) != VK_SUCCESS) {
        A.buf = VK_NULL_HANDLE;
        LOG("[gfx] kept indices: no buffer");
        return false;
    }
    A.ptr = (uint8_t*)info.pMappedData;
    return true;
}
bool alloc(uint32_t bytes, uint32_t& off, uint32_t& cls) {
    cls = kMinClass;
    while ((1u << cls) < bytes) cls++;
    if (!A.free[cls].empty()) {
        off = A.free[cls].back();
        A.free[cls].pop_back();
        return true;
    }
    if (A.top + (1u << cls) > kArenaBytes) return false;
    off = A.top;
    A.top += 1u << cls;
    return true;
}
void release(const Kept& k) {  // once the GPU is done with the submissions that may still read it
    uint32_t off = k.off, cls = k.cls;
    on_complete([off, cls] { A.free[cls].push_back(off); });
}
}  // namespace keepidx

static bool draw_indices(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, DrawIndices& out) {
    struct Entry { DrawIndices d; uint32_t prim, indexType, gen; };
    static FlatTable<Entry> cache;
    static uint64_t serial = ~0ull;
    static std::vector<uint32_t> tmp;  // render thread; storage reused across draws
    command_buffer();
    if (R.cmdSerial != serial) {  // transient memory is only valid within one submission
        cache.clear();
        serial = R.cmdSerial;
    }
    const uint32_t bytes = count * (indexType == 1 || indexType == 9 ? 4 : 2);
    const bool cacheable = indexAddr && g_track_writes && indexAddr + (uint64_t)bytes <= 0x100000000ull;
    const uint64_t key = (uint64_t)indexAddr << 32 | count;
    if (cacheable) {
        Entry* it = cache.find(key);
        if (it && it->prim == prim && it->indexType == indexType && memw::unchanged_since(indexAddr, bytes, it->gen)) {
            g_index_reused++;
            out = it->d;
            return true;
        }
    }
    using namespace keepidx;
    if (cacheable && g_on) {
        if (R.frame - lastSweep >= 256) {  // unused for ~20 s: freed; old candidates forgotten
            lastSweep = R.frame;
            for (auto it = kept.begin(); it != kept.end();)
                if (R.frame - it->second.used > 600) {
                    release(it->second);
                    it = kept.erase(it);
                } else {
                    ++it;
                }
            for (auto it = seen.begin(); it != seen.end();)
                it = R.frame - it->second.frame > 600 && it->second.drops < 2 ? seen.erase(it) : std::next(it);
        }
        auto k = kept.find(key);
        if (k != kept.end() && k->second.prim == prim && k->second.indexType == indexType) {
            Kept& e = k->second;
            bool same = memw::unchanged_since(indexAddr, bytes, e.gen);
            if (same && ((indexAddr >> 6) + R.frame) % 16 == 0) {  // compared now and then, as kept vertices
                const uint32_t gen = memw::now();
                VkPrimitiveTopology t;
                same = build_indices(prim, count, indexType, indexAddr, tmp, t) && tmp.size() == e.n &&
                       memcmp(A.ptr + e.off, tmp.data(), (size_t)e.n * 4) == 0;
                if (!same) g_stale_reuses++;
                else e.gen = gen;
            }
            if (same) {
                e.used = R.frame;
                g_index_kept++;
                out = DrawIndices{Upload{A.buf, e.off, A.ptr + e.off}, e.n, e.type};
                cache[key] = {out, prim, indexType, e.gen};
                return true;
            }
            release(e);  // rewritten: converted per submission again until it proves stable
            kept.erase(k);
            Seen& sn = seen[key];
            sn = {memw::now(), R.frame, (uint8_t)std::min(255, sn.drops + 1)};
        }
    }
    uint32_t gen = memw::now();  // before reading: a write during it makes the next use convert again
    if (!build_indices(prim, count, indexType, indexAddr, tmp, out.type)) return false;
    g_index_built++;
    out.n = (uint32_t)tmp.size();
    // unchanged since an earlier conversion: kept from now on
    if (cacheable && g_on && out.n) {
        auto sn = seen.find(key);
        if (sn == seen.end()) {
            seen[key] = {gen, R.frame, 0};
        } else if (sn->second.drops < 2 && memw::unchanged_since(indexAddr, bytes, sn->second.gen) && arena()) {
            uint32_t off, cls;
            if (auto old = kept.find(key); old != kept.end()) {  // (kept for another primitive type)
                release(old->second);
                kept.erase(old);
            }
            if (alloc(out.n * 4, off, cls)) {
                memcpy(A.ptr + off, tmp.data(), (size_t)out.n * 4);
                kept[key] = Kept{off, cls, out.n, gen, prim, indexType, out.type, R.frame};
                seen.erase(sn);
                out.u = Upload{A.buf, off, A.ptr + off};
                cache[key] = {out, prim, indexType, gen};
                return true;
            }
        } else {
            sn->second.gen = gen;
            sn->second.frame = R.frame;
        }
    }
    out.u = out.n ? upload(tmp.data(), tmp.size() * 4, 16) : Upload{};
    if (cacheable) cache[key] = {out, prim, indexType, gen};
    return true;
}

// ---------------------------------------------------------------- persistent shader cache
// Every newly translated shader and every new pipeline is appended to a recipe file: the shader
// microcode plus the register state that shaped its translation. At startup the recipes are
// replayed, so shaders and pipelines are ready before the game asks for them (the driver's own
// compiled pipelines persist in pipelines-*.vkcache next to it, one per driver). Shader records use the same format
// as the macOS build; pipeline records have their own type since Vulkan pipelines carry more state.
// WWHD_SHADER_CACHE=<file> overrides the location, WWHD_SHADER_CACHE=0 disables it.
namespace {
// kRecShaderLinked: a pixel shader translated for a vertex shader that leaves some of its inputs
// unfed (PsLink.unfed); the record ends with that vertex shader's output mask
constexpr uint32_t kRecShader = 1, kRecPipelineVk = 4, kRecShaderLinked = 5;
FILE* g_cache_out = nullptr;
bool g_cache_disabled = false;

struct PipelineRecipe {
    uint64_t vsKey, psKey, fsKey;
    PipelineState st;
    uint32_t strides[16];
};
std::vector<PipelineRecipe> g_pending_pipelines;

std::string cache_path() {
    if (const char* e = getenv("WWHD_SHADER_CACHE")) return e;
    std::string dir = config::cache_dir.empty() ? "." : config::cache_dir;
    mkdir(dir.c_str(), 0755);
    return dir + "/shaders.bin";
}

// registers saved with a shader: everything except ALU constants and unused space
bool cache_saves_reg(uint32_t r) {
    return (r >= 0x2000 && r < 0x4000) || (r >= 0xA000 && r < 0xC000) || (r >= 0xE000 && r < 0x10000);
}

void cache_write(uint32_t type, const std::vector<uint8_t>& raw) {
    if (!g_cache_out) return;
    uLongf csize = compressBound(raw.size());
    std::vector<uint8_t> comp(csize);
    if (compress2(comp.data(), &csize, raw.data(), raw.size(), 6) != Z_OK) return;
    uint32_t hdr[3] = {type, (uint32_t)raw.size(), (uint32_t)csize};
    fwrite(hdr, sizeof hdr, 1, g_cache_out);
    fwrite(comp.data(), 1, csize, g_cache_out);
    fflush(g_cache_out);
}

template <class T> void put(std::vector<uint8_t>& v, const T& x) {
    const uint8_t* p = (const uint8_t*)&x;
    v.insert(v.end(), p, p + sizeof(T));
}
template <class T> bool get(const uint8_t*& p, const uint8_t* end, T& x) {
    if (p + sizeof(T) > end) return false;
    memcpy(&x, p, sizeof(T));
    p += sizeof(T);
    return true;
}

uint32_t fetch_shader_size(const uint32_t* regs) {
    uint32_t prog = regs[mmSQ_PGM_START_FS] << 8;
    if (!prog) return 0;
    if (ld32(prog) == kFetchOurs) return 16 + ld32(prog + 4) * 16;
    if (ld32(prog) == kFetchCodeFree) return ld32(prog + 4);
    return regs[mmSQ_PGM_START_FS + 1] << 3;
}
}  // namespace

static void cache_record_shader(const uint32_t* regs, bool vertex, const PsLink& link) {
    if (g_cache_replaying || !g_cache_out) return;
    uint32_t pgm = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    uint32_t addr = regs[pgm] << 8, size = regs[pgm + 1] << 3;
    uint32_t fsAddr = vertex ? regs[mmSQ_PGM_START_FS] << 8 : 0, fsSize = vertex ? fetch_shader_size(regs) : 0;
    std::vector<uint8_t> v;
    put(v, (uint32_t)vertex);
    put(v, size);
    put(v, fsSize);
    uint32_t n = 0;
    for (uint32_t r = 0; r < 0x10000; r++)
        if (regs[r] && cache_saves_reg(r)) n++;
    put(v, n);
    v.insert(v.end(), mem::ptr(addr), mem::ptr(addr) + size);
    if (fsSize) v.insert(v.end(), mem::ptr(fsAddr), mem::ptr(fsAddr) + fsSize);
    for (uint32_t r = 0; r < 0x10000; r++)
        if (regs[r] && cache_saves_reg(r)) { put(v, r); put(v, regs[r]); }
    bool linked = !vertex && link.unfed;
    if (linked) put(v, link.vsMask);
    cache_write(linked ? kRecShaderLinked : kRecShader, v);
}

static void cache_record_pipeline(const uint32_t* regs, Shader* vs, Shader* ps, uint64_t fsKey, const PipelineState& st) {
    if (g_cache_replaying || !g_cache_out) return;
    PipelineRecipe r{};
    r.vsKey = vs->key;
    r.psKey = ps->key;
    r.fsKey = fsKey;
    r.st = st;
    for (int i = 0; i < 16; i++) r.strides[i] = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + i * 7 + 2];
    std::vector<uint8_t> v;
    put(v, r);
    cache_write(kRecPipelineVk, v);
}

// progress of cache_load for the app's loading screen (cache_progress): 0 not started, 1 loading,
// 2 done; bytes of the file read and its size; shader variants prepared
static std::atomic<int> g_cache_state{0};
static std::atomic<uint64_t> g_cache_done{0}, g_cache_total{0}, g_cache_shaders{0};
void cache_progress(uint64_t out[4]) {
    out[0] = (uint64_t)g_cache_state.load();
    out[1] = g_cache_done.load();
    out[2] = g_cache_total.load();
    out[3] = g_cache_shaders.load();
}

// load and replay the recipes; runs once on the render thread before the first draw
// Translate one recorded state (cache or head start): regs hold its registers; the program and the
// fetch shader go to guest memory so the normal translation path can read them. One host-only
// scratch buffer is reused: translation keeps no pointer into it, and a buffer per record would run
// out of guest memory for a large cache (100k+ variants). Null for a vertex shader without a usable
// fetch shader.
struct ReplaySetup {
    bool ok = false;  // false: a vertex shader without a usable fetch shader
    LatteFetchShader* fs = nullptr;
    uint64_t fsKey = 0, key = 0;
    PsLink link;
};
static ReplaySetup replay_setup(uint32_t* regs, bool vertex, const uint8_t* code, uint32_t size, const uint8_t* fsBytes,
                                uint32_t fsSize, bool linked, uint32_t vsMask) {
    static uint32_t scratch = 0, scratchSize = 0;
    uint32_t need = ((size + 0xFF) & ~0xFFu) + fsSize;
    if (need > scratchSize) {
        scratchSize = std::max<uint32_t>(need * 2, 0x10000);
        scratch = mem::host_alloc(scratchSize, 0x100);
    }
    uint32_t prog = scratch;
    memcpy(mem::ptr(prog), code, size);
    uint32_t fsProg = 0;
    if (fsSize) {
        fsProg = scratch + ((size + 0xFF) & ~0xFFu);
        memcpy(mem::ptr(fsProg), fsBytes, fsSize);
    }
    // program_hash remembers hashes by address within a frame: forget the scratch buffer's, or a
    // record whose program has the size of the previous one would get its hash (and the previous
    // program's key: a wrong shader for a live draw, e.g. a black sky)
    g_program_hashes.erase(prog);
    program_hot(prog).addr = 0;
    g_fetch_last.frame = ~0ull;
    if (fsSize) {
        g_program_hashes.erase(fsProg);
        program_hot(fsProg).addr = 0;
    }
    regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] = prog >> 8;
    if (fsSize) regs[mmSQ_PGM_START_FS] = fsProg >> 8;
    ReplaySetup r;
    r.fs = vertex ? get_fetch_shader(regs, &r.fsKey) : nullptr;
    if (vertex && !r.fs) return r;
    r.link = linked ? ps_link_mask(regs, vsMask) : PsLink{};
    r.key = shader_key(regs, vertex, r.fsKey, r.link);
    r.ok = true;
    return r;
}
static Shader* replay_state(uint32_t* regs, bool vertex, const uint8_t* code, uint32_t size, const uint8_t* fsBytes,
                            uint32_t fsSize, bool linked, uint32_t vsMask) {
    ReplaySetup r = replay_setup(regs, vertex, code, size, fsBytes, fsSize, linked, vsMask);
    return r.ok ? get_shader_uncached(regs, vertex, r.fs, r.fsKey, r.link) : nullptr;
}

// ---- template export (WWHD_TEMPLATE_EXPORT=1): the replayed cache as a code-free head-start
// template, template.bin next to the cache (or WWHD_TEMPLATE_EXPORT=<path>). It holds no shader code: per shader state the program's
// hash and size, the registers, the fetch shader in our own or the code-free encoding and the key
// the state gave here; then the pipeline recipes whose shaders all resolved. A device rebuilds the
// records from the programs in its own game files.
//   "WWHV", u32 version 1, u32 raw size, zlib( u32 nfetch, {u32 size, bytes}*, u32 nstates,
//   {u64 key, u64 program hash, u32 program size, u32 kind (0 PS, 1 VS, 2 PS linked), i32 fetch,
//    u32 vertex shader output mask, u32 nregs, (u32 reg, u32 value)*}*, u32 npipelines, PipelineRecipe* )
namespace {
struct ExportRec { long at; uint32_t type; };
struct ExportPs { uint64_t key; long at; uint32_t regs[43]; };  // unlinked pixel shader records
constexpr uint32_t kExportPsRegs[43] = {
    mmSPI_PS_IN_CONTROL_0, mmSPI_VS_OUT_ID_0, mmSPI_VS_OUT_ID_0 + 1, mmSPI_VS_OUT_ID_0 + 2, mmSPI_VS_OUT_ID_0 + 3,
    mmSPI_VS_OUT_ID_0 + 4, mmSPI_VS_OUT_ID_0 + 5, mmSPI_VS_OUT_ID_0 + 6, mmSPI_VS_OUT_ID_0 + 7, mmSPI_VS_OUT_ID_0 + 8,
    mmSPI_VS_OUT_ID_0 + 9, mmSPI_PS_INPUT_CNTL_0, mmSPI_PS_INPUT_CNTL_0 + 1, mmSPI_PS_INPUT_CNTL_0 + 2,
    mmSPI_PS_INPUT_CNTL_0 + 3, mmSPI_PS_INPUT_CNTL_0 + 4, mmSPI_PS_INPUT_CNTL_0 + 5, mmSPI_PS_INPUT_CNTL_0 + 6,
    mmSPI_PS_INPUT_CNTL_0 + 7, mmSPI_PS_INPUT_CNTL_0 + 8, mmSPI_PS_INPUT_CNTL_0 + 9, mmSPI_PS_INPUT_CNTL_0 + 10,
    mmSPI_PS_INPUT_CNTL_0 + 11, mmSPI_PS_INPUT_CNTL_0 + 12, mmSPI_PS_INPUT_CNTL_0 + 13, mmSPI_PS_INPUT_CNTL_0 + 14,
    mmSPI_PS_INPUT_CNTL_0 + 15, mmSPI_PS_INPUT_CNTL_0 + 16, mmSPI_PS_INPUT_CNTL_0 + 17, mmSPI_PS_INPUT_CNTL_0 + 18,
    mmSPI_PS_INPUT_CNTL_0 + 19, mmSPI_PS_INPUT_CNTL_0 + 20, mmSPI_PS_INPUT_CNTL_0 + 21, mmSPI_PS_INPUT_CNTL_0 + 22,
    mmSPI_PS_INPUT_CNTL_0 + 23, mmSPI_PS_INPUT_CNTL_0 + 24, mmSPI_PS_INPUT_CNTL_0 + 25, mmSPI_PS_INPUT_CNTL_0 + 26,
    mmSPI_PS_INPUT_CNTL_0 + 27, mmSPI_PS_INPUT_CNTL_0 + 28, mmSPI_PS_INPUT_CNTL_0 + 29, mmSPI_PS_INPUT_CNTL_0 + 30,
    mmSPI_PS_INPUT_CNTL_0 + 31};
struct Exporter {
    std::unordered_map<uint64_t, ExportRec> byKey;  // first record that gave a key
    std::unordered_map<uint64_t, uint32_t> linkMask;  // pixel shader keys found linked (old records)
    std::vector<ExportPs> ps;
};
Exporter* g_export = nullptr;

bool read_record(FILE* f, long at, uint32_t& type, std::vector<uint8_t>& raw) {
    uint32_t hdr[3];
    if (fseek(f, at, SEEK_SET) != 0 || fread(hdr, sizeof hdr, 1, f) != 1) return false;
    std::vector<uint8_t> comp(hdr[2]);
    raw.resize(hdr[1]);
    if (fread(comp.data(), 1, hdr[2], f) != hdr[2]) return false;
    uLongf rsize = hdr[1];
    type = hdr[0];
    return uncompress(raw.data(), &rsize, comp.data(), hdr[2]) == Z_OK && rsize == hdr[1];
}

template <class T> void put_le(std::vector<uint8_t>& v, T x) {
    const uint8_t* p = (const uint8_t*)&x;
    v.insert(v.end(), p, p + sizeof x);
}
}  // namespace

// the parse results the translation uses (not vkPipelineHashFragment: unused, and it hashes padding)
static bool same_fetch(const LatteFetchShader& a, const LatteFetchShader& b) {
    if (a.key != b.key || a.attributeBufferMask != b.attributeBufferMask) return false;
    auto same_groups = [](const std::vector<LatteParsedFetchShaderBufferGroup>& x, const std::vector<LatteParsedFetchShaderBufferGroup>& y) {
        if (x.size() != y.size()) return false;
        for (size_t g = 0; g < x.size(); g++) {
            const auto &p = x[g], &q = y[g];
            if (p.attributeBufferIndex != q.attributeBufferIndex || p.attribCount != q.attribCount ||
                p.hasVtxIndexAccess != q.hasVtxIndexAccess || p.hasInstanceIndexAccess != q.hasInstanceIndexAccess ||
                p.minOffset != q.minOffset || p.totalAttribRangeSize != q.totalAttribRangeSize)
                return false;
            for (int i = 0; i < p.attribCount; i++) {
                const auto &s = p.attrib[i], &t = q.attrib[i];
                if (s.attributeBufferIndex != t.attributeBufferIndex || s.semanticId != t.semanticId || s.format != t.format ||
                    s.fetchType != t.fetchType || s.nfa != t.nfa || s.isSigned != t.isSigned || s.endianSwap != t.endianSwap ||
                    memcmp(s.ds, t.ds, 4) || s.aluDivisor != t.aluDivisor || s.offset != t.offset)
                    return false;
            }
        }
        return true;
    };
    return same_groups(a.bufferGroups, b.bufferGroups) && same_groups(a.bufferGroupsInvalid, b.bufferGroupsInvalid);
}

// the export request; published releases (WWHD_PUBLIC_RELEASE) ignore it, every test build takes it
static const char* template_export_request() {
#ifdef WWHD_PUBLIC_RELEASE
    return nullptr;
#else
    return getenv("WWHD_TEMPLATE_EXPORT");
#endif
}

static void template_export(const std::string& cachePath) {
    Exporter& E = *g_export;
    double t0 = now_ms();
    // the distinct pipeline recipes, and which of their shaders resolve
    std::vector<PipelineRecipe> pipes;
    {
        std::unordered_set<std::string> seen;
        for (auto& r : g_pending_pipelines)
            if (seen.insert(std::string((const char*)&r, sizeof r)).second) pipes.push_back(r);
    }
    auto shader_ok = [&](uint64_t key) {
        auto it = g_shaders.find(key);
        return it != g_shaders.end() && shader_state(it->second) != CS_FAILED && E.byKey.count(key);
    };
    // pixel shaders recorded before kRecShaderLinked: their key here lacks the link. Find the record
    // whose key with the pipeline's vertex shader's link is the wanted one.
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>> wanted;  // vertex output mask -> pixel shader keys
    for (auto& r : pipes)
        if (shader_ok(r.vsKey) && !shader_ok(r.psKey) && !E.byKey.count(r.psKey) && g_shaders[r.vsKey]->dec)
            wanted[g_shaders[r.vsKey]->dec->outputParameterMask].insert(r.psKey);
    size_t linkedFound = 0;
    {
        static std::vector<uint32_t> regs(0x10000);
        for (auto& [mask, keys] : wanted)
            for (auto& p : E.ps) {
                for (int i = 0; i < 43; i++) regs[kExportPsRegs[i]] = p.regs[i];
                PsLink link = ps_link_mask(regs.data(), mask);
                if (!link.unfed) continue;
                uint64_t key = p.key ^ link.key * 37;
                if (!keys.count(key) || E.byKey.count(key)) continue;
                E.byKey[key] = {p.at, kRecShader};
                E.linkMask[key] = mask;
                linkedFound++;
            }
    }
    size_t noVs = 0, noPs = 0, noFs = 0;
    std::vector<const PipelineRecipe*> good;
    std::vector<uint64_t> keys;
    std::unordered_set<uint64_t> keySet;
    for (auto& r : pipes) {
        bool vs = shader_ok(r.vsKey);
        bool ps = E.linkMask.count(r.psKey) || shader_ok(r.psKey);
        bool fs = g_fetch.count(r.fsKey) && g_fetch[r.fsKey];
        noVs += !vs;
        noPs += vs && !ps;
        noFs += vs && ps && !fs;
        if (!vs || !ps || !fs) continue;
        good.push_back(&r);
        for (uint64_t k : {r.vsKey, r.psKey})
            if (keySet.insert(k).second) keys.push_back(k);
    }
    // the states
    FILE* f = fopen(cachePath.c_str(), "rb");
    if (!f) return;
    std::vector<uint8_t> out;
    std::vector<std::vector<uint8_t>> fetches;
    std::map<std::vector<uint8_t>, int32_t> fetchIndex;
    std::vector<uint8_t> states;
    size_t nStates = 0, codeFree = 0, fetchMismatch = 0, bad = 0;
    static std::vector<uint32_t> regs(0x10000);
    uint32_t scratch = mem::host_alloc(0x2000, 0x100);
    std::vector<uint8_t> raw;
    for (uint64_t key : keys) {
        const ExportRec& rec = E.byKey[key];
        uint32_t type;
        if (!read_record(f, rec.at, type, raw) || raw.size() < 16) { bad++; continue; }
        uint32_t vertex, size, fsSize, n;
        memcpy(&vertex, &raw[0], 4);
        memcpy(&size, &raw[4], 4);
        memcpy(&fsSize, &raw[8], 4);
        memcpy(&n, &raw[12], 4);
        if (16 + (size_t)size + fsSize + n * 8ull > raw.size() || fsSize > 0x1000) { bad++; continue; }
        const uint8_t* code = &raw[16];
        const uint8_t* fsBytes = code + size;
        const uint8_t* rp = fsBytes + fsSize;
        std::fill(regs.begin(), regs.end(), 0);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r, v;
            memcpy(&r, rp + i * 8, 4);
            memcpy(&v, rp + i * 8 + 4, 4);
            regs[r & 0xFFFF] = v;
        }
        uint32_t mask = 0, kind = vertex ? 1 : 0;
        if (type == kRecShaderLinked) {
            memcpy(&mask, rp + n * 8, 4);
            kind = 2;
        } else if (E.linkMask.count(key)) {
            mask = E.linkMask[key];
            kind = 2;
        }
        int32_t fi = -1;
        if (vertex && fsSize) {
            std::vector<uint8_t> payload(fsBytes, fsBytes + fsSize);
            if (((uint32_t)fsBytes[0] << 24 | fsBytes[1] << 16 | fsBytes[2] << 8 | fsBytes[3]) != kFetchOurs) {
                // real microcode: parse it as get_fetch_shader would and keep only the result
                memcpy(mem::ptr(scratch), fsBytes, fsSize);
                regs[mmSQ_PGM_START_FS] = scratch >> 8;
                const uint64_t ph = hash_bytes(fsBytes, fsSize);
                LatteFetchShader* a = LatteShaderRecompiler_createFetchShader(ph, regs.data(), (uint32*)mem::ptr(scratch), fsSize);
                payload = fetch_code_free(a, ph);
                memcpy(mem::ptr(scratch), payload.data(), payload.size());
                LatteFetchShader* b = fetch_from_code_free(scratch, ph);
                if (!b || !same_fetch(*a, *b)) fetchMismatch++;
                codeFree++;
            }
            auto it = fetchIndex.find(payload);
            if (it == fetchIndex.end()) {
                it = fetchIndex.emplace(payload, (int32_t)fetches.size()).first;
                fetches.push_back(payload);
            }
            fi = it->second;
        }
        put_le(states, key);
        put_le(states, hash_bytes(code, size));
        put_le(states, size);
        put_le(states, kind);
        put_le(states, fi);
        put_le(states, mask);
        const size_t countAt = states.size();
        put_le(states, (uint32_t)0);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r, v;
            memcpy(&r, rp + i * 8, 4);
            memcpy(&v, rp + i * 8 + 4, 4);
            if (r == mmSQ_PGM_START_VS || r == mmSQ_PGM_START_PS || r == mmSQ_PGM_START_FS) continue;  // addresses
            put_le(states, r);
            put_le(states, v);
            kept++;
        }
        memcpy(&states[countAt], &kept, 4);
        nStates++;
    }
    fclose(f);
    put_le(out, (uint32_t)fetches.size());
    for (auto& p : fetches) {
        put_le(out, (uint32_t)p.size());
        out.insert(out.end(), p.begin(), p.end());
    }
    put_le(out, (uint32_t)nStates);
    out.insert(out.end(), states.begin(), states.end());
    put_le(out, (uint32_t)good.size());
    for (auto* r : good) put_le(out, *r);
    uLongf csize = compressBound(out.size());
    std::vector<uint8_t> comp(csize);
    if (compress2(comp.data(), &csize, out.data(), out.size(), 9) != Z_OK) return;
    // WWHD_TEMPLATE_EXPORT=<path>: written there instead (release builds: a file in the app's external
    // files dir, where adb reads it)
    const char* to = template_export_request();
    std::string path = to && strcmp(to, "1") ? std::string(to)
                                             : cachePath.substr(0, cachePath.find_last_of('/') + 1) + "template.bin";
    if (FILE* o = fopen(path.c_str(), "wb")) {
        fwrite("WWHV", 4, 1, o);
        uint32_t hdr[2] = {1, (uint32_t)out.size()};
        fwrite(hdr, sizeof hdr, 1, o);
        fwrite(comp.data(), 1, csize, o);
        fclose(o);
    }
    LOG("[gfx] template export: %zu pipelines (%zu distinct): %zu kept, unresolved %zu vertex / %zu pixel / %zu fetch; "
        "%zu linked pixel shaders found in old records; %zu states (%zu unreadable), %zu fetch shaders (%zu states with code-free "
        "ones, %zu mismatches); %zu bytes raw, %lu written to %s (%.0f ms)",
        g_pending_pipelines.size(), pipes.size(), good.size(), noVs, noPs, noFs, linkedFound, nStates, bad, fetches.size(),
        codeFree, fetchMismatch, out.size(), (unsigned long)csize + 12, path.c_str(), now_ms() - t0);
}

// ---- usage and compaction: shaders.bin keeps every variant ever translated (the key holds state of
// both stages, so ~200k-550k variants of ~7.6k programs over many sessions; replaying them took 10-30 s
// at every start). shaders.bin.use holds the keys the game used and the session they were last used in:
//   "WWHU", u32 version 1, u32 sessions counted, u32 n, {u64 key, u32 session}*
// A session counts once it played 5 minutes (the first save). After WWHD_CACHE_KEEP_SESSIONS (default 6;
// 0 keeps everything) counted sessions, the replay skips the variants none of the last ones used and
// rewrites the file without them. The head start brings back what its template covers; any other
// dropped variant is translated again (~0.3 ms) and recorded when the game uses it.
namespace {
std::unordered_map<uint64_t, uint32_t> g_use;  // key -> the counted session it was last used in
uint32_t g_use_session = 0;                   // this session's number once it counts
std::string g_use_path;
size_t g_used_merged = 0;                     // g_used_keys entries in g_use
std::atomic<bool> g_use_writing{false};
double g_use_saved_at = 0;

uint32_t cache_keep_sessions() {
    static const uint32_t n = getenv("WWHD_CACHE_KEEP_SESSIONS") ? (uint32_t)atoi(getenv("WWHD_CACHE_KEEP_SESSIONS")) : 6;
    return n;
}

// returns the sessions counted so far; g_use gets the keys used in the last cache_keep_sessions() of them
uint32_t shader_use_load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint32_t hdr[4] = {};
    uint32_t sessions = 0;
    if (fread(hdr, sizeof hdr, 1, f) == 1 && !memcmp(hdr, "WWHU", 4) && hdr[1] == 1) {
        sessions = hdr[2];
        const uint32_t keep = cache_keep_sessions();
        std::vector<uint8_t> raw((size_t)hdr[3] * 12);
        if (fread(raw.data(), 1, raw.size(), f) == raw.size()) {
            g_use.reserve(hdr[3]);
            for (size_t i = 0; i < hdr[3]; i++) {
                uint64_t key;
                uint32_t last;
                memcpy(&key, &raw[i * 12], 8);
                memcpy(&last, &raw[i * 12 + 8], 4);
                if (!keep || last + keep > sessions) g_use[key] = last;
            }
        }
    }
    fclose(f);
    return sessions;
}

void shader_use_write(std::vector<uint8_t> data, std::string path) {
    std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "wb")) {
        bool ok = fwrite(data.data(), 1, data.size(), f) == data.size();
        ok = fclose(f) == 0 && ok;
        if (ok) rename(tmp.c_str(), path.c_str());
        else remove(tmp.c_str());
    }
    g_use_writing = false;
}
}  // namespace

// every 5 minutes of play (WWHD_CACHE_USE_SAVE_S) with new variants used: merge them and write the file (on its own thread)
static void shader_use_save() {
    if (g_use_path.empty() || g_use_writing || g_used_merged == g_used_keys.size()) return;
    const double now = now_ms();
    static const double every = (getenv("WWHD_CACHE_USE_SAVE_S") ? atof(getenv("WWHD_CACHE_USE_SAVE_S")) : 300) * 1000;
    if (now - g_use_saved_at < every) return;
    g_use_saved_at = now;
    for (; g_used_merged < g_used_keys.size(); g_used_merged++) g_use[g_used_keys[g_used_merged]] = g_use_session;
    std::vector<uint8_t> data(16 + g_use.size() * 12);
    uint32_t hdr[4] = {0, 1, g_use_session, (uint32_t)g_use.size()};
    memcpy(hdr, "WWHU", 4);
    memcpy(data.data(), hdr, 16);
    size_t at = 16;
    for (auto& [key, last] : g_use) {
        memcpy(&data[at], &key, 8);
        memcpy(&data[at + 8], &last, 4);
        at += 12;
    }
    g_use_writing = true;
    std::thread(shader_use_write, std::move(data), g_use_path).detach();
    LOG("[gfx] shader cache: %zu variants used this session, %zu in the last %u sessions", g_used_keys.size(), g_use.size(),
        cache_keep_sessions());
}

static void cache_load() {
    struct Done { ~Done() { g_cache_state = 2; } } done;  // on every way out
    g_cache_state = 1;
    std::string path = cache_path();
    if (path == "0") { g_cache_disabled = true; return; }
    size_t slash = path.find_last_of('/');
    spirv_cache_open(slash == std::string::npos ? std::string(".") : path.substr(0, slash));
    double t0 = now_ms();
    g_use_saved_at = t0;
    g_use_path = path + ".use";
    const uint32_t sessions = shader_use_load(g_use_path);
    g_use_session = sessions + 1;
    size_t shaders = 0, pipelines = 0;
    // compaction: the records to keep (offset, bytes), the variants they give, what was dropped
    const bool compact = cache_keep_sessions() && sessions >= cache_keep_sessions() && !template_export_request();
    struct Kept { long at; uint32_t bytes; };
    std::vector<Kept> kept;
    std::vector<std::pair<PipelineRecipe, Kept>> pipeRecs;
    std::unordered_set<uint64_t> keptKeys;
    size_t dropped = 0;
    uint64_t droppedBytes = 0, fileBytes = 0;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        if (fseek(f, 0, SEEK_END) == 0) {
            long size = ftell(f);
            if (size > 0) g_cache_total = (uint64_t)size;
            rewind(f);
        }
        static std::vector<uint32_t> regs(0x10000);
        g_cache_replaying = true;
        g_defer_compiles = true;
        static Exporter exporter;
        if (template_export_request()) g_export = &exporter;
        uint32_t hdr[3];
        for (long recAt = 0; fread(hdr, sizeof hdr, 1, f) == 1; recAt = ftell(f)) {
            std::vector<uint8_t> comp(hdr[2]), raw(hdr[1]);
            if (fread(comp.data(), 1, hdr[2], f) != hdr[2]) break;
            uLongf rsize = hdr[1];
            if (uncompress(raw.data(), &rsize, comp.data(), hdr[2]) != Z_OK || rsize != hdr[1]) break;
            const uint8_t* p = raw.data();
            const uint8_t* end = p + raw.size();
            const uint32_t recBytes = (uint32_t)sizeof hdr + hdr[2];
            fileBytes = (uint64_t)recAt + recBytes;
            if (hdr[0] == kRecPipelineVk) {
                PipelineRecipe r;
                if (raw.size() == sizeof r && get(p, end, r)) {
                    if (compact) pipeRecs.push_back({r, Kept{recAt, recBytes}});
                    else g_pending_pipelines.push_back(r);
                    pipelines++;
                }
                continue;
            }
            if (hdr[0] != kRecShader && hdr[0] != kRecShaderLinked) continue;
            uint32_t vertex, size, fsSize, n;
            if (!get(p, end, vertex) || !get(p, end, size) || !get(p, end, fsSize) || !get(p, end, n)) break;
            if (p + size + fsSize + n * 8 + (hdr[0] == kRecShaderLinked ? 4 : 0) > end) break;
            const uint8_t* code = p;
            const uint8_t* fsBytes = p + size;
            p += size + fsSize;
            std::fill(regs.begin(), regs.end(), 0);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t r, val;
                get(p, end, r);
                get(p, end, val);
                regs[r] = val;
            }
            uint32_t mask = 0;
            if (hdr[0] == kRecShaderLinked) get(p, end, mask);
            ReplaySetup rs = replay_setup(regs.data(), vertex != 0, code, size, fsBytes, fsSize, hdr[0] == kRecShaderLinked, mask);
            if (!rs.ok) continue;
            if (compact) {
                // not used lately, or a second record of a variant
                if (!rs.key || !g_use.count(rs.key) || !keptKeys.insert(rs.key).second) {
                    dropped++;
                    droppedBytes += recBytes;
                    continue;
                }
                kept.push_back({recAt, recBytes});
            }
            Shader* s = get_shader_uncached(regs.data(), vertex != 0, rs.fs, rs.fsKey, rs.link);
            shaders++;
            if (g_export && s) {
                g_export->byKey.emplace(s->key, ExportRec{recAt, hdr[0]});
                if (!vertex && hdr[0] == kRecShader) {
                    ExportPs e{s->key, recAt, {}};
                    for (int i = 0; i < 43; i++) e.regs[i] = regs[kExportPsRegs[i]];
                    g_export->ps.push_back(e);
                }
            }
            if ((shaders & 255) == 0) {
                g_cache_shaders = shaders;
                long at = ftell(f);
                if (at > 0) g_cache_done = (uint64_t)at;
            }
        }
        if (compact) {
            // pipelines: once each, and only with both shaders kept (or used lately: head-start states)
            std::unordered_set<std::string> seen;
            size_t pipesKept = 0;
            for (auto& [r, rec] : pipeRecs) {
                auto usable = [&](uint64_t k) { return keptKeys.count(k) || g_use.count(k); };
                if (!usable(r.vsKey) || !usable(r.psKey) || !seen.insert(std::string((const char*)&r, sizeof r)).second) {
                    droppedBytes += rec.bytes;
                    continue;
                }
                g_pending_pipelines.push_back(r);
                kept.push_back(rec);
                pipesKept++;
            }
            LOG("[gfx] shader cache: %zu variants kept (used in the last %u sessions), %zu dropped; %zu of %zu pipeline records "
                "kept", keptKeys.size(), cache_keep_sessions(), dropped, pipesKept, pipeRecs.size());
            pipeRecs = {};
            // rewrite the file without the dropped records once they're worth it
            if (droppedBytes >= (16u << 20) || droppedBytes * 5 >= fileBytes) {
                double tc = now_ms();
                std::string tmp = path + ".tmp";
                bool ok = false;
                uint64_t newBytes = 0;
                if (FILE* o = fopen(tmp.c_str(), "wb")) {
                    std::sort(kept.begin(), kept.end(), [](const Kept& a, const Kept& b) { return a.at < b.at; });
                    std::vector<uint8_t> buf;
                    ok = true;
                    for (auto& k : kept) {
                        buf.resize(k.bytes);
                        if (fseek(f, k.at, SEEK_SET) != 0 || fread(buf.data(), 1, k.bytes, f) != k.bytes ||
                            fwrite(buf.data(), 1, k.bytes, o) != k.bytes) { ok = false; break; }
                        newBytes += k.bytes;
                    }
                    ok = fclose(o) == 0 && ok;
                }
                if (ok && rename(tmp.c_str(), path.c_str()) == 0)
                    LOG("[gfx] shader cache compacted: %.1f -> %.1f MB (%.0f ms)", fileBytes / 1048576.0, newBytes / 1048576.0,
                        now_ms() - tc);
                else {
                    remove(tmp.c_str());
                    LOG("[gfx] shader cache: compacting failed, the file stays as it was");
                }
            }
        }
        fclose(f);
        if (g_export) {
            template_export(path);
            g_export = nullptr;
            exporter = Exporter{};
        }
        g_cache_replaying = false;
        g_defer_compiles = false;
    }
    void headstart_vk_load(const std::string& dir);  // vk_headstart.cpp
    headstart_vk_load(slash == std::string::npos ? std::string(".") : path.substr(0, slash));
    g_cache_state = 1;
    g_cache_out = fopen(path.c_str(), "ab");
    if (shaders || pipelines)
        LOG("[gfx] shader cache: replayed %zu shaders, %zu pipelines queued (%.0f ms) from %s", shaders, pipelines,
            now_ms() - t0, path.c_str());
}

// build queued pipelines whose shaders have finished compiling, at most `budget` of them and only
// while fewer than `maxInFlight` compiles are running; recipes that can't be resolved are dropped
static size_t g_recipes_built, g_recipes_dropped;
// Each call checks at most kChecksPerCall recipes (unless the budget is unlimited) and resumes from a
// cursor on the next: the queue holds hundreds of thousands at startup, and walking all of it every
// frame (three lookups each) was most of the render thread's time (from the original project).
static void build_pending_pipelines(int budget, int maxInFlight) {
    if (g_pending_pipelines.empty()) return;
    static std::vector<uint32_t> regs(0x10000);
    static size_t cursor = 0;
    constexpr size_t kChecksPerCall = 2048;
    size_t checks = budget == INT_MAX ? g_pending_pipelines.size() : kChecksPerCall;  // unlimited: one pass
    g_cache_replaying = true;
    g_building_ahead = true;
    if (cursor >= g_pending_pipelines.size()) cursor = 0;
    for (size_t i = cursor; !g_pending_pipelines.empty() && checks > 0 && budget > 0 && g_compiles_in_flight < maxInFlight;
         checks--) {
        if (i >= g_pending_pipelines.size()) i = 0;  // wrap around to the start
        cursor = i;
        PipelineRecipe& r = g_pending_pipelines[i];
        auto vi = g_shaders.find(r.vsKey), pi = g_shaders.find(r.psKey);
        auto fi = g_fetch.find(r.fsKey);
        if (vi == g_shaders.end() || pi == g_shaders.end() || fi == g_fetch.end() || !fi->second ||
            shader_state(vi->second) == CS_FAILED || shader_state(pi->second) == CS_FAILED) {
            g_pending_pipelines[i] = g_pending_pipelines.back();  // unusable recipe
            g_pending_pipelines.pop_back();
            g_recipes_dropped++;
            continue;
        }
        compile_deferred(vi->second);
        compile_deferred(pi->second);
        if (shader_state(vi->second) != CS_READY || shader_state(pi->second) != CS_READY) { i++; continue; }
        for (int k = 0; k < 16; k++) regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + k * 7 + 2] = r.strides[k];
        get_pipeline(regs.data(), vi->second, pi->second, fi->second, r.fsKey, r.st);
        g_pending_pipelines[i] = g_pending_pipelines.back();
        g_pending_pipelines.pop_back();
        g_recipes_built++;
        budget--;
    }
    g_building_ahead = false;
    g_cache_replaying = false;
}

// the game holds without frames (gx2::warm_while_held, every ~20 ms): the startup warming with
// every compile thread busy, nothing of the game's to keep smooth
void cache_warm_held() {
    static const int maxInFlight = 16;
    g_cache_replaying = true;
    for (int n = 0; n < 16 && !g_deferred_modules.empty() && g_compiles_in_flight < maxInFlight;) {
        Module* m = g_deferred_modules.back();
        g_deferred_modules.pop_back();
        if (m->state == CS_DEFERRED) { start_module(m); n++; }
    }
    g_cache_replaying = false;
    build_pending_pipelines(16, maxInFlight);
    static double t0 = now_ms();
    static bool logged = false;
    if (!logged && g_pending_pipelines.empty() && g_deferred_modules.empty() && g_compiles_in_flight == 0) {
        logged = true;
        LOG("[gfx] shader cache warmed while the game waited: %zu pipelines built, %zu unresolved, %.1f s", g_recipes_built,
            g_recipes_dropped, (now_ms() - t0) / 1000.0);
    }
}

// build cached pipelines whose shaders have finished compiling; a few per frame
void cache_warm_step() {
    // a few background compiles per frame keep the startup burst from starving the game, and none
    // start while the game's own compiles keep the compiler busy (WWHD_BG_COMPILES in flight, default
    // one less than the compile threads: one is always free for a shader the game needs now)
    static const int maxInFlight = getenv("WWHD_BG_COMPILES") ? atoi(getenv("WWHD_BG_COMPILES"))
                                                              : std::max(1, (int)compile_threads() - 1);
    const double t0 = now_ms();
    g_cache_replaying = true;
    for (int n = 0; n < 8 && !g_deferred_modules.empty() && g_compiles_in_flight < maxInFlight;) {
        Module* m = g_deferred_modules.back();
        g_deferred_modules.pop_back();
        if (m->state == CS_DEFERRED) { start_module(m); n++; }
    }
    g_cache_replaying = false;
    static bool queued = false;
    if (!g_pending_pipelines.empty()) queued = true;
    build_pending_pipelines(8, maxInFlight);
    g_t_warm += now_ms() - t0;
    if ((R.frame & 255) == 0) shader_use_save();
    if (queued && g_pending_pipelines.empty()) {
        queued = false;
        LOG("[gfx] background pipelines done by frame %llu: %zu built, %zu unresolved", (unsigned long long)R.frame,
            g_recipes_built, g_recipes_dropped);
    }
}

enum Skip { SK_GS, SK_RASTER_KILL, SK_NO_FETCH, SK_NO_SHADER, SK_NO_TARGET, SK_PRIM, SK_PASS, SK_PIPELINE, SK_CULL, SK_SCISSOR, SK_COMPILING,
            SK_DROPPED_DEPTH, SK_DROPPED_COLOR, SK_PIPE_COMPILING, SK_COUNT };
static uint64_t g_skip[SK_COUNT];
static const char* kSkipNames[SK_COUNT] = {"geometry-shader", "raster-kill", "no-fetch-shader", "shader-failed", "no-target",
                                           "unsupported-prim", "no-pass", "pipeline-failed", "cull-all", "empty-scissor",
                                           "compiling", "dropped-depth", "dropped-color", "pipeline-compiling"};
void report_skips() {
    char buf[512];
    int n = 0;
    for (int i = 0; i < SK_COUNT; i++)
        if (g_skip[i]) n += snprintf(buf + n, sizeof buf - n, " %s=%llu", kSkipNames[i], (unsigned long long)g_skip[i]);
    if (n) LOG("[gfx] skipped draws:%s", buf);
    if (g_waited_n[0] + g_waited_n[1] + g_waited_n[2])
        LOG("[gfx] waited for compiles (policy %d): recent targets %llu draws %.0f ms, intermittent %llu draws %.0f ms, "
            "new targets %llu draws %.0f ms", g_wait_policy, (unsigned long long)g_waited_n[0], g_waited_ms[0],
            (unsigned long long)g_waited_n[1], g_waited_ms[1], (unsigned long long)g_waited_n[2], g_waited_ms[2]);
    LOG("[gfx] %zu shaders (%zu distinct modules, %zu SPIR-V from disk), %zu pipelines; ms decompile %.0f, spirv %.0f, pipeline %.0f",
        g_shaders.size(), g_modules.size(), g_spv_disk_hits, g_pipelines.size(), g_t_decompile, g_t_spirv_us / 1000.0,
        g_t_pipeline_us / 1000.0);
    {  // where compile stutter comes from: the game's compiles waiting in the queue, or render thread time
        static double lastDecompile = 0, lastWarm = 0;
        static uint64_t lastTranslations = 0;
        uint64_t jobs = g_game_jobs.exchange(0), wait = g_game_wait_us.exchange(0), run = g_game_run_us.exchange(0);
        uint64_t waitMax = g_game_wait_max_us.exchange(0), runMax = g_game_run_max_us.exchange(0);
        LOG("[gfx] last 300 frames, game compiles: %llu (queue wait avg %.1f max %.1f ms, compile avg %.1f max %.1f ms); render "
            "thread: translating %llu shaders %.2f ms/frame, background warm-up %.2f ms/frame; narrow keys (mode %d): %llu "
            "translations reused, %llu checked, %llu differ",
            (unsigned long long)jobs, jobs ? wait / 1000.0 / jobs : 0.0, waitMax / 1000.0, jobs ? run / 1000.0 / jobs : 0.0,
            runMax / 1000.0, (unsigned long long)(g_translations - lastTranslations), (g_t_decompile - lastDecompile) / 300.0,
            (g_t_warm - lastWarm) / 300.0, g_narrow_mode, (unsigned long long)g_narrow_reused, (unsigned long long)g_narrow_checked,
            (unsigned long long)g_narrow_differ);
        lastDecompile = g_t_decompile;
        lastWarm = g_t_warm;
        lastTranslations = g_translations;
        if (!g_narrow_diff.empty()) {  // (cumulative) parts that tell same-translation variants apart
            std::vector<std::pair<uint64_t, std::string>> v;
            for (auto& [name, count] : g_narrow_diff) v.push_back({count, name});
            std::sort(v.rbegin(), v.rend());
            std::string line;
            for (size_t i = 0; i < v.size() && i < 12; i++) line += " " + v[i].second + "=" + std::to_string(v[i].first);
            LOG("[gfx] narrow key check, same translation but differing:%s", line.c_str());
        }
    }
    extern uint64_t g_stat_full_checks, g_stat_uploads, g_stat_invalidates, g_stat_invalidated_surfaces, g_stat_texture_stale;
    LOG("[gfx] last 300 frames: %llu full texture checks, %llu uploads, %llu invalidates marking %llu surfaces; %llu texture "
        "changes the write tracking missed",
        (unsigned long long)g_stat_full_checks, (unsigned long long)g_stat_uploads, (unsigned long long)g_stat_invalidates,
        (unsigned long long)g_stat_invalidated_surfaces, (unsigned long long)g_stat_texture_stale);
    g_stat_full_checks = g_stat_uploads = g_stat_invalidates = g_stat_invalidated_surfaces = g_stat_texture_stale = 0;
    LOG("[gfx] last 300 frames, KiB/frame copied: vertices %.0f (+%.0f large, once per submission; %.0f reused), uniform blocks %.0f "
        "(%.0f reused), uniform vars %.0f",
        g_bytes_vtx / 300.0 / 1024, g_bytes_vtx_shared / 300.0 / 1024, g_bytes_vtx_reused / 300.0 / 1024, g_bytes_ubo / 300.0 / 1024,
        g_bytes_ubo_reused / 300.0 / 1024, g_bytes_vars / 300.0 / 1024);
    g_bytes_vtx = g_bytes_vtx_shared = g_bytes_ubo = g_bytes_vars = g_bytes_vtx_reused = g_bytes_ubo_reused = 0;
    LOG("[gfx] last 300 frames, draws/frame: %.0f with state reused, %.0f resolved; indices %.0f reused, %.0f kept, %.0f built "
        "(%zu kept, %u KiB of the buffer)",
        g_fast_draws / 300.0, g_slow_draws / 300.0, g_index_reused / 300.0, g_index_kept / 300.0, g_index_built / 300.0,
        keepidx::kept.size(), keepidx::A.top / 1024);
    g_index_reused = g_index_built = g_index_kept = 0;
    LOG("[gfx] last 300 frames, program hashes/frame: %.0f looked up, %.0f from the map (%zu addresses); %llu changed unannounced",
        g_program_lookups / 300.0, g_program_map_lookups / 300.0, g_program_hashes.size(), (unsigned long long)g_program_stale);
    g_program_lookups = g_program_map_lookups = g_program_stale = 0;
    LOG("[gfx] last 300 frames, shader keys/frame: %.0f looked up, %.0f from the map", g_shader_lookups / 300.0, g_shader_map_lookups / 300.0);
    g_shader_lookups = g_shader_map_lookups = 0;
    LOG("[gfx] kept vertex buffers: %llu (%.1f MiB), %.0f KiB/frame reused", (unsigned long long)g_keep_count, g_keep_bytes / 1048576.0,
        g_keep_reused / 300.0 / 1024);
    g_keep_reused = 0;
    if (g_stale_reuses) LOG("[gfx] write tracking: %llu reuses were outdated (written without a flush)", (unsigned long long)g_stale_reuses);
    g_stale_reuses = 0;
    g_fast_draws = g_slow_draws = 0;
}

// ---------------------------------------------------------------- vertex data
// Guest vertex buffers are copied into transient memory per draw. Large buffers (static meshes) are
// copied once per submission and shared by the draws in it while the game hasn't written their
// pages since (mem_writes.h; WWHD_TRACK_WRITES=0: compared instead), as the original project's
// renderer never reuses a copy on its address alone: a mesh rewritten within a submission (cloth,
// morphs, a buffer reused for another mesh) drew its old vertices (fix from the PR's 65f0189).
// Kept copies: a vertex buffer the game left unchanged from one submission to the next (static meshes,
// most of the 7 MB a frame the render thread copied at Windfall on a Helio G85) gets a buffer of its own
// that stays across submissions and frames while the game hasn't written its pages (and on one frame in
// 16 the bytes are compared, as copy_tracked). A rewritten one is freed once the GPU is done with it;
// one rewritten twice after being kept stays transient. Unused ones go after ~20 s, at most 96 MiB.
// WWHD_KEEP_VERTICES=0: as before.
static const bool g_keep_vertices = g_track_writes && (!getenv("WWHD_KEEP_VERTICES") || atoi(getenv("WWHD_KEEP_VERTICES")) != 0);
static Upload kept_vertex_buffer(uint32_t addr, uint32_t size, bool& handled) {
    handled = false;
    const bool on = g_keep_vertices;
    if (!on || size < 1024) return {};
    struct Kept { VkBuffer buf; VmaAllocation alloc; uint8_t* ptr; uint32_t gen; uint64_t used; };
    struct Seen { uint32_t gen; uint64_t frame; uint8_t drops; };
    static std::unordered_map<uint64_t, Kept> kept;
    static std::unordered_map<uint64_t, Seen> seen;
    static uint64_t lastSweep = 0;
    constexpr uint64_t kMaxBytes = 96ull << 20;
    auto drop = [](const Kept& k) {  // (the caller updates the counts)
        VkBuffer b = k.buf;
        VmaAllocation a = k.alloc;
        on_complete([b, a] { vmaDestroyBuffer(R.vma, b, a); });
    };
    if (R.frame - lastSweep >= 256) {  // unused for ~20 s: freed; old candidates forgotten
        lastSweep = R.frame;
        for (auto it = kept.begin(); it != kept.end();) {
            if (R.frame - it->second.used > 600) {
                g_keep_bytes -= (uint32_t)it->first;
                g_keep_count--;
                drop(it->second);
                it = kept.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = seen.begin(); it != seen.end();)
            it = R.frame - it->second.frame > 600 && it->second.drops < 2 ? seen.erase(it) : std::next(it);
    }
    const uint64_t key = (uint64_t)addr << 32 | size;
    auto k = kept.find(key);
    if (k != kept.end()) {
        bool same = memw::unchanged_since(addr, size, k->second.gen);
        if (same && R.uploadCached && ((addr >> 6) + R.frame) % 16 == 0 && memcmp(k->second.ptr, mem::ptr(addr), size) != 0) {
            g_stale_reuses++;
            same = false;
        }
        if (same) {
            k->second.used = R.frame;
            g_keep_reused += size;
            handled = true;
            return Upload{k->second.buf, 0, k->second.ptr};
        }
        drop(k->second);  // rewritten: transient copies again until it proves stable
        g_keep_bytes -= size;
        g_keep_count--;
        kept.erase(k);
        Seen& s = seen[key];
        s = {memw::now(), R.frame, (uint8_t)std::min(255, s.drops + 1)};
        return {};
    }
    auto s = seen.find(key);
    if (s == seen.end()) {
        seen[key] = {memw::now(), R.frame, 0};
        return {};
    }
    if (s->second.drops >= 2 || !memw::unchanged_since(addr, size, s->second.gen) || g_keep_bytes + size > kMaxBytes) {
        s->second.gen = memw::now();
        s->second.frame = R.frame;
        return {};
    }
    // unchanged since an earlier copy: keep this one
    Kept n{};
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    ai.requiredFlags = VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.preferredFlags = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &n.buf, &n.alloc, &info) != VK_SUCCESS) return {};
    n.ptr = (uint8_t*)info.pMappedData;
    n.gen = memw::now();  // before the copy: a write during it makes the next use copy again
    memcpy(n.ptr, mem::ptr(addr), size);
    n.used = R.frame;
    g_bytes_vtx += size;
    g_keep_bytes += size;
    g_keep_count++;
    kept[key] = n;
    seen.erase(s);
    handled = true;
    return Upload{n.buf, 0, n.ptr};
}

static Upload vertex_buffer(uint32_t addr, uint32_t size) {
    struct Shared { Upload u; uint32_t gen; };
    static std::unordered_map<uint64_t, Shared> shared;
    static uint64_t sharedSerial = ~0ull;
    bool kept;
    Upload ku = kept_vertex_buffer(addr, size, kept);
    if (kept) return ku;
    if (size < 64 * 1024) {
        static SubmissionCopies small;
        if (g_track_writes) return copy_tracked(small, addr, size, 16, g_bytes_vtx, g_bytes_vtx_reused);
        return copy_deduped(small, mem::ptr(addr), addr, size, 16, g_bytes_vtx, g_bytes_vtx_reused);
    }
    command_buffer();
    if (R.cmdSerial != sharedSerial) {  // transient memory is only valid within one submission
        shared.clear();
        sharedSerial = R.cmdSerial;
    }
    uint64_t key = (uint64_t)addr << 32 | size;
    auto it = shared.find(key);
    if (it != shared.end()) {
        bool same = g_track_writes ? memw::unchanged_since(addr, size, it->second.gen)
                                   : R.uploadCached && memcmp(it->second.u.ptr, mem::ptr(addr), size) == 0;
        if (same) return it->second.u;
    }
    uint32_t gen = memw::now();  // before the copy: a write during it makes the next use copy again
    Upload u = upload(mem::ptr(addr), size, 16);
    g_bytes_vtx_shared += size;
    shared[key] = {u, gen};
    return u;
}

// ---------------------------------------------------------------- draws
// What a draw resolved (shaders, targets, textures, pipeline, viewport limits): reused by the next
// draws while only data registers change (see the fast path in draw)
struct Prepared {
    LatteFetchShader* fs = nullptr;
    Shader* vs = nullptr;
    Shader* ps = nullptr;
    Surface* colors[8] = {};
    Surface* depth = nullptr;
    uint32_t depthSlice = 0, w = 0, h = 0, sx = 0, sy = 0, ex = 0, ey = 0, depthControl = 0;
    float targetScale = 1.0f, k = 1.0f;     // x: image pixels per guest pixel
    float targetScaleY = 1.0f, ky = 1.0f;   // y (differs from x for aspect-widened targets)
    float ox = 0, oy = 0;                   // the viewport's offset (a 16:9 band in a reshaped GamePad target)
    bool bars = false;                      // ... black outside it
    StageTextures vtex, ptex;
    Pipeline* pipe = nullptr;
};
static Prepared g_prep;
static struct PrepKey {
    uint64_t gen, frame, writeSeq;
    size_t surfaces;
    uint64_t pass;
    uint32_t prim;
    bool valid;
} g_prep_key{};
// Records a draw whose state is resolved (Prepared): dynamic state, vertex buffers, descriptors and
// the draw itself, inside the current render pass.
static void record_draw(const uint32_t* regs, const Prepared& P, uint32_t prim, uint32_t count, uint32_t indexType,
                        uint32_t indexAddr, uint32_t baseVertex, uint32_t instances, const DrawIndices& indices) {
    LatteFetchShader* fs = P.fs;
    Shader* vs = P.vs;
    Shader* ps = P.ps;
    const StageTextures& vtex = P.vtex;
    const StageTextures& ptex = P.ptex;
    Surface* const* colors = P.colors;
    Surface* depth = P.depth;
    const uint32_t depthSlice = P.depthSlice;
    const float targetScale = P.targetScale, k = P.k, targetScaleY = P.targetScaleY, ky = P.ky, ox = P.ox, oy = P.oy;
    const uint32_t sx = P.sx, sy = P.sy, ex = P.ex, ey = P.ey, w = P.w, h = P.h;
    const uint32_t stDepthControl = P.depthControl;
    Pipeline* pipe = P.pipe;
    LATTE_PA_SU_SC_MODE_CNTL pm;
    uint32_t pmr = regs[REGADDR::PA_SU_SC_MODE_CNTL];
    memcpy(&pm, &pmr, 4);
    LATTE_PA_CL_CLIP_CNTL clipCntl;
    uint32_t clipRaw = regs[REGADDR::PA_CL_CLIP_CNTL];
    memcpy(&clipCntl, &clipRaw, 4);
    (void)w; (void)h; (void)vs; (void)ps;
    VkCommandBuffer cmd = command_buffer();
    if (!g_ds.valid || g_ds.pipeline != pipe->pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->pipeline);
        g_ds.pipeline = pipe->pipeline;
    }

    LATTE_DB_STENCILREFMASK sf;
    LATTE_DB_STENCILREFMASK_BF sbk;
    uint32_t rf = regs[REGADDR::DB_STENCILREFMASK], rb = regs[REGADDR::DB_STENCILREFMASK_BF];
    memcpy(&sf, &rf, 4);
    memcpy(&sbk, &rb, 4);
    LATTE_DB_DEPTH_CONTROL dc;
    memcpy(&dc, &stDepthControl, 4);
    bool backSeparate = dc.get_BACK_STENCIL_ENABLE();
    const uint32_t stencil[6] = {sf.get_STENCILREF_F(), sbk.get_STENCILREF_B(), sf.get_STENCILMASK_F(), sf.get_STENCILWRITEMASK_F(),
                                 backSeparate ? sbk.get_STENCILMASK_B() : sf.get_STENCILMASK_F(),
                                 backSeparate ? sbk.get_STENCILWRITEMASK_B() : sf.get_STENCILWRITEMASK_F()};
    if (!g_ds.valid || memcmp(stencil, g_ds.stencil, sizeof stencil) != 0) {
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[0]);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[1]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[2]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, stencil[3]);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[4]);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, stencil[5]);
        memcpy(g_ds.stencil, stencil, sizeof stencil);
    }
    if (!g_ds.valid || memcmp(&regs[REGADDR::CB_BLEND_RED], g_ds.blend, sizeof g_ds.blend) != 0) {
        vkCmdSetBlendConstants(cmd, (const float*)&regs[REGADDR::CB_BLEND_RED]);
        memcpy(g_ds.blend, &regs[REGADDR::CB_BLEND_RED], sizeof g_ds.blend);
    }
    float bias[3] = {0, 0, 0};  // constant, clamp, slope
    if (pm.get_OFFSET_FRONT_ENABLED()) {
        bias[2] = gx2::bitsf(regs[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE]) / 16.0f;
        bias[0] = gx2::bitsf(regs[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET]);
        bias[1] = R.features.depthBiasClamp ? gx2::bitsf(regs[REGADDR::PA_SU_POLY_OFFSET_CLAMP]) : 0.0f;
    }
    if (!g_ds.valid || memcmp(bias, g_ds.bias, sizeof bias) != 0) {
        vkCmdSetDepthBias(cmd, bias[0], bias[1], bias[2]);
        memcpy(g_ds.bias, bias, sizeof bias);
    }

    float xs = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_XSCALE]), xo = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_XOFFSET]);
    float ys = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_YSCALE]), yo = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_YOFFSET]);
    float zs = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_ZSCALE]), zo = gx2::bitsf(regs[REGADDR::PA_CL_VPORT_ZOFFSET]);
    bool halfZ = clipCntl.get_DX_CLIP_SPACE_DEF();
    VkViewport vp{ox + (xo - xs) * k, oy + (yo - ys) * ky, xs * 2.0f * k, ys * 2.0f * ky, halfZ ? zo : zo - zs, zs + zo};
    if (vp.height == 0) vp.height = 1;  // Vulkan forbids an empty viewport
    if (vp.width <= 0) { vp.x += vp.width; vp.width = std::max(-vp.width, 1.0f); }
    vp.minDepth = std::clamp(vp.minDepth, 0.0f, 1.0f);
    vp.maxDepth = std::clamp(vp.maxDepth, 0.0f, 1.0f);
    if (!g_ds.valid || memcmp(&vp, &g_ds.viewport, sizeof vp) != 0) {
        vkCmdSetViewport(cmd, 0, 1, &vp);
        g_ds.viewport = vp;
    }
    VkRect2D scissor{{(int32_t)sx, (int32_t)sy}, {ex - sx, ey - sy}};
    if (!g_ds.valid || memcmp(&scissor, &g_ds.scissor, sizeof scissor) != 0) {
        vkCmdSetScissor(cmd, 0, 1, &scissor);
        g_ds.scissor = scissor;
    }
    g_ds.valid = true;  // all dynamic state above is now set in this pass
    if (P.bars) {  // around the 16:9 band: black
        VkClearAttachment ca{VK_IMAGE_ASPECT_COLOR_BIT, 0, {}};
        ca.clearValue.color.float32[3] = 1.0f;
        uint32_t bx = (uint32_t)std::max(0.0f, ox), by = (uint32_t)std::max(0.0f, oy);
        VkClearRect rr[4];
        uint32_t n = 0;
        if (by > 0) {
            rr[n++] = {{{0, 0}, {w, by}}, 0, 1};
            rr[n++] = {{{0, (int32_t)(h - by)}, {w, by}}, 0, 1};
        }
        if (bx > 0) {
            rr[n++] = {{{0, (int32_t)by}, {bx, h - 2 * by}}, 0, 1};
            rr[n++] = {{{(int32_t)(w - bx), (int32_t)by}, {bx, h - 2 * by}}, 0, 1};
        }
        if (n) vkCmdClearAttachments(cmd, 1, &ca, n, rr);
    }

    // vertex buffers
    for (auto& g : fs->bufferGroups) {
        uint32_t addr = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7];
        uint32_t size = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + g.attributeBufferIndex * 7 + 1] + 1;
        if (!addr || addr + (uint64_t)size > 0x100000000ull) continue;
        Upload u = vertex_buffer(addr, size);
        VkDeviceSize off = u.offset;
        uint32_t slot = (uint32_t)g.attributeBufferIndex;
        if (slot < 16 && g_ds.valid && g_ds.vb[slot] == u.buf && g_ds.vbOffset[slot] == off) continue;
        vkCmdBindVertexBuffers(cmd, slot, 1, &u.buf, &off);
        if (slot < 16) {
            g_ds.vb[slot] = u.buf;
            g_ds.vbOffset[slot] = off;
        }
    }
    bind_stage(cmd, pipe->layout, regs, vs, true, vtex, targetScale, targetScaleY);
    bind_stage(cmd, pipe->layout, regs, ps, false, ptex, targetScale, targetScaleY);

    static uint64_t drawInFrame = 0, lastFrame = 0;
    if (lastFrame != R.frame) { lastFrame = R.frame; drawInFrame = 0; }
    uint64_t thisDraw = drawInFrame++;
    DLOG("[draw]   #%llu", (unsigned long long)thisDraw);
    DLOG("[draw]   -> target %ux%u fmt %d depth %d (slice %u of %u) vp %.0f,%.0f %.0fx%.0f", w, h,
         colors[0] ? (int)colors[0]->img.format : 0, depth != nullptr, depthSlice, depth ? depth->slices : 0, vp.x, vp.y, vp.width,
         vp.height);
    if (!indices.n) {
        vkCmdDraw(cmd, count, instances, baseVertex, 0);
    } else {
        // the index buffer stays bound at the start of its chunk; draws select theirs with firstIndex
        const Upload& u = indices.u;
        if (!g_ds.valid || g_ds.ib != u.buf) {
            vkCmdBindIndexBuffer(cmd, u.buf, 0, VK_INDEX_TYPE_UINT32);
            g_ds.ib = u.buf;
        }
        vkCmdDrawIndexed(cmd, indices.n, instances, (uint32_t)(u.offset / 4), (int32_t)baseVertex, 0);
    }
    // debug: WWHD_DUMP_DRAWS=frame:i,j,k dumps color target 0 after those draws
    static uint64_t dumpFrame = ~0ull;
    static std::set<uint64_t> dumpDraws = [] {
        std::set<uint64_t> d;
        if (const char* e = getenv("WWHD_DUMP_DRAWS")) {
            char* p;
            dumpFrame = strtoull(e, &p, 10);
            while (*p == ':' || *p == ',') { p++; d.insert(strtoull(p, &p, 10)); }
        }
        return d;
    }();
    // debug: WWHD_TRACE_PS=addr[:first-last] logs one compact line per matching draw
    static uint32_t tracePS = 0;
    static uint64_t traceFrom = 0, traceTo = ~0ull;
    static bool traceInit = [] {
        if (const char* e = getenv("WWHD_TRACE_PS")) {
            char* p;
            tracePS = (uint32_t)strtoul(e, &p, 16);
            if (*p == ':') { traceFrom = strtoull(p + 1, &p, 10); traceTo = *p == '-' ? strtoull(p + 1, nullptr, 10) : traceFrom; }
        }
        return true;
    }();
    (void)traceInit;
    if ((tracePS && R.frame >= traceFrom && R.frame <= traceTo && (regs[mmSQ_PGM_START_PS] << 8) == tracePS) ||
        (capturing() && count <= 6)) {
        uint32_t vb = regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START];
        uint32_t stride = (regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + 2] >> 11) & 0xFFFF;
        char line[1024];
        snprintf(line, sizeof line, "[trace] f%llu #%llu tex %08X vb %08X v0 %.1f,%.1f v1 %.1f,%.1f v2 %.1f,%.1f v3 %.1f,%.1f",
                 (unsigned long long)R.frame, (unsigned long long)thisDraw, regs[REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS + 2] << 8, vb,
                 bitsf_(ld32(vb)), bitsf_(ld32(vb + 4)), bitsf_(ld32(vb + stride)), bitsf_(ld32(vb + stride + 4)),
                 bitsf_(ld32(vb + 2 * stride)), bitsf_(ld32(vb + 2 * stride + 4)), bitsf_(ld32(vb + 3 * stride)),
                 bitsf_(ld32(vb + 3 * stride + 4)));
        if (capturing()) dlog("%s", line); else LOG("%s", line);
    }
    if (capturing() && count <= 6 && colors[0]) {  // full-screen pass
        char name[160];
        snprintf(name, sizeof name, "%s/draw_%04llu_PS%08X_CB%08X.png", g_capture_dir.c_str(), (unsigned long long)thisDraw,
                 regs[mmSQ_PGM_START_PS] << 8, regs[mmCB_COLOR0_BASE]);
        dump_texture(colors[0]->img, name, true, false);
    }
    if (R.frame == dumpFrame && dumpDraws.count(thisDraw)) {
        char name[64];
        snprintf(name, sizeof name, "draw_%llu_%llu.png", (unsigned long long)R.frame, (unsigned long long)thisDraw);
        for (int ci = 0; ci < 8; ci++) if (colors[ci]) { dump_texture(colors[ci]->img, name, true, false); break; }
        if (depth) {
            snprintf(name, sizeof name, "draw_%llu_%llu_depth.png", (unsigned long long)R.frame, (unsigned long long)thisDraw);
            dump_texture(depth->img, name, true, false);
        }
    }
    if (g_hires_redraw) { g_hires_frame = R.frame; return; }  // the private copy is ready for the occlusion pass
    if (ao_hires_enabled() && colors[0] && (regs[mmSQ_PGM_START_PS] << 8) == kDepthDownsamplePS) {
        g_hires_redraw = true;
        draw(regs, prim, count, indexType, indexAddr, baseVertex, instances);
        g_hires_redraw = false;
    }
}

void draw(const uint32_t* regs, uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, uint32_t baseVertex,
          uint32_t instances) {
    static bool cacheLoaded = (cache_load(), true);
    (void)cacheLoaded;
    R.drawCount++;
    draws_since_commit()++;
    DLOG("[draw] prim %X count %u idx %u@%08X VS %08X PS %08X CB0 %08X info %08X DB %08X depthctl %08X blend %08X mask %08X",
         prim, count, indexType, indexAddr, regs[mmSQ_PGM_START_VS] << 8, regs[mmSQ_PGM_START_PS] << 8, regs[mmCB_COLOR0_BASE],
         regs[mmCB_COLOR0_INFO], regs[mmDB_DEPTH_BASE], regs[REGADDR::DB_DEPTH_CONTROL], regs[REGADDR::CB_COLOR_CONTROL],
         regs[REGADDR::CB_TARGET_MASK]);
    if (!count || !instances) return;
    prof_draw(regs[mmSQ_PGM_START_PS] << 8);
    ((uint32_t*)regs)[REGADDR::VGT_PRIMITIVE_TYPE] = prim;
    if (regs[REGADDR::VGT_GS_MODE] & 3) { g_skip[SK_GS]++; return; }  // geometry shaders: not supported yet
    if (regs[REGADDR::PA_CL_CLIP_CNTL] & (1 << 22)) { g_skip[SK_RASTER_KILL]++; return; }  // rasterization disabled

    // fast path: nothing but data registers (uniform constants, uniform block and vertex buffer
    // addresses) changed since the previous draw, which is in the same render pass, and no surface was
    // created, written or invalidated since: everything resolved for it still holds (runs of draws of
    // the same object with different matrices: grass, trees, crowds)
    if (g_prep_key.valid && !g_hires_redraw && g_prep_key.gen == g_draw_state_gen && g_prep_key.prim == prim &&
        g_prep_key.frame == R.frame && g_prep_key.writeSeq == write_seq() && g_prep_key.surfaces == R.surfaces.size() &&
        g_prep_key.pass == g_pass_serial && R.pass != VK_NULL_HANDLE) {
        DrawIndices indices;
        if (!draw_indices(prim, count, indexType, indexAddr, indices)) { g_skip[SK_PRIM]++; return; }
        g_fast_draws++;
        record_draw(regs, g_prep, prim, count, indexType, indexAddr, baseVertex, instances, indices);
        return;
    }
    g_prep_key.valid = false;

    uint64_t fsKey = 0;
    LatteFetchShader* fs = get_fetch_shader(regs, &fsKey);
    if (!fs) {
        if (g_skip[SK_NO_FETCH]++ < 5) {
            uint32_t prog = regs[mmSQ_PGM_START_FS] << 8;
            LOG("[gfx] no fetch shader: FS=%08X size=%X words %08X %08X %08X %08X", prog, regs[mmSQ_PGM_START_FS + 1] << 3,
                prog ? ld32(prog) : 0, prog ? ld32(prog + 4) : 0, prog ? ld32(prog + 8) : 0, prog ? ld32(prog + 12) : 0);
        }
        return;
    }
    Shader* vs = get_shader(regs, true, fs, fsKey);
    Shader* ps = get_shader(regs, false, fs, fsKey, vs && shader_state(vs) != CS_FAILED ? vs : nullptr);
    if (!vs || !ps || shader_state(vs) == CS_FAILED || shader_state(ps) == CS_FAILED) { g_skip[SK_NO_SHADER]++; return; }
    compile_deferred(vs);
    compile_deferred(ps);

    const LatteContextRegister& lcr = *(const LatteContextRegister*)regs;
    Surface* colors[8] = {};
    uint32_t colorSlices[8] = {}, depthSlice = 0;
    uint8_t mask = LatteMRT::GetActiveColorBufferMask(ps->dec, lcr);
    for (int i = 0; i < 8; i++)
        if (mask & (1 << i)) colors[i] = color_target(regs, i, &colorSlices[i]);
    Surface* depth = LatteMRT::GetActiveDepthBufferMask(lcr) ? depth_target(regs, &depthSlice) : nullptr;
    if (g_hires_redraw) {
        if (!colors[0]) return;
        g_hires_src = colors[0]->addr;
        colors[0] = hires_surface(g_hires_color, colors[0]);
        colorSlices[0] = 0;
        if (depth) { depth = hires_surface(g_hires_depth, depth); depthSlice = 0; }
        if (!colors[0]) return;
    }
    // attachments that can't be rendered to on this device (or images that aren't 2D) are dropped
    auto renderable = [](Surface* s) { return s->fmt.renderable && s->img.image && s->img.type == VK_IMAGE_TYPE_2D; };
    for (auto& c : colors)
        if (c && !renderable(c)) c = nullptr;
    if (depth && !renderable(depth)) depth = nullptr;
    // A draw without a depth buffer into the color targets of the open render pass, which has one: it
    // continues that pass with depth and stencil tests off instead of ending it. Each pass end stores
    // the 960x540 color and depth targets and the next pass loads them again (on tiled GPUs a full
    // read and write each): the game switches between draws with and without depth into the same
    // targets (Windfall: 8 passes -> 2). Not when the draw samples that depth buffer (it would need a
    // copy of it instead). WWHD_SPLIT_DEPTHLESS=1: a pass of its own, as before.
    bool borrowedDepth = false;
    static const bool splitDepthless = getenv("WWHD_SPLIT_DEPTHLESS") != nullptr;
    if (!depth && !g_hires_redraw && !splitDepthless && R.pass && R.passDepth) {
        bool same = false;
        for (int i = 0; i < 8; i++) {
            if (R.passColor[i] != colors[i] || (colors[i] && R.passColorSlice[i] != colorSlices[i])) { same = false; break; }
            same |= colors[i] != nullptr;
        }
        if (same) {
            const uint32_t base = R.passDepth->addr >> 8;
            for (uint32_t tb : {(uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, (uint32_t)REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS})
                for (int t = 0; t < 18 && same; t++) same = regs[tb + t * 7 + 2] != base;
        }
        if (same) {
            depth = R.passDepth;
            depthSlice = R.passDepthSlice;
            borrowedDepth = true;
        }
    }
    // one framebuffer size (image size, which includes the resolution scale) for all attachments;
    // drop mismatching ones (as the Metal renderer does)
    uint32_t w = 0, h = 0;
    float targetScale = 1.0f, targetScaleY = 1.0f;
    for (auto* c : colors)
        if (c) { w = c->img.width; h = c->img.height; targetScale = c->rscale * c->ax; targetScaleY = c->rscale * c->ay; break; }
    if (depth && w && (depth->img.width < w || depth->img.height < h)) { depth = nullptr; g_skip[SK_DROPPED_DEPTH]++; }
    if (!w && depth) { w = depth->img.width; h = depth->img.height; targetScale = depth->rscale * depth->ax; targetScaleY = depth->rscale * depth->ay; }
    for (auto& c : colors)
        if (c && (c->img.width != w || c->img.height != h)) { c = nullptr; g_skip[SK_DROPPED_COLOR]++; }
    if (!w) { g_skip[SK_NO_TARGET]++; return; }

    // skipping while compiling only for targets drawn in each of the last 3 frames (see wait_compiled)
    bool everyFrame = true;
    int kind = 0;
    auto track = [&](Surface* s) {
        if (!s) return;
        if (s->lastDrawFrame != R.frame) {
            int k = s->lastDrawFrame == ~0ull ? 2 : s->lastDrawFrame + 1 == R.frame ? 0 : 1;
            s->drawStreak = k == 0 ? s->drawStreak + 1 : 1;
            s->lastDrawFrame = R.frame;
            s->firstDrawFrame = k == 2;
        }
        if (s->drawStreak < 3) everyFrame = false;
        if (s->firstDrawFrame) kind = 2;
        else if (s->drawStreak == 1) kind = std::max(kind, 1);
    };
    for (auto* c : colors) track(c);
    track(depth);
    struct WaitScope { ~WaitScope() { g_wait_whole = false; } } waitScope;
    g_wait_kind = kind;
    g_wait_whole = g_wait_policy == 1 ? !everyFrame : g_wait_policy == 2 ? kind == 2 : false;
    if (!wait_compiled(shader_state(vs)) || !wait_compiled(shader_state(ps))) {
        g_wait_whole = false;
        g_skip[SK_COMPILING]++;
        return;
    }

    DrawIndices indices;
    if (!draw_indices(prim, count, indexType, indexAddr, indices)) { g_skip[SK_PRIM]++; return; }
    const VkPrimitiveTopology ptype = indices.type;
    const uint64_t indicesSerial = R.cmdSerial;

    LATTE_PA_SU_SC_MODE_CNTL pm;
    uint32_t pmr = regs[REGADDR::PA_SU_SC_MODE_CNTL];
    memcpy(&pm, &pmr, 4);
    bool cf = pm.get_CULL_FRONT(), cb = pm.get_CULL_BACK();
    if (cf && cb) { g_skip[SK_CULL]++; return; }

    // viewport and scissor (Vulkan's viewport transform is the hardware's: y = YOFFSET + YSCALE * ndc)
    // guest units -> image pixels: the resolution scale, times 1.5 for the private AO depth copy
    float k = (g_hires_redraw ? 1.5f : 1.0f) * targetScale, ky = (g_hires_redraw ? 1.5f : 1.0f) * targetScaleY;
    uint32_t tl = regs[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = regs[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    // rounded as the image sizes are (lroundf in create_surface_image): truncating left the last row or
    // column of a full-target scissor out where the scaled size rounds up (120x67 at 0.8x: 53.6 -> 53 of
    // 54 rows), and that row kept old contents (after a resolution change: the blue haze along the bottom)
    uint32_t sx = std::min<uint32_t>((uint32_t)lroundf((tl & 0x7FFF) * k), w), sy = std::min<uint32_t>((uint32_t)lroundf(((tl >> 16) & 0x7FFF) * ky), h);
    uint32_t ex = std::min<uint32_t>((uint32_t)lroundf((br & 0x7FFF) * k), w), ey = std::min<uint32_t>((uint32_t)lroundf(((br >> 16) & 0x7FFF) * ky), h);
    if (ex <= sx || ey <= sy) { g_skip[SK_SCISSOR]++; return; }

    // textures: uploads, layout changes and copies happen before the render pass begins
    // (resolved in place: the prepared state is only used once g_prep_key says so again)
    Prepared hiresPrep;  // the AO redraw (called from record_draw) must not replace the fast path's state
    Prepared& P = g_hires_redraw ? hiresPrep : g_prep;
    StageTextures& vtex = P.vtex;
    StageTextures& ptex = P.ptex;
    resolve_textures(regs, vs, true, colors, depth, vtex);
    resolve_textures(regs, ps, false, colors, depth, ptex);
    prepare_sampled(vtex, true);
    prepare_sampled(ptex, false);
    // the TV picture drawn into a GamePad target of another shape (the game mirrors the TV on the
    // GamePad: title, cutscenes): 16:9 in the middle, black around it (aspect.cpp "GamePad picture")
    float ox = 0, oy = 0;
    bool bars = false;
    if (!g_hires_redraw && colors[0] && (colors[0]->ax != 1.0f || colors[0]->ay != 1.0f) &&
        target_is_drc(colors[0]->width, colors[0]->height) && ptex.tvPicture) {
        bars = true;
        k = ky = colors[0]->rscale;
        ox = std::floor((w - colors[0]->width * k) / 2);
        oy = std::floor((h - colors[0]->height * ky) / 2);
        sx = std::min<uint32_t>((uint32_t)(ox + (tl & 0x7FFF) * k), w);
        sy = std::min<uint32_t>((uint32_t)(oy + ((tl >> 16) & 0x7FFF) * ky), h);
        ex = std::min<uint32_t>((uint32_t)(ox + (br & 0x7FFF) * k), w);
        ey = std::min<uint32_t>((uint32_t)(oy + ((br >> 16) & 0x7FFF) * ky), h);
    }

    PipelineState st;
    for (int i = 0; i < 8; i++)
        if (colors[i]) {
            st.pass.color[i] = (uint32_t)colors[i]->img.format;
            if (colors[i]->fmt.kind != FormatInfo::FLOAT) st.intTargets |= 1u << i;
        }
    if (depth) {
        st.pass.depth = (uint32_t)depth->img.format;
        st.pass.stencil = depth->fmt.stencil;
    }
    for (int i = 0; i < 8; i++) st.blend[i] = regs[REGADDR::CB_BLEND0_CONTROL + i];
    st.colorControl = regs[REGADDR::CB_COLOR_CONTROL];
    st.targetMask = regs[REGADDR::CB_TARGET_MASK];
    st.topology = (uint32_t)ptype;
    st.cull = (cf ? 1 : 0) | (cb ? 2 : 0);
    st.frontCCW = pm.get_FRONT_FACE() == LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW;
    st.depthControl = depth && !borrowedDepth ? regs[REGADDR::DB_DEPTH_CONTROL] : 0;
    LATTE_PA_CL_CLIP_CNTL clipCntl;
    uint32_t clipRaw = regs[REGADDR::PA_CL_CLIP_CNTL];
    memcpy(&clipCntl, &clipRaw, 4);
    st.depthClamp = clipCntl.get_ZCLIP_FAR_DISABLE() ? 1 : 0;

    Pipeline* pipe = get_pipeline(regs, vs, ps, fs, fsKey, st);
    g_wait_whole = false;
    if (!pipe) { g_skip[SK_PIPE_COMPILING]++; return; }

    // debug: WWHD_SKIP_TARGET=WxH[,WxH...] drops the draws into render targets of that guest size (cost measurements)
    static const std::vector<std::pair<uint32_t, uint32_t>> skipTargets = [] {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        const char* e = getenv("WWHD_SKIP_TARGET");
        unsigned a, b;
        int n;
        while (e && sscanf(e, "%ux%u%n", &a, &b, &n) == 2) {
            v.push_back({a, b});
            e += n;
            if (*e != ',') break;
            e++;
        }
        return v;
    }();
    if (!skipTargets.empty()) {
        const Surface* t = colors[0] ? colors[0] : depth;
        for (auto& [sw, sh] : skipTargets)
            if (t && t->width == sw && t->height == sh) return;
    }
    // debug: WWHD_SKIP_PS=ADDR[,ADDR...] (hex, as the profiler prints them) drops the draws with that pixel shader
    static const std::vector<uint32_t> skipPS = [] {
        std::vector<uint32_t> v;
        for (const char* e = getenv("WWHD_SKIP_PS"); e && *e;) {
            char* end;
            v.push_back((uint32_t)strtoul(e, &end, 16));
            if (*end != ',') break;
            e = end + 1;
        }
        return v;
    }();
    for (uint32_t a : skipPS)
        if ((regs[mmSQ_PGM_START_PS] << 8) == a) return;
    if (!ensure_pass(colors, colorSlices, depth, depthSlice, w, h, st.pass)) { g_skip[SK_PASS]++; return; }
    P.fs = fs;
    P.vs = vs;
    P.ps = ps;
    memcpy(P.colors, colors, sizeof P.colors);
    P.depth = depth;
    P.depthSlice = depthSlice;
    P.w = w;
    P.h = h;
    P.targetScale = targetScale;
    P.targetScaleY = targetScaleY;
    P.k = k;
    P.ky = ky;
    P.ox = ox;
    P.oy = oy;
    P.bars = bars;
    P.sx = sx; P.sy = sy; P.ex = ex; P.ey = ey;
    P.pipe = pipe;
    P.depthControl = st.depthControl;
    if (!g_hires_redraw) {  // state after ensure_pass: it may have begun the render pass and marked targets written
        g_prep_key = {g_draw_state_gen, R.frame, write_seq(), R.surfaces.size(), g_pass_serial, prim, true};
        g_slow_draws++;
    }
    // a new command buffer since the indices were uploaded (render pass changes): upload them again
    if (indices.n && R.cmdSerial != indicesSerial) draw_indices(prim, count, indexType, indexAddr, indices);
    record_draw(regs, P, prim, count, indexType, indexAddr, baseVertex, instances, indices);
}

// ---------------------------------------------------------------- shader head start hooks (gfx/shader_headstart.cpp)
// Translate one program from a head-start record: regs hold its state, with the program (and the
// fetch shader) already placed in guest memory. Not recorded into the user cache. compileNow starts
// the compile right away instead of deferring it to first use.
bool headstart_translate(const uint32_t* regs, bool vertex, bool compileNow) {
    bool replaying = g_cache_replaying, defer = g_defer_compiles;
    g_cache_replaying = true;
    g_defer_compiles = !compileNow;
    uint64_t fsKey = 0;
    LatteFetchShader* fs = vertex ? get_fetch_shader(regs, &fsKey) : nullptr;
    Shader* s = (!vertex || fs) ? get_shader_uncached(regs, vertex, fs, fsKey) : nullptr;
    if (s && compileNow) compile_deferred(s);
    g_cache_replaying = replaying;
    g_defer_compiles = defer;
    return s && shader_state(s) != CS_FAILED;
}

// compiles (shaders and pipelines) that haven't finished yet
size_t headstart_compiling() { return std::max(0, g_compiles_in_flight.load()); }

// a pipeline recipe of this renderer (template head start); records of another size (the macOS
// recipe format of gfx/shader_headstart.cpp) are refused
bool headstart_queue_pipeline(const uint8_t* raw, size_t size) {
    if (size != sizeof(PipelineRecipe)) return false;
    PipelineRecipe r;
    memcpy(&r, raw, sizeof r);
    g_pending_pipelines.push_back(r);
    return true;
}
size_t headstart_pipeline_size() { return sizeof(PipelineRecipe); }

// ---- template head start (vk_headstart.cpp)
uint64_t headstart_hash(const void* data, size_t n) { return hash_bytes(data, n); }  // as program_hash
bool headstart_known(uint64_t key) { return g_shaders.count(key) != 0; }
// translate a template state like a cache record (compiled on first use or in the background);
// returns the key it got, 0 if it failed
uint64_t headstart_replay(uint32_t* regs, bool vertex, const uint8_t* code, uint32_t size, const uint8_t* fsBytes,
                          uint32_t fsSize, bool linked, uint32_t vsMask) {
    bool replaying = g_cache_replaying, defer = g_defer_compiles;
    g_cache_replaying = true;
    g_defer_compiles = true;
    Shader* s = replay_state(regs, vertex, code, size, fsBytes, fsSize, linked, vsMask);
    g_cache_replaying = replaying;
    g_defer_compiles = defer;
    return s && shader_state(s) != CS_FAILED ? s->key : 0;
}
// the loading screen (cache_progress): state 3 while the head start is built from the game files
void headstart_progress(int state, uint64_t done, uint64_t total, uint64_t shaders) {
    g_cache_state = state;
    g_cache_done = done;
    g_cache_total = total;
    g_cache_shaders = shaders;
}

size_t headstart_build_pipelines(int maxInFlight, size_t& built, size_t& dropped) {
    build_pending_pipelines(INT_MAX, maxInFlight);
    built = g_recipes_built;
    dropped = g_recipes_dropped;
    return g_pending_pipelines.size();
}

}  // namespace gfx
