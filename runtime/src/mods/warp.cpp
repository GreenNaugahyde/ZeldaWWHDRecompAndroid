// Warp (developer menu): a scene change to any stage, room and spawn point, as the game's own exits
// make it. dComIfGp_setNextStage (zeldaret/tww d_com_inf_game.cpp) fills dComIfG_play_c::mNextStage
// (dStage_nextStage_c: name[8], s16 point, s8 room, s8 layer, s8 enable, s8 wipe; WWHD play +0x5140,
// right behind the start stage at +0x5134) and the restart's last scene speed and mode; the play
// scene's draw (dScnPly_Draw, d_s_play.cpp) sees mEnable and requests the change. Applied by the
// game's main thread at the top of a frame (hook_0203593C in interp.cpp), like the cheats.
//
// The spawn point must exist in the room's player list (PLYR): the game takes the entry with that
// number without a bounds check (dStage_playerInit). The menu's list (android/warp_catalog.cpp,
// <game>/warps.json) only has spawns read from the game files. Layer -1: the stage picks it from the
// story flags, as on any door.
//
// The last scene mode (dSv_restart_c::mLastMode, dSv_info_c +0x1128 + 0x2C on the GameCube) decides how
// Link arrives (crawling on, getting up after a void out, thrown off the boat...; daPy_lk_c::playerInit);
// cleared with the last speed so he arrives as the spawn says. Only when the fields look like the
// GameCube layout (a log line shows them).
//
// Story presets: a save's story part (dSv_save_c, the first 0x778 bytes of dSv_info_c and of each
// slot in cking.sav: inventory, each stage's memory, sea chart, event flags) written into the live save
// before the warp, so the place loads as at that point of the story (OptionsMenu / StoryPresets.java).
// The stage being left writes its working memory (dSv_info_c::mMemory, +0x778) back into its table
// when it goes (dStage_Delete -> putSave), so that copy gets the preset's table of the current stage
// too (save tables per stage: <game>/warp_tables.txt, android/warp_catalog.cpp). The live save file
// on disk only changes when the player saves. Not touched: the temporary event flags (mTmp) and the
// room zones, which a save load would reset.
//
// A place (set_warp_place, e.g. the song scenes: in front of a stone tablet) puts Link at a position and facing once the
// warp has loaded and no cutscene runs: current / old.pos (+0x314 / +0x300), current / shape angle.y (+0x322 / +0x32A),
// the copies his execute restores them from each step (as setPlayerPosAndAngle does), the fall start height (+0x6A48,
// no hard landing when he drops onto the ground below).
//
// Test switch: WWHD_WARP=stage,room,spawn[,layer[,x,y,z,angle]] warps once a save file is loaded.
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

#include "mods.h"
#include "release.h"
#include "runtime.h"

