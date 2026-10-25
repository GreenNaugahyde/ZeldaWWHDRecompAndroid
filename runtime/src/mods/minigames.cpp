// Touch for the minigames: a finger on the TV picture picks what the game's cursor or stick would
// pick, and the game is played through its own input:
//   Sploosh Kaboom (daMgBoard_c): the finger moves the cursor to the square under it; letting go
//     fires there (the cursor set, A pressed).
//   The Cabana's picture puzzles (daObjApzl_c): a tap on a piece in the blank's row or column, or
//     dragging it towards the blank, slides it there (with the pieces between): one stick push per
//     move, as the game takes them.
//
// The minigame actors are found through hooks on their Execute (r3 = the actor), seen again each
// step while they exist. Their fields: the GameCube decompilation's layouts (zeldaret/tww) moved by
// WWHD's bigger actor base (fopAc_ac_c: current.pos 0x1F8 -> 0x314, +0x11C). Screen positions come
// from the game camera (camera_draw's eye / center / up / fovy) in the game's layout units: 720
// across the TV picture's height, 0 at its centre, so the TV picture's shape (Hor+) doesn't matter.
//
// WWHD_MG_WATCH=1 logs the bytes of those actors that change (each offset until it changed often)
// and marks the targets on the TV picture (ControlsView), to find and check the fields.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

#include "mods.h"
#include "runtime.h"
#include "../input.h"
#include "../aspect.h"
#include "../hud.h"
#include "../interp.h"
#include "../release.h"

extern "C" {
void f_021C5634_orig(Cpu* c);  // daMgBoard_Execute (Sploosh Kaboom)
void f_02318640_orig(Cpu* c);  // daObjApzl_Execute (the Cabana's picture puzzles)
void f_0220EFF0_orig(Cpu* c);  // daNpc_Bmsw_Execute (Rito post: mail sorting)
void f_022197C4_orig(Cpu* c);  // daNpc_Btsw_Execute (Rito post: mail sorting, Koboli)
void f_02109AF8_orig(Cpu* c);  // daCanonExecute (Spectacle Island's cannon game)
void f_025BB698_orig(Cpu* c);  // dShop_now_triggercheck (r4 = ShopItems_c)
void f_02483C0C_orig(Cpu* c);  // daShopItem_Execute
}

