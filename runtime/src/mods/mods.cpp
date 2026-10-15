// Gameplay mods: switches and shared helpers. See mods.h.
//
// Start-up / test switches (all off by default, also switchable from the Gameplay menu):
//   WWHD_MOD_DIRECT_CAMERA=1   direct right-stick camera (WWHD_MOD_CAMERA_SPEED=1.5 multiplier)
//   WWHD_MOD_MOUSE_CAMERA=1    mouse camera (WWHD_MOD_MOUSE_SENS=0.15 degrees per point)
//   WWHD_MOD_FIRST_PERSON=1    first person on R3 / mouse wheel
//   WWHD_MOD_QUICK_DOORS=1     quick doors
//   WWHD_MOD_FAST_SCENES=1     fast scene changes
//   WWHD_MOD_FF_CUTSCENES=1    fast forward of cutscenes while ZR is held (WWHD_MOD_FF_RATE=4: display clock rate)
//   WWHD_MOD_FF_DIALOGUES=1    the same for dialogues
//   WWHD_MOD_RUN_SPEED=1.5     faster running (distance per step while Link runs)
//   WWHD_MOD_SWIM_SPEED=1.5    faster swimming (the same while he swims)
//   WWHD_MOD_CLIMB_SPEED=1.5   faster climbing (ladders, ivy, moving along a ledge: animation speed)
//   WWHD_MOD_SIDLE_SPEED=1.5   faster sidling along walls (distance per step, as running)
//   WWHD_MOD_RUN_MODE=0|1|2    running applies always, while L3 is held, or L3 switches it on and off
//   WWHD_MOD_SWIM_MODE=0|1|2   the same for swimming (WWHD_MOD_CLIMB_MODE, WWHD_MOD_SIDLE_MODE: climbing,
//                              sidling)
//                              (WWHD_RUN_TRACE=1 logs Link's procedure and position on each step)
//   WWHD_MODS_TRACE=path       log of mod decisions and timing events (door events, scene changes,
//                              Link's control), one line per event with the logic step
#include "mods.h"

#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <mutex>

#include "input.h"
#include "runtime.h"

namespace interp { uint64_t logic_steps(); }

