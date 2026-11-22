// (fork: Android) Online event "Outset under siege" (a test of the raid ideas, notes/online-modes.md): monsters come
// ashore in 4 waves and march on Grandma's house; the party has to stop them before they get there.
//
// Started from Game > Online by a player standing at Grandma's door (that place is the house: HD keeps Outset's stage
// data in its packs, so no coordinates are guessed). That game runs the event (the runner) and tells the party
// (kSiegeMsg): the start, each wave with its monsters' set ids, each breach, the end. The monsters come through the
// spawn service (android_raid.cpp: made on every game, shared by coop_monsters, the same animations by android_fight),
// at places the party has walked on (so never in the sea or a cliff; else a ring around the house).
// Marching: a wave's monster with no player within 800 units goes for the house: while its logic runs (fopAc_Execute,
// cheats.cpp's hook), Link stands 250 units ahead of it on the way to the house, so it walks there as it would go for
// him (upstream's stand-in, coop_world, goes for the nearest player the same way); with a player near it fights.
// A monster within 250 of the house is a breach: taken away everywhere, the house loses a heart (10). All 4 waves
// beaten: the house is safe (the fork's server counts the quest "Outset under siege"); its hearts gone: it fell.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "../mods/coop_deck.h"
#include "../mods/coop_monsters.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "../mods/coop_sync.h"
#include "fork_proto.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "proto/wire.h"
#include "runtime.h"

namespace ss { void notice(const std::string& text); }
namespace android_say { void say(const std::string& text); }  // (the game's own message window)
namespace mods::coop::net::srv { bool android_fork_send(const std::string& body); }
namespace android_raid {
struct SpawnReq { const char* name; uint32_t params; float at[3]; int16_t yaw; };
std::vector<uint16_t> spawn(Cpu* c, uint32_t link, const std::vector<SpawnReq>& list);
bool alive(int room, uint16_t set);
void remove(Cpu* c, int room, uint16_t set);
}

