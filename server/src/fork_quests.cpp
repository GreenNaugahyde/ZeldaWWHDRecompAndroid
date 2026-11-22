// (fork) Daily quests on the fork's server (fork_quests.h).
#include "fork_quests.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "../../runtime/src/online/fork_proto.h"
#include "server.h"

namespace online::server::fork_quests {
namespace {
namespace fp = fork_proto;
using proto::Reader;
using proto::Writer;

// a quest of the pool: defeat `goal` monsters of these kinds (their process names; none: any), in this stage ("" any)
struct Def {
    uint16_t id = 0;
    std::string name, what;
    std::vector<uint16_t> procs;
    std::string stage;
    uint16_t goal = 1;
    uint32_t rupees = 0;
    bool world = false, weekly = false;  // a server-wide quest, starting over each day / each week
};
// the built-in pool: the game's process names of the monsters (as the fork's raid copies them: android_raid.cpp)
const char* const kBuiltIn =
    "1|Bokoblin trouble|Defeat 15 Bokoblins|BD|*|15|100\n"
    "2|ChuChu cleanup|Defeat 20 ChuChus|CE|*|20|50\n"
    "3|Moblin hunt|Defeat 10 Moblins|BC|*|10|150\n"
    "4|Darknut duel|Defeat 5 Darknuts|BF|*|5|300\n"
    "5|Keese swarm|Defeat 20 Keese|D7|*|20|50\n"
    "6|Lay them to rest|Defeat 10 Stalfos, ReDeads or Poes|BE,E0,D4|*|10|200\n"
    "7|Monster slayer|Defeat 50 monsters|*|*|50|200\n"
    "8|Armos breaker|Defeat 8 Armos or Armos Knights|CB,CA|*|8|200\n"
    "9|Peahat plucker|Defeat 10 Peahats|D1|*|10|100\n"
    "100|Outset under siege|Protect Grandma's house from 4 waves of monsters|*|event:siege|1|500\n"
    "101|Raiders from the sea|Keep the raiders off Outset and out of Grandma's house|*|event:shipraid|1|800\n"
    "200|Moblin purge|Everyone on this server together: defeat 300 Moblins this week|BC|*|300|300|world-week\n"
    "201|ChuChu flood|Everyone on this server together: defeat 200 ChuChus today|CE|*|200|100|world-day\n";
constexpr int kPerDay = 3;
constexpr int kMostDefeatsPerSecond = 3;  // a game reporting faster is not believed

struct Player {
    bool hello = false;
    uint16_t sid = 0;
    std::map<uint16_t, uint16_t> progress;  // today's
    std::set<uint16_t> done;
    double budget = kMostDefeatsPerSecond;
    uint32_t budget_at = 0;
};
// a server-wide quest's state this period
struct World {
    uint32_t period = 0, count = 0;
    bool done = false;
    std::map<uint64_t, uint32_t> mine;  // who added how much
    std::set<uint64_t> paid;
};
struct State {
    bool loaded = false;
    std::map<uint16_t, World> worlds;
    bool world_dirty = false;
    uint32_t world_sent = 0;
    std::vector<Def> pool;
    std::string dir;
    uint32_t day = 0;
    std::vector<uint16_t> today;
    std::map<uint64_t, Player> players;  // by key hash
    bool dirty = false;
    uint32_t saved_at = 0;
};
std::map<Server*, State> g_state;

uint32_t utc_day() { return uint32_t(std::time(nullptr) / 86400); }
uint32_t to_next_day() { return uint32_t(86400 - std::time(nullptr) % 86400); }
uint32_t utc_week() { return (utc_day() + 3) / 7; }  // (weeks from Monday: day 0 was a Thursday)
uint32_t period_of(const Def& d) { return d.weekly ? utc_week() : utc_day(); }
uint32_t left_of(const Def& d) {
    if (!d.weekly) return to_next_day();
    return uint32_t((uint64_t(utc_week() + 1) * 7 - 3) * 86400 - uint64_t(std::time(nullptr)));
}

std::vector<std::string> split(const std::string& s, char c) {
    std::vector<std::string> out;
    std::string cur;
    for (char ch : s) {
        if (ch == c) out.push_back(cur), cur.clear();
        else cur += ch;
    }
    out.push_back(cur);
    return out;
}
// "id|name|what|procs (hex, comma; * any)|stage (* any)|goal|rupees[|world-day|world-week]"; '#' comments
std::vector<Def> parse_pool(const std::string& text) {
    std::vector<Def> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        const auto f = split(line, '|');
        if (f.size() < 7) continue;
        Def d;
        d.id = uint16_t(std::strtoul(f[0].c_str(), nullptr, 10));
        d.name = f[1].substr(0, 40), d.what = f[2].substr(0, 80);
        if (f[3] != "*")
            for (const auto& p : split(f[3], ',')) d.procs.push_back(uint16_t(std::strtoul(p.c_str(), nullptr, 16)));
        if (f[4] != "*") d.stage = f[4].substr(0, f[4].rfind("event:", 0) == 0 ? 40 : 8);  // (a stage code, or event:<name>)
        d.goal = uint16_t(std::max(1ul, std::strtoul(f[5].c_str(), nullptr, 10)));
        d.rupees = uint32_t(std::min(5000ul, std::strtoul(f[6].c_str(), nullptr, 10)));
        if (f.size() >= 8) d.world = f[7].rfind("world", 0) == 0, d.weekly = f[7] == "world-week";
        if (d.id) out.push_back(d);
    }
    return out;
}
const Def* def_of(const State& st, uint16_t id) {
    for (const Def& d : st.pool) if (d.id == id) return &d;
    return nullptr;
}
// today's quests: kPerDay of the pool, the same for everyone on this server, from the day's number
void pick(State& st) {
    st.today.clear();
    std::vector<uint16_t> ids;
    for (const Def& d : st.pool) (d.id >= 100 || d.world ? st.today : ids).push_back(d.id);  // (events, server-wide: always)
    const size_t events = st.today.size();
    uint32_t h = st.day * 2654435761u + 12345;
    while (!ids.empty() && (int)(st.today.size() - events) < kPerDay) {
        h = h * 1103515245u + 12345;
        const size_t i = (h >> 8) % ids.size();
        st.today.push_back(ids[i]);
        ids.erase(ids.begin() + i);
    }
}

std::string state_path(const State& st) { return st.dir + "quests-state.txt"; }
void save(State& st, Server& srv, uint32_t now) {
    std::ofstream f(state_path(st), std::ios::trunc);
    if (!f) return;
    f << "day " << st.day << "\n";
    for (const auto& [kh, p] : st.players) {
        for (const auto& [id, n] : p.progress) f << "p " << std::hex << kh << std::dec << " " << id << " " << n << "\n";
        for (uint16_t id : p.done) f << "d " << std::hex << kh << std::dec << " " << id << "\n";
    }
    for (const auto& [id, w] : st.worlds) {
        f << "w " << id << " " << w.period << " " << w.count << " " << (w.done ? 1 : 0) << "\n";
        for (const auto& [kh, n] : w.mine) f << "m " << id << " " << std::hex << kh << std::dec << " " << n << "\n";
        for (uint64_t kh : w.paid) f << "k " << id << " " << std::hex << kh << std::dec << "\n";
    }
    st.dirty = false, st.saved_at = now;
    (void)srv;
}
void load(State& st, Server& srv) {
    st.loaded = true;
    const std::string cp = srv.config().config_path;
    const size_t slash = cp.find_last_of('/');
    st.dir = slash == std::string::npos ? "" : cp.substr(0, slash + 1);
    if (const char* d = std::getenv("WWHD_QUESTS_DIR")) st.dir = std::string(d) + "/";
    std::ifstream pf(st.dir + "quests.txt");
    std::stringstream text;
    if (pf) text << pf.rdbuf();
    st.pool = parse_pool(pf ? text.str() : kBuiltIn);
    if (st.pool.empty()) st.pool = parse_pool(kBuiltIn);
    st.day = utc_day();
    std::ifstream sf(state_path(st));
    std::string kind;
    uint32_t day = 0;
    while (sf >> kind) {
        uint64_t kh = 0;
        unsigned id = 0, n = 0, a = 0, b = 0;
        if (kind == "day") sf >> day;
        else if (kind == "p" && sf >> std::hex >> kh >> std::dec >> id >> n) { if (day == st.day) st.players[kh].progress[uint16_t(id)] = uint16_t(n); }
        else if (kind == "d" && sf >> std::hex >> kh >> std::dec >> id) { if (day == st.day) st.players[kh].done.insert(uint16_t(id)); }
        else if (kind == "w" && sf >> id >> a >> n >> b) { World& w = st.worlds[uint16_t(id)]; w.period = a, w.count = n, w.done = b; }
        else if (kind == "m" && sf >> id >> std::hex >> kh >> std::dec >> n) st.worlds[uint16_t(id)].mine[kh] = n;
        else if (kind == "k" && sf >> id >> std::hex >> kh >> std::dec) st.worlds[uint16_t(id)].paid.insert(kh);
        else break;
    }
    // server-wide quests of another period (or gone from the pool) start over
    for (auto it = st.worlds.begin(); it != st.worlds.end();) {
        const Def* d = def_of(st, it->first);
        if (!d || !d->world || it->second.period != period_of(*d)) it = st.worlds.erase(it);
        else ++it;
    }
    for (const Def& d : st.pool) if (d.world && !st.worlds.count(d.id)) st.worlds[d.id].period = period_of(d);
    pick(st);
    if (srv.log) srv.log("fork quests: " + std::to_string(st.pool.size()) + " in the pool (" + (pf ? st.dir + "quests.txt" : "built in") + "), day " + std::to_string(st.day));
}

void send(Session& s, const Bytes& body) {
    if (!s.online) return;
    Bytes out;
    Writer w(out);
    w.u16(fp::kFromServer);
    w.u8(fp::kForkApp);
    w.bytes(body.data(), body.size());
    s.chan.send_reliable(proto::kMsgAppFrom, out);
}
void send_quests(State& st, Session& s) {
    Player& p = st.players[s.key_hash];
    Bytes b;
    Writer w(b);
    w.u8(fp::kQuests);
    w.u32(st.day);
    w.u32(to_next_day());
    w.u8(uint8_t(st.today.size()));
    for (uint16_t id : st.today) {
        const Def* d = def_of(st, id);
        if (!d) continue;
        w.u16(d->id);
        w.u8(d->world ? fp::kWorldKind : d->id >= 100 ? fp::kEventKind : fp::kDefeatKind);
        w.u16(d->goal);
        if (d->world) {
            const World& wd = st.worlds[id];
            w.u16(uint16_t(std::min<uint32_t>(wd.count, d->goal))), w.u8(wd.done ? 1 : 0);
        } else {
            w.u16(p.progress.count(id) ? p.progress[id] : 0);
            w.u8(p.done.count(id) ? 1 : 0);
        }
        w.u32(d->rupees);
        w.str8(d->name);
        w.str8(d->what);
    }
    // the server-wide quests: this player's part, the time left
    std::vector<const Def*> worlds;
    for (uint16_t id : st.today) if (const Def* d = def_of(st, id); d && d->world) worlds.push_back(d);
    w.u8(uint8_t(worlds.size()));
    for (const Def* d : worlds) {
        const World& wd = st.worlds[d->id];
        const auto m = wd.mine.find(s.key_hash);
        w.u16(d->id), w.u16(uint16_t(std::min<uint32_t>(m == wd.mine.end() ? 0 : m->second, 65535))), w.u32(left_of(*d));
    }
    send(s, b);
}
void send_reward(Session& s, const Def& d) {
    Bytes r;
    Writer wr(r);
    wr.u8(fp::kReward), wr.u16(d.id), wr.u32(d.rupees), wr.str8(d.name);
    send(s, r);
}
void send_world(State& st, Session& s, const Def& d) {
    const World& wd = st.worlds[d.id];
    const auto m = wd.mine.find(s.key_hash);
    Bytes b;
    Writer w(b);
    w.u8(fp::kWorld), w.u16(d.id), w.u16(uint16_t(std::min<uint32_t>(wd.count, d.goal))), w.u8(wd.done ? 1 : 0);
    w.u16(uint16_t(std::min<uint32_t>(m == wd.mine.end() ? 0 : m->second, 65535))), w.u32(left_of(d));
    send(s, b);
}
// a server-wide quest reached: everyone who added to it and is here now is paid (the others at their next hello)
void pay_world(Server& srv, State& st, const Def& d) {
    World& wd = st.worlds[d.id];
    for (const auto& [kh, n] : wd.mine) {
        if (wd.paid.count(kh)) continue;
        Session* t = srv.find_by_key(kh);
        if (!t || !t->online || !st.players[kh].hello) continue;
        send_reward(*t, d);
        wd.paid.insert(kh);
    }
    st.dirty = true;
}
// one defeat for the server-wide quests it fits (once per defeat, for its reporter)
void credit_world(Server& srv, State& st, Session& s, const std::string& stage, uint16_t proc) {
    for (uint16_t id : st.today) {
        const Def* d = def_of(st, id);
        if (!d || !d->world) continue;
        World& wd = st.worlds[id];
        if (wd.done) continue;
        if (!d->procs.empty() && std::find(d->procs.begin(), d->procs.end(), proc) == d->procs.end()) continue;
        if (!d->stage.empty() && d->stage != stage) continue;
        if (d->stage.empty() && stage.rfind("event:", 0) == 0) continue;
        wd.count++, wd.mine[s.key_hash]++;
        st.dirty = st.world_dirty = true;
        if (wd.count >= d->goal) {
            wd.done = true;
            if (srv.log) srv.log("fork quests: server-wide " + d->name + " reached (" + std::to_string(wd.mine.size()) + " players), +" + std::to_string(d->rupees) + " rupees each");
            pay_world(srv, st, *d);
        }
    }
}
// one defeat for this player: today's quests it fits go up; one reaching its goal is done and paid
void credit(Server& srv, State& st, Session& s, const std::string& stage, uint16_t proc) {
    Player& p = st.players[s.key_hash];
    for (uint16_t id : st.today) {
        const Def* d = def_of(st, id);
        if (!d || d->world || p.done.count(id)) continue;
        if (!d->procs.empty() && std::find(d->procs.begin(), d->procs.end(), proc) == d->procs.end()) continue;
        if (!d->stage.empty() && d->stage != stage) continue;
        if (d->stage.empty() && stage.rfind("event:", 0) == 0) continue;  // (an event is not a defeat)
        uint16_t& n = p.progress[id];
        n = uint16_t(std::min<int>(n + 1, d->goal));
        st.dirty = true;
        const bool done = n >= d->goal;
        if (done) p.done.insert(id);
        if (srv.log) srv.log("fork quests: " + s.name + ": " + d->name + " " + std::to_string(n) + "/" + std::to_string(d->goal) + (done ? " done, +" + std::to_string(d->rupees) + " rupees" : ""));
        if (p.hello) {
            Bytes b;
            Writer w(b);
            w.u8(fp::kProgress), w.u16(id), w.u16(n), w.u8(done ? 1 : 0);
            send(s, b);
            if (done) send_reward(s, *d);
        }
    }
}
}  // namespace

