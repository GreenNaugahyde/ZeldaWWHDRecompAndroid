// Fly tour (developer, the shader tour's fly mode; ShaderTour.java): Link flies through the places of the
// room the tour warped to, above the ground and through walls, so its far corners, the objects that only
// appear near him and the views between them get drawn. The path: <game>/flypath.txt ("stage room x y z"
// per line, in flight order; tools/flypath.py makes it from the game files), read at the first start.
//
// Around daPy_lk_c::execute (climb.cpp), as the moon jump (cheats.cpp): before his step he's put into the
// game's fall (procFall_init) with his ground flags cleared and no fall speed, so his own procedures don't
// walk, swim or land, and his collision (dBgS_Acch) checks no ground, walls, roof or water during the step
// (GRND_NONE / WALL_NONE / ROOF_NONE / WATER_NONE, set back after it): no ground found, so no room change
// (dStage_RoomCheck, 025C35E8), no exits, lava or void. The room stays the one warped to (with its
// neighbours as the stage loads them): changing rooms in flight removed zones whose actors still ran
// (d_save.cpp assertion 0 <= zoneNo, Dragon Roost Cavern). After the step his position is overwritten with
// the flight's: the same fields the warp's place writes (warp.cpp: current / old pos, the copies his execute
// restores them from, angles, fall start height: no hard landing).
//
// While a dialogue or cutscene runs the flight waits (the tour presses A). fly_left: points still to go,
// 0 when done (he hovers there until fly_stop), -1 when no flight runs (no path for this room: the tour
// walks as before).
#include <atomic>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "mods.h"
#include "release.h"
#include "runtime.h"
#include "true60.h"

