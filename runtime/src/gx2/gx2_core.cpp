#include <pthread.h>
#include <condition_variable>
#include <deque>
// GX2 core: command execution, display lists, context states, draws, clears,
// copies and presentation.
#include <algorithm>
#include <chrono>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gx2.h"
#include "gx2_cmd.h"
#include "gx2_regs.h"
#include "gx2_texture_regs.h"
#include "platform.h"
#include "runtime.h"
#include "../aspect.h"
#include <map>
#include <unistd.h>
#include <dirent.h>
#include "mem_writes.h"

using namespace Latte;

namespace gx2 {

// ---------------------------------------------------------------- register file and context states
static uint32 g_regs[kNumRegs];
static uint32* g_shadow = nullptr;  // register copy of the active GX2ContextState
static std::unordered_map<uint32, std::vector<uint32>> g_contexts;
// Register blocks ever written (in g_regs or any context). Every other block is zero everywhere, so
// a context switch copies only these instead of the whole 256 KiB register file.
constexpr uint32 kRegBlock = 64;
static bool g_reg_touched[kNumRegs / kRegBlock];
static std::vector<uint16_t> g_reg_blocks;
static void touch_regs(uint32 first, uint32 n) {
    if (!n) return;
    for (uint32 b = first / kRegBlock, e = (first + n - 1) / kRegBlock; b <= e && b < kNumRegs / kRegBlock; b++)
        if (!g_reg_touched[b]) {
            g_reg_touched[b] = true;
            g_reg_blocks.push_back((uint16_t)b);
        }
}
static void touch_all_regs() { touch_regs(0, kNumRegs); }
static std::recursive_mutex g_exec_mutex;

uint32* regs() { return g_regs; }

// Register writes that can change how shaders are translated bump g_shader_state_gen; the
// renderer reuses its last shader lookup while it is unchanged. Uniforms, uniform/vertex buffer
// addresses and rewrites of an identical value don't count.
extern "C" { uint64_t g_shader_state_gen = 1; }
// any register change outside shader_irrelevant (the draw fast path in vk_draw.cpp: the shader
// filter leaves out texture addresses and the like, which a draw must resolve again)
extern "C" { uint64_t g_draw_state_gen = 1; }

static bool shader_irrelevant(uint32 reg) {
    if (reg >= mmSQ_ALU_CONSTANT0_0 && reg < mmSQ_ALU_CONSTANT0_0 + 0x1000) return true;
    for (uint32 base : {(uint32)mmSQ_VTX_UNIFORM_BLOCK_START, (uint32)mmSQ_PS_UNIFORM_BLOCK_START, (uint32)mmSQ_GS_UNIFORM_BLOCK_START})
        if (reg >= base && reg < base + 7 * 16) return true;
    if (reg >= mmSQ_VTX_ATTRIBUTE_BLOCK_START && reg < mmSQ_VTX_ATTRIBUTE_BLOCK_START + 7 * 16) {
        uint32 w = (reg - mmSQ_VTX_ATTRIBUTE_BLOCK_START) % 7;
        return w != 2;  // word 2 holds the stride
    }
    return false;
}

// The renderer may name exactly the register changes that alter its shader key (vk_draw.cpp);
// without a filter every register outside shader_irrelevant counts (the Metal renderer).
extern "C" { bool (*g_shader_reg_filter)(uint32 reg, uint32 oldv, uint32 newv) = nullptr; }

// debug: WWHD_GEN_STATS=1 counts what bumps g_shader_state_gen (logged with the frame line)
static const bool g_gen_stats = getenv("WWHD_GEN_STATS") != nullptr;
static uint64_t g_gen_ctx = 0, g_gen_ctx_calls = 0, g_gen_regs = 0;
static std::unordered_map<uint32, uint64_t> g_gen_by_reg;

static void apply_regs(uint32 first, const uint32* v, uint32 n) {
    if (first + n > kNumRegs) return;
    touch_regs(first, n);
    if (memcmp(&g_regs[first], v, n * 4) != 0) {
        static const bool coarse = getenv("WWHD_COARSE_SHADER_GEN") != nullptr;  // debug: the old rule
        bool draw = false, shader = false;
        for (uint32 i = 0; i < n && !(draw && shader); i++) {
            if (g_regs[first + i] == v[i] || shader_irrelevant(first + i)) continue;
            draw = true;  // any state a draw resolves (textures, samplers, targets, ...)
            if (!shader && (g_shader_reg_filter && !coarse ? g_shader_reg_filter(first + i, g_regs[first + i], v[i]) : true)) {
                shader = true;
                if (g_gen_stats) {
                    g_gen_regs++;
                    g_gen_by_reg[first + i]++;
                }
            }
        }
        if (shader) g_shader_state_gen++;
        if (draw) g_draw_state_gen++;
        memcpy(&g_regs[first], v, n * 4);
    }
    if (g_shadow) memcpy(&g_shadow[first], v, n * 4);
}

// ---------------------------------------------------------------- display list recording
struct Recording {
    uint32 start = 0, pos = 0, end = 0;
};
static thread_local Recording t_rec;

static void execute_one(Op op, const uint32* p, uint32 n);

// ---------------------------------------------------------------- render thread
// Like the real GPU, command execution runs asynchronously to the game: GX2 calls append to a
// queue that a render thread turns into GPU work (Metal or Vulkan). WWHD_NO_RENDER_THREAD=1 executes inline.
static const bool g_render_thread = getenv("WWHD_NO_RENDER_THREAD") == nullptr;
static std::mutex g_q_mutex;
static std::condition_variable g_q_cv, g_q_done_cv;
static std::vector<uint32> g_q_pending, g_q_work;
static bool g_q_waiting = false;
static uint64_t g_fence_issued = 0, g_fence_done = 0;

static void render_thread_main() {
    platform::set_thread_name("GX2 render");
    platform::set_thread_high_priority();
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(g_q_mutex);
            g_q_waiting = true;
            g_q_cv.wait(lk, [] { return !g_q_pending.empty(); });
            g_q_waiting = false;
            g_q_work.swap(g_q_pending);
        }
        {
            std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
            gfx::with_autorelease_pool([] { execute(g_q_work.data(), (uint32)g_q_work.size()); });
        }
        g_q_work.clear();
    }
}

