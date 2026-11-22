// (fork: Android) Online raids, step 1: more monsters (notes/online-modes.md, "Raids").
//
// While a session runs, every ordinary monster placed in a room (Bokoblin, Moblin, Darknut, Stalfos, ...) gets extra
// copies of itself beside it: the same kind and parameters, made in the same room's layer, so its archive is already
// loaded and a room that opens when its enemies are down waits for the copies too. How many: Game > Online > Raid
// (pref android.raid: 0 off, 1..3 per monster, 4 by the party's size), the session host's setting (player 0) for
// everyone (kRaidMsg), so every game makes the same ones.
//
// The copies join upstream's shared monsters (mods/coop_monsters.cpp) unchanged: that file knows a monster on every game
// by its kind, home room and set id, and leaves those made at run time (set id 0xFFFF) to each game. Each game makes the
// copies itself, by the same rule, and gives each the same set id no placed actor uses: 300 + source's set id * 3 +
// copy (< 512: dSv_zoneActor_c::on, which the game calls on a death, has no bounds check in the release game and keeps
// 512 bits a zone; those bits last one stage visit). So the place's owner runs them, the others' copies follow it, a hit
// goes to the owner and a kill to everyone, as for any placed monster.
//
// A copy killed stays killed for the stage visit (here or, through coop_monsters, on another game); one whose room was
// unloaded is made again when its source is there again. A copy of a copy is never made.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../mods/coop_deck.h"
#include "../mods/coop_monsters.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "../mods/coop_sync.h"
#include "../online/compat/overlay/hostui.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "runtime.h"

namespace net = mods::coop::net;
#include <array>
namespace android_siege {
std::vector<std::array<float, 3>> walked_near(const float at[3], float lo, float hi);
bool dry_place(Cpu* c, const float p[3]);
}
namespace names = mods::coop::names;