namespace mods {
namespace {
std::atomic<bool> g_wind_finger{false};  // a finger on the wind compass (mode 11) holds the GamePad touch

const bool g_watch = getenv("WWHD_MG_WATCH") != nullptr;

enum Kind { kBoard, kPuzzle, kMail, kMail2, kCannon, kKinds };
const char* const kKindName[kKinds] = {"board", "puzzle", "mail", "mail2", "cannon"};
constexpr uint32_t kBase = 0x11C;  // WWHD actor fields = GameCube + this (past fopAc_ac_c)
constexpr uint32_t kPos = 0x314, kAngle = 0x320;  // fopAc_ac_c current.pos, current.angle

// ---- the actors, as their Execute saw them
constexpr uint32_t kWatchLen = 0xA00;
struct Seen {
    uint32_t actor = 0;
    uint64_t step = 0;  // the logic step it last executed in
    uint8_t snap[kWatchLen];
    uint16_t changes[kWatchLen];
    bool snapped = false;
};
Seen g_seen[kKinds];

uint64_t now() { return step(); }
bool alive(Kind k) { return g_seen[k].actor && g_seen[k].step + 3 >= now(); }

void watch(Seen& s) {
    if (!g_watch) return;
    if (!s.snapped) {
        for (uint32_t i = 0; i < kWatchLen; i++) s.snap[i] = ld8(s.actor + i);
        memset(s.changes, 0, sizeof s.changes);
        s.snapped = true;
        std::string line;
        for (uint32_t i = 0; i < 0x700; i += 4) {
            char t[24];
            snprintf(t, sizeof t, "%s%08X", i % 32 ? " " : "\n  ", ld32(s.actor + i));
            line += t;
        }
        LOG("[mg] %s at %08X:%s", kKindName[&s - g_seen], s.actor, line.c_str());
        return;
    }
    std::string line;
    int n = 0;
    for (uint32_t i = 0; i < kWatchLen; i++) {
        uint8_t v = ld8(s.actor + i);
        if (v == s.snap[i]) continue;
        if (s.changes[i] < 0xFFFF) s.changes[i]++;
        if (s.changes[i] <= 24 && n < 40) {
            char t[32];
            snprintf(t, sizeof t, " +%X:%02X>%02X", i, s.snap[i], v);
            line += t;
            n++;
        }
        s.snap[i] = v;
    }
    if (n) LOG("[mg] %s step %llu:%s", kKindName[&s - g_seen], (unsigned long long)now(), line.c_str());
}

// ---- the camera: world -> layout units
struct V3 {
    float x, y, z;
};
V3 sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 0 ? V3{a.x / l, a.y / l, a.z / l} : a;
}
V3 ldv(uint32_t a) { return {(float)ldf32(a), (float)ldf32(a + 4), (float)ldf32(a + 8)}; }

struct View {
    V3 eye, f, r, u;
    float k;  // layout units per unit of x / z
    float dy = 0;  // the picture's shift (layout units, up)
    bool ok = false;
};
View g_view;

// camera_draw's camera (interp.cpp): eye +0xDC, center +0xE8, up +0xF4, fovy +0xD4 (degrees)
void take_view() {
    uint32_t cam = interp::last_camera();
    g_view.ok = false;
    if (!cam) return;
    V3 eye = ldv(cam + 0xDC), center = ldv(cam + 0xE8), up = ldv(cam + 0xF4);
    float fovy = (float)ldf32(cam + 0xD4);
    if (!(fovy > 1 && fovy < 170)) return;
    g_view.eye = eye;
    g_view.f = norm(sub(center, eye));
    g_view.r = norm(cross(g_view.f, up));
    g_view.u = cross(g_view.r, g_view.f);
    g_view.k = 360.0f / (std::tan(fovy * 3.14159265f / 360.0f) * aspect::view_tan_scale());  // (as drawn: zoomed out)
    g_view.dy = -aspect::view_shift_ndc() * 360.0f;
    g_view.ok = true;
}

bool project(V3 p, float& lx, float& ly) {
    if (!g_view.ok) return false;
    V3 d = sub(p, g_view.eye);
    float z = dot(d, g_view.f);
    if (z <= 1) return false;
    lx = dot(d, g_view.r) / z * g_view.k;
    ly = dot(d, g_view.u) / z * g_view.k + g_view.dy;
    return true;
}

// a point of an actor's model space (y rotation current.angle.y, as mDoMtx YrotM) in the world
V3 actor_point(uint32_t actor, float x, float y, float z) {
    V3 p = ldv(actor + kPos);
    float a = (int16_t)ld16(actor + kAngle + 2) * (3.14159265f / 32768.0f);
    float c = std::cos(a), s = std::sin(a);
    return {p.x + x * c + z * s, p.y + y, p.z - x * s + z * c};
}

// dComIfGp_getMiniGameType: play + 0x5CEA (GameCube + 0x4A3A; touch_state.cpp): 3 the cannon game,
// 7 mail sorting
const release::Data kPlay{0x1046F0B0};
uint8_t minigame_type() { return ld8(kPlay + 0x5CEA); }

// ---- touches from the app (any thread), taken by the game thread
struct Touch {
    int action;
    float x, y;
};
std::mutex g_touch_mu;
std::vector<Touch> g_touches;

std::vector<Touch> take_touches() {
    std::lock_guard<std::mutex> lk(g_touch_mu);
    std::vector<Touch> t;
    t.swap(g_touches);
    return t;
}

// ---- input for the game: stick pushes and button presses, each for some logic steps
struct Push {
    uint32_t buttons;
    float lx, ly;
    int steps;
};
std::mutex g_pad_mu;
std::deque<Push> g_pushes;
uint64_t g_push_end = 0;  // the step the front push ends (0: not started)


void push(uint32_t buttons, float lx, float ly, int steps) {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    g_pushes.push_back({buttons, lx, ly, steps});
}
bool pushing() {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    return !g_pushes.empty();
}
void clear_pushes() {
    std::lock_guard<std::mutex> lk(g_pad_mu);
    g_pushes.clear();
    g_push_end = 0;
}

// ---- debug marks
std::mutex g_mark_mu;
std::vector<float> g_marks;

// ==== Sploosh Kaboom (daMgBoard_c; its code isn't decompiled, the fields are named in the header)
constexpr uint32_t kBoardX = 0x468 + kBase, kBoardY = 0x469 + kBase;  // mBoardPosX / Y: the cursor's square
constexpr uint32_t kBoardState = 0x6DC;  // mState (GameCube 0x638: the fields between are smaller in WWHD): 1 idle, 3 playing
constexpr uint32_t kBoardPlaying = 3;
bool board_playing(uint32_t a) { return ld32(a + kBoardState) == kBoardPlaying; }
// the squares: set_mtx puts the cursor at current.pos + table[Y][X] (cXyz [8][8], filled at run time;
// 021C45EC)
const release::Data kBoardCells{0x1046500C};

V3 board_square(uint32_t a, int x, int y) {
    V3 p = ldv(a + kPos), o = ldv(kBoardCells + y * 0x60 + x * 12);
    return {p.x + o.x, p.y + o.y, p.z + o.z};
}

int board_square_at(uint32_t a, float x, float y) {
    float x0, y0, x1, y1;
    if (!project(board_square(a, 0, 0), x0, y0) || !project(board_square(a, 1, 1), x1, y1)) return -1;
    float reach = 0.75f * std::hypot(x1 - x0, y1 - y0) / std::sqrt(2.0f);
    int best = -1;
    float bd = 1e30f;
    for (int q = 0; q < 64; q++) {
        float px, py;
        if (!project(board_square(a, q % 8, q / 8), px, py)) continue;
        float d = std::hypot(px - x, py - y);
        if (d < bd) {
            bd = d;
            best = q;
        }
    }
    return bd <= reach ? best : -1;
}

struct Grip {
    bool down = false;
    float x0 = 0, y0 = 0, x = 0, y = 0;
    int cell = -1;  // the square / piece under the finger when it came down
    int done = 0;   // puzzle: moves made by this drag
};
Grip g_grip;

// ==== the picture puzzles (daObjApzl_c)
// (its fields sit 0x71C past the GameCube's, not kBase: WWHD's 16 mDoExt_btpAnm are bigger; seen in
// move_piece / _execute: mBlankIdx 0xB8D, mPiecePos 0xB92, mState 0xBA2, mEventIdx 0xBA8)
constexpr uint32_t kPzBase = 0x71C;
constexpr uint32_t kPzBlank = 0x471 + kPzBase, kPzMoveTimer = 0x473 + kPzBase, kPzPieces = 0x476 + kPzBase,
                   kPzState = 0x486 + kPzBase;
constexpr uint8_t kPzPlaying = 3;  // STATE_PLAYING

// a square of the 4 x 4 board (0 top left .. 15) in the world: set_mtx puts piece square p at
// ((p % 4) * 40 - 60, 60 - (p / 4) * 40, 0) of the actor
V3 pz_square(uint32_t a, int p) { return actor_point(a, (p % 4) * 40.0f - 60.0f, 60.0f - (p / 4) * 40.0f, 0); }

int pz_blank_square(uint32_t a) {
    uint8_t blank = ld8(a + kPzBlank);
    return blank < 16 ? ld8(a + kPzPieces + blank) : -1;
}

int pz_square_at(uint32_t a, float x, float y) {
    int best = -1;
    float bd = 1e30f;
    float x0, y0, x1, y1;
    if (!project(pz_square(a, 0), x0, y0) || !project(pz_square(a, 5), x1, y1)) return -1;
    float reach = 0.75f * std::hypot(x1 - x0, y1 - y0) / std::sqrt(2.0f);  // a little over half a square
    for (int p = 0; p < 16; p++) {
        float px, py;
        if (!project(pz_square(a, p), px, py)) continue;
        float d = std::hypot(px - x, py - y);
        if (d < bd) {
            bd = d;
            best = p;
        }
    }
    return bd <= reach ? best : -1;
}

// slides the pieces between the blank and square p towards the blank: stick pushes the way the
// pieces move (left: the piece right of the blank moves left; up: the one below it moves up)
int pz_slide(uint32_t a, int p) {
    int b = pz_blank_square(a);
    if (b < 0 || p < 0 || p == b) return 0;
    float lx = 0, ly = 0;
    int n = 0;
    if (p / 4 == b / 4) {
        n = std::abs(p - b);
        lx = p > b ? -1 : 1;
    } else if (p % 4 == b % 4) {
        n = std::abs(p - b) / 4;
        ly = p > b ? 1 : -1;
    } else {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        push(0, lx, ly, 2);
        push(0, 0, 0, 6);  // the slide's 5 steps (mMoveTimer), then the stick back in the middle
    }
    return n;
}

void puzzle_step(uint32_t a) {
    bool playing = ld8(a + kPzState) == kPzPlaying;
    for (const Touch& t : take_touches()) {
        if (!playing) continue;
        if (t.action == 0) {
            g_grip = {};
            g_grip.down = true;
            g_grip.x0 = g_grip.x = t.x;
            g_grip.y0 = g_grip.y = t.y;
            g_grip.cell = pz_square_at(a, t.x, t.y);
        } else if (t.action == 1 && g_grip.down) {
            g_grip.x = t.x;
            g_grip.y = t.y;
            // dragging: once the finger went half a square towards the blank, the pieces slide
            int b = pz_blank_square(a), p = g_grip.cell;
            if (p < 0 || b < 0 || g_grip.done || pushing()) continue;
            float px, py, bx, by;
            if (!project(pz_square(a, p), px, py) || !project(pz_square(a, b), bx, by)) continue;
            float dx = bx - px, dy = by - py, len = std::hypot(dx, dy);
            if (len < 1) continue;
            int squares = std::max(std::abs(p % 4 - b % 4), std::abs(p / 4 - b / 4));
            float along = ((t.x - g_grip.x0) * dx + (t.y - g_grip.y0) * dy) / len;
            if (along > 0.5f * len / squares) g_grip.done = pz_slide(a, p);
        } else if (t.action == 2 && g_grip.down) {
            g_grip.down = false;
            if (g_grip.done || std::isnan(t.x)) continue;
            // a tap (the finger stayed on its piece): slide it
            if (pz_square_at(a, t.x, t.y) == g_grip.cell && !pushing()) pz_slide(a, g_grip.cell);
        }
    }
    if (g_watch) {
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
        int b = pz_blank_square(a);
        float x, y;
        if (b >= 0 && project(pz_square(a, b), x, y)) g_marks.insert(g_marks.end(), {x, y});
        for (int p = 0; p < 16; p++)
            if (p != b && project(pz_square(a, p), x, y)) g_marks.insert(g_marks.end(), {x, y});
    }
}

// the finger puts the cursor on its square; letting go there fires (A)
void board_step(uint32_t a) {
    for (const Touch& t : take_touches()) {
        int q = std::isnan(t.x) ? -1 : board_square_at(a, t.x, t.y);
        if (t.action == 0) g_grip = {};
        if (q >= 0) {
            st8(a + kBoardX, (uint8_t)(q % 8));
            st8(a + kBoardY, (uint8_t)(q / 8));
        }
        if (t.action == 2 && q >= 0 && !pushing()) {
            push(input::kA, 0, 0, 2);
            push(0, 0, 0, 2);
        }
    }
    if (g_watch) {
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
        float x, y;
        if (project(board_square(a, (int8_t)ld8(a + kBoardX), (int8_t)ld8(a + kBoardY)), x, y)) g_marks.insert(g_marks.end(), {x, y});
        for (int q = 0; q < 64; q++)
            if (project(board_square(a, q % 8, q / 8), x, y)) g_marks.insert(g_marks.end(), {x, y});
    }
}

// ==== the Rito post's mail sorting (daNpc_Bmsw_c::shiwake_game_action; Koboli's daNpc_Btsw_c the same)
// the cursor's slot: column -1..1 (+0), row 0..1 (+1) (mail slots 3 x 2); the action's phase (1: sorting)
struct MailFields {
    uint32_t cursor, status;
};
const MailFields kBmsw{0xD54, 0xD7D}, kBtsw{0xD44, 0xD6D};  // GameCube 0x9B0, 0x9DD (both)
// cursor_pos_data[row][1 + column]: the cursor in front of each slot (world)
const V3 kMailSlots[2][3] = {{{-52, 804, 996}, {-140, 804, 996}, {-227, 804, 996}},
                             {{-52, 889, 996}, {-140, 889, 996}, {-227, 889, 996}}};

int mail_slot_at(float x, float y) {
    float x0, y0, x1, y1;
    if (!project(kMailSlots[0][0], x0, y0) || !project(kMailSlots[1][1], x1, y1)) return -1;
    float reach = 0.8f * std::hypot(x1 - x0, y1 - y0) / std::sqrt(2.0f);
    int best = -1;
    float bd = 1e30f;
    for (int q = 0; q < 6; q++) {
        float px, py;
        if (!project(kMailSlots[q / 3][q % 3], px, py)) continue;
        float d = std::hypot(px - x, py - y);
        if (d < bd) {
            bd = d;
            best = q;
        }
    }
    return bd <= reach ? best : -1;
}

// sorting: the game's minigame 7 runs and the action is in its sorting phase
bool mail_sorting(uint32_t a, const MailFields& f) { return minigame_type() == 7 && ld8(a + f.status) == 1; }

// a tap on a slot: the cursor there and A (the game throws the letter if one is ready)
void mail_step(uint32_t a, const MailFields& f) {
    for (const Touch& t : take_touches()) {
        if (t.action != 2 || std::isnan(t.x)) continue;
        int q = mail_slot_at(t.x, t.y);
        if (q < 0) continue;
        st8(a + f.cursor, (uint8_t)(int8_t)(q % 3 - 1));
        st8(a + f.cursor + 1, (uint8_t)(q / 3));
        if (!pushing()) {
            push(input::kA, 0, 0, 2);
            push(0, 0, 0, 1);
        }
    }
    if (g_watch) {
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
        float x, y;
        int cx = (int8_t)ld8(a + f.cursor) + 1, cy = ld8(a + f.cursor + 1);
        if (cx >= 0 && cx < 3 && cy < 2 && project(kMailSlots[cy][cx], x, y)) g_marks.insert(g_marks.end(), {x, y});
        for (int q = 0; q < 6; q++)
            if (project(kMailSlots[q / 3][q % 3], x, y)) g_marks.insert(g_marks.end(), {x, y});
    }
}

// ==== Spectacle Island's cannon game (daCanon_c::game_proc): the barrel's shape_angle y (left / right:
// home +-0x1200) and x (from upright: home .. home + 0x1000, more = lower / nearer); PadMove turns
// it towards what the stick asks, so angles set here stay. A finger drag turns it as the stick
// would (right: right, up: up / farther); a tap (no drag) fires (A).
constexpr uint32_t kHomeAngle = 0x2F8, kShapeAngle = 0x328;  // fopAc_ac_c home.angle, shape_angle
constexpr float kYawPerUnit = 0x2400 / 900.0f, kPitchPerUnit = 0x1000 / 500.0f;

bool cannon_playing() { return minigame_type() == 3; }

struct CannonGrip {
    bool down = false, moved = false;
    float x0 = 0, y0 = 0;
    int16_t yaw0 = 0, pitch0 = 0;
};
CannonGrip g_cannon;

void cannon_step(uint32_t a) {
    for (const Touch& t : take_touches()) {
        if (t.action == 0) {
            g_cannon = {};
            g_cannon.down = true;
            g_cannon.x0 = t.x;
            g_cannon.y0 = t.y;
            g_cannon.pitch0 = (int16_t)ld16(a + kShapeAngle);
            g_cannon.yaw0 = (int16_t)ld16(a + kShapeAngle + 2);
            continue;
        }
        if (!g_cannon.down || std::isnan(t.x)) {
            g_cannon.down = false;
            continue;
        }
        float dx = t.x - g_cannon.x0, dy = t.y - g_cannon.y0;
        if (std::hypot(dx, dy) > 12) g_cannon.moved = true;
        if (g_cannon.moved) {
            int16_t hy = (int16_t)ld16(a + kHomeAngle + 2), hx = (int16_t)ld16(a + kHomeAngle);
            int yaw = std::clamp(g_cannon.yaw0 - (int)(dx * kYawPerUnit), hy - 0x1200, hy + 0x1200);
            int pitch = std::clamp(g_cannon.pitch0 - (int)(dy * kPitchPerUnit), (int)hx, hx + 0x1000);
            st16(a + kShapeAngle + 2, (uint16_t)(int16_t)yaw);
            st16(a + kShapeAngle, (uint16_t)(int16_t)pitch);
        }
        if (t.action == 2) {
            g_cannon.down = false;
            if (!g_cannon.moved && !pushing()) {
                push(input::kA, 0, 0, 2);
                push(0, 0, 0, 1);
            }
        }
    }
}

void executed(Kind k, uint32_t actor) {
    Seen& s = g_seen[k];
    if (s.actor != actor) {
        s.actor = actor;
        s.snapped = false;
    }
    s.step = now();
    watch(s);
}

}  // namespace