static void enqueue(Op op, const uint32* payload, uint32 n) {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(render_thread_main).detach(); });
    std::lock_guard<std::mutex> lk(g_q_mutex);
    g_q_pending.push_back(op | (n << 8));
    g_q_pending.insert(g_q_pending.end(), payload, payload + n);
    if (g_q_waiting) g_q_cv.notify_one();
}

// block the game thread until the render thread has executed everything queued so far
static void render_sync() {
    if (!g_render_thread) return;
    uint64_t id;
    {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        id = ++g_fence_issued;
    }
    uint32 w = (uint32)id;
    enqueue(OP_FENCE, &w, 1);
    std::unique_lock<std::mutex> lk(g_q_mutex);
    g_q_done_cv.wait(lk, [&] { return g_fence_done >= id; });
}

void emit(Op op, const uint32* payload, uint32 n) {
    if (t_rec.start) {
        uint32 bytes = 4 * (n + 1);
        if (t_rec.pos + bytes > t_rec.end) {
            LOG("[gx2] display list overflow at %08X", t_rec.start);
            return;
        }
        uint32* w = (uint32*)mem::ptr(t_rec.pos);
        w[0] = op | (n << 8);
        memcpy(w + 1, payload, n * 4);
        t_rec.pos += bytes;
        return;
    }
    if (g_render_thread) {
        enqueue(op, payload, n);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    execute_one(op, payload, n);
}

// host-only commands never go into display lists
static void emit_host(Op op, std::initializer_list<uint32> payload) {
    if (g_render_thread) {
        enqueue(op, payload.begin(), (uint32)payload.size());
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    execute_one(op, payload.begin(), (uint32)payload.size());
}

void set_regs(uint32 first, const uint32* values, uint32 count) {
    if (!count) return;
    static thread_local std::vector<uint32> buf;
    buf.resize(count + 1);
    buf[0] = first;
    memcpy(&buf[1], values, count * 4);
    emit(OP_SET_REGS, buf.data(), count + 1);
}
void set_reg(uint32 reg, uint32 value) { emit(OP_SET_REGS, {reg, value}); }

void execute(const uint32* words, uint32 count) {
    uint32 i = 0;
    while (i < count) {
        uint32 hdr = words[i];
        Op op = (Op)(hdr & 0xFF);
        uint32 n = hdr >> 8;
        if (op >= OP_COUNT || i + 1 + n > count) {
            LOG("[gx2] corrupt display list command %08X", hdr);
            return;
        }
        execute_one(op, &words[i + 1], n);
        i += 1 + n;
    }
}

static void set_context(uint32 ctx) {
    if (g_gen_stats) g_gen_ctx_calls++;
    if (!ctx) {
        g_shadow = nullptr;
        return;
    }
    auto it = g_contexts.find(ctx);
    if (it == g_contexts.end()) {
        g_shadow = nullptr;
        return;
    }
    g_shadow = it->second.data();
    static const bool init = (touch_regs(REGADDR::VGT_PRIMITIVE_TYPE, 1), true);  // gfx::draw writes it
    (void)init;
    for (uint16_t b : g_reg_blocks) memcpy(&g_regs[b * kRegBlock], &g_shadow[b * kRegBlock], kRegBlock * 4);
    g_shader_state_gen++;
    g_draw_state_gen++;
    if (g_gen_stats) g_gen_ctx++;
}

constexpr uint32 kColorBufferWords = 0x9C / 4, kDepthBufferWords = 0xAC / 4, kSurfaceWords = 0x74 / 4;
// struct copies carried in a command, placed back in guest memory for the renderer (commands run
// one at a time, so a couple of fixed slots suffice)
static uint32 unpack_struct(const uint32* words, uint32 count, int slot) {
    static uint32 scratch = 0;
    if (!scratch) scratch = mem::host_alloc(2 * 0x100, 0x40);
    uint32 addr = scratch + slot * 0x100;
    memcpy(mem::ptr(addr), words, count * 4);
    return addr;
}

static void execute_one(Op op, const uint32* p, uint32 n) {
    switch (op) {
    case OP_NOP: break;
    case OP_SET_REGS: apply_regs(p[0], p + 1, n - 1); break;
    case OP_DRAW: gfx::draw(g_regs, p[0], p[1], 0, 0, p[2], p[3]); break;
    case OP_DRAW_INDEXED: gfx::draw(g_regs, p[0], p[1], p[2], p[3], p[4], p[5]); break;
    case OP_CLEAR_COLOR: {
        const uint32* q = p + kColorBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        gfx::clear_color(g_regs, unpack_struct(p, kColorBufferWords, 0), rgba);
        break;
    }
    case OP_CLEAR_DEPTH: {
        const uint32* q = p + kDepthBufferWords;
        gfx::clear_depth_stencil(g_regs, unpack_struct(p, kDepthBufferWords, 0), bitsf(q[0]), q[1], q[2]);
        break;
    }
    case OP_CLEAR_BUFFERS: {
        uint32 cb = unpack_struct(p, kColorBufferWords, 0), db = unpack_struct(p + kColorBufferWords, kDepthBufferWords, 1);
        const uint32* q = p + kColorBufferWords + kDepthBufferWords;
        float rgba[4] = {bitsf(q[0]), bitsf(q[1]), bitsf(q[2]), bitsf(q[3])};
        gfx::clear_color(g_regs, cb, rgba);
        gfx::clear_depth_stencil(g_regs, db, bitsf(q[4]), q[5], q[6]);
        break;
    }
    case OP_COPY_SURFACE: {
        uint32 src = unpack_struct(p, kSurfaceWords, 0);
        const uint32* q = p + kSurfaceWords;
        uint32 dst = unpack_struct(q + 2, kSurfaceWords, 1);
        const uint32* r = q + 2 + kSurfaceWords;
        gfx::copy_surface(src, q[0], q[1], dst, r[0], r[1]);
        break;
    }
    case OP_COPY_TO_SCAN: gfx::copy_to_scan(unpack_struct(p, kColorBufferWords, 0), p[kColorBufferWords]); break;
    case OP_CALL: execute((const uint32*)mem::ptr(p[0]), p[1] / 4); break;
    case OP_SET_CONTEXT: set_context(p[0]); break;
    case OP_INVALIDATE: gfx::invalidate(p[0], p[1], p[2]); break;
    case OP_EXPAND_COLOR: case OP_EXPAND_DEPTH: break;  // MSAA/HiZ decompression: nothing to do on the host
    case OP_FLUSH: gfx::flush(); break;
    case OP_DRAW_DONE: gfx::draw_done(); break;
    case OP_SET_PROJ_REGS: {
        uint32 v[16];
        memcpy(v, p + 1, sizeof v);
        float kx, ky;
        if (n == 17 && gfx::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky))
            for (int i = 0; i < 4; i++) {  // rows x and y: the 16:9 layout space centred in the wider picture
                v[i] = gx2::fbits(gx2::bitsf(v[i]) / kx);
                v[4 + i] = gx2::fbits(gx2::bitsf(v[4 + i]) / ky);
            }
        apply_regs(p[0], v, std::min<uint32>(n - 1, 16));
        break;
    }
    case OP_PEEK_Z: gfx::peek_z(p, n); break;
    case OP_LAYOUT_ROOT: {
        float kx, ky;
        aspect::layout_root_target(p[0], gfx::target_aspect_factors(g_regs[mmCB_COLOR0_TILE] & 0xFFFF, g_regs[mmCB_COLOR0_FRAG], kx, ky));
        break;
    }
    case OP_SWAP:
        if (n) gfx::set_frame_aspect(gx2::bitsf(p[0]));  // aspect ratio from the next frame on (aspect.cpp)
        gfx::swap();
        break;
    case OP_SETUP_CONTEXT:
        g_contexts[p[0]].assign(kNumRegs, 0);
        g_shadow = g_contexts[p[0]].data();
        break;
    case OP_FENCE: {
        std::lock_guard<std::mutex> lk(g_q_mutex);
        g_fence_done = std::max<uint64_t>(g_fence_done, p[0]);
        g_q_done_cv.notify_all();
        break;
    }
    default: break;
    }
}

// ---------------------------------------------------------------- default state
static void set_default_state() {
    // GX2SetShaderModeEx(UNIFORM_REGISTER, ...)
    LATTE_SQ_CONFIG sq;
    sq.set_DX9_CONSTS(true).set_ALU_INST_PREFER_VECTOR(true).set_PS_PRIO(3).set_VS_PRIO(2).set_GS_PRIO(1).set_ES_PRIO(0);
    set_reg(REGADDR::SQ_CONFIG, sq.getRawValue());
    set_reg(REGADDR::VGT_GS_MODE, 0);
    LATTE_PA_CL_VTE_CNTL vte{};
    vte.set_VPORT_X_OFFSET_ENA(true).set_VPORT_X_SCALE_ENA(true).set_VPORT_Y_OFFSET_ENA(true).set_VPORT_Y_SCALE_ENA(true);
    vte.set_VPORT_Z_OFFSET_ENA(true).set_VPORT_Z_SCALE_ENA(true).set_VTX_W0_FMT(true);
    set_reg(REGADDR::PA_CL_VTE_CNTL, vte.getRawValue());
    set_reg(REGADDR::DB_DEPTH_CONTROL, (1 << 1) | (1 << 2) | (1 << 4));  // z test + write, LESS
    LATTE_SX_ALPHA_TEST_CONTROL at;
    at.set_ALPHA_FUNC(LATTE_SX_ALPHA_TEST_CONTROL::E_ALPHA_FUNC::LESS).set_ALPHA_TEST_ENABLE(false);
    set_reg(REGADDR::SX_ALPHA_TEST_CONTROL, at.getRawValue());
    set_reg(REGADDR::SX_ALPHA_REF, 0);
    LATTE_PA_SU_SC_MODE_CNTL pm{};
    pm.set_FRONT_FACE(LATTE_PA_SU_SC_MODE_CNTL::E_FRONTFACE::CCW);
    pm.set_FRONT_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES).set_BACK_POLY_MODE(LATTE_PA_SU_SC_MODE_CNTL::E_PTYPE::TRIANGLES);
    set_reg(REGADDR::PA_SU_SC_MODE_CNTL, pm.getRawValue());
    set_reg(REGADDR::VGT_MULTI_PRIM_IB_RESET_INDX, 0xFFFFFFFF);
    set_reg(REGADDR::CB_TARGET_MASK, 0xFFFFFFFF);
    for (int i = 0; i < 4; i++) set_reg(REGADDR::CB_BLEND_RED + i, 0);
    set_reg(REGADDR::PA_SU_POINT_SIZE, LATTE_PA_SU_POINT_SIZE().set_WIDTH(8).set_HEIGHT(8).getRawValue());
    LATTE_CB_COLOR_CONTROL cc;
    cc.set_SPECIAL_OP(LATTE_CB_COLOR_CONTROL::E_SPECIALOP::NORMAL).set_ROP(LATTE_CB_COLOR_CONTROL::E_LOGICOP::COPY);
    set_reg(REGADDR::CB_COLOR_CONTROL, cc.getRawValue());
    LATTE_PA_CL_CLIP_CNTL clip{};
    clip.set_DX_LINEAR_ATTR_CLIP_ENA(true);
    set_reg(REGADDR::PA_CL_CLIP_CNTL, clip.getRawValue());
    set_reg(mmDB_DEPTH_CLEAR, fbits(1.0f));
}

}  // namespace gx2

