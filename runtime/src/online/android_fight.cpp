// (fork: Android) Online fights look the same on every game, step B: the monsters' animations (notes/online-modes.md,
// "Same fight on every screen").
//
// Upstream's shared monsters (mods/coop_monsters.cpp, unchanged) put another player's monster where its owner's is,
// with its heading and health, but its copy here runs its own logic against this game's Link: it swings when this Link
// is near, not when the owner's does. Now the owner tells which animation each of its monsters plays, at which frame and
// rate (kAnimMsg), and here, after the actors ran and before anything is drawn, the copy gets the same: the animation
// set on its morph (mDoExt_McaMorf::setAnm), the frame put in, the pose made again (calc). So every player sees the same
// wind-up, swing, flinch and fall, and can help where the monster really is.
//
// The animation goes by its name (the animation object's +0xC: the clip's name in the monster's archive), as a hash;
// here the name finds the monster archive's own animation object (dComIfG_getObjectRes over the archive's files: which
// archive is learned from the copy's own current animation). Kind-independent: the morph is found in the monster by its
// shape (a pointer to an object whose +0x94 is a transform animation and +0x90 a model), the first one: its body's.
//
// Not yet (step C): the copies' own attacks still hurt this game's Link as they did; the owner's swings do not yet.
#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mods/coop_monsters.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "runtime.h"

namespace net = mods::coop::net;
namespace names = mods::coop::names;

namespace android_fight {
constexpr net::Msg kAnimMsg = (net::Msg)0x65;  // (the fork's) owner -> all: 'W' 'A', stage[8], records
namespace {
constexpr uint32_t kProcName = 0x08, kSetId = 0x2D8, kGroup = 0x2DA, kHomeRoom = 0x2FE;
constexpr uint8_t kEnemy = 2;
// mDoExt_McaMorf (WWHD): the model +0x90, the animation object +0x94, the frame control +0x98 (J3DFrameCtrl, WWHD
// layout: rate +0, frame +4, start s16 +8, end s16 +0xA, loop s16 +0xC, attribute u8 +0xE, state u8 +0xF)
constexpr uint32_t kMorfModel = 0x90, kMorfAnim = 0x94, kFc = 0x98;
constexpr uint32_t kPos = 0x314, kShapeY = 0x32A, kScale = 0x330, kBaseMtx = 0xC8;  // fopAc current.pos, shape_angle.y, mScale; J3DModel mBaseTransformMtx
constexpr uint32_t kAnimName = 0xC;  // the animation object's name (a fixed string's text)
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage, kActorQueue = GD(0x101F3328);
const uint32_t kAnimType = GD(0x1016E95C);  // a transform animation object's +4 (upstream's story_resident_routes)
const uint32_t kSetAnm = GC(0x025E4A98), kCalc = GC(0x025E55A0), kGetObjectRes = GC(0x026066C4);
constexpr size_t kRecord = 4 + 4 + 4 + 4 + 1;
constexpr int kFullEvery = 15;  // steps between full updates (2 a second); changes go at once

bool mem2(uint32_t a, uint32_t n = 4) { return a >= 0x10000000u && a + n < 0x50000000u && !(a & 3); }
float f32(uint32_t a) { return (float)ldf32(a); }
void stf(uint32_t a, float v) { uint32_t u; memcpy(&u, &v, 4); st32(a, u); }
const char* stage() { return (const char*)mem::ptr(kStage); }
uint32_t key(uint32_t a) {  // (coop_monsters.cpp's)
    return ld16(a + kProcName) * 0x9E3779B1u ^ ld8(a + kHomeRoom) * 0x85EBCA6Bu ^ ld16(a + kSetId);
}
bool anim_object(uint32_t o) { return mem2(o, 0x40) && ld32(o + 4) == kAnimType; }
std::string anim_name(uint32_t o) {
    const uint32_t t = ld32(o + kAnimName);
    std::string s;
    if (mem2(t & ~3u, 0x20)) for (uint32_t i = 0; i < 31 && ld8(t + i); i++) s += (char)ld8(t + i);
    return s;
}
uint32_t hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char ch : s) h = (h ^ (uint8_t)ch) * 16777619u;
    return h ? h : 1;
}