// debug (WWHD_MG_WATCH): the file WWHD_MG_PLACE names ("x y z angle" in it) puts Link there once and is
// deleted, to test a minigame without walking there; Link's place is logged every 3 s
void debug_place() {
    static const char* path = getenv("WWHD_MG_PLACE");
    uint32_t l = link_actor();
    if (!l || now() % 15) return;
    if (now() % 90 == 0)
        LOG("[mg] Link at %.0f %.0f %.0f angle %04X", ldf32(l + kPos), ldf32(l + kPos + 4), ldf32(l + kPos + 8), ld16(l + kAngle + 2));
    FILE* f = path ? fopen(path, "r") : nullptr;
    if (!f) return;
    float x, y, z;
    int angle = 0;
    int n = fscanf(f, "%f %f %f %x", &x, &y, &z, &angle);
    fclose(f);
    remove(path);
    if (n < 3) return;
    for (uint32_t at : {0x300u, (uint32_t)kPos}) {  // old and current place
        stf32(l + at, x);
        stf32(l + at + 4, y);
        stf32(l + at + 8, z);
    }
    if (n == 4) {
        st16(l + kAngle + 2, (uint16_t)angle);
        st16(l + 0x328 + 2, (uint16_t)angle);  // shape_angle
    }
    LOG("[mg] Link placed at %.0f %.0f %.0f", x, y, z);
}