namespace mods {
namespace {
const release::Data kPlay{0x1046F0B0};
const release::Data kOverlap{0x101F36CC};  // l_fopOvlpM_overlap[0]: a fade / wipe is under way (turbo.cpp)
constexpr uint32_t kStartStage = 0x5134, kNextStage = 0x5140;  // dStage_startStage_c / dStage_nextStage_c
constexpr uint32_t kNextPoint = kNextStage + 8, kNextRoom = kNextStage + 0xA, kNextLayer = kNextStage + 0xB;
constexpr uint32_t kNextEnable = kNextStage + 0xC, kNextWipe = kNextStage + 0xD;
constexpr uint32_t kRestart = 0x1128;  // dSv_info_c::mRestart
constexpr uint32_t kRestartRoom = kRestart + 0, kLastSpeed = kRestart + 0x28, kLastMode = kRestart + 0x2C;

constexpr uint32_t kStoryLen = 0x778;  // dSv_save_c
constexpr uint32_t kMemories = 0x380, kMemoryLen = 0x24;  // dSv_save_c::mMemory[16] (dSv_memory_c)
constexpr uint32_t kWorkMemory = 0x778;  // dSv_info_c::mMemory: the current stage's copy

struct Place {
    bool on = false;
    float x = 0, y = 0, z = 0;
    int16_t angle = 0;
};
struct Request {
    char stage[8];
    int room, spawn, layer;
    std::vector<uint8_t> story;  // a story preset to write first (empty: none)
    Place place;                 // where Link goes once it has loaded (on: false = the spawn's place)
};
std::mutex g_mutex;
Request g_req;
std::atomic<bool> g_pending{false};
std::string name_at(uint32_t a) {
    const char* p = (const char*)mem::ptr(a);
    return std::string(p, strnlen(p, 8));
}

int g_watch = 0;  // frames until a warp that hasn't loaded gets its log line

// the latest warp's progress for the test overlay (warp_status): requested -> fading (a fade / scene
// change seen) -> loaded (the fade is over in the target stage); refused: another change was under way
enum Phase { kIdle, kRequested, kFading, kLoaded, kRefused };
std::atomic<int> g_phase{kIdle};
std::atomic<int64_t> g_t0{0}, g_load_ms{0};
std::atomic<bool> g_applied{false};  // the request is in the game's next-stage fields
std::string g_target;  // game thread only
Place g_next_place;    // for the next request (set_warp_place; under g_mutex)
Place g_place;         // the loaded warp's, until Link is put there (game thread)
int g_place_wait = 0;  // frames after loading
constexpr uint32_t kPos = 0x314, kOldPos = 0x300, kAngleY = 0x320 + 2, kShapeAngleY = 0x328 + 2, kFallStartY = 0x6A48;
// daPy_lk_c::execute puts Link back at these each step (the GameCube's l_debug_keep_pos, l_debug_shape_angle,
// l_debug_current_angle: USA 0240D12C on), which only the game's setPlayerPosAndAngle changes; written with him
const release::Data kKeepPos{0x1046CD48}, kKeepShapeAngle{0x1046CD10}, kKeepAngle{0x1046CD08};

int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void track() {
    int ph = g_phase.load(std::memory_order_relaxed);
    if (ph != kFading && !(ph == kRequested && g_applied.load())) return;
    if (ld32(kOverlap)) {
        g_phase = kFading;
    } else if (ph == kFading && name_at(kPlay + kStartStage) == g_target) {
        g_load_ms = now_ms() - g_t0.load();
        g_phase = kLoaded;
        LOG("[warp] loaded %s in %.1f s", g_target.c_str(), g_load_ms.load() / 1000.0);
    }
}

// a warp that hasn't loaded after 15 s: one line with where the scene change stands (a stuck change
// keeps the fade on and never gets to the new scene: the first-visit Forsaken Fortress tower)
void watch() {
    if (g_watch <= 0 || --g_watch) return;
    if (g_phase.load() == kLoaded) return;
    uint32_t p = kPlay, ovl = ld32(kOverlap);
    LOG("[warp] not loaded after 15 s: stage %s room %d point %d, next enable %d, overlap %08X task %08X, event mode %d",
        name_at(p + kStartStage).c_str(), (int8_t)ld8(p + kStartStage + 0xA), (int16_t)ld16(p + kStartStage + 8),
        ld8(p + kNextEnable), ovl, ovl ? ld32(ovl + 0x20) : 0, event_mode_now());
}

// the stages' save tables (<game>/warp_tables.txt: "stage table" lines)
int save_table(const std::string& stage) {
    static std::map<std::string, int> tables;  // read again while empty (the file is made in the background)
    if (tables.empty()) {
        if (FILE* f = fopen((config::game_dir + "/warp_tables.txt").c_str(), "r")) {
            char name[16];
            int t;
            while (fscanf(f, "%15s %d", name, &t) == 2) tables[name] = t;
            fclose(f);
        }
    }
    auto it = tables.find(stage);
    return it == tables.end() ? -1 : it->second;
}

void request(const char* stage, int room, int spawn, int layer, std::vector<uint8_t> story = {}) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_req.story = std::move(story);
    memset(g_req.stage, 0, sizeof g_req.stage);
    strncpy(g_req.stage, stage, sizeof g_req.stage);
    g_req.room = room;
    g_req.spawn = spawn;
    g_req.layer = layer;
    g_req.place = g_next_place;
    g_next_place = Place();
    g_pending = true;
}

// once loaded and no cutscene runs (a few frames for Link to be the new stage's): Link at the place
void place_link() {
    if (!g_place.on || g_phase.load() != kLoaded) return;
    if (event_mode_now() != 0) return;
    if (++g_place_wait < 30) return;  // (1 s: a spawn's own start, e.g. walking in, is over)
    uint32_t link = link_actor();
    if (!link) return;
    const Place p = g_place;
    g_place.on = false;
    for (uint32_t at : {kPos, kOldPos}) {
        stf32(link + at, p.x);
        stf32(link + at + 4, p.y);
        stf32(link + at + 8, p.z);
    }
    st16(link + kAngleY, (uint16_t)p.angle);
    st16(link + kShapeAngleY, (uint16_t)p.angle);
    stf32(kKeepPos, p.x);
    stf32(kKeepPos + 4, p.y);
    stf32(kKeepPos + 8, p.z);
    st16(kKeepShapeAngle + 2, (uint16_t)p.angle);
    st16(kKeepAngle + 2, (uint16_t)p.angle);
    stf32(link + kFallStartY, p.y);
    LOG("[warp] Link placed at %.0f %.0f %.0f, angle %d", p.x, p.y, p.z, p.angle);
}

// the benchmark (set_warp_from_title) or WWHD_WARP_TITLE=1: the title screen counts as loaded; its
// stage sea_T is a play scene with placeholder save data
std::atomic<bool> g_from_title{getenv("WWHD_WARP_TITLE") != nullptr};

uint32_t loaded_save_or_title() {
    if (uint32_t s = loaded_save()) return s;
    return g_from_title && name_at(kPlay + kStartStage) == "sea_T" ? save_info() : 0;
}

// the next stage fields as a door would set them
void write_next(const Request& r) {
    uint32_t p = kPlay;
    for (int i = 0; i < 8; i++) st8(p + kNextStage + i, (uint8_t)r.stage[i]);
    st16(p + kNextPoint, (uint16_t)(int16_t)r.spawn);
    st8(p + kNextRoom, (uint8_t)(int8_t)r.room);
    st8(p + kNextLayer, (uint8_t)(int8_t)r.layer);
    st8(p + kNextWipe, 0);
    st8(p + kNextEnable, 1);
}

// a warp from the title screen, for its scene's next draw
constexpr uint32_t kPlayScene = 7;  // fpcNm_PLAY_SCENE_e
Request g_title_req;
std::atomic<bool> g_title_change{false};

// WWHD_WARP=stage,room,spawn[,layer]
[[maybe_unused]] bool g_env_read = [] {
    const char* e = getenv("WWHD_WARP");
    if (!e) return false;
    char st[9] = {};
    int room = 0, spawn = 0, layer = -1;
    Place p;
    int angle = 0;
    if (sscanf(e, "%8[^,],%d,%d,%d,%f,%f,%f,%d", st, &room, &spawn, &layer, &p.x, &p.y, &p.z, &angle) == 8) {
        p.on = true;
        p.angle = (int16_t)angle;
        g_next_place = p;
    }
    if (sscanf(e, "%8[^,],%d,%d,%d", st, &room, &spawn, &layer) >= 3) {
        request(st, room, spawn, layer);
        g_phase = kRequested;  // (as request_warp: the progress, and a place, follow it)
    }
    return true;
}();
}  // namespace