using namespace gx2;

// ---------------------------------------------------------------- init / timing
// Display timing, modelled on the hardware: vsync ticks at 60 Hz on its own clock, and a requested
// flip executes on the first vsync that is at least `swap interval` vsyncs after the previous flip.
// Games pace themselves by waiting for vsync until their flips have executed.
static uint64_t g_swap_count = 0, g_flip_count = 0;
namespace gx2 { uint64_t flips_presented() { return __atomic_load_n(&g_flip_count, __ATOMIC_RELAXED); } }  // live fps in the title
static uint32 g_swap_interval = 1;  // as set by the game (frame interpolation halves it)
namespace interp { uint32_t effective_swap_interval(uint32_t game); }
static std::mutex g_flip_mutex;
static const auto g_vsync_epoch = std::chrono::steady_clock::now();
static constexpr std::chrono::nanoseconds kVsyncPeriod(16683333);  // 59.94 Hz
// a flip also waits for the GPU to finish that frame, as on hardware: the game reuses a frame's
// buffers once its flip has executed
struct PendingFlip { uint64_t vsync, swap; };
static std::deque<PendingFlip> g_pending_flips;
static uint64_t g_last_flip_vsync = 0;
static uint64_t g_last_flip_time = 0;  // timebase
static int64_t g_count_offset = 0;     // guest-visible swap/flip counts minus ours (set by a loaded save state)

