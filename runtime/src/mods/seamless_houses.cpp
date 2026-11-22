// SPDX-License-Identifier: MPL-2.0
// Seamless houses (branch seamless-houses), step 1 measurement: the play scene's create phases (dScnPly_Create
// 025B31E4, l_method table 101EAC64, eleven phases on HD) traced with their result: which phase waits, and how long.
// Only with WWHD_MODS_TRACE.
//
// Step 2 proof of concept (WWHD_SEAMLESS_ROOM=1): Link's house as room 50 of the sea stage. The room archive
// Common/Stage/sea_Room50.szs is made from LinkRM_Room0.szs by tools/seamless/build_room.py (its actors, paths, sounds
// and collision moved by kRoomDX/kRoomDZ, its bfres named Room50.bfres). Here: room 50 joins every room list that
// holds Outset (room 44: dStage_roomControl_c::loadRoom 025C34AC), and its model is placed by the same offset
// (dComIfGp_getMapTrans 02520630, the sea's MULT table, which has no entry for it).
#include <atomic>
#include <chrono>
#include <thread>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "mods.h"
#include "../release.h"
#include "../runtime.h"
#ifdef __ANDROID__
#include "../android/seamless_build.h"
#endif

// (written out: CMakeLists.txt finds the f_X / f_X_orig names in the sources as text)
extern "C" {
void f_025C34AC_orig(Cpu* c);  // dStage_roomControl_c::loadRoom(r4 count, r5 room bytes: room in bits 0-5)
void f_02520630_orig(Cpu* c);  // dComIfGp_getMapTrans(r3 room, r4 &x, r5 &z, r6 &angle)
void f_025217F8_orig(Cpu* c);  // dComIfGs_setGameStartStage (the save's return place; calls checkSeaLandingEvent 0252174C)
void f_025B0FB4_orig(Cpu* c);  // dScnPly phase 0
void f_025B13EC_orig(Cpu* c);  // dScnPly phase 1
void f_025B1418_orig(Cpu* c);  // dScnPly phase 2
void f_025B1590_orig(Cpu* c);  // dScnPly phase 3
void f_025B168C_orig(Cpu* c);  // dScnPly phase 4
void f_025B1F54_orig(Cpu* c);  // dScnPly phase 5
void f_025B1FF8_orig(Cpu* c);  // dScnPly phase 6
void f_025B2F90_orig(Cpu* c);  // dScnPly phase 7
void f_025B375C_orig(Cpu* c);  // dScnPly phase 8
void f_025B3114_orig(Cpu* c);  // dScnPly phase 9
void f_025B3764_orig(Cpu* c);  // dScnPly phase 10
}