namespace mods {
namespace {
struct Point {
    float x, y, z;
};
std::map<std::string, std::vector<Point>> g_paths;  // "stage room" -> path (game thread)
bool g_read = false;
std::atomic<bool> g_start{false}, g_stop{false};
std::atomic<int> g_left{-1};
std::vector<Point> g_path;  // the flight under way (game thread)
size_t g_next = 0;
Point g_at{};
float g_dwell = 0;  // 30 Hz steps still to hover at the point reached
int16_t g_face = 0;
uint32_t g_link = 0;  // the Link flying (a new one after a scene change ends the flight)

constexpr float kSpeed = 45.0f;  // units per 30 Hz step (the moon jump flies 40)
constexpr float kDwell = 12.0f;  // steps at each point
constexpr uint32_t kPos = 0x314, kOldPos = 0x300, kAngleY = 0x320 + 2, kShapeAngleY = 0x328 + 2;
constexpr uint32_t kFallStartY = 0x6A48, kSpeedF = 0x33C, kCurProc = 0x65F0, kProcFall = 0x27;
constexpr uint32_t kAcchFlags = 0x80C + 0x28, kAcchGround = 0x20 | 0x40 | 0x80;  // as cheats.cpp
constexpr uint32_t kAcchNone = 0x2 | 0x4 | 0x8 | 0x400;  // GRND_NONE, WALL_NONE, ROOF_NONE, WATER_NONE
uint32_t g_none_had = 0;  // which of them his own code had set (kept after the step)
uint32_t g_none_link = 0; // the Link whose flags are set for this step (0: none)
const release::Code kFallInit{0x023F6564};
const release::Data kKeepPos{0x1046CD48}, kKeepShapeAngle{0x1046CD10}, kKeepAngle{0x1046CD08};  // warp.cpp

void read_paths() {
    g_read = true;
    FILE* f = fopen((config::game_dir + "/flypath.txt").c_str(), "r");
    if (!f) {
        LOG("[fly] no %s/flypath.txt", config::game_dir.c_str());
        return;
    }
    char stage[16];
    int room;
    Point p;
    size_t n = 0;
    while (fscanf(f, "%15s %d %f %f %f", stage, &room, &p.x, &p.y, &p.z) == 5) {
        g_paths[std::string(stage) + " " + std::to_string(room)].push_back(p);
        n++;
    }
    fclose(f);
    LOG("[fly] %zu points in %zu rooms", n, g_paths.size());
}

// the room the warp went to: "stage room" from warp_where ("stage,room,spawn,layer")
std::string here() {
    std::string w = warp_where();
    size_t a = w.find(','), b = a == std::string::npos ? a : w.find(',', a + 1);
    if (b == std::string::npos) return "";
    return w.substr(0, a) + " " + w.substr(a + 1, b - a - 1);
}

void begin(uint32_t link) {
    if (!g_read) read_paths();
    auto it = g_paths.find(here());
    if (it == g_paths.end() || it->second.empty()) {
        LOG("[fly] no path for %s", here().c_str());
        g_left = -1;
        return;
    }
    g_path = it->second;
    g_next = 0;
    g_dwell = 0;
    g_at = {(float)ldf32(link + kPos), (float)ldf32(link + kPos + 4), (float)ldf32(link + kPos + 8)};
    g_face = (int16_t)ld16(link + kShapeAngleY);
    g_left = (int)g_path.size();
    g_link = link;
    LOG("[fly] %s: %zu points", here().c_str(), g_path.size());
}

void put(uint32_t link) {
    for (uint32_t at : {kPos, kOldPos}) {
        stf32(link + at, g_at.x);
        stf32(link + at + 4, g_at.y);
        stf32(link + at + 8, g_at.z);
    }
    stf32(kKeepPos, g_at.x);
    stf32(kKeepPos + 4, g_at.y);
    stf32(kKeepPos + 8, g_at.z);
    st16(link + kAngleY, (uint16_t)g_face);
    st16(link + kShapeAngleY, (uint16_t)g_face);
    st16(kKeepShapeAngle + 2, (uint16_t)g_face);
    st16(kKeepAngle + 2, (uint16_t)g_face);
    stf32(link + kFallStartY, g_at.y);
}

// on along the path by one step (dt: 30 Hz steps)
void advance(float dt) {
    if (g_dwell > 0) {
        g_dwell -= dt;
        return;
    }
    const Point& t = g_path[g_next];
    float dx = t.x - g_at.x, dy = t.y - g_at.y, dz = t.z - g_at.z;
    float d = std::sqrt(dx * dx + dy * dy + dz * dz), s = kSpeed * dt;
    if (dx * dx + dz * dz > 1.0f)
        g_face = (int16_t)std::lround(std::atan2(dx, dz) * (32768.0 / 3.14159265358979));
    if (d <= s) {
        g_at = t;
        g_dwell = kDwell;
        g_next++;
        g_left = (int)(g_path.size() - g_next);
        if (g_left.load() == 0) LOG("[fly] done");
        return;
    }
    g_at.x += dx / d * s;
    g_at.y += dy / d * s;
    g_at.z += dz / d * s;
}
}  // namespace

void fly_start() {
    g_left = -2;  // (being started: the tour waits for the game thread)
    g_start = true;
}
void fly_stop() { g_stop = true; }
int fly_left() { return g_left.load(); }

void fly_step(Cpu* c, uint32_t link, bool before) {
    if (g_stop.exchange(false)) {
        g_path.clear();
        g_left = -1;
    }
    if (!link) return;
    if (!before && g_none_link == link) st32(link + kAcchFlags, (ld32(link + kAcchFlags) & ~kAcchNone) | g_none_had);
    g_none_link = 0;
    if (before && g_start.exchange(false)) begin(link);
    if (!g_path.empty() && link != g_link) {
        g_path.clear();
        g_left = -1;
    }
    // (done: he stays at the last point until the tour stops the flight once the next warp has loaded; let
    // go there he'd fall, maybe into a pit, and the void out's scene change holds up the warp)
    if (g_path.empty() || event_mode_now() != 0) return;
    if (before) {
        if (ld32(link + kCurProc) != kProcFall) {
            const Cpu saved = *c;
            c->f[1].ps0 = c->f[1].ps1 = 5.0;  // animation blend
            guest_call(c, kFallInit, {link, 0});
            *c = saved;
        }
        const uint32_t fl = ld32(link + kAcchFlags);
        g_none_had = fl & kAcchNone;
        st32(link + kAcchFlags, (fl & ~kAcchGround) | kAcchNone);
        g_none_link = link;
        stf32(link + kSpeedF, 0.0f);
        stf32(link + kSpeedF + 4, 0.0f);
        stf32(link + kSpeedF + 8, 0.0f);
        put(link);
        return;
    }
    if (g_next < g_path.size()) advance(true60::dt());
    put(link);
}

}  // namespace mods
