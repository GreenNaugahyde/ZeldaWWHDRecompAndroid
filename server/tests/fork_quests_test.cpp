// (fork) End-to-end test of the fork's daily quests (src/fork_quests.cpp) on 127.0.0.1: a fork game says hello and
// hears the greeting and today's quests, its defeats count, a party member in the same place is credited too, a quest
// reaching its goal pays; a game that never says hello (upstream's) hears nothing of it.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "../../runtime/src/online/fork_proto.h"
#include "proto/client.h"
#include "server.h"

using namespace online;
using namespace online::proto;
namespace fp = fork_proto;

static int g_fail = 0;
#define CHECK(c)                                                     \
    do {                                                             \
        if (!(c)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
            ++g_fail;                                                \
        }                                                            \
    } while (0)

struct Bot {
    Client c;
    std::vector<ClientEvent> log;
    std::vector<Bytes> fork;  // the server's fork words
    PoseHeader head;
    Bytes blob = Bytes(40, 7);
    bool online() const { return c.state() == ClientState::kOnline; }
    void poll() {
        for (auto& e : c.poll(monotonic_ms())) {
            if (e.kind == ClientEvent::kApp && e.id == fp::kFromServer && e.app_type == fp::kForkApp) fork.push_back(e.body);
            log.push_back(e);
        }
        if (online()) c.send_pose(head, blob.data(), blob.size());
    }
    void say(const Bytes& b) {
        AppHeader h;
        h.type = fp::kForkApp, h.target = fp::kToServer;
        c.send_app(h, b.data(), b.size(), true);
    }
    int count(uint8_t op) const {
        int n = 0;
        for (auto& b : fork) n += !b.empty() && b[0] == op;
        return n;
    }
    const Bytes* last(uint8_t op) const {
        for (auto it = fork.rbegin(); it != fork.rend(); ++it) if (!it->empty() && (*it)[0] == op) return &*it;
        return nullptr;
    }
    const ClientEvent* last_ev(ClientEvent::Kind k) const {
        for (auto it = log.rbegin(); it != log.rend(); ++it) if (it->kind == k) return &*it;
        return nullptr;
    }
};