void on_message(Server& srv, Session& s, const uint8_t* body, size_t n, uint32_t now) {
    State& st = g_state[&srv];
    if (!st.loaded) load(st, srv);
    Reader r(body, n);
    const uint8_t op = r.u8();
    Player& p = st.players[s.key_hash];
    if (op == fp::kHello) {
        const uint16_t version = r.u16();
        r.u32();
        if (!r.ok()) return;
        p.hello = true, p.sid = s.id;
        Bytes b;
        Writer w(b);
        w.u8(fp::kWelcome), w.u16(fp::kVersion), w.u32(fp::kCapQuests | fp::kCapWorld), w.str8(srv.config().name);
        send(s, b);
        send_quests(st, s);
        for (uint16_t id : st.today)  // (a server-wide quest reached while away: paid now)
            if (const Def* d = def_of(st, id); d && d->world && st.worlds[id].done && st.worlds[id].mine.count(s.key_hash) && !st.worlds[id].paid.count(s.key_hash))
                send_reward(s, *d), st.worlds[id].paid.insert(s.key_hash), st.dirty = true;
        if (srv.log) srv.log("fork quests: " + s.name + " (fork " + std::to_string(version) + ") said hello");
        return;
    }
    if (!p.hello) return;
    if (op == fp::kAsk) return send_quests(st, s);
    if (op == fp::kEvent) {
        const std::string name = r.str8(32);
        const uint8_t result = r.u8();
        if (!r.ok() || result != 1) return;
        const std::string ev = "event:" + name;
        if (srv.log) srv.log("fork quests: " + s.name + "'s party won the event " + name);
        credit(srv, st, s, ev, 0);
        if (Party* party = srv.party_of(s))
            for (uint64_t kh : party->members) {
                if (kh == s.key_hash) continue;
                Session* t = srv.find_by_key(kh);
                if (t && t->online && t->where.stage == s.where.stage && t->where.room == s.where.room) credit(srv, st, *t, ev, 0);
            }
        return;
    }
    if (op == fp::kDefeat) {
        char stage[9] = {};
        r.bytes((uint8_t*)stage, 8);
        const int8_t room = (int8_t)r.u8();
        const uint16_t proc = r.u16();
        const uint8_t flags = r.u8();
        if (!r.ok()) return;
        // believed at most kMostDefeatsPerSecond a second (a bucket)
        p.budget = std::min<double>(kMostDefeatsPerSecond * 2, p.budget + (now - p.budget_at) / 1000.0 * kMostDefeatsPerSecond);
        p.budget_at = now;
        if (p.budget < 1) return;
        p.budget -= 1;
        const std::string where(stage, strnlen(stage, 8));
        if (srv.log) {
            char t[96];
            std::snprintf(t, sizeof t, "fork quests: %s defeated proc %X in %s room %d%s", s.name.c_str(), proc, where.c_str(), room, flags & 1 ? " (for the place)" : "");
            srv.log(t);
        }
        credit(srv, st, s, where, proc);
        credit_world(srv, st, s, where, proc);
        // one this game ran for its place: every party member in the same place helped
        if (flags & 1)
            if (Party* party = srv.party_of(s))
                for (uint64_t kh : party->members) {
                    if (kh == s.key_hash) continue;
                    Session* t = srv.find_by_key(kh);
                    if (t && t->online && t->where.stage == s.where.stage && t->where.room == s.where.room) credit(srv, st, *t, where, proc);
                }
    }
}

