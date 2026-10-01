// True 60 fps: Link's (daPy_lk_c) own per-step code. See true60.cpp for the framework.
//
// Shared primitives (smoothing, chasing, animation frames, morf) are scaled in true60.cpp. This file
// handles the per-step code that is inline in Link's functions, through instruction-level hooks
// (site_ADDR runs before the instruction at ADDR; "@ADDR" lines in tools/recomp/hooks.txt) and
// function hooks. Everything here is a no-op unless Link's execute runs with dt < 1.
//
// WWHD addresses are from the generated code (build/gen) of the functions named in build/names.tsv;
// the GameCube source lines are in tww/src/d/actor/d_a_player_main.cpp.
#include <cmath>

#include "runtime.h"
#include "true60.h"

extern "C" {
void f_02416230_orig(Cpu* c);  // daPy_lk_c::setNormalSpeedF
void f_0207A9A0_orig(Cpu* c);  // cLib_calcTimer<u8>
void f_022ED850_orig(Cpu* c);  // cLib_calcTimer<u8> (second copy)
void f_02055B64_orig(Cpu* c);  // cLib_calcTimer<s16>
void f_0211D2F8_orig(Cpu* c);  // cLib_calcTimer<s32>
void f_025AAE08_orig(Cpu* c);  // cLib_calcTimer<s32> (second copy)
}

namespace {
constexpr uint32_t kSpeed = 0x33C;  // fopAc_ac_c::speed

bool link60() { return true60::dt() < 1.0f && true60::exec_proc() != 0 && true60::exec_proc() == true60::link(); }

uint32_t scratch_vec() {
    return mem::fixed_slot(mem::kFixLinkScratch);
}
}  // namespace

// ---- daPy_lk_c::posMoveFromFootPos (023FCB9C; GameCube d_a_player_main.cpp:2352) ----

// foot-driven speed: f1 = |toe movement since the last step| (absXZ, before `f31_2 = ...`). The
// animation advanced dt frames, so the movement is per dt; the game wants it per step.
extern "C" void site_023FCEB0(Cpu* c) {
    if (link60()) c->f[1].ps0 = (float)(c->f[1].ps0 / true60::dt());
}

// gravity: `speed.y += gravity * 2.25f` (heavy boots, fmadds at 023FD338) and `speed.y += gravity`
// (fadds at 023FD35C); f9 = gravity
extern "C" void site_023FD338(Cpu* c) {
    if (link60()) c->f[9].ps0 = (float)(c->f[9].ps0 * true60::dt());
}
extern "C" void site_023FD35C(Cpu* c) {
    if (link60()) c->f[9].ps0 = (float)(c->f[9].ps0 * true60::dt());
}

// `current.pos += speed` (PSVECAdd(pos, speed, pos) at 023FD39C): pos += speed * dt. Gravity changed
// speed.y by dv during this step (f31 = speed.y before); adding dv * (1-dt)/2 makes the half steps
// land exactly on the 30 Hz path (v += g; p += v), so jump arcs keep their height and length.
extern "C" void site_023FD39C(Cpu* c) {
    if (!link60()) return;
    const float dt = true60::dt();
    uint32_t sp = c->r[4];  // &speed
    float vx = u32_as_f32(ld32(sp)), vy = u32_as_f32(ld32(sp + 4)), vz = u32_as_f32(ld32(sp + 8));
    float dv = vy - (float)c->f[31].ps0;
    uint32_t v = scratch_vec();
    st32(v, f32_as_u32(vx * dt));
    st32(v + 4, f32_as_u32(vy * dt + dv * (1.0f - dt) * 0.5f));
    st32(v + 8, f32_as_u32(vz * dt));
    c->r[4] = v;
}

// ---- daPy_lk_c::setNormalSpeedF(f32 accel, f32 scale, f32 maxStep, f32 minStep) (02416230;
// GameCube :2300): `mNormalSpeed += accel` per step, or cLib_addCalc (scaled in true60.cpp) ----
extern "C" void hook_02416230(Cpu* c) {
    if (link60()) c->f[1].ps0 = (float)(c->f[1].ps0 * true60::dt());
    f_02416230_orig(c);
}

// ---- cLib_calcTimer<T> (`if (*t != 0) --*t; return *t;`), one copy per type and file. With
// dt < 1 the timer counts on full passes only, so a pair of half steps counts one step. ----
template <int kBytes>
static bool calc_timer_held(Cpu* c) {
    if (true60::dt() >= 1.0f || !true60::half_pass()) return false;
    uint32_t p = c->r[3];
    c->r[3] = kBytes == 1 ? ld8(p) : kBytes == 2 ? (uint32_t)(int32_t)(int16_t)ld16(p) : ld32(p);
    return true;
}
extern "C" void hook_0207A9A0(Cpu* c) { if (!calc_timer_held<1>(c)) f_0207A9A0_orig(c); }
extern "C" void hook_022ED850(Cpu* c) { if (!calc_timer_held<1>(c)) f_022ED850_orig(c); }
extern "C" void hook_02055B64(Cpu* c) { if (!calc_timer_held<2>(c)) f_02055B64_orig(c); }
extern "C" void hook_0211D2F8(Cpu* c) { if (!calc_timer_held<4>(c)) f_0211D2F8_orig(c); }
extern "C" void hook_025AAE08(Cpu* c) { if (!calc_timer_held<4>(c)) f_025AAE08_orig(c); }

// JAIZelAnime::setAnimSound (0201BF08) is left alone: it tracks the animation frame itself (each
// sound key plays once when the frame passes it), and its rate argument sets the pitch of
// speed-dependent sounds, which is per original step.
