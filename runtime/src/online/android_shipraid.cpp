// (fork: Android) Online event "Raiders from the sea" (a test of what online quests can do, notes/online-modes.md):
// an enemy warship sails up to Outset, anchors off the beach nearest Grandma's house, and its crew comes ashore in 4
// waves and fights its way to her house. One raider reaching her door and the event is lost: a Moblin carries Grandma
// off to the beach, she is taken aboard and the ship sails away with her. All 4 waves beaten: the ship turns and flees.
//
// Started from Game > Online by a player on Outset (her door, from the stage data, is the target).
// That game (the runner) looks for the beach: in 24 directions from the door it probes the ground (GroundCross with
// Link's own ground check, as climb.cpp does) every 100 units against the sea's height (coop_deck::water) for the
// first deep water; the direction with the nearest shore whose way from the door is dry and without cliffs wins. The
// landing is the last dry place before the water, the anchorage 700 units beyond the first deep point.
// It tells the party (kRaidMsg): the start (door, landing, anchorage, the way out to sea), each wave's set ids, the
// end. The ship ("Oship", the game's own enemy warship: it fires its cannon at Link when he is in range) and on a loss
// Grandma ("Ba1", her demo type: she stands, no talk events) and her kidnapper ("mo2") are each game's own actors (set
// id 0xFFFF: coop_monsters leaves them to each game), moved by the same timeline everywhere: their places are set
// before their logic runs (fopAc_Execute, cheats.cpp's hook); the ship and the kidnapper cannot be beaten.
// The waves come through the spawn service (android_raid.cpp: shared monsters) on the beach by the ship, and march
// like the siege's (android_siege.cpp): Link stands a little ahead of a monster with no player near while its logic
// runs, so it walks to the door; with a player within 800 it fights.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/coop_deck.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "../mods/coop_sync.h"
#include "fork_proto.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "proto/wire.h"
#include "runtime.h"

namespace ss { void notice(const std::string& text); }
namespace android_say { void say(const std::string& text); }
namespace mods::coop::net::srv { bool android_fork_send(const std::string& body); }
namespace android_raid {
struct SpawnReq { const char* name; uint32_t params; float at[3]; int16_t yaw; };
std::vector<uint16_t> spawn(Cpu* c, uint32_t link, const std::vector<SpawnReq>& list);
bool alive(int room, uint16_t set);
void remove(Cpu* c, int room, uint16_t set);
}