bool request_warp(const char* stage, int room, int spawn, int layer, const uint8_t* story, size_t story_len) {
    if (!loaded_save_or_title()) return false;
    if (story && story_len != kStoryLen) return false;
    request(stage, room, spawn, layer, story ? std::vector<uint8_t>(story, story + story_len) : std::vector<uint8_t>());
    g_t0 = now_ms();
    g_applied = false;
    g_phase = kRequested;
    LOG("[warp] requested %.8s room %d spawn %d layer %d%s", stage, room, spawn, layer, story ? " with a story preset" : "");
    return true;
}

void set_warp_from_title(bool on) { g_from_title = on; }

void set_warp_place(float x, float y, float z, int angle) {
    std::lock_guard<std::mutex> l(g_mutex);
    g_next_place = Place{true, x, y, z, (int16_t)angle};
}

void warp_title_draw(Cpu* c, uint32_t scene) {
    if (!g_title_change.load(std::memory_order_relaxed)) return;
    if (name_at(kPlay + kStartStage) != "sea_T") {
        g_title_change = false;
        return;
    }
    if (ld32(kOverlap)) return;  // a fade is under way
    write_next(g_title_req);  // (the opening scene clears the enable)
    // fopScnM_ChangeReq(scene, fpcNm_PLAY_SCENE_e, fpcNm_OVERLAP0_e, 5), as the play scene's draw does, but
    // without its sound scene change: the play scene's load then waits for the title's sound forever
    // (and so does any later scene change from that scene: the benchmark stays in the one it reaches)
    // (from the start of the draw's hook: its arguments and the other volatile registers must survive)
    uint32_t r[13];
    uint8_t cr[32];
    std::remove_reference_t<decltype(c->f[0])> f[14];
    memcpy(r, c->r, sizeof r);
    memcpy(cr, c->cr, sizeof cr);
    memcpy(f, c->f, sizeof f);
    uint8_t so = c->xer_so, ov = c->xer_ov, ca = c->xer_ca;
    uint32_t pc = c->pc;
    uint32_t ok = guest_call(c, release::Code{0x025DC86C}, {scene, kPlayScene, 0, 5});
    memcpy(c->r, r, sizeof r);
    memcpy(c->cr, cr, sizeof cr);
    memcpy(c->f, f, sizeof f);
    c->xer_so = so, c->xer_ov = ov, c->xer_ca = ca;
    c->pc = pc;
    LOG("[warp] title scene: play scene requested%s", ok ? "" : " (refused)");
    if (ok) g_title_change = false;
}