// ==== a message's choices (hud_choice: the window's lines and the cursor): a tap on a line moves the
// cursor there (stick pushes, one line each) and takes it (A)
constexpr float kChoiceLine = 52;  // layout units between the lines (2 choices: cursor 26 over the centre)

int choice_line(const int32_t c[5], float y) {
    float first = c[3] + kChoiceLine * (c[1] - 1) / 2;
    return (int)std::lround((first - y) / kChoiceLine);
}

void choice_step() {
    int32_t c[5];
    hud_choice(c);
    for (const Touch& t : take_touches()) {
        if (t.action != 2 || std::isnan(t.x) || !c[0] || pushing()) continue;
        if (std::fabs(t.x - c[2]) > 240) continue;
        int to = choice_line(c, t.y);
        if (to < 0 || to >= c[1] || std::fabs(t.y - (c[3] + kChoiceLine * ((c[1] - 1) / 2.0f - to))) > kChoiceLine * 0.75f) continue;
        int from = std::clamp(choice_line(c, (float)c[4]), 0, c[1] - 1);
        for (int i = from; i != to; i += to > from ? 1 : -1) {
            push(0, 0, to > from ? -1.0f : 1.0f, 2);
            push(0, 0, 0, 2);
        }
        push(input::kA, 0, 0, 2);
        push(0, 0, 0, 2);
    }
    if (g_watch && c[0]) {
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
        g_marks.insert(g_marks.end(), {(float)c[2], (float)c[4]});
        for (int i = 0; i < c[1]; i++) g_marks.insert(g_marks.end(), {(float)c[2], c[3] + kChoiceLine * ((c[1] - 1) / 2.0f - i)});
    }
}

