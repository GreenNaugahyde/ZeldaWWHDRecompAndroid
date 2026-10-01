// Quick doors and fast scene changes: while a door event or a scene change is in progress, the game
// runs extra logic steps per frame (game time passes faster), so nothing is skipped: every
// animation, event cut, flag and sound of the door or the transition happens exactly as before,
// only in fewer frames.
//
// Quick doors: door actors ask the event manager for their current cut through
// dDoor_info_c::getDemoAction (0252A684; knob doors, shutter doors 10/12, kddoor) or
// daMbdoor_c::getDemoAction (021C0078) while their door event runs. While a door was in such a cut
// during the step just run (not a TALK cut: locked-door messages run at normal speed), the frame gets
// kDoorExtra more full logic steps: the steps of fpcM_Management between two executes (process
// deletion, priority, creation, cCt_Counter) and fpcEx_Handler (every process: Link, door, camera,
// event manager in the play scene). Drawing, scene management (fapGm_After) and the pad read stay
// once per frame. Button presses ("trigger" bits) are cleared for the extra steps, so a press is
// seen once.
//
// Fast scene changes: while an overlap (fade/wipe) process exists (l_fopOvlpM_overlap[0],
// 101F36CC), the frame gets kSceneExtra more runs of the transition machinery only: process
// creation (fpcCt_Handler: the new scene's loading phases), the overlap process's execute (its fade
// timers) and fapGm_After (scene and overlap request phases). The scenes themselves (actors,
// events, cutscenes) keep running at normal speed, so no story event is shortened.
//
// Both act on full logic passes only (not on the 60 fps in-between passes) and in all 60 fps modes.
#include <atomic>
#include <cstdlib>

#include "mods.h"
#include "runtime.h"

extern "C" {
void f_025DE788_orig(Cpu* c);  // fpcEx_Handler
void f_025DE024_orig(Cpu* c);  // fpcDt_Handler
void f_025E0EE4_orig(Cpu* c);  // fpcPi_Handler
void f_025DDCEC_orig(Cpu* c);  // fpcCt_Handler
void f_025D42C4_orig(Cpu* c);  // fapGm_After
void f_0200E6EC_orig(Cpu* c);  // cCt_Counter
void f_025DF940(Cpu* c);       // fpcM_Execute (true60's per-process gate runs too)
void f_0252A684_orig(Cpu* c);  // dDoor_info_c::getDemoAction
void f_021C0078_orig(Cpu* c);  // daMbdoor_c::getDemoAction
void f_025DC86C_orig(Cpu* c);  // fopScnM_ChangeReq
}

namespace interp { bool hold_pass(); }

