// The mini map on the TV picture (the app's MiniMap.java draws it with the GamePad map's own artwork): where
// Link and the King of Red Lions are on the Great Sea and which chart squares are filled in. Read on demand by
// the app (any thread), as touch_state.
//   Chart squares: 7 x 7 of 100,000 units around the origin, room 1 in the north-west corner (dMap_c::point2Grid,
//   d_map.cpp). A square's island shows on the GamePad once the fishman has filled it in: dSv_player_map_c
//   mFmapBits[49] bit 0 (isSaveArriveGrid, d_save.cpp; the fishman d_a_npc_so.cpp sets it), the player's map data at
//   save + 0xC4 as on the GameCube (+0x40 the bits).
//   The ship: dComIfG_play_c mpPlayerPtr[3] (Link, partner, ship; GameCube play + 0x48AC, right after mPlayerInfo[0]
//   {mpPlayer, mCameraID} at 0x48A4) moved with the rest of that part of play in HD (mRStatus 0x492D -> 0x5BB5):
//   found once as Link's pointer twice, 8 bytes apart, in a window around it; the partner (Medli, Makar) 0xC after
//   the first, the ship 0x10.
//   Actors (HD fopAc_ac_c): current.pos +0x314, current.roomNo +0x326 (s8), shape_angle.y +0x32A.
//   Elsewhere (houses, caves, dungeons): the stage's working memory dSv_info_c::mMemory at save + 0x778 (warp.cpp,
//   cheats.cpp): dSv_memBit_c mTbox +0x0 (chests opened), mVisitedRoom[2] +0x18, mDungeonItem +0x21 (bit 0 the map, 1
//   the compass).
//   The wind (the map's wind arrow): dScnKy_env_light_c g_env_light (US 0x10475A68, as dKy_getEnvlight 02555D0C
//   returns it), mWind.mWindVec +0x9FC (dKyw_get_wind_vec 0257DAA8: x, y, z; the way the wind blows) and mWindPower
//   +0xA18 (0..1; dKyw_get_wind_pow 0257DB04).
// WWHD_MINIMAP_LOG=1 logs the ship slot when found and the chart bits when they change.
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "mods.h"
#include "../hud.h"
#include "../release.h"
#include "../runtime.h"