// ==== the message's number entry (the auction's bid): the finger moves like the stick, one push per
// 60 layout units (up / down: the digit, left / right: which digit); a tap takes the number (A)
constexpr float kNumberStep = 60;
struct NumberGrip {
    bool down = false, moved = false;
    float x = 0, y = 0;
};
NumberGrip g_number;

void number_step() {
    for (const Touch& t : take_touches()) {
        if (std::isnan(t.x)) {
            g_number.down = false;
            continue;
        }
        if (t.action == 0) {
            g_number = {true, false, t.x, t.y};
            continue;
        }
        if (!g_number.down) continue;
        float dx = t.x - g_number.x, dy = t.y - g_number.y;
        if (std::fabs(dx) >= kNumberStep || std::fabs(dy) >= kNumberStep) {
            g_number.moved = true;
            bool horizontal = std::fabs(dx) > std::fabs(dy);
            push(0, horizontal ? (dx > 0 ? 1.0f : -1.0f) : 0, horizontal ? 0 : (dy > 0 ? 1.0f : -1.0f), 2);
            push(0, 0, 0, 2);
            g_number.x = t.x;  // the next step counts from here
            g_number.y = t.y;
        }
        if (t.action == 2) {
            g_number.down = false;
            if (!g_number.moved && !pushing()) {
                push(input::kA, 0, 0, 2);
                push(0, 0, 0, 2);
            }
        }
    }
}

