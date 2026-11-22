// (fork: Android) Online: down, not dead (notes/online-modes.md, "Deaths in co-op"). While a party member who is not down
// is in the same place, Link's last heart going does not end the game: the game takes Link's life at 0 during his damage
// flinch and starts the Game Over only once the flinch is over (daPy_lk_c::changeDeadProc, where the bottled fairy
// revives); here, in that gap, he gets a quarter heart back and is down instead:
//   - held where he is (no input but + and -: hle VPADRead asks holding()), blinking and untouchable (his own damage wait
//     timer kept up: no blow takes hold), the party told (kDownMsg) and shown a notice;
//   - a party member who is not down standing next to him for 3 s gets him up with 3 hearts (no more than his full);
//   - after 30 s, or once every party member in the place is down, the party is beaten: each downed Link is put back
//     where he came into the place, hearts full. Nothing is loaded again: the monsters, their health, what was killed and
//     opened stay as they are.
// Alone in the place (no party member, or all already down when he falls), the game's own Game Over plays as before.
#include <cmath>
#include <cstring>
#include <map>
#include <string>

#include "../mods/coop_layout.h"
#include "../mods/coop_names.h"
#include "../mods/coop_net.h"
#include "../mods/coop_sync.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "runtime.h"

namespace ss { void notice(const std::string& text); }
namespace mods::coop::world { void put_link_at(uint32_t link, const float at[3]); }