// each monster's body morph (by process id)
std::unordered_map<uint32_t, uint32_t> g_morf;
uint32_t morf_of(uint32_t a) {
    const uint32_t pid = ld32(a + 4);
    if (const auto it = g_morf.find(pid); it != g_morf.end()) return it->second;
    uint32_t found = 0, at = 0;
    for (uint32_t o = 0x3A4; o < 0x2000 && !found; o += 4) {
        const uint32_t p = ld32(a + o);
        if (mem2(p, 0xB0) && anim_object(ld32(p + kMorfAnim)) && mem2(ld32(p + kMorfModel), 0x40)) found = p, at = o;
    }
    g_morf[pid] = found;
    if (static std::unordered_map<int, bool> told; !told[ld16(a + kProcName)]) {
        told[ld16(a + kProcName)] = true;
        if (found) LOG("[fight] proc %#x: morph at +%#x", ld16(a + kProcName), at);
        else LOG("[fight] proc %#x: no morph found (its animations are its own)", ld16(a + kProcName));
    }
    return found;
}

// the animations of each kind's archive, by name hash
struct Kind { bool tried = false; std::unordered_map<uint32_t, uint32_t> anims; std::string arc; };
std::unordered_map<int, Kind> g_kinds;
const char* const kArchives[] = {"Bk", "Mo2", "Tn", "CC", "Ki", "St", "AM", "AM2", "Rd", "PW", "Mozo", "Ph", "Bl", "Mt", "Mo"};
// the archive whose files hold `cur` (one of this kind's animations): all its animations
void learn(Cpu* c, int proc, uint32_t cur) {
    Kind& k = g_kinds[proc];
    if (anim_object(cur)) k.anims.emplace(hash(anim_name(cur)), cur);  // (at least what it played)
    if (k.tried || !anim_object(cur)) return;
    k.tried = true;
    static const uint32_t text = mem::host_alloc(16), str = mem::host_alloc(8);
    for (const char* arc : kArchives) {
        memset(mem::ptr(text), 0, 16), memcpy(mem::ptr(text), arc, strlen(arc));
        st32(str, text), st32(str + 4, GD(0x10023B10));  // the name as a string object (text, vtable): android_models.cpp
        std::vector<uint32_t> files;
        bool has = false;
        for (uint32_t i = 0, misses = 0; i < 160 && misses < 3; i++) {
            Cpu call = *c;
            const uint32_t r = guest_call(&call, kGetObjectRes, {ld32(GD(0x101F4F28)), str, i});
            if (!r) { misses++; continue; }
            misses = 0, files.push_back(r), has |= r == cur;
        }
        if (!has) continue;
        k.arc = arc;
        for (uint32_t r : files) if (anim_object(r)) k.anims[hash(anim_name(r))] = r;
        LOG("[fight] proc %#x: archive %s, %zu animations", (unsigned)proc, arc, k.anims.size());
        return;
    }
    LOG("[fight] proc %#x: its archive not found (animations only as seen)", (unsigned)proc);
}

// ours: what was sent last
struct Sent { uint32_t h = 0; };
std::unordered_map<uint32_t, Sent> g_sent;  // by key
// another's: the owner's last word, and where it stands now here
struct Want { uint32_t h = 0; float frame = 0, rate = 0; uint8_t attr = 0; uint32_t got = 0; bool fresh = false; };
std::unordered_map<uint32_t, Want> g_want;  // by key
char g_stage[8];
uint32_t g_steps = 0;

template <class F> void each_monster(F f) {  // g_fopAcTg_Queue: node next +8, actor +0xC
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; n >= 0x10000000 && n < 0x50000000 && k < 4096; k++, n = ld32(n + 8))
        if (const uint32_t a = ld32(n + 0xC); a && ld8(a + kGroup) == kEnemy && ld16(a + kSetId) != 0xFFFF) f(a);
}
}  // namespace

// another player's animations (the session's poll, game thread)
void received(int, const uint8_t* d, size_t n) {
    if (n < 10 || d[0] != 'W' || d[1] != 'A' || memcmp(d + 2, stage(), 8)) return;
    for (size_t o = 10; o + kRecord <= n; o += kRecord) {
        uint32_t k, h;
        float fr, rt;
        memcpy(&k, d + o, 4), memcpy(&h, d + o + 4, 4), memcpy(&fr, d + o + 8, 4), memcpy(&rt, d + o + 12, 4);
        if (!std::isfinite(fr) || !std::isfinite(rt) || std::fabs(rt) > 10 || fr < -1 || fr > 10000) continue;
        Want& w = g_want[k];
        w.fresh |= w.h != h;
        w.h = h, w.frame = fr, w.rate = rt, w.attr = d[o + 16], w.got = g_steps;
    }
}