// The clock can run faster (fast forward, mods/turbo.cpp): the game then flips more often and plays
// more frames per second, every one calculated and drawn as usual. vsync n is at
// g_clock_base_time + (n - g_clock_base_index) * period / rate.
static std::mutex g_clock_mutex;
static std::chrono::steady_clock::time_point g_clock_base_time = g_vsync_epoch;
static uint64_t g_clock_base_index = 0;
static double g_clock_rate = 1.0;

static uint64_t vsync_index() {
    std::lock_guard<std::mutex> lk(g_clock_mutex);
    auto dt = std::chrono::steady_clock::now() - g_clock_base_time;
    return g_clock_base_index + (uint64_t)(std::chrono::duration<double>(dt).count() * g_clock_rate / std::chrono::duration<double>(kVsyncPeriod).count());
}
static std::chrono::steady_clock::time_point vsync_time(uint64_t n) {
    std::lock_guard<std::mutex> lk(g_clock_mutex);
    double s = (double)(n - g_clock_base_index) * std::chrono::duration<double>(kVsyncPeriod).count() / g_clock_rate;
    return g_clock_base_time + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(s));
}
namespace gx2 {
void set_clock_rate(double rate) {
    uint64_t now = vsync_index();
    std::lock_guard<std::mutex> lk(g_clock_mutex);
    if (rate == g_clock_rate) return;
    g_clock_base_index = now;
    g_clock_base_time = std::chrono::steady_clock::now();
    g_clock_rate = rate;
}
}  // namespace gx2