void tick(Server& srv, uint32_t now) {
    State& st = g_state[&srv];
    if (!st.loaded) return;  // (nobody said hello yet)
    if (utc_day() != st.day) {  // a new day: new quests, everyone's progress starts over
        st.day = utc_day();
        for (auto& [kh, p] : st.players) p.progress.clear(), p.done.clear();
        pick(st);
        st.dirty = true;
        for (auto& [kh, p] : st.players)
            if (p.hello)
                if (Session* s = srv.find_by_key(kh); s && s->online) send_quests(st, *s);
        if (srv.log) srv.log("fork quests: day " + std::to_string(st.day));
    }
    // server-wide quests: a new period starts over; their counts to everyone at most once a second
    for (const Def& d : st.pool) {
        if (!d.world) continue;
        World& wd = st.worlds[d.id];
        if (wd.period == period_of(d)) continue;
        wd = World{};
        wd.period = period_of(d);
        st.dirty = st.world_dirty = true;
        if (srv.log) srv.log("fork quests: server-wide " + d.name + " starts over");
    }
    if (st.world_dirty && now - st.world_sent > 1000) {
        st.world_dirty = false, st.world_sent = now;
        for (auto& [kh, p] : st.players) {
            if (!p.hello) continue;
            Session* s = srv.find_by_key(kh);
            if (!s || !s->online) continue;
            for (uint16_t id : st.today) if (const Def* d = def_of(st, id); d && d->world) send_world(st, *s, *d);
        }
    }
    if (st.dirty && now - st.saved_at > 10000) save(st, srv, now);
}
}  // namespace online::server::fork_quests
