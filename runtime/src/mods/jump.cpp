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
// minimum, so from standing he hops forward a little; speed.y comes from speedF. His collision's
// ground flags are cleared as for the moon jump (cheats.cpp), or the jump would land at once.
// WWHD_JUMP_LOG=1 logs each jump.
#include <algorithm>
#include <atomic>
#include <cstdlib>

#include "mods.h"
#include "release.h"
#include "runtime.h"

namespace mods {
namespace {
constexpr uint32_t kCurProc = 0x65F0;  // daPy_lk_c::mCurProc
constexpr uint32_t kModeFlg = 0x6A70, kModeMidair = 0x2, kModeSwim = 0x40000;
constexpr uint32_t kAcchFlags = 0x80C + 0x28, kAcchGround = 0x20 | 0x40 | 0x80;  // as in cheats.cpp
constexpr uint32_t kSpeedF = 0x370, kSpeedY = 0x33C + 4;
const release::Code kAutoJumpInit{0x023F7958};  // daPy_lk_c::procAutoJump_init()
const release::Code kFallInit{0x023F6564};  // daPy_lk_c::procFall_init(int, f32)
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
    st32(link + kAcchFlags, ld32(link + kAcchFlags) & ~kAcchGround);  // off the ground: no landing at once
    static const bool log = getenv("WWHD_JUMP_LOG") != nullptr;
    if (log)
        LOG("[jump] %s: speedF %.1f, speed.y %.1f", what, u32_as_f32(ld32(link + kSpeedF)),
            u32_as_f32(ld32(link + kSpeedY)));
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

void jump_after_step(Cpu* c, uint32_t link) {
    if (!link) return;
    if (ld32(link + kCurProc) != kProcAutoJump) {
        g_own = false;
        return;
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
    const bool l = l_held(), press = l && !g_l_was;
    g_l_was = l;
    if (!link || !g_on.load(std::memory_order_relaxed) || infinite(kMoonJump)) return;
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
