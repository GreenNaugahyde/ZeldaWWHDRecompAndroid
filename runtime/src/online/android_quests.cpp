// (fork: Android) Daily quests from the fork's server (runtime/src/online/fork_proto.h, server/src/fork_quests.cpp;
// notes/online-modes.md, "Server quests").
//
// Once the server welcomed this game (a session through a server), it says hello in the fork's words. A fork server
// answers with its greeting and today's quests; an upstream server never answers, and then nothing more is sent.
// Each monster this game sees defeated is reported: one of the monsters this game runs for its place (upstream's
// coop_monsters) counts for every party member there; one each game runs by itself (made at run time) for this player
// alone; a copy of another's monster is reported by its owner, not here. The server counts, and a quest done pays
// rupees (through the meter as the party's shared rupees come in, not told to the party again). The Online page shows
// the quests (Native.coopQuests), and progress pops up as a notice.
#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "../mods/coop_monsters.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "../mods/coop_progress.h"
#include "fork_proto.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "proto/wire.h"
#include "runtime.h"

namespace ss { void notice(const std::string& text); }  // (compat.cpp: a notice the app shows)
namespace mods::coop::net::srv { bool android_fork_send(const std::string& body); }

namespace android_quests {
namespace {
namespace fp = fork_proto;
namespace net = mods::coop::net;
namespace names = mods::coop::names;

constexpr uint32_t kProcName = 0x08, kSetId = 0x2D8, kGroup = 0x2DA, kHomeRoom = 0x2FE, kHealth = 0x3A1;
const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage, kActorQueue = GD(0x101F3328);
const uint32_t kRupeeCount = kPlay + names::inf::kItemRupeeCount;

struct Quest {
    uint16_t id = 0, goal = 1, progress = 0;
    bool done = false;
    uint32_t rupees = 0;
    std::string name, what;
    uint8_t kind = fork_proto::kDefeatKind;  // daily, server-wide or event
    uint16_t mine = 0;                       // (server-wide) this player's part
    uint32_t ends_at = 0;                    // (server-wide) net::now_ms() when it starts over
};
std::mutex g_mu;
bool g_hello_due = false, g_fork = false, g_ask = false;
std::string g_greeting;
uint32_t g_day = 0, g_next_at = 0;  // the next day: net::now_ms() then
std::vector<Quest> g_quests;
int g_rupees = 0;  // rewards not put in yet (game thread)

struct Seen { int16_t proc; uint8_t room; uint16_t set; int8_t hp; };
std::unordered_map<uint32_t, Seen> g_seen;  // monsters by process id: last step's
}  // namespace

// the server welcomed this game (a fresh session or a new worker): hello again at the next step
void welcomed() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hello_due = true, g_fork = false, g_quests.clear(), g_greeting.clear();
}
void stopped() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_hello_due = false, g_fork = false, g_quests.clear(), g_greeting.clear();
}