namespace android_raid {
constexpr net::Msg kRaidMsg = (net::Msg)0x64;  // (the fork's) player 0 -> all: 'W' 'R', u8 copies per monster
namespace {
constexpr uint32_t kProcName = 0x08, kLayer = 0x2C, kParams = 0xB0, kArgument = 0xB4, kSetId = 0x2D8, kGroup = 0x2DA,
                   kHome = 0x2EC, kHomeRoom = 0x2FE, kScale = 0x330, kHealth = 0x3A1;
constexpr uint8_t kEnemy = 2;
constexpr uint16_t kCopyBase = 300, kSetMax = 450;  // (copies 300..449; a wave's monsters 450..511)
constexpr uint16_t kWaveBase = 450, kWaveMax = 512;
constexpr int kMostCopies = 3, kMostAlive = 24;
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage, kActorQueue = GD(0x101F3328);

// the ordinary monsters copied (their process names, as upstream's native enemies name them: story_enemy_ownership,
// story_enemies_more): ChuChu, Bokoblin, Moblin, Keese, Peahat, Darknut, Bubble, Stalfos, Magtail, Mothula, Redead,
// Poe, Armos, Armos Knight. Not bosses, nor those that come in swarms of their own (rats, Morths, Miniblins) or
// grow out of the ground (Boko Babas)
constexpr int16_t kCopied[] = {0xCE, 0xBD, 0xBC, 0xD7, 0xD1, 0xBF, 0xCF, 0xBE, 0xD8, 0xCC, 0xE0, 0xD4, 0xCB, 0xCA};
bool copied(int16_t proc) { return std::find(std::begin(kCopied), std::end(kCopied), proc) != std::end(kCopied); }

struct Copy { uint32_t pid = 0xFFFFFFFFu; int8_t hp = 0; bool seen = false, killed = false, wave = false, remake = false; int wet = 0; };
std::unordered_map<uint32_t, Copy> g_copies;  // by room << 16 | set id, this stage visit
char g_stage[8];
int g_host_copies = -1;  // player 0's (guests)
uint32_t g_sent = 0;
bool g_resend = false;

int pref() {
    std::string v;
    return hostui::get("android.raid", v) ? std::clamp(atoi(v.c_str()), 0, 4) : 0;
}
int party() {
    int n = 1;
    for (int i = 0; i < net::kMaxPlayers; i++) if (i != net::my_id() && net::connected(i) && net::member(i)) n++;
    return n;
}
// copies per monster: the host's choice
int my_copies() {
    const int p = pref();
    return p == 4 ? std::clamp(party() - 1, 1, kMostCopies) : p;
}
int copies() { return net::my_id() == 0 ? my_copies() : std::max(g_host_copies, 0); }

const char* stage() { return (const char*)mem::ptr(kStage); }
template <class F> void each_actor(F f) {  // g_fopAcTg_Queue: node next +8, actor +0xC
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; n >= 0x10000000 && n < 0x50000000 && k < 4096; k++, n = ld32(n + 8))
        if (const uint32_t a = ld32(n + 0xC)) f(a);
}
uint32_t by_pid(Cpu* c, uint32_t pid) { return guest_call(c, GC(0x025DE50C), {pid}); }  // fopAcM_SearchByID

uint32_t make(Cpu* c, uint32_t src, int j) {
    static const uint32_t buf = mem::host_alloc(0x30);  // pos +0, angle +0x10, scale +0x20
    const float ang = ((int)ld16(src + kSetId) * 37 + j * 120) * 3.14159265f / 180, r = 90;
    const float at[3] = {(float)ldf32(src + kHome) + r * std::sin(ang), (float)ldf32(src + kHome + 4),
                         (float)ldf32(src + kHome + 8) + r * std::cos(ang)};
    for (int e = 0; e < 3; e++) { uint32_t u; memcpy(&u, &at[e], 4); st32(buf + 4 * e, u); }
    for (int i = 0; i < 12; i += 4) st32(buf + 0x20 + i, ld32(src + kScale + i));
    for (int i = 0; i < 6; i += 2) st16(buf + 0x10 + i, ld16(src + kHome + 0xC + i));
    const uint32_t room = (uint32_t)(int8_t)ld8(src + kHomeRoom);
    const uint32_t layer = guest_call(c, GC(0x025DED64));  // made in its source's layer (the room's): gone with the room
    guest_call(c, GC(0x025DEAB4), {ld32(src + kLayer)});
    const uint32_t pid = guest_call(c, GC(0x025D5834), {(uint32_t)(uint16_t)ld16(src + kProcName), ld32(src + kParams), buf, room,
                                                        buf + 0x10, buf + 0x20, ld32(src + kArgument), 0});  // fopAcM_create
    guest_call(c, GC(0x025DEAB4), {layer});
    return pid;
}

// ---- a wave of monsters here now (Game > Online > "Monsters here now": a test without placed monsters, e.g. Outset).
// The game asking picks the places around its Link and the set ids (450..511, a free run of 10); every game in the session
// makes the same ones (kWaveMsg), so coop_monsters shares them as placed monsters. Each kind by the stage data's name
// (dStage_searchName 025C109C: its process name and argument), made with fopAcM_create in Link's layer: its create loads
// its own archive. Killed or gone, a wave's monster is not made again.
constexpr net::Msg kWaveMsg = (net::Msg)0x66;  // (the fork's) any -> all: 'W' 'W', stage[8], s8 room, n x Spawn
struct Spawn { uint16_t set; char name[8]; uint32_t params; float at[3]; int16_t yaw; };
std::unordered_map<uint32_t, Spawn> g_spawns;  // each wave monster's spawn (made again there after a reload)
// the stage being loaded again (Game Over's Continue: same stage, new actors): wave monsters gone meanwhile are made
// again once it is done, not taken for drowned (that removed them for the whole party; user: wave 1 "beaten" after
// dying)
uint32_t g_last_link = 0, g_reload_until = 0;
struct WaveKind { const char* name; uint32_t params; int count; };
// Bokoblins (type 4: they stand, then fight), Moblins, green ChuChus, Keese: upstream's native enemy parameters
constexpr WaveKind kWave[] = {{"Bk", 0xFFFFFF04u, 3}, {"mo2", 0xFFFF0000u, 2}, {"c_green", 0xFF000000u, 3}, {"keeth", 0xFFFF0000u, 2}};
bool g_wave_asked = false;
std::vector<std::string> g_waves_in;  // the session's waves (game thread)
uint32_t g_wave_no = 0;

void make_one(Cpu* c, uint32_t link, int8_t room, const Spawn& sp, Copy& cp);
void make_wave(Cpu* c, uint32_t link, const uint8_t* d, size_t n) {
    if (n < 11 || memcmp(d + 2, stage(), 8)) return;
    const int8_t room = (int8_t)d[10];
    for (size_t o = 11; o + sizeof(Spawn) <= n; o += sizeof(Spawn)) {
        Spawn sp;
        memcpy(&sp, d + o, sizeof sp);
        if (sp.set < kCopyBase || sp.set >= kWaveMax || !std::isfinite(sp.at[0]) || !std::isfinite(sp.at[1]) || !std::isfinite(sp.at[2])) continue;
        const uint32_t k = (uint32_t)(uint8_t)room << 16 | sp.set;
        Copy& cp = g_copies[k];
        if (cp.pid != 0xFFFFFFFFu || cp.killed) continue;
        g_spawns[k] = sp;
        make_one(c, link, room, sp, cp);
    }
}
void make_one(Cpu* c, uint32_t link, int8_t room, const Spawn& sp, Copy& cp) {
    static const uint32_t buf = mem::host_alloc(0x40), name = mem::host_alloc(16);
    {
        memset(mem::ptr(name), 0, 16), memcpy(mem::ptr(name), sp.name, strnlen(sp.name, 8));
        Cpu probe = *c;
        const uint32_t entry = guest_call(&probe, GC(0x025C109C), {name});  // dStage_searchName
        if (!entry) { LOG("[raid] wave: no stage name %.8s here", sp.name); cp.killed = true; return; }
        const uint16_t proc = ld16(entry + 8);
        const uint32_t arg = (uint32_t)(int32_t)(int8_t)ld8(entry + 0xA);
        for (int e = 0; e < 3; e++) { uint32_t u; memcpy(&u, &sp.at[e], 4); st32(buf + 4 * e, u); }
        st16(buf + 0x10, 0), st16(buf + 0x12, (uint16_t)sp.yaw), st16(buf + 0x14, 0);
        const uint32_t one = 0x3F800000u;
        st32(buf + 0x20, one), st32(buf + 0x24, one), st32(buf + 0x28, one);
        const uint32_t layer = guest_call(c, GC(0x025DED64));
        guest_call(c, GC(0x025DEAB4), {ld32(link + kLayer)});
        cp.pid = guest_call(c, GC(0x025D5834), {proc, sp.params, buf, (uint32_t)(int32_t)room, buf + 0x10, buf + 0x20, arg, 0});
        guest_call(c, GC(0x025DEAB4), {layer});
        cp.wave = true, cp.seen = false, cp.hp = 0, cp.remake = false;
        if (cp.pid == 0 || cp.pid == 0xFFFFFFFFu) cp.pid = 0xFFFFFFFFu, cp.killed = true;
        LOG("[raid] wave: %.8s (proc %#x, argument %d) %d/%u asked for at %.0f %.0f %.0f: %s", sp.name, proc, (int)(int32_t)arg, room, sp.set,
            sp.at[0], sp.at[1], sp.at[2], cp.killed ? "not made" : "coming");
    }
}
void send_mine() {
    const int n = my_copies();
    net::send(-1, kRaidMsg, std::string("WR") + (char)n);
    g_sent = net::now_ms();
}
}  // namespace

