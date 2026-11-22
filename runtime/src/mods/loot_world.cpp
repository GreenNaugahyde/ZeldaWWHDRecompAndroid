// Loot found in the world (android LootWorld, notes/loot.md): enemies going down, hits landed near Link and the chests. No hooks
// of its own: coop::world::step runs it in the game's update. An enemy is any actor of the enemy group (its health at +0x3A1): its
// health falling near Link counts as a hit (wear), reaching zero as a defeat (a chance of a drop, rolled by the app). A chest is the
// game's own (daTbox_c) made at run time; it needs a number of the stage's 32 "opened" bits, so it takes one nothing here uses and the
// bit is cleared again as soon as it opens: the app keeps which were opened, the save stays as the game wrote it.
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "coop_emotes.h"
#include "coop_items.h"
#include "coop_names.h"
#include "mods.h"
#include "../guest_addr.h"
#include "../runtime.h"
#include "../savestate.h"

namespace mods {
namespace {
namespace ac = coop::names::ac;
namespace inf = coop::names::inf;
constexpr uint32_t kProcName = 0x08, kGroup = 0x2DA, kSetId = 0x2D8, kHomeRoom = 0x2FE, kParams = 0xB0, kHome = 0x2EC;
constexpr uint8_t kEnemy = 2;
constexpr float kNearHit = 400, kNearKill = 4000, kOpenNear = 500;
constexpr int kSpotsAfter = 90, kSpawnAfter = 100;  // steps in a room
constexpr uint32_t kItemBlueRupee = 2;
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + inf::kCurStage, kActorQueue = GD(0x101F3328), kObjNames = GD(0x101EBFDC), kSaveInfo = GD(0x101F84DC);
constexpr uint32_t kLiveTbox = 0x778, kRupee = 4;
uint32_t info() { return ld32(kSaveInfo) + 0x20; }
uint32_t rupee_change() { return kPlay + (release::id() == release::Id::JPN ? 0x5B48 : 0x5B4C) - 4; }

struct Chest { int key, room, yaw, flags; float pos[3]; uint32_t id; int no; bool opened; };

std::atomic<bool> g_on{false};
std::atomic<int> g_pay{0};
std::atomic<bool> g_test_enemy{false};
std::mutex g_m;
std::vector<float> g_kills, g_spots;
std::vector<int> g_opened;
int g_hits = 0, g_scene = 0, g_tick = 0;
float g_link[3] = {};
int g_yaw = 0;
std::string g_where, g_chest_stage;
std::vector<Chest> g_chests;

std::unordered_map<uint32_t, int8_t> g_hp;
unsigned g_loads = ~0u;
uint32_t g_was_link = 0;
char g_was_stage[9];
int g_room = -2, g_steps = 0;

int16_t proc_of(const char* name) {
    for (uint32_t e = kObjNames; e < kObjNames + 12 * 2000 && ld8(e); e += 12)
        if (!strncmp((const char*)mem::ptr(e), name, 8)) return (int16_t)ld16(e + 8);
    return -1;
}
int16_t tbox_proc() {
    static const int16_t proc = proc_of("takara");
    return proc;
}
float f32(uint32_t a) { return (float)ldf32(a); }
template <class F> void each_actor(F f) {  // g_fopAcTg_Queue: node next +8, actor +0xC
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; n >= 0x10000000 && n < 0x50000000 && k < 4096; k++, n = ld32(n + 8))
        if (const uint32_t a = ld32(n + 0xC)) f(a);
}

void enemies(uint32_t link, const float at[3], bool settled) {
    std::unordered_map<uint32_t, int8_t> now;
    std::vector<float> spots;
    int hits = 0;
    std::vector<float> kills;
    each_actor([&](uint32_t a) {
        if (ld8(a + kGroup) != kEnemy) return;
        const int8_t hp = (int8_t)ld8(a + ac::health);
        now[a] = hp;
        const float p[3] = {f32(a + ac::kPos), f32(a + ac::kPos + 4), f32(a + ac::kPos + 8)};
        if (g_steps == kSpotsAfter && ld16(a + kSetId) != 0xFFFF && (int8_t)ld8(a + kHomeRoom) == g_room)
            spots.insert(spots.end(), {(float)g_room, (float)ld16(a + kSetId), p[0], p[1], p[2]});
        const auto was = g_hp.find(a);
        if (was == g_hp.end() || was->second <= 0 || hp >= was->second) return;
        const float d = (float)std::hypot(p[0] - at[0], p[2] - at[2]);
        if (d <= kNearHit) hits++;
        if (hp <= 0 && d <= kNearKill) kills.insert(kills.end(), {p[0], p[1] > at[1] + 150 ? at[1] + 40 : p[1], p[2]});  // (one that flies: comes down by Link)
    });
    g_hp.swap(now);
    std::lock_guard<std::mutex> l(g_m);
    g_hits += hits;
    if (g_kills.size() < 3 * 32) g_kills.insert(g_kills.end(), kills.begin(), kills.end());
    if (g_steps == kSpotsAfter && settled) g_spots = spots, g_tick++;
}

// a number no chest here uses: not set in the save, not on a chest in the loaded rooms, not ours
int free_no(uint32_t bits) {
    uint32_t used = bits;
    each_actor([&](uint32_t a) { if ((int16_t)ld16(a + kProcName) == tbox_proc()) used |= 1u << (ld32(a + kParams) >> 7 & 0x1F); });
    for (const Chest& c : g_chests) if (c.no >= 0) used |= 1u << c.no;
    for (int n = 31; n >= 0; n--) if (!(used >> n & 1)) return n;
    return -1;
}

void chests(Cpu* c, uint32_t link, const float at[3]) {
    std::lock_guard<std::mutex> l(g_m);
    if (g_chest_stage != (const char*)mem::ptr(kStage) || tbox_proc() < 0) return;
    const uint32_t bits = info() + kLiveTbox;
    bool made = false;
    for (Chest& k : g_chests) {
        if (k.opened || k.room != g_room) continue;
        const uint32_t a = k.id ? guest_call(c, GC(0x025DE50C), {k.id}) : 0;  // fopAcM_SearchByID
        if (a) {
            if (k.no >= 0 && (ld32(bits) >> k.no & 1)
                && std::hypot(k.pos[0] - at[0], k.pos[2] - at[2]) < kOpenNear) {  // (opened: the bit goes back, the app takes it from here)
                st32(bits, ld32(bits) & ~(1u << k.no));
                k.opened = true;
                g_opened.push_back(k.key);
                LOG("[loot] chest %08X opened", k.key);
            }
            continue;
        }
        if (made || g_steps < kSpawnAfter) continue;
        uint32_t found = 0;  // (a save state brought it back: the one that stands there)
        each_actor([&](uint32_t b) {
            if (!found && (int16_t)ld16(b + kProcName) == tbox_proc() && std::hypot(f32(b + kHome) - k.pos[0], f32(b + kHome + 8) - k.pos[2]) < 8) found = b;
        });
        if (found) { k.id = ld32(found + 4), k.no = ld32(found + kParams) >> 7 & 0x1F; continue; }
        const int no = free_no(ld32(bits));
        if (no < 0) continue;
        const int16_t angle[3] = {(int16_t)(k.room & 0x3F), (int16_t)k.yaw, (int16_t)(kItemBlueRupee << 8 | 0xFF)};
        const uint32_t param = (uint32_t)(k.flags & 0xF) << 20 | 0xFFu << 12 | (uint32_t)no << 7 | (k.flags >> 8 & 1 ? 5u : 0u);
        const uint32_t made_at = coop::items::spawn(c, link, tbox_proc(), param, k.pos, angle);
        if (made_at) k.id = ld32(made_at + 4), k.no = no, made = true, LOG("[loot] chest %08X made, number %d, room %d", k.key, no, k.room);
    }
}

void step(Cpu* c, uint32_t link, bool settled) {
    const unsigned lc = ss::load_count();
    const char* st = (const char*)mem::ptr(kStage);
    if (lc != g_loads || link != g_was_link || strncmp(st, g_was_stage, 8)) {
        g_loads = lc, g_was_link = link;
        strncpy(g_was_stage, st, 8);
        g_hp.clear(), g_room = -2, g_steps = 0;
        std::lock_guard<std::mutex> l(g_m);
        g_scene++, g_kills.clear(), g_spots.clear();
        for (Chest& k : g_chests) k.id = 0, k.no = -1;
    }
    const int room = (int8_t)ld8(link + ac::kRoom);
    if (room != g_room) {
        g_room = room, g_steps = 0;
        std::lock_guard<std::mutex> l(g_m);
        for (Chest& k : g_chests) if (k.room != room) k.id = 0, k.no = -1;
    }
    if (settled) g_steps++;
    const float at[3] = {f32(link + ac::kPos), f32(link + ac::kPos + 4), f32(link + ac::kPos + 8)};
    {
        std::lock_guard<std::mutex> l(g_m);
        memcpy(g_link, at, sizeof at);
        g_yaw = (int16_t)ld16(link + ac::kAngleY);
        g_where = std::string(st, strnlen(st, 8)) + "\t" + std::to_string(room);
    }
    if (const int pay = g_pay.exchange(0)) st32(rupee_change(), ld32(rupee_change()) - (uint32_t)pay);
    if (g_test_enemy.exchange(false)) {  // (the test panel: a Chuchu to defeat)
        const float t = (float)((int16_t)ld16(link + ac::kAngleY) * M_PI / 32768);
        const float pos[3] = {at[0] + 130 * std::sin(t), at[1], at[2] + 130 * std::cos(t)};
        if (const int16_t proc = proc_of("c_green"); proc >= 0) coop::items::spawn(c, link, proc, 0, pos);
    }
    enemies(link, at, settled);
    if (settled) chests(c, link, at);
}
}  // namespace