// game thread, each full step, after the actors ran (online_compat::tick)
void step(Cpu* c) {
    g_steps++;
    if (net::role() == net::Role::kOff) return g_want.clear(), g_sent.clear(), g_morf.clear(), void();
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (!mem2(link)) return;
    if (memcmp(g_stage, stage(), 8)) memcpy(g_stage, stage(), 8), g_want.clear(), g_sent.clear(), g_morf.clear(), g_kinds.clear();
    const bool full = g_steps % kFullEvery == 0;
    std::string out(std::string("WA") + std::string(stage(), 8));
    each_monster([&](uint32_t a) {
        if (!mods::coop::monsters::takes(a)) return;
        const uint32_t m = morf_of(a);
        if (!m) return;
        const uint32_t cur = ld32(m + kMorfAnim), fc = m + kFc, k = key(a);
        const int proc = (int16_t)ld16(a + kProcName);
        learn(c, proc, cur);
        if (mods::coop::monsters::runs(a)) {  // ours: told when it changes, and now and then
            if (!anim_object(cur)) return;
            const uint32_t h = hash(anim_name(cur));
            Sent& s = g_sent[k];
            if (!full && s.h == h) return;
            if (s.h != h && std::getenv("WWHD_FIGHT_TRACE"))
                LOG("[fight] ours %08X: %s (rate %.2f, attribute %u, end %d)", k, anim_name(cur).c_str(), f32(fc), ld8(fc + 0xE), (int16_t)ld16(fc + 0xA));
            s.h = h;
            if (out.size() + kRecord > 1000) return;
            const float fr = f32(fc + 4), rt = f32(fc);
            out.append((const char*)&k, 4), out.append((const char*)&h, 4), out.append((const char*)&fr, 4), out.append((const char*)&rt, 4);
            out += (char)ld8(fc + 0xE);
            return;
        }
        // another's. Its model was placed where its own logic had it (it runs against this game's Link: a jump, a
        // fall, a step aside) before coop_monsters put it where the owner's is: drawn there it shivered between the
        // two and hung half in the air. So its model is placed again where it is now (the base matrix: its place,
        // its body's heading, its scale, as the monsters' own setBaseMtx make it), then posed
        const uint32_t model = ld32(m + kMorfModel), base = model + kBaseMtx;
        {
            const float px = f32(a + kPos), py = f32(a + kPos + 4), pz = f32(a + kPos + 8);
            if (static std::unordered_map<uint32_t, uint32_t> logged; ++logged[k] % 150 == 1)
                LOG("[fight] %08X copy: drawn at %.0f %.0f %.0f, is at %.0f %.0f %.0f", k, f32(base + 0xC), f32(base + 0x1C), f32(base + 0x2C), px, py, pz);
            const float ang = (int16_t)ld16(a + kShapeY) * 3.14159265f / 32768, sn = std::sin(ang), cs = std::cos(ang);
            const float sx = f32(a + kScale), sy = f32(a + kScale + 4), sz = f32(a + kScale + 8);
            const float mt[12] = {cs * sx, 0, sn * sz, px, 0, sy, 0, py, -sn * sx, 0, cs * sz, pz};
            if (std::isfinite(px) && std::isfinite(py) && std::isfinite(pz) && sx > 0 && sx < 20)
                for (int i = 0; i < 12; i++) stf(base + 4 * i, mt[i]);
        }
        auto it = g_want.find(k);
        Want* w = it == g_want.end() || g_steps - it->second.got > 90 ? nullptr : &it->second;  // (nothing heard for 3 s: its own)
        Kind& kind = g_kinds[proc];
        const auto an = w ? kind.anims.find(w->h) : kind.anims.end();
        if (w && an == kind.anims.end() && w->fresh) LOG("[fight] %08X: an animation of %s not known here (%08X)", k, kind.arc.c_str(), w->h), w->fresh = false;
        if (w && an != kind.anims.end()) {  // the owner's animation, at the frame it has reached by now
            const float since = (float)(g_steps - w->got);
            if (cur != an->second) {  // (a new one of the owner's blends in; its own choice is put back at once)
                Cpu call = *c;
                call.f[1].ps0 = w->fresh ? 5.0 : 0.0, call.f[2].ps0 = w->rate, call.f[3].ps0 = 0, call.f[4].ps0 = -1;
                guest_call(&call, kSetAnm, {m, an->second, w->attr, 0});
            }
            w->fresh = false;
            float fr = w->frame + w->rate * since;
            const float start = (float)(int16_t)ld16(fc + 8), end = (float)(int16_t)ld16(fc + 0xA), loop = (float)(int16_t)ld16(fc + 0xC);
            if (end > start && fr >= end) fr = (w->attr == 2 || w->attr == 4) && end > loop ? loop + std::fmod(fr - loop, end - loop) : end;
            if (fr < start) fr = start;
            stf(fc, w->rate), stf(fc + 4, fr);
        }
        Cpu call = *c;
        guest_call(&call, kCalc, {m});  // the pose made again (place and animation), for this step's draw
    });
    if (out.size() > 10) net::send(-1, kAnimMsg, out);
}
}  // namespace android_fight
