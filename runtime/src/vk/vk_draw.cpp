#include <chrono>
extern "C" uint64_t g_shader_state_gen;  // gx2_core.cpp: bumped by shader-relevant register changes
extern "C" uint64_t g_draw_state_gen;    // gx2_core.cpp: bumped by any non-data register change
// Draw calls on Vulkan: shader translation (Cemu decompiler, GLSL -> SPIR-V), pipelines, resource
// binding and primitive submission. Follows gfx/metal_draw.mm; binding conventions are those of
// the Vulkan GLSL the decompiler emits (one descriptor set per stage, as in Cemu's Vulkan renderer).
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
#include "../mem_writes.h"

#include <climits>
#include <condition_variable>
#include <cstdarg>
#include <ctime>
#include <deque>
#include <set>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <zlib.h>

LatteDecompilerShader* FinishDecompiledShader(LatteDecompilerOutput_t& decompilerOutput);
LatteFetchShader* LatteShaderRecompiler_createFetchShader(LatteFetchShader::CacheHash fsHash, uint32* contextRegister,
                                                           uint32* fsProgramCode, uint32 fsProgramSize);

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
static uint64_t g_fast_draws, g_slow_draws;  // draws through the fast path (state reused) and the full one

// Guest data copied for draws, per submission by address and size: a draw whose data is
// byte-identical to an earlier copy in the same submission reuses it (the same mesh or uniform block
// drawn many times). The comparison reads cached upload memory, much cheaper than another copy.
struct SubmissionCopies {
    struct Copy { Upload u; uint32_t gen; };
    std::unordered_map<uint64_t, Copy> map;
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
        auto it = c.map.find(key);
        if (it != c.map.end() && memcmp(it->second.u.ptr, src, size) == 0) {
            reused += size;
            return it->second.u;
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
static Upload copy_tracked(SubmissionCopies& c, uint32_t addr, uint32_t size, VkDeviceSize align, uint64_t& copied, uint64_t& reused) {
    command_buffer();
    if (R.cmdSerial != c.serial) {  // transient memory is only valid within one submission
        c.map.clear();
        c.serial = R.cmdSerial;
    }
    const uint8_t* src = mem::ptr(addr);
    uint64_t key = (uint64_t)addr << 32 | size;
    auto it = c.map.find(key);
    if (it != c.map.end() && memw::unchanged_since(addr, size, it->second.gen)) {
        bool verify = ((addr >> 6) + R.frame) % 16 == 0;
        if (!verify || memcmp(it->second.u.ptr, src, size) == 0) {
            reused += size;
            return it->second.u;
        }
        g_stale_reuses++;
        static int logged = 0;
        if (logged < 40) {
            logged++;
            uint32_t at = 0;
            while (at < size && ((const uint8_t*)it->second.u.ptr)[at] == src[at]) at++;
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
static uint64_t program_hash(uint32_t addr, uint32_t size) {
    auto& e = g_program_hashes[addr];
    if (e.size == size && e.frame == R.frame) return e.hash;
    const uint8_t* p = (const uint8_t*)mem::ptr(addr);
    uint64_t sample = program_sample(p, size);
    if (e.size != size || sample != e.sample || R.frame - e.fullFrame >= 32) {
        e.size = size;
        e.hash = hash_bytes(p, size);
        e.sample = sample;
        e.fullFrame = R.frame;
    }
    e.frame = R.frame;
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
namespace {
class Workers {
public:
    void push(std::function<void()> fn) {
        static std::once_flag once;
        std::call_once(once, [this] {
            unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 2u, 4u);
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
        std::lock_guard<std::mutex> lk(m_);
        q_.push_back(std::move(fn));
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

static LatteFetchShader* get_fetch_shader(const uint32_t* regs, uint64_t* keyOut) {
    uint32_t prog = regs[mmSQ_PGM_START_FS] << 8;
    if (!prog) return nullptr;
    // either our compact encoding (from GX2InitFetchShaderEx) or real fetch shader microcode shipped with the game
    bool ours = ld32(prog) == 0x57574653;
    uint32_t size = ours ? 16 + ld32(prog + 4) * 16 : regs[mmSQ_PGM_START_FS + 1] << 3;
    if (size > 0x1000) return nullptr;
    uint64_t h = program_hash(prog, size);
    // strides are part of the pipeline's vertex input state
    for (uint32_t b = 0; b < 16; b++) h = (h ^ ((regs[mmSQ_VTX_ATTRIBUTE_BLOCK_START + b * 7 + 2] >> 11) & 0xFFFF)) * 1099511628211ull;
    *keyOut = h;
    auto it = g_fetch.find(h);
    if (it != g_fetch.end()) return it->second;
    LatteFetchShader* fs = ours ? gx2::build_fetch_shader(prog)
                                : LatteShaderRecompiler_createFetchShader(h, (uint32*)regs, (uint32*)mem::ptr(prog), size);
    g_fetch[h] = fs;
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
    std::atomic<int> state{CS_PENDING};
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
    uint32_t varsRange = 0;                       // size of the uniform-variable block
    uint32_t blockRange[LATTE_NUM_MAX_UNIFORM_BUFFERS] = {};  // size bound for each uniform block
    std::atomic<int> state{CS_PENDING};       // translation: CS_FAILED, CS_DEFERRED, else see mod
};
// compile state of a shader (translation failure, or its module's compile)
static const std::atomic<int>& shader_state(const Shader* s) { return s->mod ? s->mod->state : s->state; }
static std::unordered_map<uint64_t, Module*> g_modules;  // by hash of the translated source; render thread
static size_t g_module_cache_hits;
static std::unordered_map<uint64_t, Shader*> g_shaders;
// compiles (shaders and pipelines) started and not finished yet; background work holds back while it's high
static std::atomic<int> g_compiles_in_flight{0};
static std::atomic<size_t> g_pipelines_created{0};
size_t pipelines_created() { return g_pipelines_created; }
// time spent per stage, reported with the skip statistics
static double g_t_decompile;
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

// ---- SPIR-V cache on disk (spirv.bin next to shaders.bin): translations seen in earlier sessions
// skip glslang. Records: uint64 source hash, uint32 word count, words.
static std::mutex g_spv_mutex;
static std::unordered_map<uint64_t, std::vector<uint32_t>> g_spv_disk;
static FILE* g_spv_out = nullptr;
static size_t g_spv_disk_hits;

static void spirv_cache_open(const std::string& dir) {
    std::string path = dir + "/spirv.bin";
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

// attach the shader to the module for its translation, compiling that module if it is new
static void compile_shader(Shader* sh) {
    std::string src = sh->dec->strBuf_shaderSource->c_str();
    // Depth-only and shaded variants of the same geometry must rasterize identical positions:
    // later passes depth-test EQUAL/LEQUAL against the first, and without invariance the driver
    // may compute gl_Position differently per shader (camera-dependent z-fighting: shadow flicker,
    // moire on the ground). Same as the official Vulkan renderer (and Metal's [[invariant]]).
    if (sh->vertex) {
        if (size_t main = src.find("void main("); main != std::string::npos)
            src.insert(main, "invariant gl_Position;\n");
    }
    // The Outset Island sun-shadow mask compares against a D16 cascade. Qualcomm's Vulkan driver
    // does not reproduce Latte's large polygon offset closely enough here, leaving the receiver to
    // self-shadow in a dense grid. Apply the game's 80-unit constant D16 bias at comparison time as
    // a driver-independent equivalent (reference <= stored depth for the LEQUAL shadow sampler).
    // Disabled by default (the official renderer has no such bias); WWHD_SHADOW_RECEIVER_BIAS=1 restores it.
    static const bool receiverBiasOn = getenv("WWHD_SHADOW_RECEIVER_BIAS") != nullptr;
    if (receiverBiasOn && !sh->vertex && src.find("// shader c2b1a9c7373b844a") != std::string::npos) {
        const char* sample =
            "R3i.x = floatBitsToInt(texture(textureUnitPS1, vec4(intBitsToFloat(R3i.x),intBitsToFloat(R3i.y),intBitsToFloat(R3i.z),intBitsToFloat(R3i.w))));";
        if (size_t p = src.find(sample); p != std::string::npos) {
            constexpr const char* receiverBias =
                "R3i.w = floatBitsToInt(max(intBitsToFloat(R3i.w) - 0.001220703125, 0.0));\r\n"
                "R0i.w = floatBitsToInt(max(intBitsToFloat(R0i.w) - 0.001220703125, 0.0));\r\n";
            src.insert(p, receiverBias);
        }
    }
    uint64_t hash = hash_bytes(src.data(), src.size(), sh->vertex ? 0x5653ull : 0x5053ull) ^ src.size();
    auto it = g_modules.find(hash);
    if (it != g_modules.end()) {
        sh->mod = it->second;
        g_module_cache_hits++;
        return;
    }
    auto* m = new Module();
    g_modules[hash] = m;
    sh->mod = m;
    uint64_t key = sh->key;
    bool vertex = sh->vertex;
    if (g_sync_shaders) {
        compile_now(m, hash, key, vertex, src);
        return;
    }
    g_compiles_in_flight++;
    g_workers.push([m, hash, key, vertex, src] {
        compile_now(m, hash, key, vertex, src);
        g_compiles_in_flight--;
    });
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

// The registers the shader key takes whole (stage_state_hash); shader_reg_relevant below derives
// from this table and the masked ones in stage_state_hash, so keep both in step.
static const struct { uint32_t first, count; } kKeyRegs[] = {
    {mmSQ_VTX_SEMANTIC_0, 32}, {mmSPI_VS_OUT_ID_0, 10}, {mmSPI_VS_OUT_CONFIG, 1}, {mmPA_CL_VS_OUT_CNTL, 1},
    {mmSPI_PS_IN_CONTROL_0, 2}, {mmSPI_PS_INPUT_CNTL_0, 32}, {REGADDR::SQ_CONFIG, 1}, {mmCB_SHADER_MASK, 1},
    {mmCB_SHADER_CONTROL, 1}, {mmDB_SHADER_CONTROL, 1}, {mmSPI_INPUT_Z, 1}, {REGADDR::SX_ALPHA_TEST_CONTROL, 1},
    {REGADDR::PA_CL_VTE_CNTL, 1}, {REGADDR::PA_CL_CLIP_CNTL, 1}, {REGADDR::CB_COLOR_CONTROL, 1}, {REGADDR::CB_TARGET_MASK, 1},
    {mmCB_COLOR0_INFO, 8}};

// the register state that shapes a stage's translation, as words (hashed into the shader key)
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

static uint64_t stage_state_hash(const uint32_t* regs, uint64_t h, uint32_t texBase) {
    uint32_t buf[400];
    uint32_t n = stage_state_words(regs, texBase, buf);
    return hash_bytes(buf, n * 4, h);
}

// Does a register change alter the shader key? gx2_core.cpp counts only those in g_shader_state_gen
// (plugged in below). The programs' addresses aren't counted: get_shader compares them itself.
extern "C" bool (*g_shader_reg_filter)(uint32_t reg, uint32_t oldv, uint32_t newv);
static bool shader_reg_relevant(uint32_t reg, uint32_t oldv, uint32_t newv) {
    static const std::vector<bool> whole = [] {
        std::vector<bool> m(0x10000);
        for (auto& k : kKeyRegs)
            for (uint32_t i = 0; i < k.count; i++) m[k.first + i] = true;
        return m;
    }();
    if (reg < whole.size() && whole[reg]) return true;
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
static bool g_reg_filter_set = (g_shader_reg_filter = &shader_reg_relevant, true);

static Shader* get_shader_uncached(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey);

static Shader* get_shader(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey) {
    const uint32_t start = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
    const uint32_t addr = regs[start], size = regs[start + 1];
    // nothing shader-relevant changed since the previous draw: same shader
    struct Last { uint64_t gen = 0, frame = ~0ull, fsKey = 0; uint32_t addr = 0, size = 0; Shader* s = nullptr; };
    static Last last[2];
    Last& L = last[vertex ? 0 : 1];
    if (L.gen == g_shader_state_gen && L.frame == R.frame && L.addr == addr && L.size == size && (!vertex || L.fsKey == fsKey))
        return L.s;
    // the same state with another program (the game switches between a few): remembered per program
    struct Memo { uint64_t gen = 0, frame = ~0ull; std::unordered_map<uint64_t, Shader*> m; };
    static Memo memo[2];
    Memo& M = memo[vertex ? 0 : 1];
    if (M.gen != g_shader_state_gen || M.frame != R.frame) {
        M.m.clear();
        M.gen = g_shader_state_gen;
        M.frame = R.frame;
    }
    uint64_t mk = ((uint64_t)addr << 32 | size) ^ (vertex ? fsKey * 0x9E3779B97F4A7C15ull : 0);
    auto it = M.m.find(mk);
    Shader* s = it != M.m.end() ? it->second : (M.m[mk] = get_shader_uncached(regs, vertex, fs, fsKey));
    L = Last{g_shader_state_gen, R.frame, fsKey, addr, size, s};
    return s;
}

static void cache_record_shader(const uint32_t* regs, bool vertex);
static bool g_defer_compiles = false;
static std::vector<Shader*> g_deferred_shaders;

static void compile_deferred(Shader* s) {
    if (s->state != CS_DEFERRED) return;
    s->state = CS_PENDING;
    compile_shader(s);
}

static Shader* get_shader_uncached(const uint32_t* regs, bool vertex, LatteFetchShader* fs, uint64_t fsKey) {
    uint32_t addr = regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] << 8;
    uint32_t size = regs[(vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS) + 1] << 3;
    if (!addr || !size) return nullptr;
    uint64_t key = program_hash(addr, size) ^ (vertex ? 0x1111 : 0x2222);
    uint64_t base = key;
    // recent lookups: the same program with the same state words is the same shader (comparing the
    // words is cheaper than hashing them and looking the key up)
    struct Memo { uint64_t base, fsKey; uint32_t n; uint32_t words[400]; Shader* s; };
    static Memo memo[64];
    uint32_t words[400];
    uint32_t n = stage_state_words(regs, vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS, words);
    uint64_t fk = vertex ? fsKey : 0;
    Memo& m = memo[(base ^ (base >> 21) ^ fk) & 63];
    if (m.s && m.base == base && m.fsKey == fk && m.n == n && memcmp(m.words, words, n * 4) == 0) return m.s;
    key = hash_bytes(words, n * 4, key);
    if (vertex) key ^= fsKey * 31;
    auto remember = [&](Shader* sh) {
        m.base = base;
        m.fsKey = fk;
        m.n = n;
        memcpy(m.words, words, n * 4);
        m.s = sh;
        return sh;
    };
    auto it = g_shaders.find(key);
    if (it != g_shaders.end()) return remember(it->second);

    auto* s = new Shader();
    s->key = key;
    s->vertex = vertex;
    g_shaders[key] = s;
    double t0 = now_ms();
    LatteShader_UpdatePSInputs((uint32*)regs);
    LatteDecompilerOptions opt;
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
    create_set_layout(s);
    cache_record_shader(regs, vertex);
    g_t_decompile += now_ms() - t0;
    // debug: WWHD_DUMP_SHADERS=1 writes the translated GLSL to ./shaders, =/some/dir there
    if (const char* dump = getenv("WWHD_DUMP_SHADERS")) {
        std::string dir = dump[0] == '/' ? dump : "shaders";
        mkdir(dir.c_str(), 0755);
        char name[96];
        snprintf(name, sizeof name, "/%s_%08X_%016llx.glsl", vertex ? "vs" : "ps", addr, (unsigned long long)key);
        if (FILE* f = fopen((dir + name).c_str(), "w")) { fputs(s->dec->strBuf_shaderSource->c_str(), f); fclose(f); }
    }
    if (g_defer_compiles) {
        s->state = CS_DEFERRED;  // cache replay: compile on first use or gradually in the background
        g_deferred_shaders.push_back(s);
    } else {
        compile_shader(s);
    }
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

static VkRenderPass get_render_pass(const PassFormats& pf) {
    static std::unordered_map<uint64_t, VkRenderPass> cache;
    uint64_t h = hash_bytes(&pf, sizeof pf);
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
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
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
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp = pf.stencil ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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

// Ambient-occlusion quirks, switchable in game (settings menu; WWHD_AO_MODE=0..3 sets the start):
//   0 = as the hardware renders it
//   1 = centre depth sampled bilinear (removes the every-third-row lines)
//   2 = 1 + noise tiled per 960x540 pixel instead of per 640x360 pixel (removes the remaining
//       uneven noise bands the game's blur can't average out)
//   3 = disabled (the AO pass writes neutral white)
static std::atomic<int> g_ao_mode{[] {
    if (const char* e = getenv("WWHD_AO_MODE")) return std::clamp(atoi(e), 0, 3);
    return getenv("WWHD_NO_AO_QUIRK") ? 0 : 2;
}()};
int ao_mode() { return g_ao_mode.load(std::memory_order_relaxed); }
void set_ao_mode(int m) { m = std::clamp(m, 0, 3); g_ao_mode = m; LOG("[gfx] ambient occlusion mode %d", m); }

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
    // shaders with identical translations share pipelines
    Module* mods[2] = {vs->mod, ps->mod};
    uint64_t h = hash_bytes(mods, sizeof mods);
    h = hash_bytes(&vs->dsl, sizeof(vs->dsl), h);
    h = hash_bytes(&ps->dsl, sizeof(ps->dsl), h);
    h ^= fsKey;
    h = hash_bytes(&st, sizeof st, h);
    auto it = g_pipelines.find(h);
    if (it != g_pipelines.end()) return wait_compiled(it->second->status) ? it->second : nullptr;
    auto* pl = new Pipeline();
    g_pipelines[h] = pl;
    cache_record_pipeline(regs, vs, ps, fsKey, st);
    pl->layout = get_pipeline_layout(vs, ps);

    auto d = std::make_shared<PipelineDesc>();
    d->stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs->mod->module, "main", nullptr};
    d->stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, ps->mod->module, "main", nullptr};
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

    auto build = [pl, d] {
        auto t0 = std::chrono::steady_clock::now();
        VkResult r = vkCreateGraphicsPipelines(R.device, R.pipelineCache, 1, &d->pi, nullptr, &pl->pipeline);
        g_t_pipeline_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
        if (r != VK_SUCCESS) LOG("[gfx] pipeline creation failed: VkResult %d", (int)r);
        else g_pipelines_created++;
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
    });
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
static VkSampler get_sampler(const uint32_t* w, bool allowAniso = false) {
    const bool anisoForce = allowAniso && aniso_enabled();
    uint64_t h = hash_bytes(w, 12) ^ (anisoForce ? 0x5A5A5A5A5A5A5A5Aull : 0);
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
    float maxAniso = 1.0f;
    if (uint32_t aniso = w0.get_MAX_ANISO_RATIO()) maxAniso = (float)std::min(1u << aniso, 16u);
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

static VkImageView texture_view(Surface* s, VkImageViewType type, uint32_t word4) {
    LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    memcpy(&w4, &word4, 4);
    uint32_t sel[4] = {(uint32_t)w4.get_DST_SEL_X(), (uint32_t)w4.get_DST_SEL_Y(), (uint32_t)w4.get_DST_SEL_Z(), (uint32_t)w4.get_DST_SEL_W()};
    if (s->fmt.depth) sel[0] = 0, sel[1] = 1, sel[2] = 2, sel[3] = 3;  // depth: shaders read .x
    uint64_t key = (uint64_t)(uintptr_t)s->img.image ^ ((uint64_t)type << 56) ^ ((uint64_t)(sel[0] | sel[1] << 4 | sel[2] << 8 | sel[3] << 12) << 40);
    auto it = g_views.find(key);
    if (it != g_views.end()) return it->second.view;
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
        c->mips = 1;
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
    VkImageCopy r{};
    r.srcSubresource = {s->img.aspect, 0, 0, std::min(s->img.layers, c->img.layers)};
    r.dstSubresource = {c->img.aspect, 0, 0, std::min(s->img.layers, c->img.layers)};
    r.extent = {std::min(s->img.width, c->img.width), std::min(s->img.height, c->img.height), 1};
    vkCmdCopyImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    return c;
}

// textures of one stage, resolved before the render pass begins
struct StageTextures {
    VkImageView view[LATTE_NUM_MAX_TEX_UNITS];
    VkSampler sampler[LATTE_NUM_MAX_TEX_UNITS];
    float unitScale[LATTE_NUM_MAX_TEX_UNITS];  // by texture unit: resolution scale of the bound surface (x)
    float unitScaleY[LATTE_NUM_MAX_TEX_UNITS]; // ... (y; differs from x for aspect-widened targets)
};

static void resolve_textures(const uint32_t* regs, Shader* sh, bool vertex, Surface* const* colors, Surface* depth, StageTextures& out) {
    LatteDecompilerShader* dec = sh->dec;
    auto& rm = dec->resourceMapping;
    uint32_t texBase = vertex ? REGADDR::SQ_TEX_RESOURCE_WORD0_N_VS : REGADDR::SQ_TEX_RESOURCE_WORD0_N_PS;
    uint32_t samplerBase = vertex ? SAMPLER_BASE_INDEX_VERTEX : SAMPLER_BASE_INDEX_PIXEL;
    for (int u = 0; u < LATTE_NUM_MAX_TEX_UNITS; u++) out.unitScale[u] = out.unitScaleY[u] = 1.0f;
    for (sint32 i = 0; i < rm.getTextureCount(); i++) {
        out.view[i] = VK_NULL_HANDLE;
        out.sampler[i] = VK_NULL_HANDLE;
        sint32 unit = rm.getRelativeTextureUnitFromRelativeBindingPoint(i);
        if (unit < 0) continue;
        VkImageViewType type = shader_view_type(dec->textureUnitDim[unit]);
        const uint32_t* tw = &regs[texBase + unit * 7];
        Surface* s = sampled_texture(tw, dec->textureUsesDepthCompare[unit]);
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
            prepare(s->img, Use::SAMPLED);
            out.unitScale[unit] = s->rscale * s->ax;
            out.unitScaleY[unit] = s->rscale * s->ay;
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
        out.sampler[i] = get_sampler(sw, s && !s->gpuWritten && s->mips > 1);
    }
}

// Descriptor sets for shaders with dynamic uniform buffers, kept across draws and frames. Everything
// they reference (image views, samplers, transient buffers) lives as long as the renderer.
static std::unordered_map<uint64_t, VkDescriptorSet> g_set_cache;
static uint64_t g_set_cache_epoch = 0;  // bumped when cached sets are dropped (bind_stage's last-set check)
static std::vector<VkDescriptorPool> g_set_pools;

static VkDescriptorSet cached_descriptor_set(uint64_t key, VkDescriptorSetLayout dsl, VkWriteDescriptorSet* writes, uint32_t nw) {
    auto it = g_set_cache.find(key);
    if (it != g_set_cache.end()) return it->second;
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
    std::vector<VkFramebuffer> fbs;
    for (auto& [k, fb] : g_framebuffers) fbs.push_back(fb);
    g_framebuffers.clear();
    g_set_cache.clear();
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
        memset(u.ptr, 0, size);
        float* f = (float*)u.ptr;
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
        if (dec->uniform.loc_uniformRegister >= 0)
            memcpy(at(dec->uniform.loc_uniformRegister), &regs[aluBase], dec->uniform.count_uniformRegister * 16);
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
        // Uniform blocks are compared, not reused by write tracking (as the official renderer's
        // uniform snapshots do): the game rewrites some (0x800-byte light/shadow blocks) without
        // the flush the tracking relies on, and a stale copy drew the shadows with another frame's
        // matrices. They are small (~100 KiB a frame). WWHD_TRACK_UBO_WRITES=1: the tracking again.
        static const bool trackUbo = g_track_writes && getenv("WWHD_TRACK_UBO_WRITES") != nullptr;
        Upload u;
        if (addr && addr + (uint64_t)size <= 0x100000000ull) {
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
    VkRenderPass rp = get_render_pass(pf);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = rp;
    bi.framebuffer = get_framebuffer(rp, views, n, w, h);
    bi.renderArea = {{0, 0}, {w, h}};
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
static uint64_t g_index_reused, g_index_built;
static bool draw_indices(uint32_t prim, uint32_t count, uint32_t indexType, uint32_t indexAddr, DrawIndices& out) {
    struct Entry { DrawIndices d; uint32_t prim, indexType, gen; };
    static std::unordered_map<uint64_t, Entry> cache;
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
        auto it = cache.find(key);
        if (it != cache.end() && it->second.prim == prim && it->second.indexType == indexType &&
            memw::unchanged_since(indexAddr, bytes, it->second.gen)) {
            g_index_reused++;
            out = it->second.d;
            return true;
        }
    }
    uint32_t gen = memw::now();  // before reading: a write during it makes the next use convert again
    if (!build_indices(prim, count, indexType, indexAddr, tmp, out.type)) return false;
    g_index_built++;
    out.n = (uint32_t)tmp.size();
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
constexpr uint32_t kRecShader = 1, kRecPipelineVk = 4;
FILE* g_cache_out = nullptr;
bool g_cache_replaying = false;
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
    return ld32(prog) == 0x57574653 ? 16 + ld32(prog + 4) * 16 : regs[mmSQ_PGM_START_FS + 1] << 3;
}
}  // namespace

static void cache_record_shader(const uint32_t* regs, bool vertex) {
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
    cache_write(kRecShader, v);
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

// load and replay the recipes; runs once on the render thread before the first draw
static void cache_load() {
    std::string path = cache_path();
    if (path == "0") { g_cache_disabled = true; return; }
    size_t slash = path.find_last_of('/');
    spirv_cache_open(slash == std::string::npos ? std::string(".") : path.substr(0, slash));
    double t0 = now_ms();
    size_t shaders = 0, pipelines = 0;
    if (FILE* f = fopen(path.c_str(), "rb")) {
        static std::vector<uint32_t> regs(0x10000);
        g_cache_replaying = true;
        g_defer_compiles = true;
        uint32_t hdr[3];
        while (fread(hdr, sizeof hdr, 1, f) == 1) {
            std::vector<uint8_t> comp(hdr[2]), raw(hdr[1]);
            if (fread(comp.data(), 1, hdr[2], f) != hdr[2]) break;
            uLongf rsize = hdr[1];
            if (uncompress(raw.data(), &rsize, comp.data(), hdr[2]) != Z_OK || rsize != hdr[1]) break;
            const uint8_t* p = raw.data();
            const uint8_t* end = p + raw.size();
            if (hdr[0] == kRecPipelineVk) {
                PipelineRecipe r;
                if (raw.size() == sizeof r && get(p, end, r)) { g_pending_pipelines.push_back(r); pipelines++; }
                continue;
            }
            if (hdr[0] != kRecShader) continue;
            uint32_t vertex, size, fsSize, n;
            if (!get(p, end, vertex) || !get(p, end, size) || !get(p, end, fsSize) || !get(p, end, n)) break;
            if (p + size + fsSize + n * 8 > end) break;
            // the microcode goes to guest memory so the normal translation path can read it. One
            // host-only scratch buffer is reused: translation keeps no pointer into it, and a buffer
            // per record would run out of guest memory for a large cache (100k+ variants)
            static uint32_t scratch = 0, scratchSize = 0;
            uint32_t need = ((size + 0xFF) & ~0xFFu) + fsSize;
            if (need > scratchSize) {
                scratchSize = std::max<uint32_t>(need * 2, 0x10000);
                scratch = mem::host_alloc(scratchSize, 0x100);
            }
            uint32_t prog = scratch;
            memcpy(mem::ptr(prog), p, size);
            p += size;
            uint32_t fsProg = 0;
            if (fsSize) {
                fsProg = scratch + ((size + 0xFF) & ~0xFFu);
                memcpy(mem::ptr(fsProg), p, fsSize);
                p += fsSize;
            }
            // program_hash remembers hashes by address within a frame: forget the scratch buffer's,
            // or a record whose program has the size of the previous one would get its hash (and
            // the previous program's key: a wrong shader for a live draw, e.g. a black sky)
            g_program_hashes.erase(prog);
            if (fsSize) g_program_hashes.erase(fsProg);
            std::fill(regs.begin(), regs.end(), 0);
            for (uint32_t i = 0; i < n; i++) {
                uint32_t r, val;
                get(p, end, r);
                get(p, end, val);
                regs[r] = val;
            }
            uint32_t pgm = vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS;
            regs[pgm] = prog >> 8;
            if (fsSize) regs[mmSQ_PGM_START_FS] = fsProg >> 8;
            uint64_t fsKey = 0;
            LatteFetchShader* fs = vertex ? get_fetch_shader(regs.data(), &fsKey) : nullptr;
            if (vertex && !fs) continue;
            get_shader_uncached(regs.data(), vertex, fs, fsKey);
            shaders++;
        }
        fclose(f);
        g_cache_replaying = false;
        g_defer_compiles = false;
    }
    void headstart_load();  // gfx/shader_headstart.cpp
    headstart_load();
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

// build cached pipelines whose shaders have finished compiling; a few per frame
void cache_warm_step() {
    // a few background compiles per frame keep the startup burst from starving the game, and none
    // start while the game's own compiles keep the compiler busy (WWHD_BG_COMPILES, default 4 in flight)
    static const int maxInFlight = getenv("WWHD_BG_COMPILES") ? atoi(getenv("WWHD_BG_COMPILES")) : 4;
    for (int n = 0; n < 8 && !g_deferred_shaders.empty() && g_compiles_in_flight < maxInFlight;) {
        Shader* s = g_deferred_shaders.back();
        g_deferred_shaders.pop_back();
        if (s->state == CS_DEFERRED) { compile_deferred(s); n++; }
    }
    static bool queued = false;
    if (!g_pending_pipelines.empty()) queued = true;
    build_pending_pipelines(8, maxInFlight);
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
    extern uint64_t g_stat_full_checks, g_stat_uploads, g_stat_invalidates, g_stat_invalidated_surfaces;
    LOG("[gfx] last 300 frames: %llu full texture checks, %llu uploads, %llu invalidates marking %llu surfaces",
        (unsigned long long)g_stat_full_checks, (unsigned long long)g_stat_uploads, (unsigned long long)g_stat_invalidates,
        (unsigned long long)g_stat_invalidated_surfaces);
    g_stat_full_checks = g_stat_uploads = g_stat_invalidates = g_stat_invalidated_surfaces = 0;
    LOG("[gfx] last 300 frames, KiB/frame copied: vertices %.0f (+%.0f large, once per submission; %.0f reused), uniform blocks %.0f "
        "(%.0f reused), uniform vars %.0f",
        g_bytes_vtx / 300.0 / 1024, g_bytes_vtx_shared / 300.0 / 1024, g_bytes_vtx_reused / 300.0 / 1024, g_bytes_ubo / 300.0 / 1024,
        g_bytes_ubo_reused / 300.0 / 1024, g_bytes_vars / 300.0 / 1024);
    g_bytes_vtx = g_bytes_vtx_shared = g_bytes_ubo = g_bytes_vars = g_bytes_vtx_reused = g_bytes_ubo_reused = 0;
    LOG("[gfx] last 300 frames, draws/frame: %.0f with state reused, %.0f resolved; indices %.0f reused, %.0f built",
        g_fast_draws / 300.0, g_slow_draws / 300.0, g_index_reused / 300.0, g_index_built / 300.0);
    g_index_reused = g_index_built = 0;
    if (g_stale_reuses) LOG("[gfx] write tracking: %llu reuses were outdated (written without a flush)", (unsigned long long)g_stale_reuses);
    g_stale_reuses = 0;
    g_fast_draws = g_slow_draws = 0;
}

// ---------------------------------------------------------------- vertex data
// Guest vertex buffers are copied into transient memory per draw. Large buffers (static meshes) are
// copied once per submission and shared by the draws in it while the game hasn't written their
// pages since (mem_writes.h; WWHD_TRACK_WRITES=0: compared instead), as the official renderer
// never reuses a copy on its address alone: a mesh rewritten within a submission (cloth, morphs,
// a buffer reused for another mesh) drew its old vertices.
static Upload vertex_buffer(uint32_t addr, uint32_t size) {
    struct Shared { Upload u; uint32_t gen; };
    static std::unordered_map<uint64_t, Shared> shared;
    static uint64_t sharedSerial = ~0ull;
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
    const float targetScale = P.targetScale, k = P.k, targetScaleY = P.targetScaleY, ky = P.ky;
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
    // Latte has independent polygon offsets for front and back faces. Shadow maps commonly cull
    // their front faces and render only back faces, so ignoring the back offset removes the bias
    // entirely and turns self-shadowing into a camera-dependent moire pattern. Vulkan exposes one
    // bias for the draw; select the state of the face that can actually survive culling.
    const bool frontBias = pm.get_OFFSET_FRONT_ENABLED();
    // Disabled by default (the official renderer uses the front offset only); WWHD_BACK_POLY_OFFSET=1 restores it.
    static const bool backBiasOn = getenv("WWHD_BACK_POLY_OFFSET") != nullptr;
    const bool backBias = backBiasOn && pm.get_OFFSET_BACK_ENABLED();
    const bool useBackBias = backBias && (!frontBias || (pm.get_CULL_FRONT() && !pm.get_CULL_BACK()));
    if (frontBias || backBias) {
        const uint32_t scaleReg = useBackBias ? REGADDR::PA_SU_POLY_OFFSET_BACK_SCALE : REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE;
        const uint32_t offsetReg = useBackBias ? REGADDR::PA_SU_POLY_OFFSET_BACK_OFFSET : REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET;
        bias[2] = gx2::bitsf(regs[scaleReg]) / 16.0f;
        bias[0] = gx2::bitsf(regs[offsetReg]);
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
    VkViewport vp{(xo - xs) * k, (yo - ys) * ky, xs * 2.0f * k, ys * 2.0f * ky, halfZ ? zo : zo - zs, zs + zo};
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
    prof_draw(regs[mmSQ_PGM_START_PS] << 8);
    draws_since_commit()++;
    DLOG("[draw] prim %X count %u idx %u@%08X VS %08X PS %08X CB0 %08X info %08X DB %08X depthctl %08X blend %08X mask %08X scmode %08X bias %08X/%08X %08X/%08X",
         prim, count, indexType, indexAddr, regs[mmSQ_PGM_START_VS] << 8, regs[mmSQ_PGM_START_PS] << 8, regs[mmCB_COLOR0_BASE],
         regs[mmCB_COLOR0_INFO], regs[mmDB_DEPTH_BASE], regs[REGADDR::DB_DEPTH_CONTROL], regs[REGADDR::CB_COLOR_CONTROL],
         regs[REGADDR::CB_TARGET_MASK], regs[REGADDR::PA_SU_SC_MODE_CNTL],
         regs[REGADDR::PA_SU_POLY_OFFSET_FRONT_SCALE], regs[REGADDR::PA_SU_POLY_OFFSET_FRONT_OFFSET],
         regs[REGADDR::PA_SU_POLY_OFFSET_BACK_SCALE], regs[REGADDR::PA_SU_POLY_OFFSET_BACK_OFFSET]);
    if (!count || !instances) return;
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
    Shader* ps = get_shader(regs, false, fs, fsKey);
    if (!vs || !ps || shader_state(vs) == CS_FAILED || shader_state(ps) == CS_FAILED) { g_skip[SK_NO_SHADER]++; return; }
    compile_deferred(vs);
    compile_deferred(ps);
    // debug: WWHD_DUMP_PS=addr,addr,... writes the translated pixel shader (and its vertex shader) of
    // the first draw with that pixel shader address into WWHD_DUMP_SHADERS's folder (or ./shaders)
    static std::set<uint32_t> dumpPS = [] {
        std::set<uint32_t> d;
        if (const char* e = getenv("WWHD_DUMP_PS"))
            for (char* p = (char*)e; *p;) {
                d.insert((uint32_t)strtoul(p, &p, 16));
                while (*p == ',') p++;
            }
        return d;
    }();
    if (!dumpPS.empty()) {
        uint32_t psAddr = regs[mmSQ_PGM_START_PS] << 8;
        if (dumpPS.erase(psAddr)) {
            const char* d = getenv("WWHD_DUMP_SHADERS");
            std::string dir = d && d[0] == '/' ? d : "shaders";
            mkdir(dir.c_str(), 0755);
            for (Shader* sh : {ps, vs}) {
                char name[64];
                snprintf(name, sizeof name, "/live_%s_%08X.glsl", sh == ps ? "ps" : "vs", psAddr);
                if (FILE* f = fopen((dir + name).c_str(), "w")) {
                    fputs(sh->dec->strBuf_shaderSource->c_str(), f);
                    fclose(f);
                }
            }
        }
    }

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

    // Diagnostic and user option: remove ambient occlusion completely. The later lighting pass
    // multiplies by this texture, so white is the neutral result. Clear the actual AO target rather
    // than merely skipping its draw, which would leave the previous frame's occlusion behind.
    if (ao_mode() == 3 && (regs[mmSQ_PGM_START_PS] << 8) == kOcclusionPS) {
        VkClearColorValue neutral{{1.0f, 1.0f, 1.0f, 1.0f}};
        for (Surface* s : colors) {
            if (!s) continue;
            prepare(s->img, Use::COPY_DST);
            VkImageSubresourceRange range{s->img.aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
            vkCmdClearColorImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &neutral, 1, &range);
            // The clear replaces this render target's contents just like a draw. Keep it the
            // newest surface when multiple Vulkan images alias the same guest address; otherwise
            // a later texture lookup can select an older AO image and make "off" ineffective.
            mark_gpu_written(s);
        }
        g_prep_key.valid = false;
        return;
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
    const float k = (g_hires_redraw ? 1.5f : 1.0f) * targetScale, ky = (g_hires_redraw ? 1.5f : 1.0f) * targetScaleY;
    uint32_t tl = regs[REGADDR::PA_SC_GENERIC_SCISSOR_TL], br = regs[REGADDR::PA_SC_GENERIC_SCISSOR_BR];
    uint32_t sx = std::min<uint32_t>((uint32_t)((tl & 0x7FFF) * k), w), sy = std::min<uint32_t>((uint32_t)(((tl >> 16) & 0x7FFF) * ky), h);
    uint32_t ex = std::min<uint32_t>((uint32_t)((br & 0x7FFF) * k), w), ey = std::min<uint32_t>((uint32_t)(((br >> 16) & 0x7FFF) * ky), h);
    if (ex <= sx || ey <= sy) { g_skip[SK_SCISSOR]++; return; }

    // textures: uploads, layout changes and copies happen before the render pass begins
    StageTextures vtex, ptex;
    resolve_textures(regs, vs, true, colors, depth, vtex);
    resolve_textures(regs, ps, false, colors, depth, ptex);

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
    st.depthControl = depth ? regs[REGADDR::DB_DEPTH_CONTROL] : 0;
    LATTE_PA_CL_CLIP_CNTL clipCntl;
    uint32_t clipRaw = regs[REGADDR::PA_CL_CLIP_CNTL];
    memcpy(&clipCntl, &clipRaw, 4);
    st.depthClamp = clipCntl.get_ZCLIP_FAR_DISABLE() ? 1 : 0;

    Pipeline* pipe = get_pipeline(regs, vs, ps, fs, fsKey, st);
    g_wait_whole = false;
    if (!pipe) { g_skip[SK_PIPE_COMPILING]++; return; }

    if (!ensure_pass(colors, colorSlices, depth, depthSlice, w, h, st.pass)) { g_skip[SK_PASS]++; return; }
    Prepared hiresPrep;  // the AO redraw (called from record_draw) must not replace the fast path's state
    Prepared& P = g_hires_redraw ? hiresPrep : g_prep;
    P.fs = fs;
    P.vs = vs;
    P.ps = ps;
    memcpy(P.colors, colors, sizeof P.colors);
    P.depth = depth;
    P.depthSlice = depthSlice;
    P.w = w;
    P.h = h;
    P.targetScale = targetScale;
    P.k = k;
    P.targetScaleY = targetScaleY;
    P.ky = ky;
    P.sx = sx; P.sy = sy; P.ex = ex; P.ey = ey;
    P.vtex = vtex;
    P.ptex = ptex;
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

// Head-start pipeline records are in the macOS recipe format, which lacks the state Vulkan pipelines
// need; their shaders are still pre-translated, and the pipelines get built on first use.
bool headstart_queue_pipeline(const uint8_t*, size_t) { return false; }

size_t headstart_build_pipelines(int maxInFlight, size_t& built, size_t& dropped) {
    build_pending_pipelines(INT_MAX, maxInFlight);
    built = g_recipes_built;
    dropped = g_recipes_dropped;
    return g_pending_pipelines.size();
}

}  // namespace gfx
