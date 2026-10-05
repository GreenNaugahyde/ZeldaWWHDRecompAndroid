// Gameplay mods: switches and shared helpers. See mods.h.
//
// Start-up / test switches (all off by default, also switchable from the Gameplay menu):
//   WWHD_MOD_DIRECT_CAMERA=1   direct right-stick camera (WWHD_MOD_CAMERA_SPEED=1.5 multiplier)
//   WWHD_MOD_MOUSE_CAMERA=1    mouse camera (WWHD_MOD_MOUSE_SENS=0.15 degrees per point)
//   WWHD_MOD_FIRST_PERSON=1    first person on R3 / mouse wheel
//   WWHD_MOD_QUICK_DOORS=1     quick doors
//   WWHD_MOD_FAST_SCENES=1     fast scene changes
//   WWHD_MOD_RUN_SPEED=1.5     faster running (distance per step while Link runs)
//   WWHD_MOD_SWIM_SPEED=1.5    faster swimming (the same while he swims)
//   WWHD_MOD_RUN_MODE=0|1|2    running applies always, while L3 is held, or L3 switches it on and off
//   WWHD_MOD_SWIM_MODE=0|1|2   the same for swimming
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
std::atomic<float> g_run{env_f("WWHD_MOD_RUN_SPEED", 1.0f)};
std::atomic<float> g_swim{env_f("WWHD_MOD_SWIM_SPEED", 1.0f)};
// running [0] and swimming [1]: when they apply (0 always, 1 while L3 is held, 2 L3 switches), and
// in the L3 modes whether they are on
std::atomic<int> g_mode[2] = {{(int)env_f("WWHD_MOD_RUN_MODE", 0)}, {(int)env_f("WWHD_MOD_SWIM_MODE", 0)}};
std::atomic<bool> g_on[2] = {{false}, {false}};
std::atomic<bool> g_swimming{false};  // Link's last procedure was a swimming one
bool g_l3_was = false;

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
bool fast_scenes() { return g_scenes.load(std::memory_order_relaxed); }
void set_fast_scenes(bool on) { g_scenes = on; note("fast scene changes", on); }
float run_speed() { return g_run.load(std::memory_order_relaxed); }
void set_run_speed(float s) {
    g_run = s;
    LOG("[mods] run speed x%.2f", s);
}

// Faster running and swimming: while Link is in his run procedure (daPy_PROC_MOVE, walking and
// running with the stick, not Z-targeted) or swims (daPy_PROC_SWIM_MOVE), the horizontal part of
// `current.pos += speed` in posMoveFromFootPos is multiplied. His speed values stay as the game computes them (its acceleration and the run
// animation's blend keep working on the original numbers); collision is resolved after the move,
// so walls and ledges still stop him. Jumps, rolls, climbing and the boat are unchanged.
float swim_speed() { return g_swim.load(std::memory_order_relaxed); }
void set_swim_speed(float s) {
    g_swim = s;
    LOG("[mods] swim speed x%.2f", s);
}

int run_mode() { return g_mode[0].load(std::memory_order_relaxed); }
int swim_mode() { return g_mode[1].load(std::memory_order_relaxed); }
static void set_mode(int which, int m) {
    g_mode[which] = m;
    g_on[which] = false;
    LOG("[mods] %s speed applies %s", which ? "swim" : "run",
        m == 0 ? "always" : m == 1 ? "while L3 is held" : "after an L3 press (until the next)");
}
void set_run_mode(int m) { set_mode(0, m); }
void set_swim_mode(int m) { set_mode(1, m); }

// L3 (left stick click) as the button of faster running and swimming, each with its own mode: held,
// or each press switches it on and off. A press switches the one for what Link is doing: swimming
// in the water, else running. The game itself sees L3 as usual.
void run_input(uint32_t buttons) {
    bool l3 = (buttons & input::kStickL) != 0;
    for (int w = 0; w < 2; w++)
        if (g_mode[w].load(std::memory_order_relaxed) == 1) g_on[w] = l3;
    int w = g_swimming.load(std::memory_order_relaxed) ? 1 : 0;
    if (l3 && !g_l3_was && g_mode[w].load(std::memory_order_relaxed) == 2) {
        g_on[w] = !g_on[w];
        LOG("[mods] fast %s %s", w ? "swimming" : "running", g_on[w] ? "on" : "off");
    }
    g_l3_was = l3;
}

float link_move_factor(uint32_t link) {
    constexpr uint32_t kCurProc = 0x65F0;  // daPy_lk_c::mCurProc (GameCube 0x31D8)
    constexpr uint32_t kProcMove = 0x06;   // daPy_PROC_MOVE
    constexpr uint32_t kProcSwimMove = 0x37;  // daPy_PROC_SWIM_MOVE
    uint32_t proc = ld32(link + kCurProc);
    const bool swimming = proc >= 0x35 && proc <= 0x37;  // SWIM_UP, SWIM_WAIT, SWIM_MOVE
    g_swimming.store(swimming, std::memory_order_relaxed);
    float k = proc == kProcMove ? run_speed() : proc == kProcSwimMove ? swim_speed() : 1.0f;
    static const bool log = getenv("WWHD_RUN_TRACE") != nullptr;  // Link's procedure and position
    if (log) {
        constexpr uint32_t kPos = 0x314;  // fopAc_ac_c::current.pos
        LOG("[mods] proc %02X x%.2f pos %.1f %.1f %.1f", proc, k, ldf32(link + kPos), ldf32(link + kPos + 4), ldf32(link + kPos + 8));
    }
    const int w = swimming ? 1 : 0;
    if (k != 1.0f && g_mode[w].load(std::memory_order_relaxed) != 0 && !g_on[w].load(std::memory_order_relaxed)) return 1.0f;
    return k;
}

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