static std::string g_dir;
static std::unique_ptr<Bot> make_bot(const std::string& name, uint16_t port) {
    auto b = std::make_unique<Bot>();
    ClientConfig cfg;
    cfg.port = port;
    cfg.game_data = 0x1111;
    cfg.identity = crypto::Identity::generate();
    cfg.name = name;
    std::string why;
    CHECK(b->c.start(cfg, why));
    b->head.where = Where::of("M_NewD2", 3);
    return b;
}
static bool run(server::Server& s, std::vector<Bot*> bots, uint32_t ms, std::function<bool()> done) {
    const uint32_t end = monotonic_ms() + ms;
    while (int32_t(monotonic_ms() - end) < 0) {
        s.step(1);
        for (Bot* b : bots) b->poll();
        if (done()) return true;
    }
    return done();
}
static Bytes hello() { Bytes b; Writer w(b); w.u8(fp::kHello), w.u16(fp::kVersion), w.u32(fp::kCapQuests); return b; }
static Bytes defeat(uint16_t proc, bool for_place) {
    Bytes b;
    Writer w(b);
    w.u8(fp::kDefeat);
    const char st[8] = {'M', '_', 'N', 'e', 'w', 'D', '2', 0};
    w.bytes(st, 8), w.u8(3), w.u16(proc), w.u8(for_place ? 1 : 0);
    return b;
}
// a kProgress: quest id, progress, done
static bool progress_of(const Bot& b, uint16_t id, uint16_t& n, bool& done) {
    for (auto it = b.fork.rbegin(); it != b.fork.rend(); ++it) {
        if (it->empty() || (*it)[0] != fp::kProgress) continue;
        Reader r(it->data() + 1, it->size() - 1);
        if (r.u16() != id) continue;
        n = r.u16(), done = r.u8();
        return true;
    }
    return false;
}

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    g_dir = argc > 1 ? argv[1] : ".";
    {  // a pool of two: both are today's
        std::ofstream q(g_dir + "/quests.txt");
        q << "# test pool\n1|Two Bokoblins|Defeat 2 Bokoblins|BD|*|2|100\n2|Any three|Defeat 3 monsters|*|*|3|50\n"
             "100|Siege|Win the siege|*|event:siege|1|500\n"
             "200|Together|Everyone: 4 Bokoblins|BD|*|4|70|world-day\n";
    }
    std::remove((g_dir + "/quests-state.txt").c_str());
    setenv("WWHD_QUESTS_DIR", g_dir.c_str(), 1);
    server::Config cfg;
    cfg.port = 0, cfg.pow_bits = 6, cfg.per_ip = 64, cfg.admin_socket = "", cfg.game_data = {0x1111};
    cfg.key_file = g_dir + "/server.key", cfg.ban_file = g_dir + "/bans.txt", cfg.report_file = g_dir + "/reports.log";
    cfg.team_state_file = "";
    server::Server s(cfg);
    std::string why;
    CHECK(s.start(why));
    auto fork = make_bot("fork", s.port()), mate = make_bot("mate", s.port()), up = make_bot("upstream", s.port());
    std::vector<Bot*> all = {fork.get(), mate.get(), up.get()};
    CHECK(run(s, all, 5000, [&] { return fork->online() && mate->online() && up->online(); }));

    // hello: the greeting and today's two quests, nothing yet done
    fork->say(hello());
    CHECK(run(s, all, 3000, [&] { return fork->count(fp::kWelcome) && fork->count(fp::kQuests); }));
    if (const Bytes* q = fork->last(fp::kQuests)) {
        Reader r(q->data() + 1, q->size() - 1);
        r.u32();
        const uint32_t left = r.u32();
        CHECK(left > 0 && left <= 86400);
        CHECK(r.u8() == 4);  // (the two, the event and the server-wide one every day)
    }
    // the upstream game, never saying hello: its defeats count for nothing, it hears nothing
    up->say(defeat(0xBD, false));
    run(s, all, 500, [] { return false; });
    CHECK(up->fork.empty());

    // two Bokoblins: quest 1 done and paid, quest 2 at 2 of 3
    fork->say(defeat(0xBD, false));
    fork->say(defeat(0xBD, false));
    CHECK(run(s, all, 3000, [&] { return fork->count(fp::kReward) == 1; }));
    uint16_t n = 0;
    bool done = false;
    CHECK(progress_of(*fork, 1, n, done) && n == 2 && done);
    CHECK(progress_of(*fork, 2, n, done) && n == 2 && !done);
    if (const Bytes* r = fork->last(fp::kReward)) {
        Reader rr(r->data() + 1, r->size() - 1);
        CHECK(rr.u16() == 1 && rr.u32() == 100);
    }
    // more Bokoblins don't pay quest 1 again
    fork->say(defeat(0xBD, false));
    CHECK(run(s, all, 3000, [&] { return fork->count(fp::kReward) == 2; }));  // (quest 2 reached 3)
    CHECK(progress_of(*fork, 2, n, done) && n == 3 && done);

    // a party: the mate (a fork game in the same place) is credited for a monster the fork game ran for the place
    mate->say(hello());
    CHECK(run(s, all, 3000, [&] { return mate->count(fp::kQuests) > 0; }));
    uint64_t code = 0;
    parse_friend_code(mate->c.friend_code(), code);
    fork->c.party_invite(code);
    CHECK(run(s, all, 3000, [&] { return mate->last_ev(ClientEvent::kPartyInvited) != nullptr; }));
    if (auto* e = mate->last_ev(ClientEvent::kPartyInvited)) mate->c.party_accept(e->party_id);
    CHECK(run(s, all, 3000, [&] { auto* e = fork->last_ev(ClientEvent::kPartyState); return e && e->party.members.size() == 2; }));
    fork->say(defeat(0xCE, true));
    CHECK(run(s, all, 3000, [&] { return progress_of(*mate, 2, n, done) && n == 1; }));
    // one not run for the place: the sender alone
    fork->say(defeat(0xCE, false));
    run(s, all, 700, [] { return false; });
    CHECK(progress_of(*mate, 2, n, done) && n == 1);
    // the event: won by the fork game's party, both credited (the mate is in the same place); defeats never count for it
    CHECK(!progress_of(*fork, 100, n, done));
    {
        Bytes b;
        Writer w(b);
        w.u8(fp::kEvent), w.str8("siege"), w.u8(1);
        fork->say(b);
    }
    CHECK(run(s, all, 3000, [&] { return progress_of(*mate, 100, n, done) && done && progress_of(*fork, 100, n, done) && done; }));
    CHECK(progress_of(*mate, 2, n, done) && n == 1);  // (an event is not a defeat)
    // server-wide: the fork game's 3 Bokoblins counted once each (3 of 4); the mate's one reaches it: both paid
    auto world_of = [](const Bot& b, uint16_t& count, bool& done, uint16_t& mine) {
        for (auto it = b.fork.rbegin(); it != b.fork.rend(); ++it) {
            if (it->empty() || (*it)[0] != fp::kWorld) continue;
            Reader r(it->data() + 1, it->size() - 1);
            if (r.u16() != 200) continue;
            count = r.u16(), done = r.u8(), mine = r.u16();
            return true;
        }
        return false;
    };
    uint16_t wc = 0, wm = 0;
    bool wd = false;
    CHECK(run(s, all, 3000, [&] { return world_of(*fork, wc, wd, wm) && wc == 3; }));
    CHECK(wm == 3 && !wd);
    const int fork_rewards = fork->count(fp::kReward), mate_rewards = mate->count(fp::kReward);
    mate->say(defeat(0xBD, true));  // (for the place: the fork game is credited personally, the world count once)
    CHECK(run(s, all, 3000, [&] { return world_of(*mate, wc, wd, wm) && wd; }));
    CHECK(wc == 4 && wm == 1);
    CHECK(run(s, all, 3000, [&] { return fork->count(fp::kReward) > fork_rewards && mate->count(fp::kReward) > mate_rewards; }));
    // ask again: the list says what is done
    mate->say(Bytes{fp::kAsk});
    const int had = mate->count(fp::kQuests);
    CHECK(run(s, all, 3000, [&] { return mate->count(fp::kQuests) > had; }));
    CHECK(up->fork.empty());

    if (g_fail) {
        std::printf("fork_quests_test: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("fork_quests_test: all passed\n");
    return 0;
}
