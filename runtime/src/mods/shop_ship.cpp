// Beedle's Shop Ship (and the other ships entered from the sea): leaving the shop closed the game
// with the game's own assertion (d_stage.cpp:5179, "dComIfGp_getIkadaShipBeforeRoomId() >= 0 &&
// dComIfGp_getIkadaShipBeforeRoomId() < 64").
// Entering (025C38B8, the scene change of an exit: types 62 Obshop, 59 Abship) stores the sea room
// the ship is in (game info +0x5D16; from the room of the collision Link touched, 024EF130) and the
// ship's position (+0x5D18); leaving (type 61) sails back to that room of "sea". A room of -1 (the
// collision gave none) was kept and the way out halted the game. Both places now make sure of a sea
// room: from the ship's position on the 7x7 grid of sea rooms (100000 units each, room 1 at the
// north-west corner) where the stored one isn't valid.
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "../release.h"
#include "../runtime.h"

namespace {
// the sea room (1..49) of a position on the sea
uint8_t sea_room(double x, double z) {
    auto cell = [](double v) {
        if (!std::isfinite(v)) return 3;
        int k = (int)std::floor((v + 350000.0) / 100000.0);
        return k < 0 ? 0 : k > 6 ? 6 : k;
    };
    return (uint8_t)(1 + cell(x) + 7 * cell(z));
}

// entering: r31 = the room about to be stored, r30 = the ship (actor position at +0x314)
void entering(Cpu* c) {
    const int8_t room = (int8_t)c->r[31];
    LOG("[ship] entering a ship of sea room %d (ship at %.0f %.0f)", (int)room, ldf32(c->r[30] + 0x314), ldf32(c->r[30] + 0x31C));
    if (room >= 0 && room < 64) return;
    const uint8_t r = sea_room(ldf32(c->r[30] + 0x314), ldf32(c->r[30] + 0x31C));
    LOG("[ship] entering a ship from sea room %d: the room from its position, %d", (int)room, (int)r);
    c->r[31] = r;
}
}  // namespace

extern "C" void site_025C3964(Cpu* c) { entering(c); }  // stb r31, 0x5D16(r3): Obshop
extern "C" void site_025C39F0(Cpu* c) { entering(c); }  // the same: Abship

// leaving (type 61): r3 = game info, about to read the stored room
// test aid: WWHD_TEST_SHIP=room:id stands for the ship entered from the sea (a shop reached by
// WWHD_TEST_WARP has none): the way out goes to that ship in that sea room
extern "C" void site_025C3A64(Cpu* c) {
    const uint32_t g = c->r[3];
    if (const char* e = getenv("WWHD_TEST_SHIP")) {
        static bool once = false;
        int room = 0, id = 0;
        if (!once && sscanf(e, "%d:%d", &room, &id) == 2) {
            once = true;
            LOG("[ship] test: leaving to the ship %d of sea room %d (stored: %d, %d)", id, room, (int)(int8_t)ld8(g + 0x5D16), (int)ld8(g + 0x5D17));
            st8(g + 0x5D16, (uint8_t)room);
            st8(g + 0x5D17, (uint8_t)id);
        }
    }
    const uint8_t room = ld8(g + 0x5D16);
    if (room < 64) return;
    const uint8_t r = sea_room(ldf32(g + 0x5D18), ldf32(g + 0x5D20));
    LOG("[ship] leaving a ship without its sea room (%d): room %d, from the ship's position", (int)(int8_t)room, (int)r);
    st8(g + 0x5D16, r);
}