std::vector<uint8_t> story_block() {
    uint32_t s = loaded_save();
    if (!s) return {};
    std::vector<uint8_t> b(kStoryLen);
    memcpy(b.data(), mem::ptr(s), kStoryLen);
    return b;
}

std::string warp_where() {
    if (!loaded_save()) return "";
    uint32_t p = kPlay;
    char b[64];
    snprintf(b, sizeof b, "%s,%d,%d,%d", name_at(p + kStartStage).c_str(), (int8_t)ld8(p + kStartStage + 0xA),
             (int16_t)ld16(p + kStartStage + 8), (int8_t)ld8(p + kStartStage + 0xB));
    return b;
}

void warp_status(int64_t out[3]) {
    int ph = g_phase.load();
    out[0] = ph;
    out[1] = ph == kLoaded ? g_load_ms.load() : ph == kIdle ? 0 : now_ms() - g_t0.load();
    out[2] = event_mode_now();  // turbo.cpp: 0 none, 1 dialogue, 2 / 3 cutscene
}

void warp_service() {
    watch();
    track();
    place_link();
    if (!g_pending.load(std::memory_order_relaxed)) return;
    uint32_t s = loaded_save_or_title();
    if (!s) return;  // stays pending (test switch: until a file is loaded)
    Request r;
    {
        std::lock_guard<std::mutex> l(g_mutex);
        r = g_req;
        g_pending = false;
    }
    uint32_t p = kPlay;
    // the previous change, as the game left it (its enable is cleared once the new scene runs)
    LOG("[warp] next stage before: %s room %d point %d layer %d enable %d wipe %d", name_at(p + kNextStage).c_str(),
        (int8_t)ld8(p + kNextRoom), (int16_t)ld16(p + kNextPoint), (int8_t)ld8(p + kNextLayer), ld8(p + kNextEnable),
        ld8(p + kNextWipe));
    if (ld8(p + kNextEnable)) {
        LOG("[warp] a scene change is already under way; skipped");
        g_phase = kRefused;
        return;
    }
    int8_t restart_room = (int8_t)ld8(s + kRestartRoom);
    uint32_t speed_bits = ld32(s + kLastSpeed), mode = ld32(s + kLastMode);
    float speed;
    memcpy(&speed, &speed_bits, 4);
    // mode: scene mode in the low byte plus the dragon shield (0x8000), soup (0x4000) and Tingle shield
    // (bit 16) flags
    bool restart_ok = restart_room >= 0 && restart_room < 64 && std::isfinite(speed) && fabsf(speed) < 200.0f &&
                      (mode & ~0x1C0FFu) == 0 && (mode & 0xFF) < 16;
    LOG("[warp] restart: room %d, last speed %.2f, last mode %08X (%s)", restart_room, speed, mode,
        restart_ok ? "cleared" : "unexpected layout, left alone");
    if (restart_ok) {
        st32(s + kLastSpeed, 0);
        st32(s + kLastMode, mode & ~0xFFu);  // the shield / soup flags stay (as dComIfGp_setNextStage sets them)
    }
    if (!r.story.empty()) {
        std::string here = name_at(p + kStartStage);
        int tbl = save_table(here);
        memcpy(mem::ptr(s), r.story.data(), kStoryLen);
        if (tbl >= 0 && tbl < 16) memcpy(mem::ptr(s + kWorkMemory), r.story.data() + kMemories + kMemoryLen * tbl, kMemoryLen);
        LOG("[warp] story preset written (life %u/%u, rupees %u); %s keeps table %d", ld16(s + 2), ld16(s), ld16(s + 4),
            here.c_str(), tbl);
    }
    write_next(r);
    // the title screen runs as the opening scene, whose draw ignores the next stage (dScnPly_Draw acts
    // only in a play scene): warp_title_draw asks for the play scene itself
    if (name_at(p + kStartStage) == "sea_T") {
        g_title_req = r;
        g_title_change = true;
    }
    g_target.assign(r.stage, strnlen(r.stage, 8));
    g_place = r.place;
    g_place_wait = 0;
    g_applied = true;
    g_watch = 450;
    LOG("[warp] %s -> %.8s room %d spawn %d layer %d", name_at(p + kStartStage).c_str(), r.stage, r.room, r.spawn,
        r.layer);
}

}  // namespace mods