namespace mods {
namespace {
bool env_on(const char* n) {
    const char* e = getenv(n);
    return e && atoi(e) != 0;
}
float env_f(const char* n, float d) {
    const char* e = getenv(n);
    return e ? (float)atof(e) : d;
}
std::atomic<bool> g_direct{env_on("WWHD_MOD_DIRECT_CAMERA")};
std::atomic<float> g_speed{env_f("WWHD_MOD_CAMERA_SPEED", 1.0f)};
std::atomic<bool> g_mouse{env_on("WWHD_MOD_MOUSE_CAMERA")};
std::atomic<float> g_sens{env_f("WWHD_MOD_MOUSE_SENS", 0.15f)};
std::atomic<bool> g_fp{env_on("WWHD_MOD_FIRST_PERSON")};
std::atomic<bool> g_doors{env_on("WWHD_MOD_QUICK_DOORS")};
std::atomic<bool> g_scenes{env_on("WWHD_MOD_FAST_SCENES")};
std::atomic<bool> g_ff_cut{env_on("WWHD_MOD_FF_CUTSCENES")};
std::atomic<bool> g_ff_talk{env_on("WWHD_MOD_FF_DIALOGUES")};
std::atomic<bool> g_zr{false};
std::atomic<float> g_run{env_f("WWHD_MOD_RUN_SPEED", 1.0f)};
std::atomic<float> g_swim{env_f("WWHD_MOD_SWIM_SPEED", 1.0f)};
std::atomic<float> g_climb{env_f("WWHD_MOD_CLIMB_SPEED", 1.0f)};
std::atomic<float> g_sidle{env_f("WWHD_MOD_SIDLE_SPEED", 1.0f)};
// running [0], swimming [1], climbing [2] and sidling [3]: when they apply (0 always, 1 while L3 is
// held, 2 L3 switches), and in the L3 modes whether they are on
enum { kRun, kSwim, kClimb, kSidle, kMoves };
std::atomic<int> g_mode[kMoves] = {{(int)env_f("WWHD_MOD_RUN_MODE", 0)}, {(int)env_f("WWHD_MOD_SWIM_MODE", 0)},
                                   {(int)env_f("WWHD_MOD_CLIMB_MODE", 0)}, {(int)env_f("WWHD_MOD_SIDLE_MODE", 0)}};
std::atomic<bool> g_on[kMoves] = {{false}, {false}, {false}, {false}};
std::atomic<int> g_doing{kRun};  // which of them Link's last procedure belongs to
const char* const kMoveName[kMoves] = {"run", "swim", "climb", "sidle"};
bool g_l3_was = false;
std::atomic<bool> g_l_held{false};  // the moon jump cheat's button (cheats.cpp)

void note(const char* what, bool on) { LOG("[mods] %s %s", what, on ? "on" : "off"); }
}  // namespace

bool direct_camera() { return g_direct.load(std::memory_order_relaxed); }
void set_direct_camera(bool on) { g_direct = on; note("direct right-stick camera", on); }
float camera_speed() { return g_speed.load(std::memory_order_relaxed); }
void set_camera_speed(float s) {
    g_speed = s;
    LOG("[mods] camera speed x%.2f", s);
}
bool mouse_camera() { return g_mouse.load(std::memory_order_relaxed); }
void set_mouse_camera(bool on) {
    g_mouse = on;
    note("mouse camera", on);
    if (!on) mouse_release();
}
float mouse_sensitivity() { return g_sens.load(std::memory_order_relaxed); }
void set_mouse_sensitivity(float s) {
    g_sens = s;
    LOG("[mods] mouse sensitivity %.3f degrees per point", s);
}
bool first_person_wheel() { return g_fp.load(std::memory_order_relaxed); }
void set_first_person_wheel(bool on) { g_fp = on; note("first person on R3 / mouse wheel", on); }
bool quick_doors() { return g_doors.load(std::memory_order_relaxed); }
void set_quick_doors(bool on) { g_doors = on; note("quick doors", on); }
bool ff_cutscenes() { return g_ff_cut.load(std::memory_order_relaxed); }
void set_ff_cutscenes(bool on) { g_ff_cut = on; note("fast forward of cutscenes (ZR)", on); }
bool ff_dialogues() { return g_ff_talk.load(std::memory_order_relaxed); }
void set_ff_dialogues(bool on) { g_ff_talk = on; note("fast forward of dialogues (ZR)", on); }
bool ff_button() { return g_zr.load(std::memory_order_relaxed); }
bool fast_scenes() { return g_scenes.load(std::memory_order_relaxed); }
void set_fast_scenes(bool on) { g_scenes = on; note("fast scene changes", on); }
float run_speed() { return g_run.load(std::memory_order_relaxed); }
void set_run_speed(float s) {
    g_run = s;
    LOG("[mods] run speed x%.2f", s);
}

// Faster running, swimming and sidling: while Link is in his run procedure (daPy_PROC_MOVE, walking and
// running with the stick, not Z-targeted), swims (daPy_PROC_SWIM_MOVE) or sidles along a wall
// (daPy_PROC_WHIDE_MOVE), the horizontal part of
// `current.pos += speed` in posMoveFromFootPos is multiplied. His speed values stay as the game computes them (its acceleration and the run
// animation's blend keep working on the original numbers); collision is resolved after the move,
// so walls and ledges still stop him (sidling: the game's wall checks run each step and end the sidle
// at the wall's end as usual). Jumps, rolls and the boat are unchanged.
// Faster climbing: ladders, ivy and moving along a ledge while hanging are driven by their animation
// (the game moves Link by the hands' and feet's travel), so the animation speed the game asks for each
// step is multiplied instead (hooks below).
float swim_speed() { return g_swim.load(std::memory_order_relaxed); }
void set_swim_speed(float s) {
    g_swim = s;
    LOG("[mods] swim speed x%.2f", s);
}
float climb_speed() { return g_climb.load(std::memory_order_relaxed); }
void set_climb_speed(float s) {
    g_climb = s;
    LOG("[mods] climb speed x%.2f", s);
}
float sidle_speed() { return g_sidle.load(std::memory_order_relaxed); }
void set_sidle_speed(float s) {
    g_sidle = s;
    LOG("[mods] sidle speed x%.2f", s);
}

int run_mode() { return g_mode[kRun].load(std::memory_order_relaxed); }
int swim_mode() { return g_mode[kSwim].load(std::memory_order_relaxed); }
int climb_mode() { return g_mode[kClimb].load(std::memory_order_relaxed); }
int sidle_mode() { return g_mode[kSidle].load(std::memory_order_relaxed); }
static void set_mode(int which, int m) {
    g_mode[which] = m;
    g_on[which] = false;
    LOG("[mods] %s speed applies %s", kMoveName[which],
        m == 0 ? "always" : m == 1 ? "while L3 is held" : "after an L3 press (until the next)");
}
void set_run_mode(int m) { set_mode(kRun, m); }
void set_swim_mode(int m) { set_mode(kSwim, m); }
void set_climb_mode(int m) { set_mode(kClimb, m); }
void set_sidle_mode(int m) { set_mode(kSidle, m); }

// the speed factor k of one of them, or 1 while its L3 mode has it off
static float move_factor(int which, float k) {
    if (k != 1.0f && g_mode[which].load(std::memory_order_relaxed) != 0 && !g_on[which].load(std::memory_order_relaxed)) return 1.0f;
    return k;
}

// L3 (left stick click) as the button of faster running, swimming, climbing and sidling, each with its
// own mode: held, or each press switches it on and off. A press switches the one for what Link is
// doing (swimming, climbing, sidling, else running). The game itself sees L3 as usual.
bool l_held() { return g_l_held.load(std::memory_order_relaxed); }
std::atomic<float> g_lx{0}, g_ly{0};
void stick_input(float x, float y) {
    g_lx.store(x, std::memory_order_relaxed);
    g_ly.store(y, std::memory_order_relaxed);
}
void left_stick(float& x, float& y) {
    x = g_lx.load(std::memory_order_relaxed);
    y = g_ly.load(std::memory_order_relaxed);
}

void run_input(uint32_t buttons) {
    g_zr.store((buttons & input::kZR) != 0, std::memory_order_relaxed);  // fast forward (turbo.cpp)
    bool l3 = (buttons & input::kStickL) != 0;
    g_l_held.store((buttons & input::kL) != 0, std::memory_order_relaxed);
    for (int w = 0; w < kMoves; w++)
        if (g_mode[w].load(std::memory_order_relaxed) == 1) g_on[w] = l3;
    int w = g_doing.load(std::memory_order_relaxed);
    if (l3 && !g_l3_was && g_mode[w].load(std::memory_order_relaxed) == 2) {
        g_on[w] = !g_on[w];
        LOG("[mods] fast %s %s", kMoveName[w], g_on[w] ? "on" : "off");
    }
    g_l3_was = l3;
}

float link_move_factor(uint32_t link) {
    constexpr uint32_t kCurProc = 0x65F0;  // daPy_lk_c::mCurProc (GameCube 0x31D8)
    constexpr uint32_t kProcMove = 0x06;   // daPy_PROC_MOVE
    constexpr uint32_t kProcSwimMove = 0x37;  // daPy_PROC_SWIM_MOVE
    constexpr uint32_t kProcWHideMove = 0x15;  // daPy_PROC_WHIDE_MOVE (sidling)
    uint32_t proc = ld32(link + kCurProc);
    // what L3 switches: SWIM_UP..SWIM_MOVE; HANG_START..HANG_WALL_CATCH, LADDER_UP_START..CLIMB_MOVE_SIDE;
    // WHIDE_READY..WHIDE_PEEP
    const int doing = proc >= 0x35 && proc <= 0x37                                      ? kSwim
                      : (proc >= 0x2B && proc <= 0x31) || (proc >= 0x38 && proc <= 0x40) ? kClimb
                      : proc >= 0x13 && proc <= 0x16                                      ? kSidle
                                                                                          : kRun;
    g_doing.store(doing, std::memory_order_relaxed);
    const int w = proc == kProcMove ? kRun : proc == kProcSwimMove ? kSwim : proc == kProcWHideMove ? kSidle : -1;
    float k = w == kRun ? run_speed() : w == kSwim ? swim_speed() : w == kSidle ? sidle_speed() : 1.0f;
    static const bool log = getenv("WWHD_RUN_TRACE") != nullptr;  // Link's procedure and position
    if (log) {
        constexpr uint32_t kPos = 0x314;  // fopAc_ac_c::current.pos
        LOG("[mods] proc %02X x%.2f pos %.1f %.1f %.1f", proc, k, ldf32(link + kPos), ldf32(link + kPos + 4), ldf32(link + kPos + 8));
    }
    return w < 0 ? 1.0f : move_factor(w, k);
}

}  // namespace mods