namespace android_shipraid {
constexpr mods::coop::net::Msg kRaidMsg = (mods::coop::net::Msg)0x6A;  // (the fork's) runner -> party: 'W' 'B', op, ...
namespace {
namespace net = mods::coop::net;
namespace sync = mods::coop::sync;
namespace names = mods::coop::names;

enum Op : uint8_t { kStart = 1, kWave = 2, kLost = 3, kWon = 4 };
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage, kActorQueue = GD(0x101F3328);
// fopAc_ac_c in WWHD (GameCube + 0x11C): current pos / angle / room, shape angle, speed, health; base process id, name
constexpr uint32_t kPid = 0x04, kProcName = 0x08, kPos = 0x314, kAngle = 0x320, kRoom = 0x326, kShape = 0x328, kSpeed = 0x33C,
                   kSetId = 0x2D8, kGroup = 0x2DA, kHomeRoom = 0x2FE, kHealth = 0x3A1;
constexpr uint32_t kGndChk = 0xB14;  // daPy_lk_c::mGndChk (dBgS_GndChk): its probe point at +0x24 (climb.cpp)
const uint32_t kBgsAccess = GC(0x025200D4), kGroundCross = GC(0x02008974);
constexpr int kWaves = 4;
// (kCarrot: Link is put that far ahead of a marching monster; out of its swing range, else it swings while marching
// and hits the monster in front of it: their weapons hit monsters too, GrpAll; user: mobs fighting each other)
constexpr uint32_t kHome = 0x2EC;  // fopAc_ac_c::home.pos (HD)
constexpr float kBreachAt = 250, kEngage = 800, kCarrot = 450;
constexpr uint32_t kSailIn = 25000, kSailOut = 30000, kTurn = 4000;  // ms
constexpr float kSeaOut = 7000;  // where the ship comes from and goes to, beyond the anchorage
constexpr float kKidnapSpeed = 7;  // units per step

const char* const kIntro = "A raiders' ship is coming for Outset Island!\nIts crew will storm Grandma's house.\nStop them on the beach!";
const char* const kWin = "The raiders are beaten!\nTheir ship turns and flees.\nGrandma is safe, thanks to you all!";
const char* const kLose = "The raiders have taken Grandma!\nTheir ship is gone over the horizon...\nGather your friends and try again!";

enum Phase { kOff, kArriving, kAnchored, kKidnap, kLeaving };
struct Event {
    Phase phase = kOff;
    bool runner = false, won = false;
    char stage[8] = {};
    int room = 0, wave = 0;
    float house[3] = {}, land[3] = {}, anchor[3] = {}, out[2] = {};  // out: the way out to sea (unit, x z)
    std::vector<uint16_t> sets;
    uint32_t t0 = 0, wave_at = 0, quiet_since = 0, phase_at = 0;
    uint32_t ship = 0xFFFFFFFFu, flag = 0xFFFFFFFFu, granny = 0xFFFFFFFFu, carrier = 0xFFFFFFFFu;  // process ids
    float carry[3] = {};  // where the kidnapper is now (its timeline; only with a made kidnapper)
    uint16_t taker = 0xFFFF;  // the raider who reached the door: it takes Grandma to the beach itself
    uint32_t taker_gone = 0;  // since when it is not here (beaten: Grandma saved)
    bool flag_looked = false, told_aboard = false;
    uint32_t ship_at = 0;  // when the ship was last made here
    struct Prog { uint16_t set; float best; uint32_t at; };
    std::vector<Prog> prog;  // each marching raider's nearest to the door so far, and when (stuck ones moved on)
} g;
uint32_t g_last_link = 0;
int g_settle = 0;  // steps left before actors are made / moved after a stage change (Game Over's Continue: crash)
std::vector<std::string> g_in;  // the runner's words (the session's poll)
bool g_ask = false;

const char* stage() { return (const char*)mem::ptr(kStage); }
float f32(uint32_t a) { return (float)ldf32(a); }
// Grandma's door on Outset (sea, room 44), from the stage data: LinkRM's exit (SCLS: sea, start 1, room 44) puts Link
// at spawn 1 (PLYR), the place of her door (TGDR KNOB00D); not Link's place at the start (user: the raiders went for
// the spot where the event was started, 2600 from her door)
constexpr float kGrandmaDoor[3] = {-205170.47f, 720.f, 316800.69f};
bool near_outset(uint32_t link) {
    const float p[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
    return !memcmp(stage(), "sea", 4) && std::hypot(p[0] - kGrandmaDoor[0], p[2] - kGrandmaDoor[2]) < 8000;
}
uint32_t bits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
void stf(uint32_t a, float f) { st32(a, bits(f)); }
float dist2d(const float* a, const float* b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }
int16_t yaw_to(float dx, float dz) { return (int16_t)(std::atan2(dx, dz) * 32768 / 3.14159265f); }
bool valid(uint32_t a) { return a >= 0x10000000u && a < 0x50000000u; }
uint32_t by_pid(Cpu* c, uint32_t pid) {
    if (pid == 0xFFFFFFFFu || !pid) return 0;
    const uint32_t r3 = c->r[3];
    const uint32_t a = guest_call(c, GC(0x025DE50C), {pid});  // fopAcM_SearchByID
    c->r[3] = r3;
    return valid(a) ? a : 0;
}
void kill(Cpu* c, uint32_t& pid) {
    if (pid != 0xFFFFFFFFu && by_pid(c, pid)) guest_call(c, GC(0x025D57E4), {pid});  // fopAcM_delete
    pid = 0xFFFFFFFFu;
}
void send(const std::string& body) { net::send(-1, kRaidMsg, std::string("WB") + body); }
std::string fbytes(const float* v, int n) { return std::string((const char*)v, 4 * n); }

// the ground's height under (x, z), looked for from y down (Link's own ground check, put back after); -1e9: none
float ground(Cpu* c, uint32_t link, float x, float y, float z) {
    uint8_t keep[0x80];
    memcpy(keep, mem::ptr(link + kGndChk), sizeof keep);
    const Cpu saved = *c;
    stf(link + kGndChk + 0x24, x), stf(link + kGndChk + 0x28, y), stf(link + kGndChk + 0x2C, z);
    const uint32_t bgs = guest_call(c, kBgsAccess) + 0x12A0;
    guest_call(c, kGroundCross, {bgs, link + kGndChk});
    const float gy = (float)c->f[1].ps0;
    *c = saved;
    memcpy(mem::ptr(link + kGndChk), keep, sizeof keep);
    return std::isfinite(gy) && gy > -1e8f ? gy : -1e9f;
}
float sea_at(Cpu* c, float x, float z) {
    float y = 0;
    return mods::coop::deck::water(c, x, z, y) ? y : 0.f;
}

// the beach nearest the door with a walkable way to it: landing, anchorage, the way out (runner, at the start). In 48
// directions the ground is probed every 50 units: the way must be walkable all along (no step over 60 units, no
// water deeper than wading); best a real beach (the landing low, near the sea's height, deep water close beyond),
// else the last walkable place before the shore (a cliff top: the raiders are there, the ship below); never a place
// they cannot walk on from (user: the ship landed at a cliff the raiders could not climb).
bool find_beach(Cpu* c, uint32_t link, float max_step = 60) {
    const float* h = g.house;
    float best = 1e9f;
    int kind_best = 0;
    for (int k = 0; k < 48; k++) {
        const float a = k * 6.2831853f / 48, dx = std::sin(a), dz = std::cos(a);
        float prev = h[1], land_r = -1, land_y = 0, land_sea = 0;
        bool walk = true;  // (the way so far walkable)
        int deep_r = -1;
        for (int r = 100; r <= 8000; r += 50) {
            const float x = h[0] + dx * r, z = h[2] + dz * r;
            const float gy = ground(c, link, x, h[1] + 3000, z), sea = sea_at(c, x, z);
            if (gy < sea - 150) { deep_r = r; break; }  // deep (or no ground: the open sea)
            const bool dry = gy > sea + 10, wade = !dry && gy > sea - 50;
            if (r > 250 && std::fabs(gy - prev) > max_step) walk = false;  // (a step, a cliff; next to the door: its house)
            if (!dry && !wade) walk = false;  // (swimming)
            prev = gy;
            if (walk && dry) land_r = (float)r, land_y = gy, land_sea = sea;
        }
        if (deep_r < 0 || land_r < 500) continue;
        // 2: a beach (low, deep water within 1500); 1: walkable to the shore's top only
        const int kind = (land_y < land_sea + 200 && deep_r - land_r <= 1500) ? 2 : 1;
        const float score = (kind == 2 ? 0 : 100000) + deep_r + (deep_r - land_r);
        LOG("[shipraid] direction %d: walkable to %.0f (ground %.0f, sea %.0f), deep at %d: %s", k, land_r, land_y, land_sea, deep_r,
            kind == 2 ? "beach" : "cliff top");
        if (score >= best) continue;
        best = score, kind_best = kind;
        g.land[0] = h[0] + dx * land_r, g.land[1] = land_y, g.land[2] = h[2] + dz * land_r;
        g.anchor[0] = h[0] + dx * (deep_r + 700), g.anchor[2] = h[2] + dz * (deep_r + 700);
        g.anchor[1] = sea_at(c, g.anchor[0], g.anchor[2]);
        g.out[0] = dx, g.out[1] = dz;
    }
    if (kind_best) LOG("[shipraid] landing: %s", kind_best == 2 ? "a beach" : "a cliff top (no walkable beach)");
    return best < 1e9f;
}

// a wave's monsters: the raiders (upstream's native enemy parameters), more with a bigger party
std::vector<android_raid::SpawnReq> wave_list(Cpu* c, uint32_t link, int wave, int party) {
    struct K { const char* name; uint32_t params; };
    const K bk{"Bk", 0xFFFFFF04u}, mo{"mo2", 0xFFFF0000u}, tn{"Tn", 0xFFFF9604u};
    std::vector<K> ks;
    if (wave == 1) ks = {bk, bk, bk, bk};
    if (wave == 2) ks = {bk, bk, bk, mo, mo};
    if (wave == 3) ks = {mo, mo, bk, bk, bk, bk};
    if (wave == 4) ks = {tn, mo, mo, bk, bk, bk};
    for (int i = 0; i < std::min(4, (party - 1) * 2); i++) ks.push_back(i % 2 ? mo : bk);
    // on the beach by the ship, spread along it; a place in the water is moved inland until dry
    const float px = -g.out[1], pz = g.out[0];  // (along the beach)
    std::vector<android_raid::SpawnReq> out;
    for (size_t i = 0; i < ks.size(); i++) {
        // (along the beach, each place checked: dry and at the landing's height, else nearer the landing)
        const float side = ((int)(i % 2) * 2 - 1) * (float)((i + 1) / 2) * 70;
        float x = g.land[0], z = g.land[2], y = g.land[1];
        for (float part : {1.f, 0.5f, 0.25f}) {
            const float tx = g.land[0] + px * side * part - g.out[0] * 60, tz = g.land[2] + pz * side * part - g.out[1] * 60;
            const float gy = ground(c, link, tx, g.land[1] + 300, tz);
            if (gy > sea_at(c, tx, tz) + 10 && std::fabs(gy - g.land[1]) < 60) { x = tx, z = tz, y = gy; break; }
        }
        android_raid::SpawnReq r{ks[i].name, ks[i].params, {x, y + 40, z}, yaw_to(g.house[0] - x, g.house[2] - z)};
        out.push_back(r);
    }
    return out;
}
int party_here() {
    int n = 1;
    float p[3];
    for (int id = 0; id < net::kMaxPlayers; id++) if (id != net::my_id() && net::member(id) && sync::place(id, p)) n++;
    return n;
}
uint32_t actor_of(uint16_t set) {
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; valid(n) && k < 4096; k++, n = ld32(n + 8)) {
        const uint32_t a = ld32(n + 0xC);
        if (a && ld8(a + kGroup) == 2 && ld16(a + kSetId) == set && (int8_t)ld8(a + kHomeRoom) == g.room) return a;
    }
    return 0;
}

// an actor of ours made here, by its stage data name (dStage_searchName: process name + argument), in Link's layer
uint32_t make(Cpu* c, uint32_t link, const char* nm, uint32_t params, const float at[3], int16_t angle_x, int16_t yaw) {
    static const uint32_t buf = mem::host_alloc(0x40), name = mem::host_alloc(16);
    memset(mem::ptr(name), 0, 16), memcpy(mem::ptr(name), nm, strnlen(nm, 8));
    Cpu probe = *c;
    const uint32_t entry = guest_call(&probe, GC(0x025C109C), {name});
    if (!entry) { LOG("[shipraid] no stage name %s", nm); return 0xFFFFFFFFu; }
    const uint16_t proc = ld16(entry + 8);
    const uint32_t arg = (uint32_t)(int32_t)(int8_t)ld8(entry + 0xA);
    for (int e = 0; e < 3; e++) stf(buf + 4 * e, at[e]);
    st16(buf + 0x10, (uint16_t)angle_x), st16(buf + 0x12, (uint16_t)yaw), st16(buf + 0x14, 0);
    st32(buf + 0x20, 0x3F800000u), st32(buf + 0x24, 0x3F800000u), st32(buf + 0x28, 0x3F800000u);
    const uint32_t r3 = c->r[3];
    const uint32_t layer = guest_call(c, GC(0x025DED64));
    guest_call(c, GC(0x025DEAB4), {ld32(link + 0x2C)});
    uint32_t pid = guest_call(c, GC(0x025D5834), {proc, params, buf, (uint32_t)(int32_t)g.room, buf + 0x10, buf + 0x20, arg, 0});
    guest_call(c, GC(0x025DEAB4), {layer});
    c->r[3] = r3;
    LOG("[shipraid] %s (proc %#x) made at %.0f %.0f %.0f: pid %08X", nm, proc, at[0], at[1], at[2], pid);
    return pid ? pid : 0xFFFFFFFFu;
}

// the ship's place and heading now (its timeline)
float smooth(float t) { t = std::clamp(t, 0.f, 1.f); return t * t * (3 - 2 * t); }
void ship_now(uint32_t now, float p[3], int16_t& yaw) {
    const float far_in[2] = {g.anchor[0] + g.out[0] * kSeaOut, g.anchor[2] + g.out[1] * kSeaOut};
    const int16_t in = yaw_to(-g.out[0], -g.out[1]), away = yaw_to(g.out[0], g.out[1]);
    p[1] = g.anchor[1];
    if (g.phase == kArriving) {
        const float t = smooth((float)(now - g.t0) / kSailIn);
        p[0] = far_in[0] + (g.anchor[0] - far_in[0]) * t, p[2] = far_in[1] + (g.anchor[2] - far_in[1]) * t;
        yaw = in;
    } else if (g.phase == kLeaving) {
        const uint32_t dt = now - g.phase_at;
        const float turn = smooth((float)dt / kTurn);
        yaw = (int16_t)(in + (int16_t)(away - in) * turn);
        const float t = dt < kTurn ? 0.f : smooth((float)(dt - kTurn) / kSailOut);
        p[0] = g.anchor[0] + g.out[0] * kSeaOut * 1.3f * t, p[2] = g.anchor[2] + g.out[1] * kSeaOut * 1.3f * t;
    } else {
        p[0] = g.anchor[0], p[2] = g.anchor[2], yaw = in;
    }
}

// ---- a dark red sky while the raiders are here (sky and clouds; the fog half way; user: not everything tinted).
// The stage file as loaded (Stage.dzs inside the stage's Stage.bfres in HD): its chunk table found in guest memory
// ("STAG" first, a count before it, 12-byte entries tag / count / offset; the offsets made absolute addresses once loaded), and in it the palettes (Pale, 0x6C bytes
// each in HD: the GameCube's 0x2C fields first, fog colour +0x1E, fog start/end +0x24/+0x28) and the skies (Virt,
// 0x38 in HD: clouds +0x10, their middle +0x14, sky +0x18, far sea +0x1B, haze +0x1E). The game blends from these each
// frame (d_kankyo setLight); faded towards dark red and back, the originals kept and put back. (g_env_light's own
// pointers are not the GameCube's in HD: written through them, the picture went black.)
struct Table { uint32_t pale = 0, vr = 0; int npale = 0, nvr = 0; std::vector<uint8_t> pale_bak, vr_bak; };
struct Tint {
    std::vector<Table> t;
    float f = 0;  // 0 the game's colours .. 1 dark red
    bool tried = false;
} g_tint;
constexpr uint32_t kPaleSize = 0x6C, kVirtSize = 0x38;
bool tint_ok(const Tint& t) {  // (still the same data: its first palette as kept)
    for (const Table& x : t.t) if (memcmp(mem::ptr(x.pale) + 0x24, x.pale_bak.data() + 0x24, 8)) return false;
    return !t.t.empty();
}
bool tint_find() {
    Tint& t = g_tint;
    const uint8_t* base = mem::ptr(mem::kMem2Start);
    const size_t size = mem::kMem2End - mem::kMem2Start;
    for (const uint8_t* p = base + 4; (p = (const uint8_t*)memmem(p, size - (p - base) - 64, "STAG", 4)); p += 4) {
        const uint32_t hdr = mem::kMem2Start + (uint32_t)(p - base) - 4;
        if (hdr & 3) continue;
        const uint32_t n = ld32(hdr);
        if (n < 4 || n > 40) continue;
        uint32_t pale = 0, vr = 0, np = 0, nv = 0, pale_end = 0, vr_end = 0;
        bool ok = true;
        std::vector<std::pair<uint32_t, uint32_t>> offs;
        for (uint32_t i = 0; i < n && ok; i++) {
            const uint32_t e = hdr + 4 + i * 12;
            for (int k = 0; k < 4; k++) { const uint8_t ch = ld8(e + k); ok = ok && ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')); }
            // (once loaded, the game makes the offsets absolute addresses: dStage_dt_c_offsetToPtr; 0 = none)
            const uint32_t cnt = ld32(e + 4), raw = ld32(e + 8);
            const uint32_t off = raw >= hdr && raw - hdr <= 0x100000 ? raw - hdr : raw;
            if (off > 0x100000 || cnt > 4096) ok = false;
            offs.push_back({off, cnt});
            if (!memcmp(mem::ptr(e), "Pale", 4)) pale = hdr + off, np = cnt;
            if (!memcmp(mem::ptr(e), "Virt", 4)) vr = hdr + off, nv = cnt;
        }
        if (!ok || !pale || !np || np > 128 || nv > 128) continue;
        // (the sizes checked against the next chunk's start: HD's, not the GameCube's)
        for (auto& [off, cnt] : offs) {
            if (hdr + off > pale && (!pale_end || hdr + off < pale_end)) pale_end = hdr + off;
            if (vr && hdr + off > vr && (!vr_end || hdr + off < vr_end)) vr_end = hdr + off;
        }
        if (pale_end && pale + np * kPaleSize > pale_end) { LOG("[shipraid] sky: palettes at %08X do not fit %u bytes each", pale, kPaleSize); continue; }
        if (vr && vr_end && vr + nv * kVirtSize > vr_end) nv = 0, vr = 0;
        const float fs = f32(pale + 0x24), fe = f32(pale + 0x28);
        if (!std::isfinite(fs) || !std::isfinite(fe) || fe <= fs || fe > 1e7f) continue;
        Table x;
        x.pale = pale, x.npale = (int)np, x.vr = vr, x.nvr = (int)nv;
        x.pale_bak.assign(mem::ptr(pale), mem::ptr(pale) + np * kPaleSize);
        if (vr) x.vr_bak.assign(mem::ptr(vr), mem::ptr(vr) + nv * kVirtSize);
        LOG("[shipraid] sky: stage data at %08X: %u palettes, %u skies (palette 0 fog %u %u %u, %.0f..%.0f)", hdr, np, nv, ld8(pale + 0x1E),
            ld8(pale + 0x1F), ld8(pale + 0x20), fs, fe);
        t.t.push_back(std::move(x));
        if (t.t.size() >= 4) break;
    }
    return !t.t.empty();
}
uint8_t mix(uint8_t from, float to, float f) { return (uint8_t)std::clamp(from + (to - from) * f, 0.f, 255.f); }
void tint_put(const Table& x, float f) {
    const float h = f * 0.5f;  // (the fog half way: the horizon meets the red sky; the light on Link and the island stays)
    for (int i = 0; i < x.npale; i++) {
        const uint32_t e = x.pale + i * kPaleSize;
        const uint8_t* o = x.pale_bak.data() + i * kPaleSize;
        st8(e + 0x1E, mix(o[0x1E], 95, h)), st8(e + 0x1F, mix(o[0x1F], 12, h)), st8(e + 0x20, mix(o[0x20], 10, h));
        // the light on Link, the actors and the island (C0 / K0 of actors and BG0..3): dimmed to a red dusk, so the
        // beach is not sunlit under the red sky (user); weaker than the sky (user before: everything red was too much)
        static const float kDusk[3] = {0.72f, 0.40f, 0.36f};
        for (int k = 0; k < 0x1E; k++) st8(e + k, mix(o[k], o[k] * kDusk[k % 3], f));
    }
    static const uint8_t kSky[5][3] = {{125, 22, 16}, {170, 40, 24}, {70, 6, 6}, {45, 5, 5}, {115, 16, 12}};  // clouds, their middle, sky, far sea, haze
    static const int kAt[5] = {0x10, 0x14, 0x18, 0x1B, 0x1E};
    for (int v = 0; v < x.nvr; v++) {
        const uint32_t e = x.vr + v * kVirtSize;
        const uint8_t* o = x.vr_bak.data() + v * kVirtSize;
        for (int k = 0; k < 5; k++)
            for (int c = 0; c < 3; c++) st8(e + kAt[k] + c, mix(o[kAt[k] + c], kSky[k][c], f));
    }
}
// each step: towards `want` (1 dark red, 0 the game's) over `secs`; the originals back once at 0
void tint_step(float want, float secs) {
    Tint& t = g_tint;
    if (!t.t.empty() && !tint_ok(t)) t.t.clear(), t.f = 0;  // (another stage's data now: nothing to put back)
    if (want <= 0 && t.t.empty()) return void(t.f = 0);
    if (t.t.empty()) {
        if (t.tried) return;
        t.tried = true;
        if (!tint_find()) return void(LOG("[shipraid] sky: no stage palettes found"));
    }
    const float d = 1.f / (secs * 30);
    t.f = want > t.f ? std::min(want, t.f + d) : std::max(want, t.f - d);
    for (const Table& x : t.t) tint_put(x, t.f);
    if (t.f <= 0) {
        for (const Table& x : t.t) {
            memcpy(mem::ptr(x.pale), x.pale_bak.data(), x.pale_bak.size());
            if (!x.vr_bak.empty()) memcpy(mem::ptr(x.vr), x.vr_bak.data(), x.vr_bak.size());
        }
        t.t.clear();
    }
}

void start_here(Cpu* c, uint32_t link) {
    g.t0 = g.phase_at = net::now_ms();
    g.phase = kArriving;
    g_tint.tried = false;  // (look for the palettes again)
    float at[3];
    int16_t yaw;
    ship_now(g.t0, at, yaw);
    // subMode 0 (no path), no triforce chart, no switches; angle x: switch B none (0xFF), model 0
    g.ship = make(c, link, "Oship", 0xFFFFFF00u, at, 0x00FF, yaw), g.ship_at = net::now_ms();
    android_say::say(kIntro);
    ss::notice("A ship is coming! Get to the beach");
}
void stop(Cpu* c) {
    for (uint16_t s : g.sets) android_raid::remove(c, g.room, s);
    if (g.taker != 0xFFFF) android_raid::remove(c, g.room, g.taker);
    kill(c, g.granny), kill(c, g.carrier), kill(c, g.flag), kill(c, g.ship);
    g = Event{};
}
// one raider at the door: that raider (user: not a new Moblin) walks to the beach dragging Grandma behind it (she has
// no walk animation: she stands, pulled along; user's choice); the others go. A runner without the raider's set id
// (an older game) or a game without that raider: a Moblin made for it, on the old timeline.
void lose(Cpu* c, uint32_t link, uint16_t taker) {
    g.taker = taker != 0xFFFF && actor_of(taker) ? taker : 0xFFFF, g.taker_gone = 0;
    for (uint16_t s : g.sets) if (s != g.taker) android_raid::remove(c, g.room, s);
    g.sets.clear();
    g.phase = kKidnap, g.phase_at = net::now_ms();
    memcpy(g.carry, g.house, 12);
    const float at[3] = {g.house[0], g.house[1] + 20, g.house[2]};
    const int16_t yaw = yaw_to(g.land[0] - g.house[0], g.land[2] - g.house[2]);
    if (g.taker == 0xFFFF) g.carrier = make(c, link, "mo2", 0xFFFF0000u, at, 0, yaw);
    g.granny = make(c, link, "Ba1", 0xFFFFFF02u, at, 0, yaw);  // (type 2: her demo type, she just stands)
    ss::notice("The raiders broke into Grandma's house! They are carrying her off!");
    LOG("[shipraid] lost: Grandma carried off");
}
void win(Cpu* c) {
    for (uint16_t s : g.sets) android_raid::remove(c, g.room, s);
    g.sets.clear();
    if (g.taker != 0xFFFF) android_raid::remove(c, g.room, g.taker), g.taker = 0xFFFF;
    kill(c, g.carrier);
    g.won = true;
    g.phase = kLeaving, g.phase_at = net::now_ms();
    android_say::say(kWin);
    if (g.runner) {  // the fork's server: the event quest (an upstream server drops it)
        online::proto::Bytes b;
        online::proto::Writer w(b);
        w.u8(fork_proto::kEvent), w.str8("shipraid"), w.u8(1);
        mods::coop::net::srv::android_fork_send(std::string(b.begin(), b.end()));
    }
    LOG("[shipraid] won: the ship flees");
}

void received_now(Cpu* c, uint32_t link, const uint8_t* d, size_t n) {
    const uint8_t op = d[2];
    if (op == kStart && n >= 3 + 8 + 1 + 4 * 11) {
        stop(c);
        memcpy(g.stage, d + 3, 8), g.room = (int8_t)d[11];
        const float* f = (const float*)(d + 12);
        float v[11];
        memcpy(v, f, sizeof v);
        memcpy(g.house, v, 12), memcpy(g.land, v + 3, 12), memcpy(g.anchor, v + 6, 12), memcpy(g.out, v + 9, 8);
        const float me[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
        if (memcmp(g.stage, stage(), 8) || dist2d(me, g.house) > 12000) {  // (elsewhere: the ship made when this game gets there)
            g.t0 = g.phase_at = net::now_ms(), g.phase = kArriving;
            ss::notice("Raiders are attacking Outset Island! Go and help!");
            LOG("[shipraid] started elsewhere: joining when here");
            return;
        }
        start_here(c, link);
    } else if (op == kWave && g.phase != kOff && n >= 5) {
        g.wave = d[3];
        g.sets.clear();
        for (int i = 0; i < d[4] && 5 + 2 * i + 1 < (int)n; i++) g.sets.push_back((uint16_t)(d[5 + 2 * i] | d[6 + 2 * i] << 8));
        ss::notice("Wave " + std::to_string(g.wave) + " of " + std::to_string(kWaves) + ": " + std::to_string(d[4]) + " raiders come ashore");
    } else if (op == kLost && g.phase != kOff && g.phase != kKidnap && g.phase != kLeaving) {
        lose(c, link, n >= 5 ? (uint16_t)(d[3] | d[4] << 8) : 0xFFFF);
    } else if (op == kWon && g.phase != kOff && g.phase != kLeaving) {
        win(c);
    }
}
}  // namespace

void ask() { g_ask = true; }
void received(int, const uint8_t* d, size_t n) {
    if (n >= 3 && d[0] == 'W' && d[1] == 'B' && g_in.size() < 16) g_in.emplace_back((const char*)d, n);
}

// the point `ahead` further along the way (landing -> door) than `m` is (its foot on the way), at most the door
void on_way(const float m[3], float ahead, float out[3]) {
    const float wx = g.house[0] - g.land[0], wz = g.house[2] - g.land[2], wl = std::hypot(wx, wz) + 0.01f;
    const float ux = wx / wl, uz = wz / wl;
    const float at = std::clamp((m[0] - g.land[0]) * ux + (m[2] - g.land[2]) * uz + ahead, 0.f, wl);
    out[0] = g.land[0] + ux * at, out[1] = m[1], out[2] = g.land[2] + uz * at;
}

// fopAc_Execute (cheats.cpp's hook), before the actor's logic: our ship / Grandma / kidnapper put where the timeline
// says; a wave's monster with no player near goes for the door (Link stands ahead of it: `was` holds his place, put
// back by unmarch); true: Link was moved
bool before(uint32_t a, float was[3]) {
    if (g.phase == kOff) return false;
    const uint32_t pid = ld32(a + kPid);
    const uint32_t now = net::now_ms();
    if (pid == g.ship) {
        float p[3];
        int16_t yaw;
        ship_now(now, p, yaw);
        stf(a + kPos, p[0]), stf(a + kPos + 8, p[2]);
        st16(a + kAngle + 2, (uint16_t)yaw), st16(a + kShape + 2, (uint16_t)yaw);
        st8(a + kHealth, 3);  // (it cannot be sunk)
        return false;
    }
    if (pid == g.carrier) {
        stf(a + kPos, g.carry[0]), stf(a + kPos + 8, g.carry[2]);
        const int16_t yaw = yaw_to(g.land[0] - g.house[0], g.land[2] - g.house[2]);
        st16(a + kAngle + 2, (uint16_t)yaw), st16(a + kShape + 2, (uint16_t)yaw);
        st8(a + kHealth, 20);
        return false;
    }
    if (pid == g.granny) {  // dragged along right behind her kidnapper, facing the way it goes (no fall, no speed)
        float k[3] = {g.carry[0], g.carry[1], g.carry[2]};
        if (const uint32_t t = g.taker != 0xFFFF ? actor_of(g.taker) : 0) k[0] = f32(t + kPos), k[1] = f32(t + kPos + 4), k[2] = f32(t + kPos + 8);
        if (g.phase == kKidnap) {
            const float dx = g.land[0] - k[0], dz = g.land[2] - k[2], d = std::hypot(dx, dz) + 0.01f;
            const float bob = 4 * std::fabs(std::sin(now * 0.012f));  // (her steps, pulled)
            stf(a + kPos, k[0] - dx / d * 110), stf(a + kPos + 4, k[1] + bob), stf(a + kPos + 8, k[2] - dz / d * 110);
            const int16_t yaw = yaw_to(dx, dz);
            st16(a + kAngle + 2, (uint16_t)yaw), st16(a + kShape + 2, (uint16_t)yaw);
        }
        stf(a + kSpeed, 0), stf(a + kSpeed + 4, 0), stf(a + kSpeed + 8, 0);
        return false;
    }
    if (g.phase == kKidnap && g.taker != 0xFFFF && ld8(a + kGroup) == 2 && ld16(a + kSetId) == g.taker &&
        (int8_t)ld8(a + kHomeRoom) == g.room) {  // the kidnapper: down the way to the beach, no fighting on the way
        const uint32_t link = ld32(kPlay + names::inf::kLink);
        if (!valid(link)) return false;
        const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
        for (int e = 0; e < 3; e++) was[e] = f32(link + kPos + 4 * e);
        float t[3];
        on_way(m, -kCarrot, t);
        if (dist2d(t, m) < 50) t[0] = g.land[0], t[2] = g.land[2];
        stf(link + kPos, t[0]), stf(link + kPos + 4, m[1]), stf(link + kPos + 8, t[2]);
        stf(a + kHome, t[0]), stf(a + kHome + 4, m[1]), stf(a + kHome + 8, t[2]);
        return true;
    }
    if (g.sets.empty() || ld8(a + kGroup) != 2 || (int8_t)ld8(a + kHomeRoom) != g.room) return false;
    if (std::find(g.sets.begin(), g.sets.end(), ld16(a + kSetId)) == g.sets.end()) return false;
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (!valid(link)) return false;
    const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
    for (int e = 0; e < 3; e++) was[e] = f32(link + kPos + 4 * e);
    if (dist2d(m, was) < kEngage) return false;
    float p[3];
    for (int id = 0; id < net::kMaxPlayers; id++)
        if (id != net::my_id() && sync::place(id, p) && dist2d(m, p) < kEngage) return false;
    // Link put on the way (the line from the landing to the door: the beach search walked it, dry and without cliffs),
    // kCarrot further along it than the raider is: the raiders come onto the way and follow it up (user: they stood
    // around on the beach going straight for the door)
    float t[3];
    on_way(m, kCarrot, t);
    stf(link + kPos, t[0]), stf(link + kPos + 4, m[1]), stf(link + kPos + 8, t[2]);
    // its home (where a monster with no one to fight walks back to) moved along too: else they turned back to the
    // beach where they were made (user: they were dancing around on the beach)
    stf(a + kHome, t[0]), stf(a + kHome + 4, m[1]), stf(a + kHome + 8, t[2]);
    return true;
}
void unmarch(const float was[3]) {
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    for (int e = 0; e < 3; e++) stf(link + kPos + 4 * e, was[e]);
}

// a marching raider not nearer the door for 4 s (a fence, a tree, a step too high) is moved 150 on along the way
void unstick(Cpu* c, uint32_t link, uint32_t now) {
    g.prog.erase(std::remove_if(g.prog.begin(), g.prog.end(),
                                [](const Event::Prog& p) { return std::find(g.sets.begin(), g.sets.end(), p.set) == g.sets.end(); }),
                 g.prog.end());
    for (uint16_t s : g.sets) {
        const uint32_t a = actor_of(s);
        if (!a) continue;
        const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
        const float d = dist2d(m, g.house);
        auto it = std::find_if(g.prog.begin(), g.prog.end(), [s](const Event::Prog& p) { return p.set == s; });
        if (it == g.prog.end()) { g.prog.push_back({s, d, now}); continue; }
        // (fighting a player: not stuck)
        const float me[3] = {f32(link + kPos), 0, f32(link + kPos + 8)};
        bool fighting = dist2d(m, me) < kEngage;
        float p[3];
        for (int id = 0; id < net::kMaxPlayers && !fighting; id++)
            if (id != net::my_id() && sync::place(id, p) && dist2d(m, p) < kEngage) fighting = true;
        if (fighting || d < it->best - 100) { it->best = std::min(it->best, d), it->at = now; continue; }
        if (now - it->at < 4000 || d < kBreachAt) continue;
        float t[3];
        on_way(m, 150, t);
        const float y = ground(c, link, t[0], std::max(m[1], g.house[1]) + 300, t[2]);
        it->at = now;
        if (y < -1e8f) continue;
        stf(a + kPos, t[0]), stf(a + kPos + 4, y), stf(a + kPos + 8, t[2]);
        it->best = dist2d(t, g.house);
        LOG("[shipraid] raider %u stuck at %.0f from the door: moved on to %.0f %.0f %.0f", s, d, t[0], y, t[2]);
    }
}

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    if (net::role() == net::Role::kOff) { if (g.phase != kOff) stop(c); g_in.clear(); tint_step(0, 1); return; }
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (!valid(link)) return;
    const int room = (int8_t)ld8(link + kRoom);
    // after a stage change (a new Link, the next stage asked for) 2 s before anything is made or moved
    if (link != g_last_link || ld8(kPlay + names::inf::kNextStageEnabled)) g_settle = 60, g_last_link = link;
    else if (g_settle > 0) g_settle--;
    if (g_settle > 0) return;
    for (const std::string& m : g_in) received_now(c, link, (const uint8_t*)m.data(), m.size());
    g_in.clear();
    if (g_ask) {
        g_ask = false;
        if (g.phase != kOff) return ss::notice("The raiders are already here");
        if (!near_outset(link)) return ss::notice("Start it on Outset Island");
        g = Event{};
        g.runner = true, g.room = room;
        memcpy(g.stage, stage(), 8);
        memcpy(g.house, kGrandmaDoor, 12);
        if (!find_beach(c, link) && !find_beach(c, link, 120)) { g = Event{}; LOG("[shipraid] no beach found"); return ss::notice("No beach found for the raiders"); }
        LOG("[shipraid] started: door %.0f %.0f %.0f, landing %.0f %.0f %.0f, anchorage %.0f %.0f %.0f", g.house[0], g.house[1], g.house[2],
            g.land[0], g.land[1], g.land[2], g.anchor[0], g.anchor[1], g.anchor[2]);
        float v[11];
        memcpy(v, g.house, 12), memcpy(v + 3, g.land, 12), memcpy(v + 6, g.anchor, 12), memcpy(v + 9, g.out, 8);
        send(std::string(1, (char)kStart) + std::string(g.stage, 8) + (char)g.room + fbytes(v, 11));
        start_here(c, link);
        return;
    }
    if (g.phase == kOff) return tint_step(0, 6);
    const float me[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
    if (memcmp(g.stage, stage(), 8) || dist2d(me, g.house) > 12000) { tint_step(0, 2); return; }  // (away: the actors went with the room)
    const uint32_t now = net::now_ms();
    // the sky: dark red while the ship comes in, back when the raiders are beaten or the ship is gone
    if (g.won) tint_step(0, 8);
    else tint_step(1, 20);
    // the ship not here (this game came later, or left and came back: it went with the room): made where it is now
    if (g.phase != kOff && now - g.ship_at > 3000 && !by_pid(c, g.ship)) {
        float at[3];
        int16_t yaw;
        ship_now(now, at, yaw);
        g.ship = make(c, link, "Oship", 0xFFFFFF00u, at, 0x00FF, yaw), g.ship_at = now, g.flag_looked = false;
        LOG("[shipraid] the ship made here (late or back)");
    }
    // the ship's flag (made by the ship itself at its place: taken away with it)
    if (!g.flag_looked) {
        if (const uint32_t s = by_pid(c, g.ship)) {
            uint32_t n = ld32(kActorQueue);
            for (int k = 0; valid(n) && k < 4096; k++, n = ld32(n + 8)) {
                const uint32_t a = ld32(n + 0xC);
                if (!a || a == s || ld16(a + kSetId) != 0xFFFF) continue;
                const float p[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)}, q[3] = {f32(s + kPos), f32(s + kPos + 4), f32(s + kPos + 8)};
                if (dist2d(p, q) < 30 && ld32(a + kPid) > ld32(s + kPid) && ld32(a + kPid) < ld32(s + kPid) + 8) {
                    g.flag = ld32(a + kPid);
                    LOG("[shipraid] the ship's flag: proc %#x", ld16(a + kProcName));
                    break;
                }
            }
            g.flag_looked = true;
        }
    }
    if (g.phase == kArriving && now - g.t0 >= kSailIn) {
        g.phase = kAnchored, g.phase_at = now;
        g.wave_at = now + 3000;
        ss::notice("The raiders' ship has dropped anchor!");
    }
    if (g.phase == kKidnap && g.taker != 0xFFFF) {  // the raider walks her to the beach; there she is taken aboard
        const uint32_t k = actor_of(g.taker);
        if (!k || ld8(k + kHealth) == 0) {  // beaten on the way: Grandma saved (the runner decides for the party)
            if (!g.taker_gone) g.taker_gone = now;
            if (g.runner && now - g.taker_gone > 1500) {
                LOG("[shipraid] the kidnapper was beaten: Grandma saved");
                send(std::string(1, (char)kWon));
                win(c);
            }
            return;
        }
        g.taker_gone = 0;
        const float m[3] = {f32(k + kPos), f32(k + kPos + 4), f32(k + kPos + 8)};
        if (!by_pid(c, g.granny)) g.granny = make(c, link, "Ba1", 0xFFFFFF02u, m, 0, 0);  // (gone with a reload: again)
        if (dist2d(m, g.land) < 200 || now - g.phase_at > 90000) {  // (90 s: never stuck)
            kill(c, g.granny), android_raid::remove(c, g.room, g.taker), g.taker = 0xFFFF;
            ss::notice("Grandma was dragged aboard the raiders' ship!");
            g.phase = kLeaving, g.phase_at = now;
        }
        return;
    }
    if (g.phase == kKidnap) {  // the kidnapper walks Grandma to the beach; there she is taken aboard
        const float dx = g.land[0] - g.carry[0], dz = g.land[2] - g.carry[2], d = std::hypot(dx, dz);
        if (d > kKidnapSpeed) {
            g.carry[0] += dx / d * kKidnapSpeed, g.carry[2] += dz / d * kKidnapSpeed;
            if (const uint32_t k = by_pid(c, g.carrier)) g.carry[1] = f32(k + kPos + 4);
        } else {
            kill(c, g.granny), kill(c, g.carrier);
            ss::notice("Grandma was dragged aboard the raiders' ship!");
            g.phase = kLeaving, g.phase_at = now;
        }
        if (now - g.phase_at > 60000) g.carry[0] = g.land[0], g.carry[2] = g.land[2];  // (never stuck)
        return;
    }
    if (g.phase == kLeaving) {
        if (now - g.phase_at >= kTurn + kSailOut) {
            if (!g.won) android_say::say(kLose);
            LOG("[shipraid] over: the ship is gone");
            stop(c);
        }
        return;
    }
    if (g.phase == kAnchored) unstick(c, link, now);
    if (!g.runner || g.phase != kAnchored) return;
    // the runner: a raider at the door, the next wave, the end
    for (uint16_t s : g.sets) {
        const uint32_t a = actor_of(s);
        if (!a) continue;
        const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
        if (dist2d(m, g.house) > kBreachAt || std::fabs(m[1] - g.house[1]) > 300) continue;
        LOG("[shipraid] raider %u reached the door", s);
        send(std::string(1, (char)kLost) + (char)(s & 0xFF) + (char)(s >> 8));
        return lose(c, link, s);
    }
    const bool wave_left = std::any_of(g.sets.begin(), g.sets.end(), [](uint16_t s) { return android_raid::alive(g.room, s); });
    if (wave_left) return void(g.quiet_since = 0);
    if (!g.quiet_since) {
        g.quiet_since = now;
        if (g.wave > 0) g.wave_at = std::max(g.wave_at, now + 6000);
        if (g.wave > 0 && g.wave < kWaves) ss::notice("Wave " + std::to_string(g.wave) + " beaten! More are coming from the ship");
    }
    if ((int32_t)(now - g.wave_at) < 0) return;
    if (g.wave >= kWaves) {
        send(std::string(1, (char)kWon));
        return win(c);
    }
    g.wave++;
    auto list = wave_list(c, link, g.wave, party_here());
    g.sets = android_raid::spawn(c, link, list);
    std::string b = std::string(1, (char)kWave) + (char)g.wave + (char)g.sets.size();
    for (uint16_t s : g.sets) b += (char)(s & 0xFF), b += (char)(s >> 8);
    send(b);
    ss::notice("Wave " + std::to_string(g.wave) + " of " + std::to_string(kWaves) + ": " + std::to_string(g.sets.size()) + " raiders come ashore");
    LOG("[shipraid] wave %d: %zu raiders", g.wave, g.sets.size());
    g.quiet_since = 0;
}
}  // namespace android_shipraid
