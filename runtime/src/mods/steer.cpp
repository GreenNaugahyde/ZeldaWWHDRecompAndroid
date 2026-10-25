// Walking direction (Controls menu): turning the camera while Link walks doesn't turn him.
//
// The game moves Link in the direction the left stick points as seen from the camera, and works it
// out again every step (daPy_lk_c::setStickData, GameCube 80105A38, USA 023F32E4):
//   m34DC (+0x6924) = stick angle + 0x8000
//   m34E8 (+0x6930) = m34DC + dCam_getControledAngleY(camera)   <- the direction he turns to and walks
// so while the stick is held, every turn of the camera turns Link with it. The mod keeps the
// direction the stick was pushed in: from the moment the stick leaves the centre, camera turns are
// added up and taken off again, i.e. the stick works as if the camera were still where it was when
// the push began.
//
// Re-sync: the kept frame lasts only while the stick stays where it was. Once the stick moves more
// than 20 degrees (WWHD_STEER_RESYNC_DEG; mode 2: pref steer_resync, 1..20, default 1) from where it was when the player last turned the camera
// (or when the push began; the swing below doesn't count), the turns
// are dropped and the stick is relative to the camera as it is now (the player is steering again,
// by what they see). Letting go of the stick (the game's 0.05 walking threshold) ends it too. Link
// turns to the new direction at his own turning speed. With the stick held still, half a second
// after the camera stopped turning, the kept angle is eased away over about a second (Link curves
// to the camera-relative direction; the player chose this over keeping it until the stick moves).
//
// Modes (pref steer_mode): 0 off (the game's own), 1 only turns the player makes (right stick, swipe,
// mouse, the swing below: dCamera_c::manualCamera ran in the last few steps, which covers its
// ease-out), the camera's own following still turns him as in the original; 2 every camera turn,
// also the following camera.
//
// Camera swings behind Link (pref steer_swing, with mode 1 or 2): while he runs (stick held for a
// moment, more than half way) and the right stick has been left alone for a while (pref
// steer_swing_delay, default 1 s), the camera is
// turned towards his back through the game's own right-stick camera: filter_pad holds the right stick
// at x 0.45 (above manualCamera's 0.3, below the 0.5 where the stick counts as a button, as the mouse
// camera does) and manual_speed sets the turning speed: 8% of the angle still off per step, 0.5 to 4
// degrees (15..120 degrees/s), so it eases in at the end. When he runs towards the camera (more
// than 135 degrees off) it waits until he has done so for that long too (a short turn-around doesn't
// spin the view), and near 180 degrees it keeps the way it began turning. Which way a right-stick
// push turns the camera angle depends on the game's camera settings (manualCamera negates the turn
// for one, 02510468), so the sign is learned: if the camera keeps turning away from where it was
// pushed, the sign flips (logged). Those turns count as the player's, so Link keeps his direction
// while the camera swings, and pushing up afterwards (the stick moved: re-sync) walks on into the
// picture.
//
// Left as the game does it: events, Z-targeting (ATN procs: strafing must stay relative to the
// locked camera), sailing (IN_SHIP: the boat reads the stick itself) and the bow minigame (its own
// branch, not hooked).
//
// Sites (tools/recomp/hooks_mods.txt): @023F35BC and @023F3610, the two `sth r9, 0x6930(r31)` of
// the normal and the confused branch; r9 = m34DC + camera angle, r3 = camera angle, r10 = m34DC.
// WWHD_STEER_LOG=1 logs camera, Link's facing and the kept direction every 15 steps while it's active.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "input.h"
#include "mods.h"
#include "runtime.h"