static void update_flips() {  // g_flip_mutex held
    uint64_t now = vsync_index();
    while (!g_pending_flips.empty()) {
        uint64_t at = std::max(g_pending_flips.front().vsync + 1, g_last_flip_vsync + interp::effective_swap_interval(g_swap_interval));
        if (at > now || gfx::frames_completed() < g_pending_flips.front().swap) break;
        at = now;
        g_pending_flips.pop_front();
        g_last_flip_vsync = at;
        g_last_flip_time = timebase::now();
        g_flip_count++;
    }
}

HLE(gx2, GX2Init) {
    set_default_state();
    LOG("[gx2] initialized (native GX2 -> %s)", gfx::backend_name());
}

HLE(gx2, GX2SetupContextStateEx) {
    uint32 ctx = arg(c, 0);
    emit_host(OP_SETUP_CONTEXT, {ctx});
    set_default_state();
    // the context's "restore" display list lives inside the (0xA100 byte) context structure
    uint32 dl = ctx + 0x9800;
    uint32* w = (uint32*)mem::ptr(dl);
    w[0] = OP_SET_CONTEXT | (1u << 8);
    w[1] = ctx;
}
HLE(gx2, GX2SetContextState) { emit(OP_SET_CONTEXT, {arg(c, 0)}); }
HLE(gx2, GX2GetContextStateDisplayList) {
    if (arg(c, 1)) st32(arg(c, 1), arg(c, 0) + 0x9800);
    if (arg(c, 2)) st32(arg(c, 2), 8);
}

// ---------------------------------------------------------------- display lists
HLE(gx2, GX2BeginDisplayListEx) {
    t_rec.start = t_rec.pos = arg(c, 0);
    t_rec.end = arg(c, 0) + arg(c, 1);
}
HLE(gx2, GX2EndDisplayList) {
    uint32 size = t_rec.pos - t_rec.start;
    t_rec = Recording{};
    ret(c, size);
}
HLE(gx2, GX2GetCurrentDisplayList) {
    if (arg(c, 0)) st32(arg(c, 0), t_rec.start);
    if (arg(c, 1)) st32(arg(c, 1), t_rec.start ? t_rec.end - t_rec.start : 0);
    ret(c, t_rec.start != 0);
}
HLE(gx2, GX2CallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }
HLE(gx2, GX2DirectCallDisplayList) { emit(OP_CALL, {arg(c, 0), arg(c, 1)}); }

// ---------------------------------------------------------------- draws
HLE(gx2, GX2DrawEx) { emit(OP_DRAW, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3)}); }
HLE(gx2, GX2DrawIndexedEx) { emit(OP_DRAW_INDEXED, {arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3), arg(c, 4), arg(c, 5)}); }