// ---- faster climbing: the climbing animations' speed (getAnmSpeedStickRate: slow .. fast by the stick),
// asked for each step by procLadderMove / procClimbMoveUpDown (ladder speed), procClimbMoveSide and
// procHangMove (and their _init). GameCube d_a_player_ladder.inc, _climb.inc, _hang.inc. ----
extern "C" {
void f_02417FF4_orig(Cpu* c);  // daPy_lk_c::getLadderMoveAnmSpeed (ladders, ivy up / down)
void f_024291E0_orig(Cpu* c);  // daPy_lk_c::getClimbMoveAnmSpeed (ivy sideways)
void f_02427D8C_orig(Cpu* c);  // daPy_lk_c::getHangMoveAnmSpeed (along a ledge while hanging)
}
static void climb_anim_speed(Cpu* c) {
    const float k = mods::move_factor(mods::kClimb, mods::climb_speed());
    if (k == 1.0f) return;
    const double v = (float)(c->f[1].ps0 * k);
    c->f[1].ps0 = c->f[1].ps1 = v;
}
extern "C" void hook_02417FF4(Cpu* c) { f_02417FF4_orig(c); climb_anim_speed(c); }
extern "C" void hook_024291E0(Cpu* c) { f_024291E0_orig(c); climb_anim_speed(c); }
extern "C" void hook_02427D8C(Cpu* c) { f_02427D8C_orig(c); climb_anim_speed(c); }

namespace mods {

uint64_t step() { return interp::logic_steps(); }
double game_time() { return (double)interp::logic_steps() / 30.0; }

static FILE* g_trace = [] {
    const char* p = getenv("WWHD_MODS_TRACE");
    return p ? fopen(p, "w") : nullptr;
}();
bool trace_on() { return g_trace != nullptr; }
void trace(const char* fmt, ...) {
    if (!g_trace) return;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    fprintf(g_trace, "%llu ", (unsigned long long)step());
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_trace, fmt, ap);
    va_end(ap);
    fputc('\n', g_trace);
    fflush(g_trace);
}

}  // namespace mods