namespace mods {
namespace {
constexpr uint32_t kCurProc = 0x65F0;      // daPy_lk_c::mCurProc
constexpr uint32_t kModeFlg = 0x6A70, kModeInShip = 0x2000;
constexpr uint32_t kStickDistance = 0x6A08;  // mStickDistance (already stored at the sites)
constexpr uint32_t kAngleY = 0x322;          // the way Link faces
constexpr float kDeg = 180.0f / 32768.0f;

std::atomic<int> g_mode{0};
std::atomic<bool> g_swing{false};
std::atomic<int> g_resync2{1};  // mode 2: degrees the stick moves before it re-syncs (pref steer_resync)
std::atomic<uint64_t> g_swing_wait{30};  // steps before the swing starts (pref steer_swing_delay)
bool g_hold = false;     // the stick is pushed and the camera turns are being kept off
int16_t g_prev_cam = 0;  // camera angle at the last step
int16_t g_offset = 0;    // camera turns since the push that are taken off
int16_t g_anchor = 0;    // stick angle when the camera last turned (or at the push)
uint64_t g_hold_from = 0;  // step the push began

// shared with the pad filter (input read)
std::atomic<uint64_t> g_active_step{~0ull};  // last step steer() ran active
std::atomic<int> g_cam{0}, g_face{0};
std::atomic<float> g_dist{0};
std::atomic<int> g_swing_sign{-1};  // right-stick x sign that turns the camera angle up (-1 measured on a Snapdragon 7+ Gen 3)
std::atomic<int> g_swing_want{0};   // the way the swing is turning the camera angle (+1 / -1, 0 none)
std::atomic<uint64_t> g_swing_step{~0ull};
std::atomic<float> g_swing_rate{0};  // degrees per step, signed like a right-stick x
int g_wrong = 0;  // steps the camera turned against the swing
uint64_t g_far_since = ~0ull;  // input step he began running towards the camera (swing waits a second)

bool targeting(uint32_t proc) { return proc >= 0x07 && proc <= 0x0A; }  // ATN_MOVE .. SIDE_STEP
}  // namespace

int steer_mode() { return g_mode.load(std::memory_order_relaxed); }
void set_steer_mode(int mode) {
    g_mode = std::clamp(mode, 0, 2);
    static const char* const kName[3] = {"as the game (follows the camera)", "kept while the player turns the camera",
                                         "kept while the stick is held"};
    LOG("[mods] walking direction: %s", kName[g_mode.load()]);
}
bool steer_swing() { return g_swing.load(std::memory_order_relaxed); }
void set_steer_swing_delay(int ms) {
    g_swing_wait = (uint64_t)((std::clamp(ms, 0, 10000) * 30 + 500) / 1000);  // logic steps, rounded
    LOG("[mods] camera swing starts after %d ms", ms);
}
void set_steer_resync(int deg) {
    g_resync2 = std::clamp(deg, 1, 20);
    LOG("[mods] walking direction re-syncs after %d degrees of stick movement", g_resync2.load());
}
void set_steer_swing(bool on) {
    g_swing = on;
    LOG("[mods] camera swings behind Link: %s", on ? "on" : "off");
}

static void steer(Cpu* c) {
    const uint32_t link = c->r[31];
    const int16_t cam = (int16_t)c->r[3];
    const int16_t stick = (int16_t)c->r[10];
    const int mode = g_mode.load(std::memory_order_relaxed);
    const float dist = u32_as_f32(ld32(link + kStickDistance));
    const bool active = mode != 0 && dist > 0.05f && event_mode_now() == 0 &&
                        !(ld32(link + kModeFlg) & kModeInShip) && !targeting(ld32(link + kCurProc));
    if (!active) {
        g_hold = false;
        g_offset = 0;
        return;
    }
    const uint64_t now = step();
    if (!g_hold) {
        g_hold = true;
        g_prev_cam = cam;
        g_offset = 0;
        g_anchor = stick;
        g_hold_from = now;
    }
    const int16_t turn = (int16_t)(cam - g_prev_cam);
    g_prev_cam = cam;
    const bool by_player = camera_turned_by_player();
    static uint64_t turned_step = 0;  // last step a counted camera turn happened
    if (mode == 2 || by_player) {
        g_offset = (int16_t)(g_offset + turn);
        if (std::abs(turn) > 8) turned_step = now;  // more than jitter (0.04 degrees)
    }
    // eased back: half a second after the camera stopped turning, the kept angle shrinks by a tenth
    // per step (at least 2 degrees), about a second, so Link curves to the camera-relative direction
    if (g_offset != 0 && now > turned_step + 15) {
        const int mag = std::abs((int)g_offset), d = std::max(mag / 10, (int)(2.0f / kDeg));
        g_offset = mag <= d ? 0 : (int16_t)(g_offset > 0 ? g_offset - d : g_offset + d);
    }
    // mode 2 re-syncs after the player's setting (default 1 degree): every camera turn is kept
    // there, so steering by the stick should snap back to the camera's view quickly
    static const int16_t resync1 = (int16_t)(std::clamp(getenv("WWHD_STEER_RESYNC_DEG") ? atof(getenv("WWHD_STEER_RESYNC_DEG")) : 20.0, 1.0, 180.0) / kDeg);
    const int16_t resync = mode == 2 ? (int16_t)(g_resync2.load(std::memory_order_relaxed) / kDeg) : resync1;
    static const bool log = getenv("WWHD_STEER_LOG") != nullptr;
    // the swing's turns don't move the anchor: steering during or after a swing re-syncs (the swing
    // keeps chasing his facing, so it would never end while he turns)
    const uint64_t sw = g_swing_step.load(std::memory_order_relaxed);
    const bool swinging = sw != ~0ull && now <= sw + 8;
    if (by_player && turn != 0 && !swinging) {
        g_anchor = stick;
    } else if (std::abs((int16_t)(stick - g_anchor)) > resync) {
        if (log && g_offset != 0) LOG("[steer] the stick moved: relative to the camera again (dropped %.1f)", g_offset * kDeg);
        g_offset = 0;
        g_anchor = stick;
    }
    // the swing's sign: the camera should turn the way it was pushed
    const int want = g_swing_want.load(std::memory_order_relaxed);
    const uint64_t ss = g_swing_step.load(std::memory_order_relaxed);
    if (want != 0 && ss != ~0ull && now <= ss + 2 && std::abs(turn) > 16) {
        if ((turn > 0 ? 1 : -1) != want) {
            if (++g_wrong >= 4) {
                g_swing_sign = -g_swing_sign.load();
                g_wrong = 0;
                LOG("[steer] the camera turned against the swing: right stick sign now %+d", g_swing_sign.load());
            }
        } else {
            g_wrong = 0;
        }
    }
    g_cam.store(cam, std::memory_order_relaxed);
    g_face.store((int16_t)ld16(link + kAngleY), std::memory_order_relaxed);
    g_dist.store(now >= g_hold_from + 10 ? dist : 0.0f, std::memory_order_relaxed);  // running for a moment
    g_active_step.store(now, std::memory_order_relaxed);
    static uint64_t n = 0;
    if (log && (n++ % 15) == 0) {
        LOG("[steer] camera %.1f, turned %.1f since the push; Link faces %.1f, walks to %.1f (game: %.1f); stick %.1f",
            cam * kDeg, g_offset * kDeg, (int16_t)ld16(link + kAngleY) * kDeg, (int16_t)(stick + cam - g_offset) * kDeg,
            (int16_t)(stick + cam) * kDeg, (int16_t)(stick - 0x8000) * kDeg);
    }
    if (g_offset == 0) return;
    c->r[9] = (uint32_t)(int32_t)(int16_t)(stick + cam - g_offset);
}

// input read (camera.cpp filter_pad, when the mouse isn't driving the right stick): the swing
void steer_pad(input::PadState& s, bool looking) {
    static uint64_t user_step = 0;  // last step the player used the right stick
    const uint64_t now = step();
    if (std::fabs(s.rx) > 0.2f || std::fabs(s.ry) > 0.2f) user_step = now;
    g_swing_want.store(0, std::memory_order_relaxed);
    const uint64_t act = g_active_step.load(std::memory_order_relaxed);
    if (!g_swing.load(std::memory_order_relaxed) || g_mode.load(std::memory_order_relaxed) == 0 || looking ||
        act == ~0ull || now > act + 1 || now < user_step + g_swing_wait.load(std::memory_order_relaxed) || g_dist.load(std::memory_order_relaxed) < 0.5f) {
        g_far_since = ~0ull;
        return;
    }
    // behind him: the camera angle equals the way he faces (stick up walks along the camera angle;
    // measured: the stick's raw angle for up is 0x8000, so m34DC = 0 and m34E8 = camera angle)
    const int16_t err = (int16_t)(g_face.load(std::memory_order_relaxed) - g_cam.load(std::memory_order_relaxed));
    const float deg = err * kDeg;
    static int last_want = 0;
    static uint64_t last_step = 0;
    if (now > last_step + 2) last_want = 0;  // the last swing ended
    if (std::fabs(deg) <= 135.0f) g_far_since = ~0ull;
    else if (g_far_since == ~0ull) g_far_since = now;
    if (std::fabs(deg) < 8.0f || (g_far_since != ~0ull && now < g_far_since + g_swing_wait.load(std::memory_order_relaxed))) return;
    int want = deg > 0 ? 1 : -1;
    if (std::fabs(deg) > 150.0f && last_want != 0) want = last_want;  // no flip-flopping around 180
    last_want = want;
    last_step = now;
    const float off = want * deg < 0 ? 360.0f - std::fabs(deg) : std::fabs(deg);  // the way it turns
    s.rx = 0.45f * want * g_swing_sign.load(std::memory_order_relaxed);
    s.ry = 0.0f;
    g_swing_rate.store(std::clamp(off * 0.08f, 0.5f, 4.0f) * want * g_swing_sign.load(std::memory_order_relaxed),
                       std::memory_order_relaxed);
    g_swing_want.store(want, std::memory_order_relaxed);
    g_swing_step.store(now, std::memory_order_relaxed);
}

bool steer_swing_turn(float& deg) {
    const uint64_t ss = g_swing_step.load(std::memory_order_relaxed);
    if (g_swing_want.load(std::memory_order_relaxed) == 0 || ss == ~0ull || step() > ss + 1) return false;
    deg = g_swing_rate.load(std::memory_order_relaxed);
    return true;
}

}  // namespace mods

extern "C" void site_023F35BC(Cpu* c) { mods::steer(c); }
extern "C" void site_023F3610(Cpu* c) { mods::steer(c); }