// ==== shops (Beedle, Zunari, ...): dShop_now_triggercheck runs while the stick picks an item; its
// ShopItems_c (GameCube layout: mSelectedItemIdx s16 +0, the items' process ids +4 [8]) and the shop
// items themselves (daShopItem_c, their process id at +4 and place). A tap on an item steers the stick
// towards it (one push at a time, by where it is on the screen) until it is the chosen one; a tap on
// the chosen one is A (buy: the game asks).
constexpr uint32_t kProcId = 0x4;  // base_process_class::mBsPcId
uint32_t g_shop = 0;               // the ShopItems_c
uint64_t g_shop_step = 0;
struct ShopItem {
    uint32_t actor = 0, id = 0;
    uint64_t step = 0;
};
ShopItem g_shop_items[16];
int g_shop_target = -1, g_shop_pushes = 0;

bool shop_open() { return g_shop && g_shop_step + 3 >= now(); }

// the shop item actor of slot i, or 0
uint32_t shop_item(int i) {
    uint32_t id = ld32(g_shop + 4 + 4 * i);
    for (const ShopItem& it : g_shop_items)
        if (it.actor && it.id == id && it.step + 3 >= now()) return it.actor;
    return 0;
}

bool shop_item_xy(int i, float& x, float& y) {
    uint32_t a = shop_item(i);
    return a && project(ldv(a + kPos), x, y);
}

void shop_step() {
    int sel = (int16_t)ld16(g_shop);
    for (const Touch& t : take_touches()) {
        if (t.action != 2 || std::isnan(t.x)) continue;
        int best = -1;
        float bd = 90;  // layout units around the item
        for (int i = 0; i < 8; i++) {
            float x, y;
            if (shop_item_xy(i, x, y) && std::hypot(x - t.x, y - t.y) < bd) {
                bd = std::hypot(x - t.x, y - t.y);
                best = i;
            }
        }
        if (best < 0 || pushing()) continue;
        if (best == sel) {
            push(input::kA, 0, 0, 2);
            push(0, 0, 0, 2);
        } else {
            g_shop_target = best;
            g_shop_pushes = 0;
        }
    }
    // steering: one push towards the target, then wait for the game to take it
    if (g_shop_target >= 0 && !pushing()) {
        float sx, sy, tx, ty;
        if (sel == g_shop_target || g_shop_pushes >= 10 || !shop_item_xy(g_shop_target, tx, ty)) {
            g_shop_target = -1;
        } else {
            if (sel < 0 || !shop_item_xy(sel, sx, sy)) sx = sy = 0;  // nothing chosen yet: from the middle
            float dx = tx - sx, dy = ty - sy;
            bool horizontal = std::fabs(dx) >= std::fabs(dy);
            push(0, horizontal ? (dx > 0 ? 1.0f : -1.0f) : 0, horizontal ? 0 : (dy > 0 ? 1.0f : -1.0f), 2);
            push(0, 0, 0, 3);
            g_shop_pushes++;
        }
    }
    if (g_watch) {
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
        float x, y;
        if (sel >= 0 && shop_item_xy(sel, x, y)) g_marks.insert(g_marks.end(), {x, y});
        for (int i = 0; i < 8; i++)
            if (shop_item_xy(i, x, y)) g_marks.insert(g_marks.end(), {x, y});
    }
}

// ==== looking at a figure (the Nintendo Gallery's, daObjFigure_c::eventMesSet: A Info, B Cancel forced):
// the stick turns it, the C stick's up / down zooms. A finger held on the picture is a stick around where
// it came down (120 layout units = full); two fingers pinching are the C stick (wider = nearer)
uint32_t act_base() { return release::id() == release::Id::JPN ? 0x5BB1 : 0x5BB5; }
bool figure_view() {
    return ld8(kPlay + act_base() + 2 + 3) == 0x21 && ld8(kPlay + act_base() + 1 + 3) == 0x27;  // A Info, B Cancel
}
struct Hold {
    bool on = false;
    float lx = 0, ly = 0, rx = 0, ry = 0;
};
std::mutex g_hold_mu;
Hold g_hold;
struct FigureGrip {
    bool down = false;
    float x0 = 0, y0 = 0;
};
FigureGrip g_figure;