namespace android_down {
constexpr mods::coop::net::Msg kDownMsg = (mods::coop::net::Msg)0x67;  // (the fork's) any -> party: 'W' 'D', u8 down
namespace {
namespace net = mods::coop::net;
namespace sync = mods::coop::sync;
namespace names = mods::coop::names;
namespace sv = mods::coop::layout::sv;

const uint32_t kPlay = GD(0x1046F0B0), kStage = kPlay + names::inf::kCurStage;
constexpr uint32_t kPos = 0x314, kRoom = 0x326, kDamageWait = 0x3B0;  // daPy_py_c::mDamageWaitTimer
constexpr int kReviveSteps = 90, kBeatenSteps = 900, kReach = 170;   // 3 s next to him, 30 s alone, units
uint32_t save_status() { const uint32_t info = ld32(GD(0x101F84DC)); return info ? info + 0x20 + sv::kStatusA : 0; }

bool g_down = false;
int g_steps = 0, g_help = 0, g_helper = -1;
std::map<int, bool> g_others;  // party members' down state, by player number
uint32_t g_told = 0;
// where Link came into this place (stage, room): a beaten party comes back there
char g_at_stage[8];
int g_at_room = -999, g_at_wait = 0;
float g_at[3];

bool other_down(int id) { const auto it = g_others.find(id); return it != g_others.end() && it->second; }
void tell(bool down) {
    net::send(-1, kDownMsg, std::string("WD") + (char)(down ? 1 : 0));
    g_told = net::now_ms();
}
// the party members shown in this place (sync::place: a member's puppet here), and whether one of them is up
int up_here(uint32_t link, int& nearest_up, float& dist, int& down_here) {
    int n = 0;
    down_here = 0;
    nearest_up = -1, dist = 1e9f;
    const float lx = (float)ldf32(link + kPos), ly = (float)ldf32(link + kPos + 4), lz = (float)ldf32(link + kPos + 8);
    for (int id = 0; id < net::kMaxPlayers; id++) {
        float p[3];
        if (id == net::my_id() || !net::member(id) || !sync::place(id, p)) continue;
        if (other_down(id)) { down_here++; continue; }
        n++;
        const float d = std::hypot(p[0] - lx, p[2] - lz);
        if (std::fabs(p[1] - ly) < 200 && d < dist) dist = d, nearest_up = id;
    }
    return n;
}
void get_up(uint32_t link, uint32_t st, int quarters, const char* why) {
    const int full = ld16(st + sv::kMaxLife) / 4 * 4;
    st16(st + sv::kLife, (uint16_t)std::max(1, std::min(quarters, full)));
    st16(link + kDamageWait, 0);
    g_down = false, g_steps = 0, g_help = 0, g_helper = -1;
    tell(false);
    LOG("[down] up again: %s", why);
}
}  // namespace

// hle VPADRead: Link is held (no buttons but + and -, no sticks)
bool holding() { return g_down; }

// another player's word (the session's poll, game thread)
void received(int from, const uint8_t* d, size_t n) {
    if (n < 3 || d[0] != 'W' || d[1] != 'D') return;
    const bool down = d[2] != 0, was = other_down(from);
    g_others[from] = down;
    if (down != was) {
        const std::string who = net::name(from);
        ss::notice(down ? who + " is down: stand next to them to help them up" : who + " is back up");
    }
}

// game thread, each full step (online_compat::tick)
void step(Cpu* c) {
    (void)c;
    if (net::role() == net::Role::kOff) return g_others.clear(), g_down = false, void();
    for (auto it = g_others.begin(); it != g_others.end();) it = net::connected(it->first) ? std::next(it) : g_others.erase(it);
    const uint32_t link = ld32(kPlay + names::inf::kLink), st = save_status();
    if (link < 0x10000000u || link >= 0x50000000u || !st) return void(g_down = false);
    // where Link came into this place: his place a second after he came (once settled)
    const int room = (int8_t)ld8(link + kRoom);
    if (memcmp(g_at_stage, (const char*)mem::ptr(kStage), 8) || room != g_at_room) {
        memcpy(g_at_stage, (const char*)mem::ptr(kStage), 8), g_at_room = room, g_at_wait = 30;
        if (g_down) get_up(link, st, 12, "another place");
    }
    if (g_at_wait > 0 && --g_at_wait == 0 && !g_down)
        for (int e = 0; e < 3; e++) g_at[e] = (float)ldf32(link + kPos + 4 * e);
    int helper = -1;
    float dist = 0;
    int down_here = 0;
    const int up = up_here(link, helper, dist, down_here);
    const int life = ld16(st + sv::kLife);
    if (!g_down) {
        // the last heart gone, a party member up in this place: down, not dead (in the flinch, before the Game Over)
        // (the last one standing goes down too: the beaten party comes back together, no Game Over)
        if (life == 0 && (up > 0 || down_here > 0)) {
            st16(st + sv::kLife, 1);
            g_down = true, g_steps = 0, g_help = 0;
            tell(true);
            ss::notice("You are down: a party member next to you for 3 s gets you up");
            LOG("[down] down (%d party members up here, %d down)", up, down_here);
        }
        return;
    }
    // down: held, untouchable, a quarter heart kept
    g_steps++;
    st16(link + kDamageWait, 60);
    if (ld16(st + sv::kLife) == 0) st16(st + sv::kLife, 1);
    if (net::now_ms() - g_told > 2000) tell(true);  // (a newcomer hears it too)
    // a helper next to him: up after 3 s
    if (helper >= 0 && dist < kReach) {
        if (helper != g_helper) g_helper = helper, g_help = 0;
        if (++g_help == 1) ss::notice(net::name(helper) + " is helping you up");
        if (g_help >= kReviveSteps) {
            ss::notice(net::name(helper) + " got you up");
            return get_up(link, st, 12, "helped");
        }
    } else {
        g_help = 0, g_helper = -1;
    }
    // beaten: nobody up here any more (2 s to be sure), or 30 s down: back where he came in, hearts full
    if ((up == 0 && g_steps > 60) || g_steps >= kBeatenSteps) {
        if (g_at_wait == 0) mods::coop::world::put_link_at(link, g_at);
        ss::notice(up == 0 ? "The party is beaten: back at the way in" : "Nobody came: back at the way in");
        get_up(link, st, 999, up == 0 ? "the party beaten" : "30 s down");
    }
}
}  // namespace android_down
