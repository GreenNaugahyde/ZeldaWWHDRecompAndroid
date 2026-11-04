// Jump anywhere (Mods menu): L makes Link jump from where he stands or runs, with the game's own jump
// off a ledge (daPy_lk_c::procAutoJump_init: take-off animation, voice, his run speed carried on,
// landing as usual). Optionally once more in the air (double jump, the same jump again) before
// he lands again.
//
// On the ground he jumps from standing, walking, running, Z-targeting, turning and landing; not in
// events (dialogue, cutscenes), swimming, climbing, hanging, crawling, on ladders or with the moon
// jump cheat on (it uses L too; the app switches one off when the other goes on). The double jump
// starts the same jump again from the jump itself, a fall, a back flip or a side hop.
//
// No automatic ledge jumps (a switch under it): when the game itself starts its jump off a ledge
// (running off an edge; daPy_lk_c's ledge check, the only caller of procAutoJump_init), right after
// that step he's put into the plain fall instead (procFall_init(0): keeps his forward speed; speed.y
// cleared), so he drops off the edge and the player jumps with L.
//
// Grace window: for a moment after he leaves the ground without jumping (runs or walks off an edge,
// also the fall that replaced a ledge jump) L still counts as the jump from the ground, so the double
// jump stays for later. The jump from the ground is a little higher than the game's ledge jump
// (speed.y x1.15, at least 16: the ledge jump from standing gives 12.5, at full run 23.7); the double
// jump keeps the game's. WWHD_JUMP_GRACE_MS, WWHD_JUMP_BOOST, WWHD_JUMP_MIN override them.
//
// procAutoJump_init (GameCube 80115EA4, USA 023F7958) clamps speedF to at least the auto jump's
// minimum (9..17; speed.y comes from it) and sets the air step's timer (mProcVar0) to -1, with which
// procAutoJump speeds him up to a full run along his facing whatever the stick says; the take-off
// animation (root-motion mode 1) adds about 37 units more. Right for the game's jump off a ledge (he is
// running); from standing it carried him about 180 units forward (off a narrow pier on a double jump).
// Ours sets the timer to 0, procAutoJump's stick rule (stick pushed keeps or gains the run speed, back
// brakes, released slows down), and with the stick released at the jump (mStickDistance <= 0.05, the
// rule's own threshold) speedF and mNormalSpeed are 0 and the take-off's root motion is taken back after
// each execute: straight up. (The original project's 65840f6d, which found it.) His collision's
// ground flags are cleared as for the moon jump (cheats.cpp), or the jump would land at once.
// WWHD_JUMP_LOG=1 logs each jump.
//
// Deku Leaf lift (a switch of its own): the game's own wave of the leaf while gliding (the item's button
// again, daPy_lk_c::procFanGlide: a gust, voice and FAN_CHUTE_RISE sound, but only a frame of the normal
// gravity) lifts Link: speed.y at least the lift (WWHD_LEAF_LIFT, default 10) and the glide's gravity kept,
// so he rises about a body height and glides on. The wave is found after daPy_lk_c::execute: still in
// FAN_GLIDE and the gravity pulled down from the glide's (-0.5) in that step, which only the wave does
// (the glide's own step only puts the glide's gravity back). The glide's magic drain stays the limit.
// Mods > its height (percent of the default lift's height, which grows with speed.y squared) scales the lift.
// WWHD_LEAF_LOG=1 logs each lift.
// Held, the leaf's button waves on and on (leaf_pad, from filter_pad): the game waves only on a new press, so
// while Link glides with the lift on and an item button (X / Y / R) is held, that button reads as let go for a
// moment (50 ms) once 300 ms have passed since the last wave and the last let-go (WWHD_LEAF_REPEAT_MS): the
// next read is a new press. During the glide the other item buttons have no use.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>

#include "input.h"
#include "mods.h"
#include "release.h"
#include "runtime.h"