struct SpawnReq { const char* name; uint32_t params; float at[3]; int16_t yaw; };
std::vector<uint16_t> spawn(Cpu* c, uint32_t link, const std::vector<SpawnReq>& list);
void ask_wave(Cpu* c, uint32_t link);
void remove(Cpu* c, int room, uint16_t set);
// a wave's monster lost in the water (drowned, or under the surface 3 s): the game running it tells everyone
constexpr net::Msg kGoneMsg = (net::Msg)0x69;  // (the fork's) 'W' 'X', stage[8], s8 room, u16 set
std::vector<std::string> g_gone_in;
void gone_received(int, const uint8_t* d, size_t n) {
    if (n >= 13 && d[0] == 'W' && d[1] == 'X' && g_gone_in.size() < 64) g_gone_in.emplace_back((const char*)d, n);
}

void received(int from, const uint8_t* d, size_t n) {
    if (from != 0 || n < 3 || d[0] != 'W' || d[1] != 'R') return;
    const int was = g_host_copies;
    g_host_copies = std::min<int>(d[2], kMostCopies);
    if (was != g_host_copies) LOG("[raid] the host's raid: %d more of each monster", g_host_copies);
}
void joined() { g_resend = true; }
// another player's wave (the session's poll, game thread)
void wave_received(int, const uint8_t* d, size_t n) {
    if (n >= 11 && d[0] == 'W' && d[1] == 'W' && g_waves_in.size() < 8) g_waves_in.emplace_back((const char*)d, n);
}
// the Online page (any thread): a wave here at the next step
void ask_wave() { g_wave_asked = true; }

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    if (net::role() == net::Role::kOff) return g_copies.clear(), g_host_copies = -1, void();
    if (net::my_id() == 0) {  // (the host: its choice to everyone, now and then and to a newcomer)
        static int had = -1;
        const int n = my_copies();
        if (g_resend || n != had || net::now_ms() - g_sent > 5000) send_mine(), g_resend = false, had = n;
    }
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (link < 0x10000000u || link >= 0x50000000u) return;
    if (memcmp(g_stage, stage(), 8)) memcpy(g_stage, stage(), 8), g_copies.clear(), g_spawns.clear();
    const uint32_t now = net::now_ms();
    if (link != g_last_link || ld8(kPlay + names::inf::kNextStageEnabled)) g_reload_until = now + 2000, g_last_link = link;
    const bool reloading = (int32_t)(now - g_reload_until) < 0;
    if (!reloading)
        for (auto& [k, cp] : g_copies)
            if (cp.remake && !cp.killed && g_spawns.count(k)) {
                LOG("[raid] wave monster %d/%u made again after the reload", (int8_t)(k >> 16), k & 0xFFFF);
                make_one(c, link, (int8_t)(k >> 16), g_spawns[k], cp);
            }
    if (!reloading) {  // (a wave told meanwhile: made once the stage is there again)
        for (const std::string& w : g_waves_in) make_wave(c, link, (const uint8_t*)w.data(), w.size());
        g_waves_in.clear();
    }
    if (g_wave_asked) g_wave_asked = false, ask_wave(c, link);
    const int want = copies();
    // the copies made: found once made (their shared identity put on), killed or gone with their room
    std::unordered_set<uint32_t> live;
    std::vector<uint32_t> lost;
    for (const std::string& g : g_gone_in)
        if (!memcmp(g.data() + 2, stage(), 8)) remove(c, (int8_t)g[10], (uint16_t)((uint8_t)g[11] | (uint8_t)g[12] << 8));
    g_gone_in.clear();
    for (auto& [k, cp] : g_copies) {
        if (cp.killed || cp.pid == 0xFFFFFFFFu) continue;
        const uint32_t a = by_pid(c, cp.pid);
        if (a >= 0x10000000u && a < 0x50000000u) {
            if (!cp.seen) {
                st16(a + kSetId, (uint16_t)(k & 0xFFFF)), st8(a + kHomeRoom, (uint8_t)(k >> 16));
                cp.seen = true;
                LOG(cp.wave ? "[raid] wave monster %d/%u made: proc %#x at %.0f %.0f %.0f, health %d" : "[raid] copy %d/%u made: proc %#x at %.0f %.0f %.0f, health %d", (int8_t)(k >> 16), k & 0xFFFF,
                    ld16(a + kProcName), (float)ldf32(a + 0x314), (float)ldf32(a + 0x318), (float)ldf32(a + 0x31C), (int8_t)ld8(a + kHealth));
            }
            cp.hp = (int8_t)ld8(a + kHealth);
            live.insert(k);
            // a wave's monster this game runs, under the sea's surface for 3 s: lost in the water, gone everywhere
            if (cp.wave && (mods::coop::monsters::runs(a) || !mods::coop::monsters::takes(a))) {
                float sea = 0;
                const bool under = mods::coop::deck::water(c, (float)ldf32(a + 0x314), (float)ldf32(a + 0x31C), sea) && (float)ldf32(a + 0x318) < sea - 30;
                cp.wet = under ? cp.wet + 1 : 0;
                if (cp.wet == 90) {
                    const int room = (int8_t)(k >> 16);
                    const uint16_t set = (uint16_t)(k & 0xFFFF);
                    LOG("[raid] wave monster %d/%u lost in the water: gone everywhere", room, set);
                    lost.push_back(k);
                }
            }
        } else if (cp.seen && reloading && cp.wave && cp.hp > 0) {  // (gone with the reloaded stage: made again after)
            cp.pid = 0xFFFFFFFFu, cp.seen = false, cp.remake = true;
        } else if (cp.seen) {  // gone: killed (no health left, or killed on another game), else its room went
            const int room = (int8_t)(k >> 16), set = (int)(k & 0xFFFF);
            // (a wave's monster this game ran, gone with health left: drowned; gone everywhere)
            if (cp.wave && cp.hp > 0 && !mods::coop::monsters::killed(set, room)) lost.push_back(k);
            cp.killed = cp.wave || cp.hp <= 0 || mods::coop::monsters::killed(set, room);
            LOG("[raid] copy %d/%d %s", room, set, cp.killed ? "killed" : "gone with its room");
            cp.pid = 0xFFFFFFFFu, cp.seen = false;
        }
    }
    for (const uint32_t k : lost) {
        const int8_t room = (int8_t)(k >> 16);
        const uint16_t set = (uint16_t)(k & 0xFFFF);
        remove(c, room, set);
        net::send(-1, kGoneMsg, std::string("WX") + std::string(stage(), 8) + (char)room + (char)(set & 0xFF) + (char)(set >> 8));
    }
    if (want <= 0) return;
    // the sources: the ordinary placed monsters now here, alive; their copies made where none is
    std::unordered_set<uint32_t> placed;  // room << 16 | set id of every placed actor here (no copy takes one's)
    std::vector<uint32_t> sources;
    each_actor([&](uint32_t a) {
        const uint16_t set = ld16(a + kSetId);
        if (set == 0xFFFF) return;
        placed.insert((uint32_t)ld8(a + kHomeRoom) << 16 | set);
        if (ld8(a + kGroup) == kEnemy && set < kCopyBase && (int8_t)ld8(a + kHealth) > 0) {
            const int16_t proc = (int16_t)ld16(a + kProcName);
            if (copied(proc)) sources.push_back(a);
            else if (static std::unordered_set<int> told; told.insert(proc).second)
                LOG("[raid] monster proc %#x (room %d set %u) not copied", (unsigned)(uint16_t)proc, (int8_t)ld8(a + kHomeRoom), set);
        }
    });
    int alive = (int)live.size();
    for (const uint32_t src : sources) {
        const uint32_t room = ld8(src + kHomeRoom);
        for (int j = 0; j < want && alive < kMostAlive; j++) {
            const uint32_t set = kCopyBase + ld16(src + kSetId) * kMostCopies + j;
            if (set >= kSetMax) break;
            const uint32_t k = room << 16 | set;
            Copy& cp = g_copies[k];
            if (cp.killed || cp.pid != 0xFFFFFFFFu) continue;
            if (placed.count(k)) continue;  // (a placed actor has that set id: no copy)
            if (mods::coop::monsters::killed((int)set, (int)(int8_t)room)) { cp.killed = true; continue; }
            cp.pid = make(c, src, j), cp.seen = false, cp.hp = 0;
            if (cp.pid == 0xFFFFFFFFu || cp.pid == 0) { cp.pid = 0xFFFFFFFFu, cp.killed = true; LOG("[raid] copy %d/%u not made", (int8_t)room, set); continue; }
            alive++;
        }
    }
}