// ---------------------------------------------------------------- clears and copies
static float farg(Cpu* c, int i) { return (float)c->f[1 + i].ps0; }
// Struct parameters are copied into the command (as GX2 encodes them into the command buffer):
// games reuse one GX2ColorBuffer/GX2DepthBuffer and change its view between calls, also while
// recording display lists that run later.
static void put_struct(std::vector<uint32>& p, uint32 addr, uint32 words) {
    size_t at = p.size();
    p.resize(at + words);
    if (addr) memcpy(&p[at], mem::ptr(addr), words * 4);
}
HLE(gx2, GX2ClearColor) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    for (int i = 0; i < 4; i++) p.push_back(fbits(farg(c, i)));
    emit(OP_CLEAR_COLOR, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearDepthStencilEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), arg(c, 1) & 0xFF, arg(c, 2)});
    emit(OP_CLEAR_DEPTH, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ClearBuffersEx) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    put_struct(p, arg(c, 1), kDepthBufferWords);
    p.insert(p.end(), {fbits(farg(c, 0)), fbits(farg(c, 1)), fbits(farg(c, 2)), fbits(farg(c, 3)), fbits(farg(c, 4)),
                       arg(c, 2) & 0xFF, arg(c, 3)});
    emit(OP_CLEAR_BUFFERS, p.data(), (uint32)p.size());
}
HLE(gx2, GX2SetClearDepthStencil) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(arg(c, 0));
    db->clearDepth = farg(c, 0);
    db->clearStencil = arg(c, 1) & 0xFF;
}
HLE(gx2, GX2CopySurface) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kSurfaceWords);
    p.insert(p.end(), {arg(c, 1), arg(c, 2)});
    put_struct(p, arg(c, 3), kSurfaceWords);
    p.insert(p.end(), {arg(c, 4), arg(c, 5)});
    emit(OP_COPY_SURFACE, p.data(), (uint32)p.size());
    // A linear destination is read by the CPU right after the call, without GX2DrawDone: the Picto
    // Box JPEG-encodes its picture from such a copy (issue #22). The render thread writes the copy
    // back to guest memory (vk_surfaces.cpp), so wait for it.
    auto tm = (uint32)((GX2Surface*)mem::ptr(arg(c, 3)))->tileMode.value();
    if (!t_rec.start && (tm == 1 || tm == 0x10)) render_sync();
}
HLE(gx2, GX2CopyColorBufferToScanBuffer) {
    std::vector<uint32> p;
    put_struct(p, arg(c, 0), kColorBufferWords);
    p.push_back(arg(c, 1));
    emit(OP_COPY_TO_SCAN, p.data(), (uint32)p.size());
}
HLE(gx2, GX2ExpandAAColorBuffer) { emit(OP_EXPAND_COLOR, {arg(c, 0)}); }
HLE(gx2, GX2ExpandDepthBuffer) { emit(OP_EXPAND_DEPTH, {arg(c, 0)}); }
HLE(gx2, GX2Invalidate) {
    // the CPU bit flushes the range on the console (mem_writes.h); "everything" carries no information
    if ((arg(c, 0) & 0x40) && arg(c, 2) < 0x10000000) memw::mark(arg(c, 1), arg(c, 2));
    emit(OP_INVALIDATE, {arg(c, 0), arg(c, 1), arg(c, 2)});
}

// ---------------------------------------------------------------- submission and presentation
HLE(gx2, GX2Flush) { emit_host(OP_FLUSH, {}); }
// CPU time a thread of this process (by its name) used since the previous call, in ms (Linux
// /proc/self/task/<tid>/stat: utime + stime in clock ticks); 0 where unreadable
static double thread_cpu_ms(const char* name) {
#ifdef __linux__
    static std::map<std::string, long long> last;
    long long ticks = -1;
    if (DIR* d = opendir("/proc/self/task")) {
        while (dirent* e = readdir(d)) {
            if (e->d_name[0] == '.') continue;
            char path[64], comm[64] = {}, stat[512] = {};
            snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
            if (FILE* f = fopen(path, "r")) { if (!fgets(comm, sizeof comm, f)) comm[0] = 0; fclose(f); }
            comm[strcspn(comm, "\n")] = 0;
            if (strcmp(comm, name) != 0) continue;
            snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
            if (FILE* f = fopen(path, "r")) {
                size_t n = fread(stat, 1, sizeof stat - 1, f);
                stat[n] = 0;
                fclose(f);
                const char* p = strrchr(stat, ')');
                unsigned long long ut = 0, st = 0;
                // after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime
                if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) == 2) ticks = (long long)(ut + st);
            }
            break;
        }
        closedir(d);
    }
    if (ticks < 0) return 0;
    long long& prev = last[name];
    double ms = prev ? (ticks - prev) * 1000.0 / sysconf(_SC_CLK_TCK) : 0;
    prev = ticks;
    return ms;
#else
    (void)name;
    return 0;
#endif
}