namespace mods {
namespace {
constexpr uint32_t kCurProc = 0x65F0;  // daPy_lk_c::mCurProc
constexpr uint32_t kModeFlg = 0x6A70, kModeMidair = 0x2, kModeSwim = 0x40000;
constexpr uint32_t kAcchFlags = 0x80C + 0x28, kAcchGround = 0x20 | 0x40 | 0x80;  // as in cheats.cpp
constexpr uint32_t kSpeedF = 0x370, kSpeedY = 0x33C + 4, kGravity = 0x374;
const release::Code kAutoJumpInit{0x023F7958};  // daPy_lk_c::procAutoJump_init()
const release::Code kFallInit{0x023F6564};  // daPy_lk_c::procFall_init(int, f32)
constexpr uint32_t kNormalSpeed = 0x6A14, kStickDistance = 0x6A08;  // daPy_lk_c: mNormalSpeed, mStickDistance
constexpr uint32_t kProcTimer = 0x6916;                             // mProcVar0 (s16; GameCube 0x34D0)
constexpr uint32_t kPos = 0x314, kFacing = 0x32A;                   // current.pos (x, +8 z), shape_angle.y
constexpr uint32_t kRoot = 0x7308, kRootMode = 0x68DE;              // m3700 (GameCube 0x3700), m34C2 (u8)
const release::Data kHeldPos{0x1046CD48};  // the HD's retained copy of his position (warp.cpp)
constexpr float kStickDead = 0.05f;        // procAutoJump's own threshold on mStickDistance
constexpr uint32_t kProcAutoJump = 0x24;

std::atomic<bool> g_on{getenv("WWHD_MOD_JUMP") != nullptr};
std::atomic<bool> g_double{false};
std::atomic<bool> g_no_auto{false};
bool g_own = false;      // the jump under way is ours (L), not the game's ledge jump
double g_ground_time = -1;  // game time Link was last on the ground (a ground proc)
constexpr uint32_t kProcFall = 0x27;
float env_float(const char* name, float def) {
    const char* e = getenv(name);
    return e ? (float)atof(e) : def;
}
bool g_l_was = false;
bool g_doubled = false;  // the double jump was used in this airtime
bool g_straight = false;  // our jump goes straight up: the take-off's root motion is taken back
float g_root_x = 0, g_root_z = 0;  // m3700 before Link's execute

constexpr uint32_t kProcFanGlide = 0x93;
std::atomic<bool> g_leaf_lift{false};
std::atomic<int> g_leaf_height{100};  // percent
float g_glide_gravity = 0;  // the gravity after the last step, while gliding (0: not gliding)
std::atomic<int> g_leaf_waves{0};  // waves seen (leaf_pad repeats a held button after one)

// daPyProc_*: where a jump may start on the ground ...
bool ground_proc(uint32_t p) {
    switch (p) {
        case 0x04: case 0x05:              // WAIT, FREE_WAIT
        case 0x06: case 0x07:              // MOVE, ATN_MOVE
        case 0x08: case 0x09:              // ATN_ACTOR_WAIT, ATN_ACTOR_MOVE
        case 0x0B: case 0x23: case 0x25:   // SIDE_STEP_LAND, BACK_JUMP_LAND, LAND
        case 0x17: case 0x18:              // WAIT_TURN, MOVE_TURN
            return true;
        default: return false;
    }
}
// ... and the double jump in the air
bool air_proc(uint32_t p) {
    switch (p) {
        case 0x24: case 0x27:              // AUTO_JUMP, FALL
        case 0x22: case 0x0A:              // BACK_JUMP, SIDE_STEP
        case 0x29: case 0x2A:              // SMALL_JUMP, VERTICAL_JUMP
            return true;
        default: return false;
    }
}

void jump(Cpu* c, uint32_t link, bool from_ground, const char* what) {
    const Cpu saved = *c;
    guest_call(c, kAutoJumpInit, {link});
    g_own = true;
    *c = saved;
    if (from_ground) {  // a little higher than the game's ledge jump
        static const float boost = env_float("WWHD_JUMP_BOOST", 1.15f), lo = env_float("WWHD_JUMP_MIN", 16.0f);
        stf32(link + kSpeedY, std::max(u32_as_f32(ld32(link + kSpeedY)) * boost, lo));
    }
    st16(link + kProcTimer, 0);  // the air step follows the stick
    g_straight = u32_as_f32(ld32(link + kStickDistance)) <= kStickDead;
    if (g_straight) {
        stf32(link + kSpeedF, 0.0f);
        stf32(link + kNormalSpeed, 0.0f);
        g_root_x = u32_as_f32(ld32(link + kRoot)), g_root_z = u32_as_f32(ld32(link + kRoot + 8));  // (the init zeroed it)
    }
    st32(link + kAcchFlags, ld32(link + kAcchFlags) & ~kAcchGround);  // off the ground: no landing at once
    static const bool log = getenv("WWHD_JUMP_LOG") != nullptr;
    if (log)
        LOG("[jump] %s: speedF %.1f, speed.y %.1f, stick %.2f%s", what, u32_as_f32(ld32(link + kSpeedF)),
            u32_as_f32(ld32(link + kSpeedY)), u32_as_f32(ld32(link + kStickDistance)), g_straight ? " (straight up)" : "");
}
}  // namespace

bool jump_anywhere() { return g_on.load(std::memory_order_relaxed); }
void set_jump_anywhere(bool on) {
    g_on = on;
    LOG("[mods] jump anywhere %s", on ? "on" : "off");
}
bool double_jump() { return g_double.load(std::memory_order_relaxed); }
void set_double_jump(bool on) {
    g_double = on;
    LOG("[mods] double jump %s", on ? "on" : "off");
}

bool no_auto_jump() { return g_no_auto.load(std::memory_order_relaxed); }
void set_no_auto_jump(bool on) {
    g_no_auto = on;
    LOG("[mods] automatic ledge jumps %s", on ? "off" : "on");
}

bool leaf_lift() { return g_leaf_lift.load(std::memory_order_relaxed); }
void set_leaf_lift(bool on) {
    g_leaf_lift = on;
    LOG("[mods] deku leaf lift %s", on ? "on" : "off");
}

int leaf_lift_height() { return g_leaf_height.load(std::memory_order_relaxed); }
void set_leaf_lift_height(int percent) {
    g_leaf_height = std::clamp(percent, 10, 1000);
    LOG("[mods] deku leaf lift height %d%%", g_leaf_height.load());
}

namespace {
void leaf_after_step(uint32_t link) {
    static const bool trace = getenv("WWHD_LEAF_TRACE") != nullptr;  // each glide step whose gravity changed
    if (trace) {
        static uint32_t last_proc = ~0u;
        static float last_g = 99;
        const uint32_t p = ld32(link + kCurProc);
        const float g = u32_as_f32(ld32(link + kGravity));
        if (p != last_proc && (p == kProcFanGlide || last_proc == kProcFanGlide))
            LOG("[leaf] proc 0x%X -> 0x%X, gravity %.2f, speed.y %.2f, lift %s", last_proc, p, g,
                u32_as_f32(ld32(link + kSpeedY)), g_leaf_lift.load() ? "on" : "off");
        else if (p == kProcFanGlide && g != last_g)
            LOG("[leaf] glide: gravity %.2f -> %.2f, speed.y %.2f", last_g, g, u32_as_f32(ld32(link + kSpeedY)));
        last_proc = p;
        last_g = g;
    }
    if (ld32(link + kCurProc) != kProcFanGlide || !g_leaf_lift.load(std::memory_order_relaxed)) {
        g_glide_gravity = 0;
        return;
    }
    const float g = u32_as_f32(ld32(link + kGravity)), was = g_glide_gravity;
    g_glide_gravity = g;
    if (was >= 0 || g >= was) return;  // no wave in this step
    static const float base = env_float("WWHD_LEAF_LIFT", 10.0f);
    const float lift = base * std::sqrt(g_leaf_height.load(std::memory_order_relaxed) / 100.0f);
    const float vy = u32_as_f32(ld32(link + kSpeedY));
    stf32(link + kSpeedY, std::max(vy, lift));
    stf32(link + kGravity, was);  // the glide's gravity, not the wave's frame of the normal one
    g_glide_gravity = was;
    g_leaf_waves.fetch_add(1, std::memory_order_relaxed);
    static const bool log = getenv("WWHD_LEAF_LOG") != nullptr;
    if (log) LOG("[leaf] wave: speed.y %.1f -> %.1f, gravity %.2f (wave %.2f)", vy, std::max(vy, lift), was, g);
}
}  // namespace

void leaf_pad(input::PadState& s) {
    using clk = std::chrono::steady_clock;
    static clk::time_point last_wave, last_drop, drop_until;
    static int waves = -1;
    const uint32_t items = input::kX | input::kY | input::kR, link = link_actor();
    const clk::time_point now = clk::now();
    const int w = g_leaf_waves.load(std::memory_order_relaxed);
    if (w != waves) waves = w, last_wave = now, drop_until = now;  // (a wave ends a let-go under way)
    if (!(s.buttons & items) || !link || !g_leaf_lift.load(std::memory_order_relaxed) ||
        ld32(link + kCurProc) != kProcFanGlide) {
        last_drop = now;  // a fresh hold waits as long as after a wave: its own press waves first
        drop_until = now;
        return;
    }
    static const auto every = std::chrono::milliseconds((int)env_float("WWHD_LEAF_REPEAT_MS", 300.0f));
    if (now < drop_until) {
        s.buttons &= ~items;  // the let-go under way
    } else if (now - last_wave >= every && now - last_drop >= every) {
        last_drop = now;
        drop_until = now + std::chrono::milliseconds(50);
        s.buttons &= ~items;
    }
}

void jump_after_step(Cpu* c, uint32_t link) {
    if (!link) return;
    leaf_after_step(link);
    if (ld32(link + kCurProc) != kProcAutoJump) {
        g_own = false;
        g_straight = false;
        return;
    }
    // a straight-up jump takes back what the take-off animation moved him in this execute: posMove adds
    // the root translation's change (m3700, model space) turned by his facing
    if (g_straight && ld8(link + kRootMode) == 1) {
        const float lx = u32_as_f32(ld32(link + kRoot)) - g_root_x, lz = u32_as_f32(ld32(link + kRoot + 8)) - g_root_z;
        const float a = (float)(int16_t)ld16(link + kFacing) * 3.14159265f / 32768.0f, sn = std::sin(a), cs = std::cos(a);
        const float dx = lz * sn + lx * cs, dz = lz * cs - lx * sn;
        for (uint32_t x : {link + kPos, (uint32_t)kHeldPos}) {  // (and the HD's copy the next execute reads)
            stf32(x, u32_as_f32(ld32(x)) - dx);
            stf32(x + 8, u32_as_f32(ld32(x + 8)) - dz);
        }
    }
    if (g_own || !g_on.load(std::memory_order_relaxed) || !g_no_auto.load(std::memory_order_relaxed)) return;
    // the game's own ledge jump began in this step: a plain fall instead
    stf32(link + kSpeedY, 0.0f);
    const Cpu saved = *c;
    c->f[1].ps0 = c->f[1].ps1 = 5.0;  // animation blend
    guest_call(c, kFallInit, {link, 0});
    *c = saved;
    static const bool log = getenv("WWHD_JUMP_LOG") != nullptr;
    if (log) LOG("[jump] the game's ledge jump made a fall (speedF %.1f)", u32_as_f32(ld32(link + kSpeedF)));
}

void jump_step(Cpu* c, uint32_t link) {
    const bool l = jump_held(), press = l && !g_l_was;
    g_l_was = l;
    if (link && g_straight) g_root_x = u32_as_f32(ld32(link + kRoot)), g_root_z = u32_as_f32(ld32(link + kRoot + 8));
    if (!link || !g_on.load(std::memory_order_relaxed) || (infinite(kMoonJump) && moon_held())) return;
    const uint32_t proc = ld32(link + kCurProc), mode = ld32(link + kModeFlg);
    if (ground_proc(proc)) {
        g_doubled = false;
        g_ground_time = game_time();
    }
    if (!press || event_mode_now() != 0 || (mode & kModeSwim)) return;
    static const double grace = env_float("WWHD_JUMP_GRACE_MS", 200.0f) / 1000.0;
    const bool just_left = proc == kProcFall && !g_own && !g_doubled && g_ground_time >= 0 && game_time() - g_ground_time <= grace;
    if (ground_proc(proc) || just_left) {
        jump(c, link, true, just_left ? "grace jump" : "jump");
        g_ground_time = -1;  // one grace jump per edge
    } else if (air_proc(proc) && g_double.load(std::memory_order_relaxed) && !g_doubled) {
        g_doubled = true;
        jump(c, link, false, "double jump");
    }
}

}  // namespace mods