namespace {
// Game > Walk-through doors > Houses without loading (pref mod_seamless_houses; WWHD_SEAMLESS_ROOM=1 for tests); only
// with walk-through doors (without them the game's own door scene runs, and its scene change is left to the game)
std::atomic<bool> g_on{[] { const char* v = getenv("WWHD_SEAMLESS_ROOM"); return v && atoi(v) != 0; }()};
const release::Data kPlay{0x1046F0B0};
bool on_sea() {
    if (!g_on.load(std::memory_order_relaxed) || !mods::walk_doors()) return false;
    const char* p = (const char*)mem::ptr(kPlay + 0x5134);  // the start stage's name (USA/EUR)
    return strncmp(p, "sea", 8) == 0;
}
constexpr uint32_t kOldPos = 0x300, kPos = 0x314, kAngle = 0x320, kShapeAngle = 0x328;  // fopAc_ac_c (WWHD)
const release::Data kStayNo{0x1047E6C8}, kRoomStatus{0x1047E8E8};  // dStage_roomControl_c: mStayNo, mStatus[0] flags
constexpr uint32_t kRoomStride = 0x22C;
uint8_t room_flags(int room) { return ld8(kRoomStatus + (uint32_t)room * kRoomStride); }
bool room_ready(int room) { return (room_flags(room) & 7) == 1; }  // loaded, not being made or deleted
constexpr uint8_t kHidden = 8;
uint64_t g_bg_made[64] = {};  // the step a room's daBg was made in (bg_trans), 0: not since it was last hidden

// ---- the islands' houses: Common/Stage/seamless_<host>.txt, made with the room archives by
// tools/seamless/build_island.py (see there for the lines)
struct Spawn {
    float x, y, z;
    int16_t angle;
};
struct House {
    std::string stage;
    int room = 0, sea_room = 0, save = 11;
    float dx = 0, dy = 0, dz = 0;
    std::map<int, Spawn> in, out;
    std::string file;  // its room archive, handed to the game as sea_Room50.szs while it is the one loading
};
struct Island {
    int host = -1;
    std::vector<House> houses;
};
// <game>/seamless/: made on the device by android/seamless_build.cpp (the islands in kHosts), in the background the
// first time the sea stage runs without them; read once complete (each island's table is written last)
std::string data_dir() { return config::game_dir + "/seamless/"; }
constexpr int kHosts[] = {44};  // Outset (more islands: their room numbers here)
std::atomic<int> g_build{0};    // 0 not started, 1 building, 2 done
void build_start() {
    // (only from 0: an exchange(1) put a finished 2 back to 1 on the next step, and the houses never counted as
    // ready again; user: walked in with the normal load)
    int expect = 0;
    if (!g_build.compare_exchange_strong(expect, 1)) return;
    std::thread([] {
        for (int host : kHosts) {
            FILE* f = fopen((data_dir() + "seamless_" + std::to_string(host) + ".txt").c_str(), "r");
            if (f) {
                fclose(f);
                continue;
            }
#ifdef __ANDROID__
            const auto t0 = std::chrono::steady_clock::now();
            const std::string err = seamless_build::build_island(config::game_dir, config::game_dir + "/seamless", host);
            LOG("[seamless] island room %d: %s (%.1f s)", host, err.empty() ? "built" : err.c_str(),
                std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
#endif
        }
        g_build = 2;
    }).detach();
}
const std::vector<Island>& islands() {
    static std::vector<Island> none;
    if (g_build.load() != 2) return none;
    static std::vector<Island> all = [] {
        std::vector<Island> v;
        const std::string dir = data_dir();
        for (int host = 0; host < 50; host++) {
            FILE* f = fopen((dir + "seamless_" + std::to_string(host) + ".txt").c_str(), "r");
            if (!f) continue;
            Island is;
            char line[256];
            while (fgets(line, sizeof line, f)) {
                char st[16];
                House h;
                Spawn sp;
                int pt, a;
                if (sscanf(line, "host %d", &is.host) == 1) continue;
                if (sscanf(line, "house %15s %d %d save %d dx %f dy %f dz %f", st, &h.room, &h.sea_room, &h.save, &h.dx, &h.dy, &h.dz) == 7) {
                    h.stage = st;
                    is.houses.push_back(h);
                } else if (!is.houses.empty() && sscanf(line, "in %d %f %f %f %d", &pt, &sp.x, &sp.y, &sp.z, &a) == 5) {
                    sp.angle = (int16_t)a;
                    is.houses.back().in[pt] = sp;
                } else if (!is.houses.empty() && sscanf(line, "file %15s", st) == 1 && strncmp(line, "file ", 5) == 0) {
                    char f[96];
                    if (sscanf(line, "file %95s", f) == 1) is.houses.back().file = f;
                } else if (!is.houses.empty() && sscanf(line, "out %d %f %f %f %d", &pt, &sp.x, &sp.y, &sp.z, &a) == 5) {
                    sp.angle = (int16_t)a;
                    is.houses.back().out[pt] = sp;
                }
            }
            fclose(f);
            mods::trace("seamless: island room %d: %zu houses", is.host, is.houses.size());
            if (is.host >= 0) v.push_back(std::move(is));
        }
        return v;
    }();
    return all;
}
// all houses are sea room 50, one at a time: g_slot is the house whose archive the room holds (or is loading).
// (Sea room numbers past 51 were not drawn, the sea's own per-room tables stop at 51; user: Sue-Belle's house, room 54,
// blue, the same archive as room 51 drawn)
constexpr int kSlotRoom = 50;
std::atomic<const House*> g_slot{nullptr};
const House* house_by_sea_room(int r) { return r == kSlotRoom ? g_slot.load() : nullptr; }
const Island* island_of(const House* h) {
    for (const Island& is : islands())
        for (const House& x : is.houses)
            if (&x == h) return &is;
    return nullptr;
}
const House* inside() { return on_sea() ? house_by_sea_room((int8_t)ld8(kStayNo)) : nullptr; }

// a house's rooms load while Link is within kNear of one of its doors (its "out" spawns; kKeep once loaded) or inside
constexpr float kNear = 6000.0f, kKeep = 9000.0f;
// Only the nearest such house (one at a time): with six loaded at once, Orca's walls were not drawn (his room's
// models were made and placed; the rooms' shader archives all have the same names: model.sharcfb, ...)
float door_dist(const House& h) {
    const uint32_t link = mods::link_actor();
    if (!link) return 1e30f;
    const float lx = u32_as_f32(ld32(link + kPos)), lz = u32_as_f32(ld32(link + kPos + 8));
    float best = 1e30f;
    for (const auto& [p, s] : h.out) best = std::fmin(best, std::hypot(s.x - lx, s.z - lz));
    return best;
}
bool wanted(const House& h) {
    if (inside() == &h) return true;
    if (inside()) return false;
    // the slot holds another house (or is still letting it go): wait
    if ((room_flags(kSlotRoom) & 7) && g_slot.load() != &h) return false;
    const float d = door_dist(h);
    if (d >= ((room_flags(h.sea_room) & 1) ? kKeep : kNear)) return false;
    for (const Island& is : islands())
        for (const House& o : is.houses)
            if (&o != &h && door_dist(o) < d) return false;
    return true;
}

void load_room(Cpu* c) {
    const uint32_t n = c->r[4], rooms = c->r[5];
    const Island* is = nullptr;
    if (on_sea())
        for (uint32_t i = 0; i < n && i < 60 && !is; i++)
            for (const Island& x : islands())
                if ((ld8(rooms + i) & 0x3F) == x.host) is = &x;
    if (is) {
        static uint32_t buf = mem::runtime_alloc(64);
        uint32_t m = 0;
        for (uint32_t i = 0; i < n && m < 50; i++) st8(buf + m++, ld8(rooms + i));
        for (const House& h : is->houses)
            // without bit 7 (dStage_roomRead_dt_c_ChkBg): loadRoom makes the room hidden (flags 0x0A): its archive is
            // read, but neither its daBg (model, collision) nor its actors are made (d_s_room objectSetCheck) until
            // the hidden flag is cleared, when Link goes in (show_rooms below)
            if (wanted(h) && m < 64) {
                if (g_slot.load() != &h) {
                    g_slot = &h;
                    mods::trace("seamless: slot room %d is now %s %d (%s)", kSlotRoom, h.stage.c_str(), h.room, h.file.c_str());
                }
                st8(buf + m++, kSlotRoom);
                break;  // one
            }
        if (m != n) c->r[4] = m, c->r[5] = buf;
    }
    f_025C34AC_orig(c);
}
// where a house room's model goes (daBg: transS(x, y, z) then YrotM(angle)): its build offset (y 150000: daBg_c::draw
// clips room models beyond 100000 from the camera, so from around the island the houses are never drawn)
void map_trans(Cpu* c) {
    const House* h = on_sea() ? house_by_sea_room((int)c->r[3]) : nullptr;
    if (h) {
        st32(c->r[4], f32_as_u32(h->dx));
        st32(c->r[5], f32_as_u32(h->dz));
        st16(c->r[6], 0);
        c->r[3] = 1;
        return;
    }
    f_02520630_orig(c);
}
void bg_trans(Cpu* c) {
    const House* h = on_sea() ? house_by_sea_room((int)c->r[29]) : nullptr;
    if (h) {
        c->f[2].ps0 = h->dy, g_bg_made[h->sea_room] = mods::step() ? mods::step() : 1;
        // (called once per model of the room: r28 the model, r26 the loop's countdown from 4)
        mods::trace("seamless: sea room %d model %08X placed (loop %u)", h->sea_room, c->r[28], c->r[26]);
    }
}

// a door between an island and a house: the scene change becomes a move; Link is put kStepIn in front of the spawn the
// way it faces, both positions (current and old, for the wall checks) set
constexpr float kStepIn = 120.0f, kStepOut = 20.0f;
constexpr uint64_t kWatchSteps = 120;
uint64_t g_watch_until = 0;  // diagnostics (watch below) until this step
constexpr uint32_t kNextStage = 0x5140;  // dStage_nextStage_c in the play info: name[8], s16 point, s8 room, layer, enable

// the music and sound banks follow the move as in a scene change: mDoAud_setSceneName (025E17CC: the audio's spot,
// room and layer; the load timer set to 36, cut to 3 here as turbo.cpp does for walk-through doors), then
// mDoAud_load1stDynamicWave (025E1828) once the timer is down to 1, then, when no wave is loading any more
// (025E18D0), sceneBgmStart (025E18E0) and load2ndDynamicWave (025E18C4) as dScnPly_Execute does after a fade-in
const release::Code kSetSceneName{0x025E17CC}, kLoad1st{0x025E1828}, kWaveLoading{0x025E18D0}, kBgmStart{0x025E18E0},
    kLoad2nd{0x025E18C4};
const release::Data kAudioLoadTimer{0x101F4707};
int g_audio = 0;  // 0 idle, 1 waiting for the load timer, 2 waiting for the first waves
void audio_scene(Cpu* c, const char* stage, int room, int layer) {
    static uint32_t name = mem::runtime_alloc(16);
    mem::write_cstr(name, stage, 16);
    st8(kAudioLoadTimer, 0);  // a pending change of ours: start over
    guest_call(c, kSetSceneName, {name, (uint32_t)room, (uint32_t)layer});
    if (ld8(kAudioLoadTimer) > 3) st8(kAudioLoadTimer, 3);
    g_audio = 1;
    mods::trace("seamless: audio scene %s room %d layer %d", stage, room, layer);
}
void audio_step(Cpu* c) {
    if (g_audio == 1 && ld8(kAudioLoadTimer) <= 1) {
        guest_call(c, kLoad1st);
        g_audio = 2;
        mods::trace("seamless: audio first waves");
    } else if (g_audio == 2 && guest_call(c, kWaveLoading) == 0) {
        guest_call(c, kBgmStart);
        guest_call(c, kLoad2nd);
        g_audio = 0;
        mods::trace("seamless: audio scene music started");
    }
}

const release::Data kOverlapReq{0x101F36CC};  // l_fopOvlpM_overlap[0]: a fade / wipe under way (turbo.cpp)
const release::Data kKeepPos{0x1046CD48}, kKeepShapeAngle{0x1046CD10}, kKeepAngle{0x1046CD08};
Spawn g_put{};  // where the last move put Link (held for kHoldSteps: see hold_put)
uint64_t g_moved_at = 0;  // a second move within kMoveGap steps is refused (the trace: a move that did not hold
constexpr uint64_t kMoveGap = 30;  // repeated every other step, setting the music each time: the game slowed down)

// the player camera goes along: its points (dCamera_c, camera process +0x248: mCenter 0x10, mEye 0x1C, the view
// cache's 0x44 / 0x50) moved and turned around Link by his move, its direction angles (mDirection yaw 0x0E, the view
// cache's 0x42, mAngleY 0x6C) turned by the same; the camera process is game info 1046F0B0 + 0x5AF8
void camera_along(float ox, float oy, float oz, int16_t oa, float nx, float ny, float nz, int16_t na) {
    const uint32_t proc = ld32(kPlay + 0x5AF8);
    if (proc < 0x10000000 || proc >= 0x50000000) return;
    const uint32_t cam = proc + 0x248;
    const int16_t d = (int16_t)(na - oa);
    const float r = d * 3.14159265f / 32768.0f, cs = std::cos(r), sn = std::sin(r);
    // every point the camera keeps near Link (its own centre and eye, the view cache's, the follow work's targets,
    // ...; only 0x10 / 0x1C / 0x44 / 0x50 turned, the camera came back unturned): any three floats within 3000 of him
    std::string found;
    for (uint32_t at = 0; at + 12 <= 0x1000;) {
        const float x = u32_as_f32(ld32(cam + at)) - ox, y = u32_as_f32(ld32(cam + at + 4)) - oy,
                    z = u32_as_f32(ld32(cam + at + 8)) - oz;
        if (std::fabs(x) < 3000.0f && std::fabs(y) < 3000.0f && std::fabs(z) < 3000.0f &&
            (std::fabs(x) + std::fabs(y) + std::fabs(z)) > 0.0f && std::isfinite(x + y + z) &&
            std::fabs(u32_as_f32(ld32(cam + at))) > 1000.0f) {  // a world position, not a small offset
            st32(cam + at, f32_as_u32(nx + x * cs + z * sn));
            st32(cam + at + 4, f32_as_u32(ny + y));
            st32(cam + at + 8, f32_as_u32(nz - x * sn + z * cs));
            char b[16];
            snprintf(b, sizeof b, " %X", at);
            found += b;
            at += 12;
        } else {
            at += 4;
        }
    }
    mods::trace("seamless: camera points moved:%s", found.c_str());
    // the yaw is the globe's second angle (trace: 0x0E = mAngleY + 0x8000; 0x0C is the pitch). Turned alone it did
    // not hold: the follow camera keeps its own centre and eye (mWork.follow) and eased back to them, the camera
    // sideways by about the move's turn (user). m108 = 0 (dCamera_c 0x108: steps in the mode) makes followCamera
    // start over from the view cache, as on a mode change
    const int16_t u0 = (int16_t)ld16(cam + 0x0C), v0 = (int16_t)ld16(cam + 0x0E), y0 = (int16_t)ld16(cam + 0x6C);
    // the stick keeps the old camera angle while it is held the same way (dCamera_c::Run: mAngleY =
    // getDMCAngle, mDMCSystem GameCube 0x220, its angle at +2): turned along if it holds the camera's angle
    const int16_t dmc = (int16_t)ld16(cam + 0x222);
    mods::trace("seamless: camera DMC %02X %02X %d %d", ld8(cam + 0x220), ld8(cam + 0x221), dmc, (int16_t)ld16(cam + 0x224));
    if (std::abs((int16_t)(dmc - y0)) < 0x1000) st16(cam + 0x222, (uint16_t)(dmc + d));
    // the camera's yaw and facing angle wherever the camera process keeps them (found by a search for their values,
    // trace 2026-11-16: the view's angle p+0x236, the globe yaws 0x0E / 0x42, mAngleY 0x6C, the follow work's
    // 0x3B2; turning only the first three, the next step brought the old yaw back). A search turning every match
    // also hit halves of pointers (p+0xB52, p+0xC66) and crashed the game: a fixed list
    std::string angles;
    for (uint32_t at : {0x236u, 0x248u + 0x0Eu, 0x248u + 0x42u, 0x248u + 0x6Cu, 0x248u + 0x3B2u}) {
        const int16_t v = (int16_t)ld16(proc + at);
        if (std::abs((int16_t)(v - v0)) > 0x800 && std::abs((int16_t)(v - y0)) > 0x800) continue;  // not an angle of it now
        st16(proc + at, (uint16_t)(v + d));
        char b[24];
        snprintf(b, sizeof b, " p+%X(%d)", at, v);
        angles += b;
    }
    mods::trace("seamless: camera angles turned:%s", angles.c_str());
    st32(cam + 0x108, 0);
    mods::trace("seamless: camera right after: yaw %d/%d angleY %d centre %.0f %.0f eye %.0f %.0f", (int16_t)ld16(cam + 0x0E),
                (int16_t)ld16(cam + 0x42), (int16_t)ld16(cam + 0x6C), u32_as_f32(ld32(cam + 0x10)), u32_as_f32(ld32(cam + 0x18)),
                u32_as_f32(ld32(cam + 0x1C)), u32_as_f32(ld32(cam + 0x24)));
    mods::trace("seamless: camera along, turned %d (globe %d/%d, angle Y %d before; Link %d -> %d)", d, u0, v0, y0, oa, na);
}

// the exit polygon started a compulsory event (HUD hidden) and Link's walk-on demo (DEMO_N_WALK): the event is reset
// (dEvt_control_c reset flag 8 at play + 0x52B8, as walk_doors.cpp's arrival); Link's own step then ends his demo once
// no event runs. Going in this is done before the house's actors are made (their own events must not be reset)
void end_door_event() { st16(kPlay + 0x52B8, ld16(kPlay + 0x52B8) | 8); }

void put_link(const Spawn& s, float step_in = kStepIn) {
    const uint32_t link = mods::link_actor();
    if (!link) return;
    const float a = s.angle * 3.14159265f / 32768.0f;
    const float x = s.x + std::sin(a) * step_in, z = s.z + std::cos(a) * step_in;
    camera_along(u32_as_f32(ld32(link + kPos)), u32_as_f32(ld32(link + kPos + 4)), u32_as_f32(ld32(link + kPos + 8)),
                 (int16_t)ld16(link + kShapeAngle + 2), x, s.y, z, s.angle);
    for (uint32_t base : {kOldPos, kPos}) {
        st32(link + base, f32_as_u32(x)), st32(link + base + 4, f32_as_u32(s.y)), st32(link + base + 8, f32_as_u32(z));
    }
    st16(link + kAngle + 2, (uint16_t)s.angle), st16(link + kShapeAngle + 2, (uint16_t)s.angle);
    // daPy_lk_c::execute puts Link back at these each step (warp.cpp: l_debug_keep_pos and its angles)
    st32(kKeepPos, f32_as_u32(x)), st32(kKeepPos + 4, f32_as_u32(s.y)), st32(kKeepPos + 8, f32_as_u32(z));
    st16(kKeepShapeAngle + 2, (uint16_t)s.angle), st16(kKeepAngle + 2, (uint16_t)s.angle);
    st32(link + 0x6A48, f32_as_u32(s.y));  // where his fall started (warp.cpp: no fall damage)
    // the exit polygon's scene change set daPyFlg1_UNK100000 (mNoResetFlg1, GameCube 0x2A0 + 0x11C); the stage change
    // would have reset it. Left on, a roll into a wall was no crash (procFrontRoll: !checkNoResetFlg1(UNK100000);
    // user: Orca's lesson, rolling into the wall did nothing)
    st32(link + 0x3BC, ld32(link + 0x3BC) & ~0x100000u);
    g_moved_at = mods::step();
    g_put = {x, s.y, z, s.angle};
    mods::walk_doors_arrived();
    g_watch_until = mods::step() + kWatchSteps;
    mods::trace("seamless: Link put at %.0f %.0f %.0f, angle %d", x, s.y, z, s.angle);
}

// a door's procedure Link was still in set his height from the door for one more step (out of Sue-Belle's house he
// kept the house's height and fell 150000 to the sea; user): for kHoldSteps after a move he is put back if he is off
constexpr uint64_t kHoldSteps = 10;
void hold_put() {
    const uint32_t link = mods::link_actor();
    if (!link || !g_moved_at || mods::step() - g_moved_at > kHoldSteps) return;
    const float x = u32_as_f32(ld32(link + kPos)), y = u32_as_f32(ld32(link + kPos + 4)), z = u32_as_f32(ld32(link + kPos + 8));
    // (the height only: a house's event may put Link elsewhere in the room right away, Orca's does)
    if (std::fabs(y - g_put.y) < 2000.0f) return;
    for (uint32_t base : {kOldPos, kPos}) {
        st32(link + base, f32_as_u32(g_put.x)), st32(link + base + 4, f32_as_u32(g_put.y)), st32(link + base + 8, f32_as_u32(g_put.z));
    }
    st32(kKeepPos, f32_as_u32(g_put.x)), st32(kKeepPos + 4, f32_as_u32(g_put.y)), st32(kKeepPos + 8, f32_as_u32(g_put.z));
    st32(link + 0x6A48, f32_as_u32(g_put.y));
    mods::trace("seamless: Link was at %.0f %.0f %.0f after the move: put back", x, y, z);
}

void phase(Cpu* c, int n, void (*orig)(Cpu*)) {
    if (!mods::trace_on()) return orig(c);
    static int last_n = -1;
    static uint32_t last_r = 0xFFFFFFFF, calls = 0;
    orig(c);
    const uint32_t r = c->r[3];
    if (n == last_n && r == last_r) { calls++; return; }
    if (calls) mods::trace("scene phase %d -> %u (x%u more)", last_n, last_r, calls);
    mods::trace("scene phase %d -> %u", n, r);
    last_n = n, last_r = r, calls = 0;
}
}  // namespace

// a new scene starts: what seamless_step knew of the last one's rooms is dropped there (new_scene below)
std::atomic<bool> g_new_scene{false};
extern "C" void hook_025B0FB4(Cpu* c) {
    g_new_scene = true;
    phase(c, 0, f_025B0FB4_orig);
}
extern "C" void hook_025B13EC(Cpu* c) { phase(c, 1, f_025B13EC_orig); }
extern "C" void hook_025B1418(Cpu* c) { phase(c, 2, f_025B1418_orig); }
extern "C" void hook_025B1590(Cpu* c) { phase(c, 3, f_025B1590_orig); }
extern "C" void hook_025B168C(Cpu* c) { phase(c, 4, f_025B168C_orig); }
extern "C" void hook_025B1F54(Cpu* c) { phase(c, 5, f_025B1F54_orig); }
extern "C" void hook_025B1FF8(Cpu* c) { phase(c, 6, f_025B1FF8_orig); }
extern "C" void hook_025B2F90(Cpu* c) { phase(c, 7, f_025B2F90_orig); }
extern "C" void hook_025B375C(Cpu* c) { phase(c, 8, f_025B375C_orig); }
extern "C" void hook_025B3114(Cpu* c) { phase(c, 9, f_025B3114_orig); }
extern "C" void hook_025B3764(Cpu* c) { phase(c, 10, f_025B3764_orig); }
extern "C" void hook_025C34AC(Cpu* c) { load_room(c); }
extern "C" void hook_02520630(Cpu* c) { map_trans(c); }
extern "C" void site_0207A4F4(Cpu* c) { bg_trans(c); }

namespace {

// diagnostics: Link and the rooms each step for kWatchSteps after a move, and on every change of his procedure
void watch() {
    const uint32_t link = mods::link_actor();
    if (!link) return;
    static uint32_t last_proc = 0;
    const uint32_t proc = ld32(link + 0x65F0);
    if (mods::step() > g_watch_until && proc == last_proc) return;
    last_proc = proc;
    const int stay = (int8_t)ld8(kStayNo);
    if (const uint32_t cp = ld32(kPlay + 0x5AF8); cp >= 0x10000000 && cp < 0x50000000) {
        auto f = [&](uint32_t o) { return u32_as_f32(ld32(cp + 0x248 + o)); };
        mods::trace("seamless watch: camera centre %.0f %.0f %.0f eye %.0f %.0f %.0f", f(0x10), f(0x14), f(0x18), f(0x1C), f(0x20), f(0x24));
    }
    mods::trace("seamless watch: demo %u/%u event %04X proc %u pos %.0f %.0f %.0f acch flags %08X stay %d (flags %02X)",
                ld16(link + 0x420), ld32(link + 0x430), ld16(kPlay + 0x52B8), proc, u32_as_f32(ld32(link + kPos)),
                u32_as_f32(ld32(link + kPos + 4)), u32_as_f32(ld32(link + kPos + 8)), ld32(link + 0x834), stay,
                stay >= 0 && stay < 64 ? room_flags(stay) : 0);
}

// the {count, entries} pair of a stage table: a word in the stage data pointing at it, the fourcc in front
uint32_t stage_table(const char* fourcc) {
    const uint32_t stage = kPlay + 0x5150;
    uint32_t want;
    memcpy(&want, fourcc, 4);
    want = __builtin_bswap32(want);
    for (uint32_t o = 4; o < 0x100; o += 4) {
        const uint32_t p = ld32(stage + o);
        if (p >= 0x10000004 && p < 0x50000000 && ld32(p - 4) == want) return p;
    }
    return 0;
}
std::vector<uint8_t> read_file(const std::string& name) {
    std::vector<uint8_t> v;
    FILE* f = fopen((data_dir() + name).c_str(), "rb");
    if (!f) return v;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0 && n < (8 << 20)) {
        v.resize((size_t)n);
        if (fread(v.data(), 1, v.size(), f) != v.size()) v.clear();
    }
    fclose(f);
    return v;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

std::map<std::string, std::pair<uint32_t, std::vector<uint8_t>>> g_env_stages;  // stage -> its first Colo, its EnvR
uint32_t g_env_envr = 0;  // our EnvR entries
const void* g_env_slot = nullptr;  // the house EnvR 50 was last written for
// EnvR 50: the slot house's stage's EnvR of its room
void env_slot() {
    const House* h = g_slot.load();
    if (!h || !g_env_envr || g_env_slot == h || ld32(stage_table("EnvR") + 4) != g_env_envr) return;
    auto it = g_env_stages.find(h->stage);
    g_env_slot = h;
    if (it == g_env_stages.end() || it->second.second.size() < 8) return;
    const std::vector<uint8_t>& e = it->second.second;
    const size_t k = (size_t)h->room * 8 + 8 <= e.size() ? (size_t)h->room * 8 : 0;
    for (uint32_t j = 0; j < 8; j++) st8(g_env_envr + kSlotRoom * 8 + j, (uint8_t)(e[k + j] + it->second.first));
}

// indoor lighting: kankyo lights a room by the stage's EnvR entry of its number (dKy_tevstr_init: env index = room),
// which picks Colo entries (per weather) that pick Pale palettes (per time of day) that pick Virt sky colours. The sea
// has none for an interior. build_env.py saves each house stage's tables (<stage>.env: 'SHEV', then EnvR, Colo, Pale,
// Virt as count, entry size, entries); the sea's Colo / Pale / Virt get them appended (their indices moved) and EnvR
// <sea room> becomes the stage's EnvR <room>. The stage keeps each table as a {count, entries} pair inside its chunk
// table, g_env_light (10475A68) a copy of the entries pointers: both are pointed at the new tables
const release::Data kEnvLight{0x10475A68};
void env_patch() {
    static uint32_t done_for = 0;  // the EnvR entries we made (patched while the stage still points at them)
    const char* names[4] = {"EnvR", "Colo", "Pale", "Virt"};
    const uint32_t kSize[4] = {8, 12, 108, 56};
    uint32_t tab[4];
    for (int i = 0; i < 4; i++)
        if (!(tab[i] = stage_table(names[i]))) return;
    if (ld32(tab[0] + 4) == done_for) return;
    uint32_t n[4], old[4];
    for (int i = 0; i < 4; i++) n[i] = ld32(tab[i]), old[i] = ld32(tab[i] + 4);
    if (n[0] < 52) return;  // the sea's EnvR has entries up to room 51; ours go past it (made below)
    static uint32_t buf[4] = {};
    constexpr uint32_t kCap[4] = {64 * 8, 0x400, 0x8000, 0x4000};
    for (int i = 0; i < 4; i++) {
        if (!buf[i]) buf[i] = mem::runtime_alloc(kCap[i]);
        memcpy(mem::ptr(buf[i]), mem::ptr(old[i]), (size_t)n[i] * kSize[i]);
    }
    uint32_t m[4] = {64, n[1], n[2], n[3]};  // EnvR: all 64 rooms (the sea's own kept)
    for (uint32_t r = n[0]; r < 64; r++) memcpy(mem::ptr(buf[0] + r * 8), mem::ptr(old[0]), 8);  // unused: as room 0
    // each house stage's tables once (appended), then each house's EnvR entry
    std::map<std::string, std::pair<uint32_t, std::vector<uint8_t>>> stages;  // stage -> its first Colo here, its EnvR
    for (const Island& is : islands())
        for (const House& h : is.houses) {
            if (stages.count(h.stage)) continue;
            const std::vector<uint8_t> f = read_file(h.stage + ".env");
            if (f.size() < 8 || memcmp(f.data(), "SHEV", 4) != 0) continue;
            size_t o = 8;
            uint32_t cnt[4], sz[4], at[4];
            bool ok = true;
            for (int i = 0; i < 4 && ok; i++) {
                ok = o + 8 <= f.size();
                if (!ok) break;
                cnt[i] = be32(&f[o]), sz[i] = be32(&f[o + 4]), at[i] = (uint32_t)o + 8;
                o += 8 + (size_t)cnt[i] * sz[i];
                ok = sz[i] == kSize[i] && o <= f.size();
            }
            bool fits = ok;
            for (int i = 1; i < 4 && fits; i++) fits = (m[i] + cnt[i]) * kSize[i] <= kCap[i];
            if (!fits) continue;
            const uint32_t colo0 = m[1], pale0 = m[2], virt0 = m[3];
            for (int i = 1; i < 4; i++) {
                memcpy(mem::ptr(buf[i] + m[i] * kSize[i]), &f[at[i]], (size_t)cnt[i] * kSize[i]);
                for (uint32_t k = 0; k < cnt[i]; k++) {
                    const uint32_t e = buf[i] + (m[i] + k) * kSize[i];
                    if (i == 1)  // Colo -> Pale: 8 bytes
                        for (uint32_t j = 0; j < 8; j++) st8(e + j, (uint8_t)(ld8(e + j) + pale0));
                    if (i == 2) st8(e + 0x21, (uint8_t)(ld8(e + 0x21) + virt0));  // Pale -> Virt
                }
                m[i] += cnt[i];
            }
            stages[h.stage] = {colo0, std::vector<uint8_t>(f.begin() + at[0], f.begin() + at[0] + (size_t)cnt[0] * 8)};
        }
    g_env_stages = stages, g_env_envr = buf[0], g_env_slot = nullptr;
    for (int i = 0; i < 4; i++) {
        st32(tab[i], m[i]);
        st32(tab[i] + 4, buf[i]);
        for (uint32_t o = 0; o < 0x40; o += 4)  // g_env_light's copies
            if (ld32(kEnvLight + o) == old[i]) st32(kEnvLight + o, buf[i]);
    }
    done_for = buf[0];
    mods::trace("seamless: indoor lighting: %zu stages (tables %u/%u/%u/%u)", stages.size(), m[0], m[1], m[2], m[3]);
}

// saved progress: a stage keeps its switches, chests and items in the save table its STAG names (dSv_info_c's
// mMemory, copied from / back to mSavedata.mSave[table] at the stage's start / end); Outset (the sea) uses table 0,
// its houses table 11 (STAGE_MISC). Moving in and out does what the stage change would: the current memory back to
// the table it came from, the other table's in, and the sea's STAG made to name the house's table while Link is in
// the house (a save made there writes the right table). dSv_info_c 101F84DC: mSave[k] at +0x3A0 + 0x24 k, mMemory at
// +0x798 (GameCube offsets + 0x20; checked on the first step: the memory equals table 0's after the stage started);
// 101F84DC holds the dSv_info_c's address
const release::Data kSaveInfoPtr{0x101F84DC};  // holds the dSv_info_c's address (cheats.cpp, warp.cpp)
uint32_t save_info() { return ld32(kSaveInfoPtr); }
constexpr uint32_t kMemSize = 0x24, kMemCur = 0x798, kSeaTable = 0;
uint32_t mem_table(uint32_t k) { return save_info() + 0x3A0 + kMemSize * k; }
bool g_tables_ok = false;
void tables_check() {
    if (save_info() < 0x10000000 || save_info() >= 0x50000000) return;
    // until the stage's memory was copied in (at the first sea step after loading a save it was still all zeros):
    // checked every step until it equals table 0 once (dComIfGs_onSwitch 02520864 uses +0x798 / +0x3A0 + 0x24 k)
    if (g_tables_ok) return;
    static uint32_t tries = 0;
    if (++tries > 600) return;
    g_tables_ok = memcmp(mem::ptr(save_info() + kMemCur), mem::ptr(mem_table(kSeaTable)), kMemSize) == 0;
    if (!g_tables_ok && tries != 1) return;
    if (mods::trace_on()) {  // where the blocks really are: the current memory's bytes, and every block equal to it
        std::string h, eq;
        for (uint32_t i = 0; i < kMemSize; i += 4) {
            char b[12];
            snprintf(b, sizeof b, " %08X", ld32(save_info() + kMemCur + i));
            h += b;
        }
        for (uint32_t o = 0; o + kMemSize <= 0x1400; o += 4)
            if (o != kMemCur && memcmp(mem::ptr(save_info() + o), mem::ptr(save_info() + kMemCur), kMemSize) == 0) {
                char b[12];
                snprintf(b, sizeof b, " +%X", o);
                eq += b;
            }
        mods::trace("seamless: memory at +%X:%s; equal blocks:%s", kMemCur, h.c_str(), eq.c_str());
        h.clear();
        for (uint32_t i = 0; i < 0x40; i += 4) {
            char b[12];
            snprintf(b, sizeof b, " %08X", ld32(save_info() + 0x3A0 + i));
            h += b;
        }
        mods::trace("seamless: table 0 guess at +3A0:%s", h.c_str());
    }
    mods::trace("seamless: save tables %s (try %u)", g_tables_ok ? "found (current memory = table 0)" : "not confirmed yet", tries);
}
void set_stag_table(uint32_t k) {
    const uint32_t t = stage_table("STAG");
    if (!t) return;
    const uint32_t stag = ld32(t + 4);
    st8(stag + 9, (uint8_t)((ld8(stag + 9) & 1) | (k << 1)));
}
void switch_tables(uint32_t from, uint32_t to) {
    if (!g_tables_ok) return;
    memcpy(mem::ptr(mem_table(from)), mem::ptr(save_info() + kMemCur), kMemSize);
    memcpy(mem::ptr(save_info() + kMemCur), mem::ptr(mem_table(to)), kMemSize);
    set_stag_table(to);
    mods::trace("seamless: saved progress table %u -> %u", from, to);
}

// where a loaded save continues (dSv_player_return_place_c: name[8], room, point; save player data +0x30, so
// dSv_info_c + 0x50): dComIfGs_setGameStartStage, at a save, takes Link's room on a sea stage (here room 50, high in
// the air: the game picked a boat spot far out on the sea, user). Inside the house it is kept at what saving in the
// real Link's house gives (a STAGE_MISC stage: the sea, the island's room, point 0)
// the return place's offset in the HD save (not GameCube + 0x20: that held other data): the first 8-byte stage name
// "sea" (or a house stage) in the player data (+0x20 .. +0x3A0) followed by a room in 0..63
uint32_t return_place() {
    static uint32_t off = 0;
    if (off) return save_info() + off;
    const uint32_t si = save_info();
    if (si < 0x10000000 || si >= 0x50000000) return 0;
    for (uint32_t o = 0x20; o + 12 <= 0x3A0; o++) {
        const char* n = (const char*)mem::ptr(si + o);
        const bool sea = memcmp(n, "sea\0\0\0\0\0", 8) == 0;
        if ((sea || memcmp(n, "LinkRM\0\0", 8) == 0) && ld8(si + o + 8) < 64) {
            off = o;
            mods::trace("seamless: return place found at +%X: %.8s room %d point %u", o, n, (int8_t)ld8(si + o + 8), ld8(si + o + 9));
            return si + off;
        }
    }
    return 0;
}
void keep_return_place(int host) {
    const uint32_t rp = return_place();
    if (!rp) return;
    static bool told = false;
    if (!told) {
        told = true;
        mods::trace("seamless: return place now %.8s room %d point %u", (const char*)mem::ptr(rp), (int8_t)ld8(rp + 8), ld8(rp + 9));
    }
    if (memcmp(mem::ptr(rp), "sea\0", 4) != 0 && memcmp(mem::ptr(rp), "LinkRM", 6) != 0) return;  // not what it should hold
    if (memcmp(mem::ptr(rp), "sea\0", 4) == 0 && (int8_t)ld8(rp + 8) == host && ld8(rp + 9) == 0) return;
    memset(mem::ptr(rp), 0, 8);
    memcpy(mem::ptr(rp), "sea", 3);
    st8(rp + 8, (uint8_t)host), st8(rp + 9, 0);
    mods::trace("seamless: return place set to sea room %d point 0", host);
}


// events: a stage has one event list (dEvent_manager_c at play + 0x52C4: its mList pointers in the first 8 words, set
// by setData 025443B4 from the stage's event_list.dat). A house's actors look their events up by name when they are
// made (Orca: "Ji1_StartSpeak"), while Link is still outside, so the sea runs with sea_<host>.evt instead: the sea's
// list with the island's houses' appended (tools/seamless/merge_events.py; the sea's entries keep their numbers). Put
// in at the first steps of the sea stage, before the houses' rooms load (they wait for Link near their doors).
// The stage's EVNT table (the events its actors' parameters name by number) is the house stage's while Link is in a
// house (<stage>.evn: count, entries; the sea's kept from the steps outside and put back)
const release::Code kEvtSetData{0x025443B4};
constexpr uint32_t kEvtMgr = 0x52C4;
uint32_t load_guest(const std::string& name, uint32_t* size = nullptr) {
    std::vector<uint8_t> v = read_file(name);
    if (v.empty()) return 0;
    const uint32_t a = mem::runtime_alloc((uint32_t)v.size(), 32);
    memcpy(mem::ptr(a), v.data(), v.size());
    if (size) *size = (uint32_t)v.size();
    return a;
}
void events_list(Cpu* c) {
    static std::map<int, uint32_t> merged;  // island -> its merged list in guest memory (0: none)
    const uint32_t mgr = kPlay + kEvtMgr;
    for (const Island& is : islands()) {
        auto it = merged.find(is.host);
        if (it == merged.end()) {
            it = merged.emplace(is.host, load_guest("sea_" + std::to_string(is.host) + ".evt")).first;
            mods::trace("seamless: merged event list for room %d %s", is.host, it->second ? "read" : "missing");
        }
        if (it->second && ld32(mgr) != it->second) {
            guest_call(c, kEvtSetData, {mgr, it->second});
            mods::trace("seamless: merged event list for room %d in", is.host);
        }
        break;  // (one island for now: the sea has one list)
    }
}
struct Evnt {
    uint32_t at = 0, n = 0;
};
const Evnt& evnt_of(const std::string& stage) {
    static std::map<std::string, Evnt> all;
    auto it = all.find(stage);
    if (it != all.end()) return it->second;
    Evnt e;
    uint32_t size = 0;
    const uint32_t a = load_guest(stage + ".evn", &size);
    if (a && size >= 4) e.n = ld32(a), e.at = a + 4;
    return all[stage] = e;
}
uint32_t g_sea_evnt[2] = {};
bool g_sea_evnt_ok = false;
const House* g_evnt_house = nullptr;
void events_step(Cpu* c) {
    events_list(c);
    const uint32_t evnt = stage_table("EVNT");
    if (!evnt) return;
    const House* h = inside();
    if (!h) {
        if (g_evnt_house) {  // back outside: the sea's put back
            if (g_sea_evnt_ok) st32(evnt, g_sea_evnt[0]), st32(evnt + 4, g_sea_evnt[1]);
            g_evnt_house = nullptr;
        } else {
            g_sea_evnt[0] = ld32(evnt), g_sea_evnt[1] = ld32(evnt + 4);
            g_sea_evnt_ok = true;
        }
        return;
    }
    if (g_evnt_house == h || !g_sea_evnt_ok) return;
    const Evnt& e = evnt_of(h->stage);
    st32(evnt, e.n), st32(evnt + 4, e.n ? e.at : g_sea_evnt[1]);
    g_evnt_house = h;
    mods::trace("seamless: EVNT of %s in (%u)", h->stage.c_str(), e.n);
}

// fixed places in actors' own code (static points in their stage's coordinates), moved while Link is in that house's
// moved copy and put back otherwise (the real stage uses them too). Orca (d_a_npc_ji1 daNpc_Ji1_plRoomOutCheck,
// 02248F70): out_chk_pos (0, 0, 500), the spot by his door where leaving mid-lesson makes him call Link back (user:
// he did not react); its function-static guard 104673AC, the point 10467258
struct FixedPoint {
    const char* stage;
    release::Data guard, point;
    float x, y, z;
    uint16_t done_bit;  // the event bit after which the door no longer matters (walk-through doors open by themselves)
};
const FixedPoint kFixedPoints[] = {{"Ojhous", {0x104673AC}, {0x10467258}, 0.0f, 0.0f, 500.0f, 0x0501}};
void fixed_points(const House* in) {
    for (const FixedPoint& f : kFixedPoints) {
        const bool here = in && in->stage == f.stage;
        const float x = f.x + (here ? in->dx : 0), y = f.y + (here ? in->dy : 0), z = f.z + (here ? in->dz : 0);
        if (u32_as_f32(ld32(f.point)) == x && u32_as_f32(ld32(f.point + 8)) == z && ld32(f.guard)) continue;
        if (!here && !ld32(f.guard)) continue;  // never used yet: the game sets it itself
        st32(f.guard, 1);
        st32(f.point, f32_as_u32(x)), st32(f.point + 4, f32_as_u32(y)), st32(f.point + 8, f32_as_u32(z));
        mods::trace("seamless: %s's fixed point now %.0f %.0f %.0f", f.stage, x, y, z);
    }
}

const House* g_house_was = nullptr;  // the house Link was in at the last step (save tables follow its changes)
const House* g_entered = nullptr;  // the house Link was last moved into
const House* g_going = nullptr;  // going in: its room is being shown, Link waits for its floor
int g_going_point = 0, g_going_layer = 0;
uint64_t g_going_since = 0;

// a house's actors wait while Link is not in it: made when Link comes near its door, Orca started his scene at once
// (Ji1_kaiten_exp) and its event took Link to the house's own coordinates (user: fell into the void, swimming by).
// Every actor whose room is a house room is held (fpcM_Execute, true60.cpp) unless Link is in that house; its room's
// daBg (execute 0207A0E0, from its method table 10190E20) runs (its unload timer)
const release::Code kDaBgExecute{0x0207A0E0};
constexpr uint32_t kActorType = 0xC4, kHomeRoom = 0x2FE, kSubMethod = 0xF0;  // fopAc_ac_c (as true60.cpp, coop_monsters.cpp)
}  // namespace

// actors that put themselves at fixed coordinates of their own stage (Orca's create: current.pos = (0, 0, -850), and
// that spot kept as his anchor; user: the scene waited for him in the open sea). At an actor's first step: if it
// stands far from its house (more than kFar from the house's offset), it is moved by the offset, and so is every copy
// of that position in its first kScan bytes (the anchors its code keeps; their place differs per actor)
constexpr float kFar = 20000.0f;
constexpr uint32_t kScan = 0x4000;
void fix_place(uint32_t proc, const House& h) {
    static std::set<uint32_t> seen;
    const uint32_t id = ld32(proc + 4);
    if (!seen.insert(id).second) return;
    if (seen.size() > 100000) seen.clear();
    const float x = u32_as_f32(ld32(proc + kPos)), y = u32_as_f32(ld32(proc + kPos + 4)), z = u32_as_f32(ld32(proc + kPos + 8));
    if (std::fabs(x - h.dx) < kFar && std::fabs(z - h.dz) < kFar && std::fabs(y - h.dy) < kFar) return;
    const uint32_t bx = ld32(proc + kPos), by = ld32(proc + kPos + 4), bz = ld32(proc + kPos + 8);
    int copies = 0;
    for (uint32_t o = 0; o + 12 <= kScan; o += 4)
        if (ld32(proc + o) == bx && ld32(proc + o + 4) == by && ld32(proc + o + 8) == bz) {
            st32(proc + o, f32_as_u32(x + h.dx)), st32(proc + o + 4, f32_as_u32(y + h.dy)), st32(proc + o + 8, f32_as_u32(z + h.dz));
            copies++;
        }
    mods::trace("seamless: process %u (name %04X) of sea room %d stood at %.0f %.0f %.0f: moved into the house (%d copies)", id,
                ld16(proc + 8), h.sea_room, x, y, z, copies);
}

namespace mods {
// walk-through doors open on A (the game's "Open" prompt shown) in a house whose people watch the door: Orca calls Link
// back from his lesson only while the prompt shows at his door (daNpc_Ji1_plRoomOutCheck: dComIfGp_getDoStatus() ==
// OPEN); opening by itself, the door never showed it (user: Orca did not stop him)
bool seamless_doors_by_a() {
    const House* in = inside();
    if (!in) return false;
    for (const FixedPoint& f : kFixedPoints)
        if (in->stage == f.stage) {
            // only until the lesson is over: Orca gives Link his sword at its end (the event bit Orca checks, 0x0501,
            // did not end it: always A, user); the sword slot of the player data (touch_state.cpp: +0x0E, 0xFF none)
            const uint32_t ps = player_save();
            return !ps || ld8(ps + 0x0E) == 0xFF;
        }
    return false;
}
bool seamless_hold(uint32_t proc) {
    if (!on_sea() || islands().empty()) return false;
    const uint32_t link = link_actor();
    if (!link || proc == link || ld32(proc + kActorType) != ld32(link + kActorType)) return false;
    // by the room the actor was placed in (home.roomNo): a door passed through takes the far room as its current
    // room (Orca's outside door became room 51 and, held while Link was outside, never opened again; user)
    const House* h = house_by_sea_room((int8_t)ld8(proc + kHomeRoom));
    if (!h) return false;
    fix_place(proc, *h);
    if (inside() == h) return false;
    const uint32_t sub = ld32(proc + kSubMethod);
    if (sub >= 0x10000000 && sub < 0x50000000 && ld32(sub + 8) == kDaBgExecute) return false;
    if (trace_on()) {  // each held process once
        static std::set<uint32_t> seen;
        if (seen.size() < 4096 && seen.insert(ld32(proc + 4)).second)
            trace("seamless: holding process %u (name %04X) of sea room %d", ld32(proc + 4), ld16(proc + 8), h->sea_room);
    }
    return true;
}

void seamless_step(Cpu* c) {
    if (trace_on() && mods::step() % 300 == 0) {  // diagnostics: why no house loads (every 10 s)
        const char* st = (const char*)mem::ptr(kPlay + 0x5134);
        float best = 1e30f;
        for (const Island& is : islands())
            for (const House& h : is.houses) best = std::fmin(best, door_dist(h));
        mods::trace("seamless state: on %d doors %d stage '%.8s' build %d islands %zu stay %d slot flags %02X slot %s nearest %.0f",
                    (int)g_on.load(), (int)mods::walk_doors(), st, g_build.load(), islands().size(), (int8_t)ld8(kStayNo),
                    room_flags(kSlotRoom), g_slot.load() ? g_slot.load()->stage.c_str() : "-", best);
    }
    // a scene change the game made itself while Link was in a house (its own door scene, or a story scene such as
    // Grandma's, which loads Link's house as its stage): back on the sea, the house's room is new again. Kept, the
    // last stage's daBg step let Link into it a step after it showed, before its actors were made: the game stopped
    // at an assert in actor creation (user: crash entering Link's house again after Grandma's scene)
    if (g_new_scene.exchange(false)) {
        if (g_going || g_house_was || g_entered) trace("seamless: new scene, house state dropped");
        for (uint64_t& b : g_bg_made) b = 0;
        g_going = nullptr, g_entered = nullptr, g_house_was = nullptr, g_moved_at = 0;
    }
    if (!on_sea()) return;
    build_start();
    if (islands().empty()) return;
    if (trace_on()) watch();
    const uint32_t lr = c->lr, r3 = c->r[3], r4 = c->r[4], r5 = c->r[5], r6 = c->r[6];
    env_patch();
    tables_check();
    if (trace_on()) return_place();  // found once (traced)
    const House* in = inside();
    if (in) keep_return_place(island_of(in)->host);
    events_step(c);
    audio_step(c);
    fixed_points(in);
    hold_put();
    g_house_was = in;
    // a house room is shown (actors made) while Link is in it or going in, else hidden (its actors removed, as
    // leaving a stage would)
    env_slot();
    if (const House* hp = g_slot.load()) {
        {
            const House& h = *hp;
            const uint8_t f = room_flags(h.sea_room);
            if (!(f & 1) || (f & 6)) goto shown;
            // (and right after the move, until the ground under Link makes it the room he stays in)
            const bool show = in == &h || g_going == &h || (g_entered == &h && step() - g_moved_at < 30);
            if (show && (f & kHidden)) {
                st8(kRoomStatus + h.sea_room * kRoomStride, f & ~kHidden);
                trace("seamless: sea room %d shown", h.sea_room);
            } else if (!show && !(f & kHidden)) {
                st8(kRoomStatus + h.sea_room * kRoomStride, f | kHidden);
                g_bg_made[h.sea_room] = 0;
                trace("seamless: sea room %d hidden", h.sea_room);
            }
        }
    }
shown:
    // going in: once the room's daBg exists (its collision registered: two steps later) Link is moved in
    if (g_going) {
        const House* to = g_going;
        if (g_bg_made[to->sea_room] && step() >= g_bg_made[to->sea_room] + 2) {
            // the door walk still on (DEMO_N_WALK 14 in Link's daPy_demo_c mode, 0x430): ended (not an event the
            // house's actors ordered while being made)
            if (const uint32_t link = link_actor(); link && ld32(link + 0x430) == 14) end_door_event();
            // Link's own demo from the exit polygon (daPy_demo_c type 3, setOriginalDemoType, at 0x420; mode at 0x430)
            // is cleared: with a house actor's event already running (Orca's, ordered while he was made) it was not
            // ended by Link's step, and Link did not follow the event's cuts (user: stuck in Orca's first scene)
            if (const uint32_t link = link_actor(); link && ld16(link + 0x420) == 3) {
                st16(link + 0x420, 0), st32(link + 0x430, 0);
                trace("seamless: Link's door demo cleared");
            }
            put_link(to->in.at(g_going_point));
            switch_tables(in ? (uint32_t)in->save : kSeaTable, (uint32_t)to->save);
            audio_scene(c, to->stage.c_str(), to->room, g_going_layer);
            trace("seamless: into sea room %d after %llu steps", to->sea_room, (unsigned long long)(step() - g_going_since));
            g_entered = to;
            g_going = nullptr;
        } else if (step() - g_going_since > 90 || !(room_flags(to->sea_room) & 1)) {
            trace("seamless: sea room %d not ready: the game's own scene change", to->sea_room);
            st8(kPlay + kNextStage + 12, 1);  // the request back (a normal load)
            g_going = nullptr;
        }
    }
    const uint32_t ns = kPlay + kNextStage;
    if (!ld8(ns + 12)) {  // no scene change asked for
        c->lr = lr, c->r[3] = r3, c->r[4] = r4, c->r[5] = r5, c->r[6] = r6;
        return;
    }
    char name[9] = {};
    memcpy(name, mem::ptr(ns), 8);
    const int point = (int16_t)ld16(ns + 8), room = (int8_t)ld8(ns + 10), layer = (int8_t)ld8(ns + 11);
    // a scene change of the game's own door scene (A on a door: its fade already running) is left alone: it loads
    // normally (taken over, the fade stayed white and the door's walk kept Link at the house's height; user)
    // (the door scene's event runs Link as a system demo: daPy_demo_c type 2; a walk-through door's exit polygon
    // gives him type 3)
    const uint32_t lk = link_actor();
    if (ld32(kOverlapReq) != 0 || (lk && ld16(lk + 0x420) == 2)) {
        trace("seamless: scene change to %s left to the game (its own door scene)", name);
        c->lr = lr, c->r[3] = r3, c->r[4] = r4, c->r[5] = r5, c->r[6] = r6;
        return;
    }
    // into a house: its room loaded (hidden) and the spawn in it
    const House* to = nullptr;
    for (const Island& is : islands())
        for (const House& h : is.houses)
            if (h.stage == name && h.room == room && h.in.count(point) && g_slot.load() == &h && room_ready(h.sea_room)) to = &h;
    // out of a house: to its island's room, a spawn its exits lead to
    const bool out = in && strcmp(name, "sea") == 0 && room == island_of(in)->host && in->out.count(point);
    if ((to || out) && ((g_moved_at && step() - g_moved_at < kMoveGap) || g_going)) {
        st8(ns + 12, 0);  // just moved (or going in): the request is dropped, Link stays
        // still on the exit polygon while the room is made: its compulsory event started again (Link walked on by
        // himself after the move, no controls; user): ended again
        if (g_going) end_door_event();
        trace("seamless: scene change to %s %d dropped", name, point);
    } else if (to && to != in) {
        st8(ns + 12, 0);
        end_door_event();
        g_going = to, g_going_point = point, g_going_layer = layer, g_going_since = step();
        trace("seamless: scene change to %s %d room %d kept back: showing sea room %d", name, point, room, to->sea_room);
    } else if (out) {
        st8(ns + 12, 0);
        trace("seamless: scene change to sea %d room %d kept back: out of sea room %d", point, room, in->sea_room);
        end_door_event();
        // outside: at the spawn itself (no exit polygon there to clear; a step further, past Orca's porch, he fell)
        put_link(in->out.at(point), kStepOut);
        switch_tables((uint32_t)in->save, kSeaTable);
        audio_scene(c, "sea", room, layer);
    }
    c->lr = lr, c->r[3] = r3, c->r[4] = r4, c->r[5] = r5, c->r[6] = r6;
}
}  // namespace mods

// a save made in a house: the game's return place (worked out from Link's room and the boat: a boat spot) is set to
// what saving in the real house gives: the sea, the island's room, point 0 (the per-step keep came too late)
extern "C" void hook_025217F8(Cpu* c) {
    f_025217F8_orig(c);
    if (const House* h = inside()) keep_return_place(island_of(h)->host);
}

namespace mods {
// coreinit FS (hle/fs.cpp): the game opens sea_Room50.szs, the slot house's archive is read instead
std::string seamless_file(const std::string& gpath) {
    // (exactly the game's Common folder: it first asks for /Cafe/Common/Stage/..., and served there the room never
    // finished loading: every door then fell back to the normal load; user: leaving with the scene, slower)
    static const std::string kName = "/content/Common/Stage/sea_Room50.szs";
    if (gpath.size() < kName.size() || gpath.compare(gpath.size() - kName.size(), kName.size(), kName) != 0) return {};
    const House* h = g_slot.load();
    if (!h || h->file.empty()) return {};
    return data_dir() + h->file;
}
}  // namespace mods

namespace mods {
bool seamless_houses() { return g_on.load(std::memory_order_relaxed); }
void set_seamless_houses(bool on) {
    g_on = on;
    LOG("[mods] houses without loading %s", on ? "on" : "off");
}
}  // namespace mods