namespace mods {
namespace {
const release::Data kPlay{0x1046F0B0};
constexpr uint32_t kStartStage = 0x5134, kPlayerStatus0 = 0x5CD8, kPlayerStatus1 = 0x5CDC;
constexpr uint32_t kPos = 0x314, kRoomNo = 0x326, kShapeAngleY = 0x32A;
constexpr uint32_t kStageMemory = 0x778, kVisitedRooms = kStageMemory + 0x18, kDungeonItems = kStageMemory + 0x21;
constexpr uint32_t kFmapBits = 0xC4 + 0x40;
// the player pointers' window: GameCube 0x48AC + the HD shift of its neighbours (0x1288 .. 0x12B0)
constexpr uint32_t kPtrFrom = 0x5B00, kPtrTo = 0x5C00;
// Link aiming or looking (mPlayerStatus 0 / 1; ItemRing.java): bow, first person, hookshot, grappling hook,
// boomerang, telescope; conducting, Picto Box
constexpr uint32_t kBusy0 = 0x1000 | 0x2000 | 0x4000 | 0x20000 | 0x80000 | 0x200000, kBusy1 = 0x1 | 0x8;
constexpr uint32_t kShip = 0x10000;  // on the boat
const release::Data kOverlap{0x101F36CC};  // a fade / wipe (touch_state.cpp)
const release::Data kEnvLight{0x10475A68};
constexpr uint32_t kWindVec = 0x9FC, kWindPower = 0xA18;

bool ptr_ok(uint32_t p) { return p >= 0x10000000 && p < 0x50000000; }

bool log_on() {
    static const bool on = getenv("WWHD_MINIMAP_LOG") != nullptr;
    return on;
}

// play + offset of mPlayerInfo[0].mpPlayer (Link; mpPlayerPtr[0] 8 bytes on, the ship's 0x10 on), 0 until found
uint32_t player_ptrs(uint32_t link) {
    static uint32_t found = 0;
    if (found && ld32(kPlay + found) == link) return found;
    for (uint32_t o = kPtrFrom; o + 8 < kPtrTo; o += 4)
        if (ld32(kPlay + o) == link && ld32(kPlay + o + 8) == link) {
            if (log_on() && o != found) LOG("[minimap] player pointers at play + 0x%X (ship slot 0x%08X)", o, ld32(kPlay + o + 0x10));
            found = o;
            return o;
        }
    return 0;
}

float angle(uint32_t actor) { return (int16_t)ld16(actor + kShapeAngleY) * (float)(M_PI / 32768.0); }
}  // namespace

void minimap_state(float out[kMiniMapLen]) {
    for (int i = 0; i < kMiniMapLen; i++) out[i] = 0;
    uint32_t s = player_save(), link = link_actor();
    char stage[9] = {};
    memcpy(stage, mem::ptr(kPlay + kStartStage), 8);
    if (!s || !link || !stage[0] || !strcmp(stage, "sea_T") || !strcmp(stage, "Name")) return;  // (title, file select)
    bool sea = !strcmp(stage, "sea");
    out[0] = sea ? 1 : 2;
    out[61] = ldf32(link + kPos + 4);
    out[62] = (int8_t)ld8(link + kRoomNo);
    if (!sea) {
        out[63] = ld8(s + kDungeonItems);
        uint32_t tbox = ld32(s + kStageMemory);  // mTbox: the stage's chests opened
        out[68] = (float)(tbox & 0xFFFF);
        out[69] = (float)(tbox >> 16);
        for (int i = 0; i < 2; i++) {
            uint32_t v = ld32(s + kVisitedRooms + 4 * i);
            out[64 + 2 * i] = (float)(v & 0xFFFF);
            out[65 + 2 * i] = (float)(v >> 16);
        }
    }
    out[1] = ldf32(link + kPos);
    out[2] = ldf32(link + kPos + 8);
    out[3] = angle(link);
    uint32_t st0 = ld32(kPlay + kPlayerStatus0), st1 = ld32(kPlay + kPlayerStatus1);
    out[4] = (st0 & kShip) != 0;
    if (uint32_t o = player_ptrs(link)) {
        uint32_t ship = ld32(kPlay + o + 0x10);
        if (ptr_ok(ship)) {
            out[5] = 1;
            out[6] = ldf32(ship + kPos);
            out[7] = ldf32(ship + kPos + 8);
            out[8] = angle(ship);
        }
        uint32_t partner = ld32(kPlay + o + 0xC);
        if (!sea && ptr_ok(partner) && partner != link) {
            float x = ldf32(partner + kPos), y = ldf32(partner + kPos + 4), z = ldf32(partner + kPos + 8);
            if (std::isfinite(x) && std::isfinite(y) && std::isfinite(z)) {
                out[72] = 1;
                out[73] = x;
                out[74] = z;
                out[75] = angle(partner);
                out[76] = y;
            }
        }
    }
    out[9] = event_mode_now();
    out[10] = (st0 & kBusy0) || (st1 & kBusy1) || hud_menu_open();
    out[11] = ld32(kOverlap) != 0;
    // the chart's squares filled in (also indoors: the full map shows them anywhere)
    static uint8_t last[49];
    static bool logged = false;
    bool changed = false;
    for (int i = 0; i < 49; i++) {
        uint8_t b = ld8(s + kFmapBits + i);
        out[12 + i] = b & 1;
        changed |= b != last[i];
        last[i] = b;
    }
    if (log_on() && (changed || !logged)) {
        logged = true;
        char l[160];
        int n = 0;
        for (int i = 0; i < 49; i++) n += snprintf(l + n, sizeof l - n, "%02X", last[i]);
        LOG("[minimap] chart bits %s (Link %.0f, %.0f)", l, out[1], out[2]);
    }
    // the wind: on every stage (the mini map's wind dial; a swipe there sets it, songs.cpp set_wind)
    float wx = ldf32(kEnvLight + kWindVec), wz = ldf32(kEnvLight + kWindVec + 8), wp = ldf32(kEnvLight + kWindPower);
    if (std::isfinite(wx) && std::isfinite(wz) && std::isfinite(wp) && (wx != 0 || wz != 0)) {
        out[70] = std::atan2(wx, wz);  // (0: the wind blows south, +x east)
        out[71] = std::fmin(std::fmax(wp, 0.f), 1.f);
    }
}
std::string minimap_stage() {
    char stage[9] = {};
    memcpy(stage, mem::ptr(kPlay + kStartStage), 8);
    for (char c : stage)
        if (c && (c < 0x20 || c > 0x7E)) return "";
    return stage;
}
}  // namespace mods