void figure_step() {
    for (const Touch& t : take_touches()) {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        if (t.action == 0) {
            g_figure = {true, t.x, t.y};
            g_hold = {};
        } else if (t.action == 1 && g_figure.down) {
            g_hold.on = true;
            g_hold.lx = std::clamp((t.x - g_figure.x0) / 120.0f, -1.0f, 1.0f);
            g_hold.ly = std::clamp((t.y - g_figure.y0) / 120.0f, -1.0f, 1.0f);
        } else if (t.action == 2) {
            g_figure.down = false;
            g_hold = {};
        } else if (t.action == 3) {  // pinch: x = the fingers' distance now / when the second came down
            g_hold.on = true;
            g_hold.lx = g_hold.ly = 0;
            g_hold.ry = std::clamp((t.x - 1.0f) * 3.0f, -1.0f, 1.0f);
        } else if (t.action == 4) {  // pinch over
            g_hold = {};
            g_figure.down = false;
        }
    }
}

// ==== conducting with the Wind Waker (Link's mode 1 bit 0; daPy_lk_c::procTactWait): the stick held left /
// right is the beat (3/4, 6/4; back in the middle 4/4), the C stick swings the baton (the notes). Each
// finger is a stick around where it came down (120 layout units = full): one that came down on the
// picture's left half the stick, on the right half the C stick; both can be held at once (actions + 10:
// the second finger)
bool conducting() { return ld32(kPlay + 0x5CDC) & 1; }
struct StickGrip {
    bool down = false, c_stick = false;
    float x0 = 0, y0 = 0, x = 0, y = 0;
};
StickGrip g_conduct[2];

void conduct_step() {
    for (const Touch& t : take_touches()) {
        StickGrip& g = g_conduct[t.action >= 10 ? 1 : 0];
        int action = t.action % 10;
        if (action == 0) g = {true, t.x > 0, t.x, t.y, t.x, t.y};
        else if (action == 1 && g.down) g.x = t.x, g.y = t.y;
        else if (action == 2) g.down = false;
    }
    std::lock_guard<std::mutex> lk(g_hold_mu);
    g_hold = {};
    for (const StickGrip& g : g_conduct) {
        if (!g.down) continue;
        float sx = std::clamp((g.x - g.x0) / 120.0f, -1.0f, 1.0f), sy = std::clamp((g.y - g.y0) / 120.0f, -1.0f, 1.0f);
        g_hold.on = true;
        (g.c_stick ? g_hold.rx : g_hold.lx) = sx;
        (g.c_stick ? g_hold.ry : g_hold.ly) = sy;
    }
}

bool choice_shown() {
    int32_t c[5];
    hud_choice(c);
    return c[0] && c[1] >= 2;
}

void minigame_step() {
    if (g_watch) debug_place();
    static int last_mode = 0;
    int mode = minigame_mode();
    if (mode != last_mode) {  // touches from before don't carry over
        if (g_watch) LOG("[mg] mode %d -> %d (minigame type %u)", last_mode, mode, minigame_type());
        last_mode = mode;
        if (g_wind_finger.exchange(false)) input::set_touch(false, 0.5f, 0.5f);  // (the compass closed under it)
        take_touches();
        g_grip = {};
        g_conduct[0] = g_conduct[1] = {};
        std::lock_guard<std::mutex> lk(g_mark_mu);
        g_marks.clear();
    }
    if (mode == 6) choice_step();
    if (mode == 7) number_step();
    if (mode == 8) shop_step();
    if (mode == 9) figure_step();
    if (mode == 10) conduct_step();
    if (mode != 9 && mode != 10) {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        g_hold = {};
    }
    // the auction (minigame 5): a tap anywhere bids (A); the gauge rises with each press
    if (minigame_mode() == 5) {
        for (const Touch& t : take_touches())
            if (t.action == 0 && !pushing()) {
                push(input::kA, 0, 0, 1);
                push(0, 0, 0, 1);
            }
    }
    // the front push runs for its steps, then the next one
    std::lock_guard<std::mutex> lk(g_pad_mu);
    if (g_pushes.empty()) return;
    uint64_t t = now();
    if (!g_push_end) g_push_end = t + g_pushes.front().steps;
    if (t >= g_push_end) {
        g_pushes.pop_front();
        g_push_end = g_pushes.empty() ? 0 : t + g_pushes.front().steps;
    }
}

void minigame_pad(input::PadState& s) {
    {
        std::lock_guard<std::mutex> lk(g_hold_mu);
        if (g_hold.on) {
            s.lx = g_hold.lx;
            s.ly = g_hold.ly;
            s.rx = g_hold.rx;
            s.ry = g_hold.ry;
        }
    }
    std::lock_guard<std::mutex> lk(g_pad_mu);
    if (g_pushes.empty()) return;
    if (!minigame_mode()) {  // the minigame ended
        g_pushes.clear();
        g_push_end = 0;
        return;
    }
    const Push& p = g_pushes.front();
    s.buttons |= p.buttons;
    s.lx = p.lx;
    s.ly = p.ly;
}

bool wind_compass() {
    int32_t c[2];
    return hud_wind_compass(c);
}