// ---- the spawn service for events (android_siege.cpp): monsters made on every game in the session, shared
// these monsters in Link's current room, with free set ids (450..511; none: fewer made); the set ids used
std::vector<uint16_t> spawn(Cpu* c, uint32_t link, const std::vector<SpawnReq>& list) {
    const int8_t room = (int8_t)ld8(link + 0x326);
    // fresh set ids only: one used before in this visit is still known as killed (here: g_copies; and coop_monsters'
    // kill union, which deletes a new one with it at once, or leaves it dying for ever), so none is used twice. From
    // 511 down; below 450 only while the raid makes no copies (they take 300..449 by their sources' ids)
    std::unordered_set<uint16_t> placed;
    each_actor([&](uint32_t a) { if ((int8_t)ld8(a + kHomeRoom) == room) placed.insert(ld16(a + kSetId)); });
    const uint16_t lowest = copies() > 0 ? kWaveBase : kCopyBase;
    std::vector<uint16_t> sets;
    std::string b = std::string("WW") + std::string(stage(), 8) + (char)room;
    for (int s = kWaveMax - 1; s >= lowest && sets.size() < list.size(); s--) {
        const uint32_t k = (uint32_t)(uint8_t)room << 16 | (uint32_t)s;
        if (g_copies.count(k) || placed.count((uint16_t)s) || mods::coop::monsters::killed(s, room)) continue;
        const SpawnReq& r = list[sets.size()];
        Spawn sp{};
        sp.set = (uint16_t)s;
        memcpy(sp.name, r.name, strnlen(r.name, 8));
        sp.params = r.params;
        memcpy(sp.at, r.at, 12);
        sp.yaw = r.yaw;
        b.append((const char*)&sp, sizeof sp);
        sets.push_back((uint16_t)s);
    }
    if (sets.size() < list.size()) LOG("[raid] spawn: only %zu of %zu set ids left in this visit (leave the place and come back)", sets.size(), list.size());
    if (sets.empty()) return sets;
    net::send(-1, kWaveMsg, b);
    make_wave(c, link, (const uint8_t*)b.data(), b.size());
    return sets;
}
// "Monsters here now": a ring of them around Link (the spawn service: fresh set ids)
void ask_wave(Cpu* c, uint32_t link) {
    const float lx = (float)ldf32(link + 0x314), ly = (float)ldf32(link + 0x318), lz = (float)ldf32(link + 0x31C);
    // where: places walked on (solid ground, not the sea: android_siege) 200..900 from Link; too few: a ring of 260 on
    // his height, its points over the sea left out
    const float me[3] = {lx, ly, lz};
    const auto near = android_siege::walked_near(me, 200, 900);
    std::vector<SpawnReq> list;
    int i = 0;
    const int total = 10;
    for (const WaveKind& w : kWave)
        for (int j = 0; j < w.count; j++, i++) {
            SpawnReq r{w.name, w.params, {0, 0, 0}, 0};
            const float ang = i * 6.2831853f / total, rr = 260 + 60 * (i % 2);
            if (near.size() >= 3) {
                const auto& p = near[(i * 7) % near.size()];
                r.at[0] = p[0] + 50 * std::sin(ang), r.at[1] = p[1] + (w.name[0] == 'k' ? 150 : 30), r.at[2] = p[2] + 50 * std::cos(ang);
            } else {
                r.at[0] = lx + rr * std::sin(ang), r.at[1] = ly + (w.name[0] == 'k' ? 150 : 30), r.at[2] = lz + rr * std::cos(ang);
                const float ground[3] = {r.at[0], ly, r.at[2]};
                if (!android_siege::dry_place(c, ground)) continue;
            }
            r.yaw = (int16_t)(std::atan2(lx - r.at[0], lz - r.at[2]) * 32768 / 3.14159265f);
            list.push_back(r);
        }
    const auto sets = spawn(c, link, list);
    LOG("[raid] wave asked for here: %zu monsters", sets.size());
}
// one of them still here (asked for, being made or alive)
bool alive(int room, uint16_t set) {
    const auto it = g_copies.find((uint32_t)(uint8_t)room << 16 | set);
    return it != g_copies.end() && !it->second.killed;
}
// taken away here (reached its goal, the event over): no drop, not made again
void remove(Cpu* c, int room, uint16_t set) {
    const auto it = g_copies.find((uint32_t)(uint8_t)room << 16 | set);
    if (it == g_copies.end()) return;
    if (it->second.pid != 0xFFFFFFFFu) guest_call(c, GC(0x025D57E4), {it->second.pid});  // fopAcM_delete
    it->second.killed = true, it->second.pid = 0xFFFFFFFFu;
}
}  // namespace android_raid