namespace mods {
namespace {
constexpr uint32_t kOverlap = 0x101F36CC;  // l_fopOvlpM_overlap[0] (fopOvlpM_IsPeek 025DBE00)
constexpr uint32_t kOvlpTask = 0x20;       // overlap_request_class::mpTask (fopOvlpM_SceneIsStart)
constexpr uint32_t kPadPtr = 0x101F5088;   // the game's pad state (pad accessors 0200763C...)
constexpr uint32_t kCurProc = 0x65F0;      // daPy_lk_c::mCurProc
constexpr int kDoorTalk = 16;              // dDoor_info_c action table: "TALK"

int env_i(const char* n, int d) {
    const char* e = getenv(n);
    return e ? atoi(e) : d;
}
const int kDoorExtra = env_i("WWHD_MOD_DOOR_EXTRA", 3);    // 4 logic steps per frame
const int kSceneExtra = env_i("WWHD_MOD_SCENE_EXTRA", 3);  // 4 transition steps per frame

bool g_door_cut = false;  // a door asked for its cut during the logic step just run
int g_door_action = -1;
bool g_door_event = false;  // for traces: door event in progress
uint64_t g_extra_door = 0, g_extra_scene = 0;

// "pressed this frame" / "released this frame" words of the game's pad state (sead controller:
// +0x124 held, +0x18 pressed, +0x1C released, +0x40 hold counter; measured with A presses), cleared
// for the extra steps. WWHD_MODS_PADLOG=1 logs the pad state words when they change.
const uint32_t kTrigWords[] = {0x18, 0x1C};

void padlog() {
    static const bool on = getenv("WWHD_MODS_PADLOG") != nullptr;
    if (!on) return;
    uint32_t p = ld32(kPadPtr);
    if (!p) return;
    static uint32_t last[0x60];
    for (uint32_t i = 0; i < 0x60; i++) {
        uint32_t v = ld32(p + 4 * i);
        if (v != last[i]) LOG("[mods] pad +%03X: %08X -> %08X (step %llu)", 4 * i, last[i], v, (unsigned long long)step());
        last[i] = v;
    }
}

bool overlap_active() { return ld32(kOverlap) != 0; }

// debug: WWHD_MODS_OVLPLOG=1 traces the overlap request words (and its task's first words) on change
void ovlplog() {
    static const bool on = getenv("WWHD_MODS_OVLPLOG") != nullptr;
    uint32_t r = ld32(kOverlap);
    if (!on || !r) return;
    static uint32_t last[16], lastt[8];
    char buf[400];
    int n = 0;
    bool ch = false;
    for (int i = 0; i < 16; i++) {
        uint32_t v = ld32(r + 4 * i);
        ch |= v != last[i];
        last[i] = v;
        n += snprintf(buf + n, sizeof buf - n, " %08X", v);
    }
    uint32_t t = ld32(r + kOvlpTask);
    if (t) {
        n += snprintf(buf + n, sizeof buf - n, " | task");
        for (int i = 0; i < 8; i++) {
            uint32_t v = ld32(t + 0xC0 + 4 * i);
            ch |= v != lastt[i];
            lastt[i] = v;
            n += snprintf(buf + n, sizeof buf - n, " %08X", v);
        }
    }
    if (ch) trace("ovlp%s", buf);
}

void call(Cpu* c, void (*f)(Cpu*), uint32_t r3) {
    c->r[3] = r3;
    f(c);
}
}  // namespace

// interp.cpp's fpcEx_Handler hook, after the step's own execute
void after_execute(Cpu* c, uint32_t execute_fn) {
    // in-between passes of the 60 fps modes: no 30 Hz logic ran (doors, scenes), nothing to do
    if (interp::hold_pass()) return;
    padlog();
    ovlplog();
    bool door = g_door_cut;
    g_door_cut = false;
    if (door != g_door_event) {
        g_door_event = door;
        trace("door event %s", door ? "running" : "done");
    }
    static bool ovl = false;
    if (overlap_active() != ovl) {
        ovl = !ovl;
        trace("overlap (fade) %s", ovl ? "starts" : "ends");
    }
    uint32_t lr = c->lr, r3 = c->r[3];
    // quick doors: whole logic steps
    if (quick_doors()) {
        uint32_t pad = ld32(kPadPtr);
        uint32_t saved[sizeof kTrigWords / sizeof *kTrigWords];
        bool cleared = false;
        for (int i = 0; i < kDoorExtra && door; i++) {
            if (pad && !cleared) {
                for (size_t k = 0; k < sizeof kTrigWords / sizeof *kTrigWords; k++) {
                    saved[k] = ld32(pad + kTrigWords[k]);
                    st32(pad + kTrigWords[k], 0);
                }
                cleared = true;
            }
            call(c, f_0200E6EC_orig, 0);  // cCt_Counter(0) (end of the previous step)
            call(c, f_025DE024_orig, 0);  // fpcDt_Handler
            call(c, f_025E0EE4_orig, 0);  // fpcPi_Handler
            call(c, f_025DDCEC_orig, 0);  // fpcCt_Handler
            g_door_cut = false;
            call(c, f_025DE788_orig, execute_fn);
            g_extra_door++;
            door = g_door_cut;  // continue only while the door event still runs
            g_door_cut = false;
        }
        if (cleared)
            for (size_t k = 0; k < sizeof kTrigWords / sizeof *kTrigWords; k++) st32(pad + kTrigWords[k], saved[k]);
    }
    // fast scene changes: transition machinery only
    if (fast_scenes()) {
        for (int i = 0; i < kSceneExtra && overlap_active(); i++) {
            call(c, f_025DDCEC_orig, 0);  // fpcCt_Handler: loading phases of the new scene
            uint32_t ovl_req = ld32(kOverlap);
            uint32_t task = ovl_req ? ld32(ovl_req + kOvlpTask) : 0;
            if (task) call(c, f_025DF940, task);  // the overlap process: fade timers
            call(c, f_025D42C4_orig, 0);  // fapGm_After: scene / overlap request phases
            g_extra_scene++;
        }
    }
    c->lr = lr;
    c->r[3] = r3;
}

// Link executed (camera.cpp's daPy_Execute hook): control traces
void link_executed(uint32_t link) {
    if (!trace_on()) return;
    static uint32_t last = 0xFFFFFFFF;
    uint32_t p = ld32(link + kCurProc);
    if (p != last) {
        trace("link proc %u -> %u", last, p);
        last = p;
    }
}

}  // namespace mods

using namespace mods;

// door cuts
extern "C" void hook_0252A684(Cpu* c) {
    f_0252A684_orig(c);
    int a = (int)c->r[3];
    if (a != kDoorTalk) g_door_cut = true;
    if (a != g_door_action) {
        trace("door cut %d", a);
        g_door_action = a;
    }
}
extern "C" void hook_021C0078(Cpu* c) {
    f_021C0078_orig(c);
    g_door_cut = true;
}

// fopScnM_ChangeReq: scene change trigger (traces only)
extern "C" void hook_025DC86C(Cpu* c) {
    uint32_t r3 = c->r[3];
    f_025DC86C_orig(c);
    if (c->r[3]) trace("scene change request (proc %u)", r3 & 0xFFFF);
}