// GX2DrawDone calls and the time the game spent in them, for the periodic frame report
static std::atomic<uint64_t> g_drawdone_calls{0}, g_drawdone_us{0};
HLE(gx2, GX2DrawDone) {
    BlockingScope b;
    auto t0 = std::chrono::steady_clock::now();
    emit_host(OP_DRAW_DONE, {});
    render_sync();
    g_drawdone_calls++;
    g_drawdone_us += (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    ret(c, 1);
}
HLE(gx2, GX2SwapScanBuffers) {
    // debug: WWHD_TRACE_SWAP=n logs the guest call chain of the first n swaps
    static int trace = getenv("WWHD_TRACE_SWAP") ? atoi(getenv("WWHD_TRACE_SWAP")) : 0;
    if (trace > 0) {
        trace--;
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[gx2] swap from lr=%08X", c->lr);
        uint32_t sp = c->r[1];
        for (int i = 0; i < 8 && sp; i++) {
            uint32_t prev = ld32(sp);
            if (!prev || prev <= sp) break;
            n += snprintf(buf + n, sizeof buf - n, " <- %08X", ld32(prev + 4));
            sp = prev;
        }
        LOG("%s", buf);
    }
    float a = aspect::on_swap();  // aspect ratio of the next frame (game projections, render targets)
    emit_host(OP_SWAP, {gx2::fbits(a)});
    {
        std::lock_guard<std::mutex> lk(g_flip_mutex);
        update_flips();
        g_swap_count++;
        g_pending_flips.push_back({vsync_index(), g_swap_count});
    }
    if (g_swap_count % 300 == 1) {
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double s = std::chrono::duration<double>(now - last).count();
        last = now;
        double renderMs = thread_cpu_ms("GX2 render");
        LOG("[gx2] frame %llu, %.1f swaps/s, swap interval %u; GX2DrawDone %.1f/frame, %.1f ms/frame; render thread CPU %.1f ms/frame",
            (unsigned long long)g_swap_count, 300 / s, g_swap_interval, g_drawdone_calls.exchange(0) / 300.0,
            g_drawdone_us.exchange(0) / 300.0 / 1000.0, renderMs / 300.0);
        if (getenv("WWHD_SCHED_STATS")) threads::report_sched();
        if (g_gen_stats) {
            std::vector<std::pair<uint64_t, uint32>> top;
            for (auto& [r, c] : g_gen_by_reg) top.push_back({c, r});
            std::sort(top.rbegin(), top.rend());
            std::string t;
            for (size_t k = 0; k < top.size() && k < 8; k++) {
                char b[32];
                snprintf(b, sizeof b, " %04X:%.1f", top[k].second, top[k].first / 300.0);
                t += b;
            }
            LOG("[gx2] per frame: set_context %.1f (loads %.1f), shader-relevant register bumps %.1f; by register%s", g_gen_ctx_calls / 300.0,
                g_gen_ctx / 300.0, g_gen_regs / 300.0, t.c_str());
            g_gen_ctx = g_gen_ctx_calls = g_gen_regs = 0;
            g_gen_by_reg.clear();
        }
    }
}
HLE(gx2, GX2GetSwapStatus) {
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    if (arg(c, 0)) st32(arg(c, 0), (uint32)(g_swap_count + g_count_offset));
    if (arg(c, 1)) st32(arg(c, 1), (uint32)(g_flip_count + g_count_offset));
    if (arg(c, 2)) st64(arg(c, 2), timebase::to_guest(g_last_flip_time));
    if (arg(c, 3)) st64(arg(c, 3), timebase::guest_now());
}
HLE(gx2, GX2SetSwapInterval) { g_swap_interval = std::max<uint32>(arg(c, 0), 1); }
HLE(gx2, GX2WaitForVsync) {
    threads::park_sleep_until(vsync_time(vsync_index() + 1));
    std::lock_guard<std::mutex> lk(g_flip_mutex);
    update_flips();
    static uint64_t calls = 0;
    if (getenv("WWHD_LOG_VSYNC") && ++calls % 60 == 0)
        LOG("[gx2] vsync %llu: swaps %llu flips %llu pending %zu", (unsigned long long)vsync_index(),
            (unsigned long long)g_swap_count, (unsigned long long)g_flip_count, g_pending_flips.size());
}

// scan buffers: the game renders into its own color buffers and copies to "scan buffers";
// the renderer presents whatever was copied to the TV target.
// (buffer, size, mode, surfaceFormat, bufferingMode): an sRGB format means scan-out applies the encoding
HLE(gx2, GX2SetTVBuffer) {
    LOG("[gx2] TV buffer format %X, mode %u (1-2 480p, 3 720p, 5-7 1080p), %u buffers", arg(c, 3), arg(c, 2), arg(c, 4));
    gfx::set_tv_format(arg(c, 3), true);
}
HLE(gx2, GX2SetDRCBuffer) { gfx::set_tv_format(arg(c, 3), false); }
HLE(gx2, GX2SetTVScale) {}
HLE(gx2, GX2SetDRCScale) {}
HLE(gx2, GX2SetTVEnable) {}
HLE(gx2, GX2SetDRCEnable) {}
HLE(gx2, GX2CalcTVSize) {
    // (mode, format, bufferingMode, uint32* size, bool* scaleNeeded)
    uint32 mode = arg(c, 0), buffers = std::max<uint32>(arg(c, 2), 1);
    uint32 w = mode >= 5 ? 1920 : mode <= 2 ? 854 : 1280, h = mode >= 5 ? 1080 : mode <= 2 ? 480 : 720;
    st32(arg(c, 3), w * h * 4 * buffers);
    st32(arg(c, 4), 0);
}
HLE(gx2, GX2CalcDRCSize) {
    st32(arg(c, 3), 854 * 480 * 4 * std::max<uint32>(arg(c, 2), 1));
    st32(arg(c, 4), 0);
}

// ---------------------------------------------------------------- misc queries
HLE(gx2, GX2TempGetGPUVersion) { ret(c, 2); }
HLE(gx2, GX2CalcGeometryShaderInputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcGeometryShaderOutputRingBufferSize) { ret(c, arg(c, 0) * 4 * 0x1000); }
HLE(gx2, GX2CalcFetchShaderSizeEx) {
    uint32 n = arg(c, 0);
    uint32 cf = ((((n + 15) / 16) + 1) * 8 + 0xF) & ~0xFu;
    ret(c, std::max<uint32>(cf + n * 16, 16 + n * 16));
}
HLE(gx2, GX2GPUTimeToCPUTime) { ret64(c, arg64(c, 3)); }
HLE(gx2, GX2SampleTopGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }
HLE(gx2, GX2SampleBottomGPUCycle) { if (arg(c, 0)) st64(arg(c, 0), timebase::guest_now()); }

// ---------------------------------------------------------------- save states
#include "../savestate.h"
namespace gfx { void ss_reset_surfaces(); }

// the game is frozen between frames: finish all queued GPU work and let pending flips execute, so no
// command reads guest memory while it is replaced and the swap/flip counts agree
void gx2_ss_drain() {
    emit_host(OP_DRAW_DONE, {});
    render_sync();
    for (int i = 0; i < 300; i++) {
        {
            std::lock_guard<std::mutex> lk(g_flip_mutex);
            update_flips();
            if (g_pending_flips.empty()) return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    LOG("[savestate] flips still pending after 300 ms");
}

void gx2_ss_save(ss::Writer& w) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    w.u32(kNumRegs);
    w.bytes(g_regs, sizeof g_regs);
    uint32 active = 0;
    std::vector<uint32> keys;
    for (auto& [k, v] : g_contexts) {
        keys.push_back(k);
        if (g_shadow == v.data()) active = k;
    }
    std::sort(keys.begin(), keys.end());
    w.u32((uint32)keys.size());
    for (uint32 k : keys) {
        w.u32(k);
        w.u32((uint32)g_contexts[k].size());
        w.bytes(g_contexts[k].data(), g_contexts[k].size() * 4);
    }
    w.u32(active);
    w.u32(g_swap_interval);
    std::lock_guard<std::mutex> fl(g_flip_mutex);
    w.u64(g_swap_count + g_count_offset);
}

bool gx2_ss_check(ss::Reader r, std::string& why) {
    if (r.u32() != kNumRegs) { why = "GX2 register file size differs"; return false; }
    return r.ok;
}

void gx2_ss_load(ss::Reader& r) {
    std::lock_guard<std::recursive_mutex> lk(g_exec_mutex);
    r.u32();
    r.bytes(g_regs, sizeof g_regs);
    touch_all_regs();  // a loaded state may use any register
    g_contexts.clear();
    uint32 n = r.u32();
    for (uint32 i = 0; i < n && r.ok; i++) {
        uint32 k = r.u32(), words = r.u32();
        auto& v = g_contexts[k];
        v.resize(words);
        r.bytes(v.data(), (size_t)words * 4);
    }
    uint32 active = r.u32();
    auto it = g_contexts.find(active);
    g_shadow = active && it != g_contexts.end() ? it->second.data() : nullptr;
    g_shader_state_gen++;
    g_draw_state_gen++;
    g_swap_interval = std::max<uint32>(r.u32(), 1);
    uint64_t guest_swaps = r.u64();
    {
        std::lock_guard<std::mutex> fl(g_flip_mutex);
        g_count_offset = (int64_t)guest_swaps - (int64_t)g_swap_count;
    }
    gfx::ss_reset_surfaces();
    aspect::ss_reset();  // layouts recompute their matrices for the current aspect
}