int minigame_mode() {
    if (choice_shown()) return 6;
    if (wind_compass()) return 11;
    if (hud_number_entry()) return 7;
    if (shop_open()) return 8;
    if (figure_view()) return 9;
    if (conducting()) return 10;
    if (alive(kBoard) && board_playing(g_seen[kBoard].actor)) return 1;
    if ((alive(kMail) && mail_sorting(g_seen[kMail].actor, kBmsw)) || (alive(kMail2) && mail_sorting(g_seen[kMail2].actor, kBtsw))) return 3;
    if (alive(kCannon) && cannon_playing()) return 4;
    if (minigame_type() == 5) return 5;
    if (alive(kPuzzle) && ld8(g_seen[kPuzzle].actor + kPzState) == kPzPlaying) return 2;
    return 0;
}

// ==== the wind compass after the Wind's Requiem (mode 11): HD sets the wind by touch on the GamePad (ControlWind_00's
// B_ControlArea_00, 340 x 340 around the compass; the blue arrow follows the finger). The layout shows on the TV
// at the same layout position, so a finger on the TV's compass goes to the game as that GamePad touch. Only
// fingers that land on the compass (the game's OK button, ControlWindOK_00, isn't on the TV picture).
namespace {
void wind_touch(int action, float x, float y) {
    int32_t c[2];
    if (action == 0) {
        g_wind_finger = hud_wind_compass(c) && std::hypot(x - c[0], y - c[1]) < 190.0f;
        if (!g_wind_finger) return;
    } else if (!g_wind_finger || action != 1 && action != 2) {
        return;
    }
    if (action == 2) g_wind_finger = false;
    if (!std::isfinite(x) || !std::isfinite(y)) {  // cancelled
        input::set_touch(false, 0.5f, 0.5f);
        return;
    }
    // layout units (720 across the picture's height, +y up) -> the GamePad's 16:9 screen, 0..1 from the top left
    float u = std::fmin(std::fmax((x + 640.0f) / 1280.0f, 0.0f), 1.0f);
    float v = std::fmin(std::fmax((360.0f - y) / 720.0f, 0.0f), 1.0f);
    input::set_touch(action != 2, u, v);
}
}  // namespace

void minigame_touch(int action, float x, float y) {
    if (action <= 2 && minigame_mode() == 11) { wind_touch(action, x, y); return; }
    std::lock_guard<std::mutex> lk(g_touch_mu);
    if (g_touches.size() < 64) g_touches.push_back({action, x, y});
}

int minigame_marks(float* xy) {
    if (!g_watch || !minigame_mode()) return 0;
    std::lock_guard<std::mutex> lk(g_mark_mu);
    int n = (int)std::min<size_t>(g_marks.size() / 2, kMinigameMarks);
    memcpy(xy, g_marks.data(), 2 * n * sizeof(float));
    return n;
}

}  // namespace mods

using namespace mods;

extern "C" void hook_021C5634(Cpu* c) {
    uint32_t a = c->r[3];
    executed(kBoard, a);
    take_view();
    if (board_playing(a)) board_step(a);
    f_021C5634_orig(c);
}
extern "C" void hook_02318640(Cpu* c) {
    // (a room has several puzzles: the one being played is followed)
    uint32_t a = c->r[3];
    bool playing = ld8(a + kPzState) == kPzPlaying;
    const Seen& s = g_seen[kPuzzle];
    if (playing || s.actor == a || !alive(kPuzzle) || ld8(s.actor + kPzState) != kPzPlaying) executed(kPuzzle, a);
    if (playing) {
        take_view();
        puzzle_step(a);
    }
    f_02318640_orig(c);
}
extern "C" void hook_0220EFF0(Cpu* c) {
    uint32_t a = c->r[3];
    executed(kMail, a);
    take_view();
    if (mail_sorting(a, kBmsw)) mail_step(a, kBmsw);
    f_0220EFF0_orig(c);
}
extern "C" void hook_022197C4(Cpu* c) {
    uint32_t a = c->r[3];
    executed(kMail2, a);
    take_view();
    if (mail_sorting(a, kBtsw)) mail_step(a, kBtsw);
    f_022197C4_orig(c);
}
extern "C" void hook_02109AF8(Cpu* c) {
    uint32_t a = c->r[3];
    executed(kCannon, a);
    if (cannon_playing()) cannon_step(a);
    f_02109AF8_orig(c);
}

// a shop's item choice: its ShopItems_c (r4) and the camera of this step
extern "C" void hook_025BB698(Cpu* c) {
    if (!g_shop_step || g_shop != c->r[4]) LOG("[mg] shop items at %08X", c->r[4]);
    g_shop = c->r[4];
    g_shop_step = now();
    take_view();
    f_025BB698_orig(c);
}
extern "C" void hook_02483C0C(Cpu* c) {
    uint32_t a = c->r[3], id = ld32(a + kProcId);
    ShopItem* slot = nullptr;
    for (ShopItem& it : g_shop_items)
        if (it.actor == a || (!slot && (!it.actor || it.step + 30 < now()))) slot = &it;
    for (ShopItem& it : g_shop_items)
        if (it.actor == a) slot = &it;
    if (slot) *slot = {a, id, now()};
    f_02483C0C_orig(c);
}
