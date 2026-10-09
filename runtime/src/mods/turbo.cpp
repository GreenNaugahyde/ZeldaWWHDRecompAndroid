// Quick doors and fast scene changes: while a door event or a scene change is in progress, the game
// runs extra logic steps per frame (game time passes faster), so nothing is skipped: every
// animation, event cut, flag and sound of the door or the transition happens exactly as before,
// only in fewer frames.
//
// Quick doors: door actors ask the event manager for their current cut through
// dDoor_info_c::getDemoAction (0252A684; knob doors, shutter doors 10/12, kddoor) or
// daMbdoor_c::getDemoAction (021C0078) while their door event runs. While a door was in such a cut
// during the step just run (not a TALK cut: locked-door messages run at normal speed), the frame gets
// kDoorExtra more logic steps: the steps of fpcM_Management between two executes (priority,
// creation, cCt_Counter) and fpcEx_Handler (every process: Link, door, camera, event manager in the
// play scene). Drawing, process deletion (fpcDt_Handler: its timers count drawn frames), scene
// management (fapGm_After) and the pad read stay once per frame. Button presses ("trigger" bits) are cleared for the extra steps, so a press is
// seen once.
//
// Fast scene changes: while an overlap (fade/wipe) process exists (l_fopOvlpM_overlap[0],
// 101F36CC), the frame gets kSceneExtra more runs of the transition machinery only: process
// creation (fpcCt_Handler: the new scene's loading phases), the overlap process's execute (its fade
// timers) and fapGm_After (scene and overlap request phases). The scenes themselves (actors,
// events, cutscenes) keep running at normal speed, so no story event is shortened.
//
// Fast forward: while ZR is held during an event, the emulated display clock runs kFfRate times
// as fast (gx2::set_clock_rate), so the game plays whole frames faster: every frame is calculated
// and drawn as usual, so nothing differs from normal play but the speed, which the device's
// rendering limits. (Extra logic steps between two drawn frames, as quick doors uses, halted the
// game in cutscenes: J3DPacket "mEntryPtr == 0" when a model was created in such a step.) The event
// controller's mode (dEvt_control_c::mMode) says what runs: 1 a dialogue (talk event), 2 or 3 a
// cutscene (demo or compulsory event, also the messages inside it). Cutscenes and dialogues are
// switched separately. Text boxes still wait for their button.
//
// All act on full logic passes only (not on the 60 fps in-between passes) and in all 60 fps modes.
#include <algorithm>
#include <chrono>
#include <atomic>
#include <cstdlib>

#include "mods.h"
#include "runtime.h"
#include "../release.h"

namespace gx2 { void set_clock_rate(double rate); }  // gx2_core.cpp
namespace gfx { void set_fast_forward(bool on); }     // vk_device.cpp

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
const release::Data kOverlap{0x101F36CC};  // l_fopOvlpM_overlap[0] (fopOvlpM_IsPeek 025DBE00)
constexpr uint32_t kOvlpTask = 0x20;       // overlap_request_class::mpTask (fopOvlpM_SceneIsStart)
const release::Data kPadPtr{0x101F5088};   // the game's pad state (pad accessors 0200763C...)
constexpr uint32_t kCurProc = 0x65F0;      // daPy_lk_c::mCurProc
constexpr int kDoorTalk = 16;              // dDoor_info_c action table: "TALK"
// dComIfGp_getEvent(): the play info (1046F0B0, from the getter 025200D4) + 0x51D0, as
// dEvent_manager_c::runProc (02544374) passes it to dEvt_control_c::check
const release::Data kEvtCtrl{0x10474280};
const release::Data kCounter{0x101FF558};  // g_Counter.mCounter0: +1 per logic step
constexpr uint32_t kEvtMode = 0xC2;        // dEvt_control_c::mMode (as on GameCube)

int env_i(const char* n, int d) {
    const char* e = getenv(n);
    return e ? atoi(e) : d;
}
const int kDoorExtra = env_i("WWHD_MOD_DOOR_EXTRA", 3);    // 4 logic steps per frame
const int kSceneExtra = env_i("WWHD_MOD_SCENE_EXTRA", 3);  // 4 transition steps per frame
const double kFfRate = env_i("WWHD_MOD_FF_RATE", 4);     // display clock rate while fast forwarding

// 0 no event, 1 dialogue, 2 / 3 cutscene
int event_mode() { return ld8(kEvtCtrl + kEvtMode); }
}  // namespace
int event_mode_now() { return event_mode(); }
namespace {
// fast forward applies to the event running now
bool ff_now() {
    if (!ff_button()) return false;
    int m = event_mode();
    return m == 1 ? ff_dialogues() : (m == 2 || m == 3) ? ff_cutscenes() : false;
}

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
    touch_state_step();
    static int mode = 0;
    if (event_mode() != mode) {
        mode = event_mode();
        LOG("[mods] event mode %d", mode);
    }
    // fast forward: the display clock runs faster (gx2_core.cpp), so whole frames come faster
    bool ff = ff_now();
    static bool ff_was = false;
    if (ff != ff_was) {
        ff_was = ff;
        gx2::set_clock_rate(ff ? kFfRate : 1.0);
#ifdef __ANDROID__
        gfx::set_fast_forward(ff);  // frame generation would pace the game to its own rate
#endif
        // the speed reached: game frames per second of real time, against the normal 30
        static uint32_t f0 = 0;
        static std::chrono::steady_clock::time_point t0;
        uint32_t frames = ld32(kCounter);
        auto now = std::chrono::steady_clock::now();
        if (ff) {
            f0 = frames;
            t0 = now;
            LOG("[mods] fast forward on (event mode %d)", mode);
        } else {
            double s = std::chrono::duration<double>(now - t0).count();
            LOG("[mods] fast forward off (event mode %d): %u frames in %.1f s, %.1fx", mode, frames - f0, s,
                s > 0 ? (frames - f0) / s / 30.0 : 0.0);
        }
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
            // No fpcDt_Handler: process deletion stays once per frame (original project 9fa6698, its
            // issue #61). fopAcM_delete queues a process with a timer of 1 and each fpcDt_Handler
            // counts it down, so the game deletes it two frames later, after its J3D packets left the
            // draw buffers. Run in every extra step, a process queued during a door event was deleted
            // in the same frame with packets still listed: "J3DPacket.cpp:157 mEntryPtr == (0)"
            // (Tingle's jail on Windfall).
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