// the server's fork words (the session's poll: game thread)
void from_server(const uint8_t* d, size_t n) {
    online::proto::Reader r(d, n);
    const uint8_t op = r.u8();
    std::string note;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (op == fp::kWelcome) {
            r.u16(), r.u32();
            const std::string g = r.str8(255);
            if (!r.ok()) return;
            g_fork = true, g_greeting = g;
            LOG("[quests] the server has quests (%s)", g.c_str());
        } else if (op == fp::kQuests) {
            g_day = r.u32();
            g_next_at = net::now_ms() + r.u32() * 1000u;
            const int count = r.u8();
            std::vector<Quest> qs;
            for (int i = 0; i < count && r.ok(); i++) {
                Quest q;
                q.id = r.u16();
                q.kind = r.u8();
                q.goal = std::max<uint16_t>(r.u16(), 1), q.progress = r.u16(), q.done = r.u8(), q.rupees = r.u32();
                q.name = r.str8(255), q.what = r.str8(255);
                if (r.ok()) qs.push_back(q);
            }
            if (!r.ok()) return;
            // (after the list: the server-wide quests' parts and times; a server without them ends here)
            const int worlds = r.u8();
            for (int i = 0; i < worlds && r.ok(); i++) {
                const uint16_t id = r.u16(), mine = r.u16();
                const uint32_t left = r.u32();
                if (!r.ok()) break;
                for (Quest& q : qs) if (q.id == id) q.mine = mine, q.ends_at = net::now_ms() + left * 1000u;
            }
            const bool first = g_quests.empty();
            g_quests = qs;
            LOG("[quests] day %u: %zu quests", g_day, qs.size());
            if (first && !qs.empty()) note = "Today's quests are in: + > Game > Online > Daily quests";
        } else if (op == fp::kProgress) {
            const uint16_t id = r.u16(), p = r.u16();
            const bool done = r.u8();
            if (!r.ok()) return;
            for (Quest& q : g_quests)
                if (q.id == id) {
                    const bool tell = !done && (p == 1 || p * 4 / q.goal != q.progress * 4 / q.goal);  // (the first, then each quarter)
                    q.progress = p, q.done = done;
                    if (tell) note = q.name + ": " + std::to_string(p) + " / " + std::to_string(q.goal);
                }
        } else if (op == fp::kWorld) {
            const uint16_t id = r.u16(), count = r.u16();
            const bool done = r.u8();
            const uint16_t mine = r.u16();
            const uint32_t left = r.u32();
            if (!r.ok()) return;
            for (Quest& q : g_quests)
                if (q.id == id) {
                    // (each tenth of the way, and reached)
                    if (!done && count * 10 / q.goal != q.progress * 10 / q.goal)
                        note = "Server event " + q.name + ": " + std::to_string(count * 100 / q.goal) + "% (" + std::to_string(count) + " / " +
                               std::to_string(q.goal) + ")";
                    if (done && !q.done) note = "Server event " + q.name + " reached by everyone together!";
                    q.progress = count, q.done = done, q.mine = mine, q.ends_at = net::now_ms() + left * 1000u;
                }
        } else if (op == fp::kReward) {
            r.u16();
            const uint32_t rupees = r.u32();
            const std::string what = r.str8(255);
            if (!r.ok()) return;
            g_rupees += (int)std::min<uint32_t>(rupees, 5000);
            note = "Quest done: " + what + (rupees ? " (+" + std::to_string(rupees) + " rupees)" : "");
        }
    }
    if (!note.empty()) ss::notice(note);
}

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    (void)c;
    bool hello = false, ask = false, fork = false;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        hello = g_hello_due, ask = g_ask, fork = g_fork;
        g_hello_due = false, g_ask = false;
    }
    if (net::role() == net::Role::kOff || !net::via_server()) return g_seen.clear();
    if (hello) {
        online::proto::Bytes b;
        online::proto::Writer w(b);
        w.u8(fp::kHello), w.u16(fp::kVersion), w.u32(fp::kCapQuests);
        if (!mods::coop::net::srv::android_fork_send(std::string(b.begin(), b.end()))) {
            std::lock_guard<std::mutex> lk(g_mu);
            g_hello_due = true;  // (no client yet: again next step)
        }
    }
    if (!fork) return g_seen.clear();
    if (ask) mods::coop::net::srv::android_fork_send(std::string(1, (char)fp::kAsk));
    // the rewards: through the meter (with the party's shared rupees as theirs come in: not told to them again)
    if (g_rupees) {
        if (net::party_flag(net::kShareRupees, false)) {
            const int32_t g = g_rupees;
            mods::coop::progress::rupees_received((const uint8_t*)&g, sizeof g);
        } else {
            st32(kRupeeCount, ld32(kRupeeCount) + (uint32_t)g_rupees);
        }
        g_rupees = 0;
    }
    // the monsters defeated since the last step (health gone, once each)
    const uint32_t link = ld32(kPlay + names::inf::kLink);
    if (link < 0x10000000u || link >= 0x50000000u) return g_seen.clear();
    std::unordered_map<uint32_t, Seen> now;
    uint32_t n = ld32(kActorQueue);
    for (int k = 0; n >= 0x10000000 && n < 0x50000000 && k < 4096; k++, n = ld32(n + 8)) {
        const uint32_t a = ld32(n + 0xC);
        if (!a || ld8(a + kGroup) != 2) continue;
        const uint32_t pid = ld32(a + 4);
        const Seen s{(int16_t)ld16(a + kProcName), ld8(a + kHomeRoom), ld16(a + kSetId), (int8_t)ld8(a + kHealth)};
        now[pid] = s;
        const auto was = g_seen.find(pid);
        if (was == g_seen.end() || was->second.hp <= 0 || s.hp > 0) continue;
        // defeated now: reported by the game that runs it (a copy of another's: by that one)
        const bool shared = s.set != 0xFFFF && mods::coop::monsters::takes(a);
        if (shared && !mods::coop::monsters::runs(a)) continue;
        online::proto::Bytes b;
        online::proto::Writer w(b);
        w.u8(fp::kDefeat);
        w.bytes(mem::ptr(kStage), 8);
        w.u8((uint8_t)s.room), w.u16((uint16_t)s.proc), w.u8(shared ? 1 : 0);
        mods::coop::net::srv::android_fork_send(std::string(b.begin(), b.end()));
        LOG("[quests] defeated: proc %#x room %d set %u%s", (unsigned)(uint16_t)s.proc, (int8_t)s.room, s.set, shared ? " (for the place)" : "");
    }
    g_seen = std::move(now);
}

// the Online page (any thread): "" without a fork server; else the greeting, then "id\tname\twhat\tprogress\tgoal\tdone\trupees"
// lines (then kind: 1 daily, 2 server-wide, 3 event; mine; minutes left), the first line "greeting\thours to the next day"
std::vector<std::string> list() {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<std::string> out;
    if (!g_fork) return out;
    const int32_t left = (int32_t)(g_next_at - net::now_ms());
    out.push_back(g_greeting + "\t" + std::to_string(std::max(0, left / 3600000)) + "\t" + std::to_string(std::max(0, left / 60000 % 60)));
    for (const Quest& q : g_quests)
        out.push_back(std::to_string(q.id) + "\t" + q.name + "\t" + q.what + "\t" + std::to_string(q.progress) + "\t" + std::to_string(q.goal) + "\t" +
                      (q.done ? "1" : "0") + "\t" + std::to_string(q.rupees) + "\t" + std::to_string(q.kind) + "\t" + std::to_string(q.mine) + "\t" +
                      std::to_string(q.ends_at ? std::max(0, (int32_t)(q.ends_at - net::now_ms()) / 60000) : 0));
    return out;
}
void ask() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_ask = true;
}
}  // namespace android_quests