namespace android_siege {
constexpr mods::coop::net::Msg kSiegeMsg = (mods::coop::net::Msg)0x68;  // (the fork's) runner -> party: 'W' 'G', op, ...
namespace {
namespace net = mods::coop::net;
namespace sync = mods::coop::sync;
namespace names = mods::coop::names;

enum Op : uint8_t { kStart = 1, kWave = 2, kBreach = 3, kEnd = 4 };
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage, kActorQueue = GD(0x101F3328);
constexpr uint32_t kPos = 0x314, kRoom = 0x326, kSetId = 0x2D8, kHomeRoom = 0x2FE, kGroup = 0x2DA;
constexpr int kWaves = 4, kHouseHearts = 10;
const char* const kIntro = "Outset Island is overrun by monsters!\nThey are marching on Grandma's house.\nStop them before they get there!";
// (kCarrot: Link is put that far ahead of a marching monster; out of its swing range, else it swings while marching
// and hits the monster in front of it: their weapons hit monsters too, GrpAll; user: mobs fighting each other)
constexpr uint32_t kHome = 0x2EC;  // fopAc_ac_c::home.pos (HD)
constexpr float kBreachAt = 250, kEngage = 800, kCarrot = 450;

struct Event {
    bool on = false, runner = false;
    char stage[8] = {};
    int room = 0, wave = 0, hearts = kHouseHearts;
    float house[3] = {};
    std::vector<uint16_t> sets;  // this wave's monsters
    uint32_t wave_at = 0, quiet_since = 0;
} g;
std::vector<uint16_t> pending_remove;  // breaches the runner told: taken away here at the next step
int end_pending = 0;                   // the runner's end: 1 won, 2 lost
std::vector<std::array<float, 3>> g_walked;  // where the party has walked here (200-unit grid)
char g_walked_stage[8];
int g_walked_room = -999;

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
float dist2d(const float* a, const float* b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

void send(const std::string& body) { net::send(-1, kSiegeMsg, std::string("WG") + body); }
std::string f3(const float* v) { return std::string((const char*)v, 12); }

// a place to come ashore: on solid ground, clearly above the sea here (deck::water: the sea's height at x, z)
Cpu* g_cpu = nullptr;
bool dry(const float p[3]) {
    float sea = 0;
    return !g_cpu || !mods::coop::deck::water(g_cpu, p[0], p[2], sea) || p[1] > sea + 40;
}
void walked(const float p[3]) {
    if (!dry(p)) return;
    for (const auto& w : g_walked) if (std::fabs(w[0] - p[0]) < 200 && std::fabs(w[2] - p[2]) < 200) return;
    if (g_walked.size() < 2000) g_walked.push_back({p[0], p[1], p[2]});
}

// the monsters of a wave: their kinds (stage data names, upstream's native enemy parameters), more with a bigger party
std::vector<android_raid::SpawnReq> wave_list(int wave, int party) {
    struct K { const char* name; uint32_t params; };
    const K bk{"Bk", 0xFFFFFF04u}, mo{"mo2", 0xFFFF0000u}, cc{"c_green", 0xFF000000u}, ki{"keeth", 0xFFFF0000u}, tn{"Tn", 0xFFFF9604u};
    std::vector<K> ks;
    const int extra = std::min(4, (party - 1) * 2);
    if (wave == 1) ks = {bk, bk, bk, cc, cc, cc};
    if (wave == 2) ks = {bk, bk, bk, bk, mo, cc, cc, ki};
    if (wave == 3) ks = {mo, mo, bk, bk, bk, ki, ki, cc, cc};
    if (wave == 4) ks = {tn, mo, mo, bk, bk, bk, bk, ki, ki};
    for (int i = 0; i < extra; i++) ks.push_back(i % 2 ? mo : bk);
    std::vector<android_raid::SpawnReq> out;
    for (const K& k : ks) out.push_back({k.name, k.params, {0, 0, 0}, 0});
    return out;
}
// where they come ashore: places walked on, 900..3500 from the house, spread out; else a ring of 700 around it
void place(std::vector<android_raid::SpawnReq>& list, int wave) {
    std::vector<std::array<float, 3>> far;
    for (const auto& w : g_walked) {
        const float d = dist2d(w.data(), g.house);
        if (d > 900 && d < 3500) far.push_back(w);
    }
    for (size_t i = 0; i < list.size(); i++) {
        float* at = list[i].at;
        if (far.size() >= 3) {
            const auto& w = far[(i * 7 + wave * 13) % far.size()];
            const float a = i * 2.39996f;  // (a little apart around that place)
            at[0] = w[0] + 60 * std::sin(a), at[1] = w[1] + 40, at[2] = w[2] + 60 * std::cos(a);
        } else {
            const float a = i * 6.2831853f / list.size() + wave;
            at[0] = g.house[0] + 700 * std::sin(a), at[1] = g.house[1] + 40, at[2] = g.house[2] + 700 * std::cos(a);
        }
        if (list[i].name[0] == 'k') at[1] += 150;  // (Keese fly)
        list[i].yaw = (int16_t)(std::atan2(g.house[0] - at[0], g.house[2] - at[2]) * 32768 / 3.14159265f);
    }
}
int party_here() {
    int n = 1;
    float p[3];
    for (int id = 0; id < net::kMaxPlayers; id++) if (id != net::my_id() && net::member(id) && sync::place(id, p)) n++;
    return n;
}
uint32_t actor_of(uint16_t set) {
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; n >= 0x10000000 && n < 0x50000000 && k < 4096; k++, n = ld32(n + 8)) {
        const uint32_t a = ld32(n + 0xC);
        if (a && ld8(a + kGroup) == 2 && ld16(a + kSetId) == set && (int8_t)ld8(a + kHomeRoom) == g.room) return a;
    }
    return 0;
}
void end(Cpu* c, bool won) {
    for (uint16_t s : g.sets) android_raid::remove(c, g.room, s);
    if (g.runner) {
        send(std::string(1, (char)kEnd) + (char)(won ? 1 : 0));
        if (won) {  // the fork's server: the event quest (an upstream server drops it)
            online::proto::Bytes b;
            online::proto::Writer w(b);
            w.u8(fork_proto::kEvent), w.str8("siege"), w.u8(1);
            mods::coop::net::srv::android_fork_send(std::string(b.begin(), b.end()));
        }
    }
    android_say::say(won ? "You did it! Grandma's house is safe.\nThe monsters are gone from Outset Island.\nGrandma will be so proud of you all!"
                         : "Grandma's house has fallen...\nThe monsters got through.\nGather your friends and try again!");
    LOG("[siege] over: %s", won ? "won" : "lost");
    g = Event{};
}
}  // namespace

// the places walked on here between `lo` and `hi` units of `at` (spawn places for "Monsters here now")
std::vector<std::array<float, 3>> walked_near(const float at[3], float lo, float hi) {
    std::vector<std::array<float, 3>> out;
    for (const auto& w : g_walked) { const float d = dist2d(w.data(), at); if (d >= lo && d <= hi) out.push_back(w); }
    return out;
}
// a place clearly above the sea (deck::water)
bool dry_place(Cpu* c, const float p[3]) { g_cpu = c; return dry(p); }

// the Online page (any thread): start at the next step, here
bool g_ask = false;
void ask() { g_ask = true; }

// the runner's words (the session's poll, game thread)
void received(int, const uint8_t* d, size_t n) {
    if (n < 3 || d[0] != 'W' || d[1] != 'G') return;
    online::proto::Reader r(d + 3, n - 3);
    const uint8_t op = d[2];
    if (op == kStart && n >= 3 + 8 + 1 + 12) {
        g = Event{};
        g.on = true;
        memcpy(g.stage, d + 3, 8), g.room = (int8_t)d[11], memcpy(g.house, d + 12, 12);
        android_say::say(kIntro);
    } else if (op == kWave && g.on && n >= 5) {
        g.wave = d[3];
        const int count = d[4];
        g.sets.clear();
        for (int i = 0; i < count && 5 + 2 * i + 1 < (int)n; i++) g.sets.push_back((uint16_t)(d[5 + 2 * i] | d[6 + 2 * i] << 8));
        ss::notice("Wave " + std::to_string(g.wave) + " of " + std::to_string(kWaves) + ": " + std::to_string(count) + " monsters");
    } else if (op == kBreach && g.on && n >= 6) {
        const uint16_t set = (uint16_t)(d[3] | d[4] << 8);
        g.hearts = d[5];
        g.sets.erase(std::remove(g.sets.begin(), g.sets.end(), set), g.sets.end());
        ss::notice("A monster reached Grandma's house! " + std::to_string(g.hearts) + " hearts left");
        pending_remove.push_back(set);
    } else if (op == kEnd && g.on && n >= 4) {
        end_pending = d[3] ? 1 : 2;
    }
}

// fopAc_Execute (cheats.cpp's hook): a wave's monster with no player near goes for the house; true: Link was moved
// (`was` holds his place: put back after)
bool march(uint32_t a, float was[3]) {
    if (!g.on || g.sets.empty() || ld8(a + kGroup) != 2 || (int8_t)ld8(a + kHomeRoom) != g.room) return false;
    if (std::find(g.sets.begin(), g.sets.end(), ld16(a + kSetId)) == g.sets.end()) return false;
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (link < 0x10000000u || link >= 0x50000000u) return false;
    const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
    for (int e = 0; e < 3; e++) was[e] = f32(link + kPos + 4 * e);
    if (dist2d(m, was) < kEngage) return false;
    float p[3];
    for (int id = 0; id < net::kMaxPlayers; id++)
        if (id != net::my_id() && sync::place(id, p) && dist2d(m, p) < kEngage) return false;
    const float dx = g.house[0] - m[0], dz = g.house[2] - m[2], d = std::hypot(dx, dz) + 0.01f;
    const float step = std::min(kCarrot, d);
    st32(link + kPos, bits(m[0] + dx / d * step)), st32(link + kPos + 4, bits(m[1])), st32(link + kPos + 8, bits(m[2] + dz / d * step));
    // its home (where a monster with no one to fight walks back to) moved along too, else it turns back
    st32(a + kHome, bits(m[0] + dx / d * step)), st32(a + kHome + 4, bits(m[1])), st32(a + kHome + 8, bits(m[2] + dz / d * step));
    return true;
}
void unmarch(float was[3]) {
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    for (int e = 0; e < 3; e++) st32(link + kPos + 4 * e, bits(was[e]));
}

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    if (net::role() == net::Role::kOff) return void(g = Event{});
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (link < 0x10000000u || link >= 0x50000000u) return;
    // where the party walks here (spawn places)
    const int room = (int8_t)ld8(link + kRoom);
    if (memcmp(g_walked_stage, stage(), 8) || room != g_walked_room) memcpy(g_walked_stage, stage(), 8), g_walked_room = room, g_walked.clear();
    {
        g_cpu = c;
        const float me[3] = {f32(link + kPos), f32(link + kPos + 4), f32(link + kPos + 8)};
        // (Link on his feet: not swimming, not on the boat; daPy_lk_c mModeFlg SWIM, mCurProc's ship procs)
        const uint32_t mode = ld32(link + names::py::mModeFlg), proc = ld32(link + names::py::mCurProc);
        if (!(mode & 0x40000) && !(proc >= 0x86 && proc <= 0x91)) walked(me);
        float p[3];
        for (int id = 0; id < net::kMaxPlayers; id++) if (id != net::my_id() && net::member(id) && sync::place(id, p)) walked(p);
    }
    for (uint16_t s : pending_remove) android_raid::remove(c, g.room, s);
    pending_remove.clear();
    if (end_pending) { const bool won = end_pending == 1; end_pending = 0; if (g.on) end(c, won); }
    if (g_ask) {
        g_ask = false;
        if (g.on) return ss::notice("The siege is already on");
        if (!near_outset(link)) return ss::notice("Start it on Outset Island");
        g = Event{};
        g.on = true, g.runner = true, g.room = room;
        memcpy(g.stage, stage(), 8);
        memcpy(g.house, kGrandmaDoor, 12);
        send(std::string(1, (char)kStart) + std::string(g.stage, 8) + (char)g.room + f3(g.house));
        android_say::say(kIntro);
        LOG("[siege] started at %.0f %.0f %.0f (room %d), %zu walked places known", g.house[0], g.house[1], g.house[2], g.room, g_walked.size());
        g.wave_at = net::now_ms() + 8000;  // (the first wave after 8 s: time to read the window)
        return;
    }
    if (!g.on || memcmp(g.stage, stage(), 8) || room != g.room) return;
    if (!g.runner) return;
    // the runner: breaches, the next wave, the end
    for (uint16_t s : std::vector<uint16_t>(g.sets)) {
        const uint32_t a = actor_of(s);
        if (!a) continue;
        const float m[3] = {f32(a + kPos), f32(a + kPos + 4), f32(a + kPos + 8)};
        if (dist2d(m, g.house) > kBreachAt || std::fabs(m[1] - g.house[1]) > 300) continue;
        g.hearts--;
        g.sets.erase(std::remove(g.sets.begin(), g.sets.end(), s), g.sets.end());
        android_raid::remove(c, g.room, s);
        send(std::string(1, (char)kBreach) + (char)(s & 0xFF) + (char)(s >> 8) + (char)std::max(0, g.hearts));
        ss::notice("A monster reached Grandma's house! " + std::to_string(std::max(0, g.hearts)) + " hearts left");
        LOG("[siege] breach by %u: %d hearts left", s, g.hearts);
        if (g.hearts <= 0) return end(c, false);
    }
    const bool wave_left = std::any_of(g.sets.begin(), g.sets.end(), [](uint16_t s) { return android_raid::alive(g.room, s); });
    const uint32_t now = net::now_ms();
    if (wave_left) return void(g.quiet_since = 0);
    if (!g.quiet_since) {  // (a wave beaten: the next 5 s later)
        g.quiet_since = now;
        if (g.wave > 0) g.wave_at = std::max(g.wave_at, now + 5000);
        if (g.wave > 0 && g.wave < kWaves) ss::notice("Wave " + std::to_string(g.wave) + " beaten!");
    }
    if ((int32_t)(now - g.wave_at) < 0) return;
    if (g.wave >= kWaves) return end(c, true);
    g.wave++;
    auto list = wave_list(g.wave, party_here());
    place(list, g.wave);
    g.sets = android_raid::spawn(c, link, list);
    std::string b = std::string(1, (char)kWave) + (char)g.wave + (char)g.sets.size();
    for (uint16_t s : g.sets) b += (char)(s & 0xFF), b += (char)(s >> 8);
    send(b);
    ss::notice("Wave " + std::to_string(g.wave) + " of " + std::to_string(kWaves) + ": " + std::to_string(g.sets.size()) + " monsters");
    LOG("[siege] wave %d: %zu monsters", g.wave, g.sets.size());
    g.quiet_since = 0;
}
}  // namespace android_siege