void loot_world_on(bool on) { g_on = on; }
void loot_world_service(Cpu* c, uint32_t link, bool settled) {
    if (!g_on.load(std::memory_order_relaxed)) return;
    const Cpu saved = *c;
    step(c, link, settled);
    *c = saved;
}
std::vector<float> loot_world_poll() {
    std::lock_guard<std::mutex> l(g_m);
    std::vector<float> o = {(float)g_scene, (float)g_tick, g_link[0], g_link[1], g_link[2], (float)g_hits, (float)g_yaw};
    o.insert(o.end(), g_kills.begin(), g_kills.end());
    g_hits = 0, g_kills.clear();
    return o;
}
std::vector<float> loot_world_spots() { std::lock_guard<std::mutex> l(g_m); return g_spots; }
std::vector<int> loot_world_opened() { std::lock_guard<std::mutex> l(g_m); std::vector<int> o; o.swap(g_opened); return o; }
std::string loot_world_where() { std::lock_guard<std::mutex> l(g_m); return g_where; }
void loot_world_chests(const std::string& stage, const std::vector<int>& keys, const std::vector<int>& rooms, const std::vector<float>& xyz, const std::vector<int>& yaw, const std::vector<int>& flags) {
    std::lock_guard<std::mutex> l(g_m);
    std::vector<Chest> out;
    for (size_t i = 0; i < keys.size(); i++) {
        Chest k{keys[i], rooms[i], yaw[i], flags[i], {xyz[3 * i], xyz[3 * i + 1], xyz[3 * i + 2]}, 0, -1, false};
        for (const Chest& o : g_chests) if (o.key == k.key && stage == g_chest_stage) k.id = o.id, k.no = o.no;  // (one already standing stays)
        out.push_back(k);
    }
    g_chest_stage = stage, g_chests.swap(out);
}
bool loot_world_project(float* p) {
    float x, y;
    if (!coop::emotes::to_layout(p, x, y)) return false;
    p[0] = x, p[1] = y;
    return true;
}
int loot_world_rupees() { return (int)ld16(info() + kRupee) + (int32_t)ld32(rupee_change()); }
void loot_world_pay(int n) { g_pay += n; }
void loot_world_test_enemy() { g_test_enemy = true; }
}  // namespace mods
