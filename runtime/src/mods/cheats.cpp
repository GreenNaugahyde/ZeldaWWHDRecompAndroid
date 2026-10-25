// Cheats (Gameplay menu): edits of the live save data, applied by the game's main thread at the top
// of the frame (hook_0203593C in interp.cpp), like the save states. One-shot cheats run once per
// click; the infinite health / magic / ammo switches top up every frame.
//
// The save data is dSv_info_c of the GameCube decompilation (zeldaret/tww include/d/d_save.h),
// unchanged in WWHD as far as used here. Each cheat does what the game's own item_func_*
// (d_item.cpp) does for that item. Resolve the live area through the mapped save global;
// heap allocation addresses are not part of the executable address map. (This port: the two
// addresses are release::Data, resolved when used: the game version is only known once it loads;
// the app's menu shows the confirmations, there is no on-screen notice.)
//
// Note: the game itself takes the sword away in Forsaken Fortress (first visit) and the bow in the
// Tower of the Gods when those stages load (d_s_play.cpp), cheat or not.
//
// Moon jump (a switch like the infinite ones, our own: the GameCube Gecko/Action Replay codes don't
// apply to WWHD): while L is held Link rises and the left stick steers him; on release gravity takes
// over again (moon_jump_step). WWHD_MOON_JUMP_SPEED / _LIFT / _FLY override rise, lift-off and flight
// speed.
//
// Test switch: WWHD_CHEAT=items,sword,stats,songs,triforce,dungeon,key applies those once a save
// file is loaded; WWHD_CHEAT_INFINITE=health,magic,ammo,breath,moonjump switches those on.
// WWHD_CHEAT_SAVE_ADDR=hex overrides the address (another game version).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "mods.h"
#include "interp.h"
#include "true60.h"
#include "release.h"
#include "runtime.h"

namespace mods {
namespace {
// dSv_player_c offsets
constexpr uint32_t kMaxLife = 0x00, kLife = 0x02, kRupee = 0x04;  // u16, life in quarter hearts
constexpr uint32_t kSelectEquip = 0x0E;  // u8[4]: sword, shield, bracelet
constexpr uint32_t kWallet = 0x12, kMaxMagic = 0x13, kMagic = 0x14;
constexpr uint32_t kReturnName = 0x30;  // char[8] stage
constexpr uint32_t kItems = 0x3C;       // u8[21] item number per inventory slot (0xFF empty)
constexpr uint32_t kGetItems = 0x51;    // u8[21] "got" bits per slot (bow: 1 fire/ice, 2 light)
constexpr uint32_t kArrowNum = 0x69, kBombNum = 0x6A, kArrowMax = 0x6F, kBombMax = 0x70;
constexpr uint32_t kCollect = 0xB4;     // u8[8]: [0] swords, [1] shields, [2] bracelets
constexpr uint32_t kTact = 0xBD, kTriforce = 0xBE;  // song bits (6), Triforce shard bits (8)
// the bags (dSv_player_bag_item_c / _get_bag_item_c / _bag_item_record_c): the items in the spoils /
// bait / delivery bag (u8[8] each), their "got" flags (beast u8 at +0x94, bait +0x95) and counts
// (spoils by dBeastIdx, bait per slot); X / Y / R select items (inventory slots, bait 36..43)
constexpr uint32_t kBeast = 0x76, kBait = 0x7E, kBeastFlags = 0x94, kBaitFlags = 0x95, kBeastNum = 0x9C, kBaitNum = 0xA4;
constexpr uint32_t kSelectItems = 0x09;
constexpr uint8_t kBirdBait = 0x82, kHyoiPear = 0x83;
// the spoils by dBeastIdx: Skull Necklace, Boko Baba Seed, Golden Feather, Knight's Crest, Red / Green
// / Blue Chu Jelly, Joy Pendant
constexpr uint8_t kSpoils[8] = {0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x1F};
// dSv_info_c::mMemory: the current stage's dSv_memBit_c (copied back to its save table on leaving)
constexpr uint32_t kStageKeys = 0x778 + 0x20, kStageDungeonItems = 0x778 + 0x21;  // bits: map, compass, boss key

const release::Data kSaveInfo{0x101F84DC}, kPlay{0x1046F0B0};
// the swim meter (dComIfG_play_c::mAirMeter, GameCube play + 0x48C4; its max follows): set to 900 when
// Link enters deep water (daPy_lk_c::setSwimTimerStartStop, 023F900C), counted down by the meter while he
// swims; at 0 he sinks and the room restarts (procSwimWait). Only the swimming uses it.
uint32_t air_meter() { return kPlay + (release::id() == release::Id::JPN ? 0x5B48 : 0x5B4C); }
// rupees still to be added or taken (dComIfG_play_c::mItemRupeeCount, GameCube play + 0x48C0, right
// before the swim meter; a shop does `-= price`, e.g. 02262AE8 USA). The HUD's rupee counter
// (dMeter_rupyMove) moves it into the save and counts its shown number towards the new total.
uint32_t rupee_change() { return air_meter() - 4; }

uint32_t save_addr() {
    const char* override_addr = getenv("WWHD_CHEAT_SAVE_ADDR");
    return override_addr ? (uint32_t)strtoul(override_addr, nullptr, 16) : ld32(kSaveInfo) + 0x20;
}

// current stage, as in savestate.cpp
std::string stage() {
    const char* p = (const char*)mem::ptr(kPlay + 0x5134);
    return std::string(p, strnlen(p, 8));
}

// false on the title screen (it has placeholder save data that a file load replaces), before a file
// is loaded, or if the address is wrong
bool save_loaded(uint32_t s) {
    std::string st = stage();
    if (st.empty() || st == "sea_T" || st == "Name") return false;  // title, file select
    uint16_t max = ld16(s + kMaxLife), life = ld16(s + kLife);
    if (max < 12 || max > 80 || life > max || ld16(s + kRupee) > 5000 || ld8(s + kWallet) > 2) return false;
    // the return stage: a NUL-terminated name in 8 bytes, at least 3 characters ("sea", "sea_T", "M_NewD2")
    int n = 0;
    for (; n < 8; n++) {
        uint8_t ch = ld8(s + kReturnName + n);
        if (!ch) break;
        if (ch < '0' || ch > 'z') return false;
    }
    return n >= 3;
}

void give(uint32_t s, int slot, uint8_t item, uint8_t got_bits = 1) {
    st8(s + kItems + slot, item);
    st8(s + kGetItems + slot, ld8(s + kGetItems + slot) | got_bits);
}

void all_items(uint32_t s) {
    give(s, 0, 0x20);         // telescope
    give(s, 1, 0x78);         // sail
    give(s, 2, 0x22);         // Wind Waker (songs are not given: they drive story events)
    give(s, 3, 0x25);         // grappling hook
    give(s, 4, 0x24);         // spoils bag
    give(s, 5, 0x2D);         // boomerang
    give(s, 6, 0x34);         // Deku leaf
    give(s, 8, 0x26, 3);      // deluxe picto box
    give(s, 9, 0x29);         // iron boots
    give(s, 10, 0x2A);        // magic armor
    give(s, 11, 0x2C);        // bait bag
    give(s, 12, 0x36, 7);     // bow with fire/ice and light arrows
    give(s, 13, 0x31);        // bombs
    for (int b = 14; b < 18; b++)
        if (ld8(s + kItems + b) == 0xFF) st8(s + kItems + b, 0x50);  // empty bottles, contents kept
    give(s, 18, 0x30);        // delivery bag
    give(s, 19, 0x2F);        // hookshot
    give(s, 20, 0x33);        // skull hammer
    st8(s + kCollect + 2, ld8(s + kCollect + 2) | 1);  // power bracelets
    st8(s + kSelectEquip + 2, 0x28);
    st8(s + kArrowMax, 99);
    st8(s + kArrowNum, 99);
    st8(s + kBombMax, 99);
    st8(s + kBombNum, 99);
    if (ld8(s + kMaxMagic) < 16) st8(s + kMaxMagic, 16);  // the Deku leaf needs magic
    st8(s + kMagic, ld8(s + kMaxMagic));
}

void best_sword(uint32_t s) {
    // Sword ownership is story progress: bit 2 removes Medli from Dragon Roost and the
    // Earth Temple; bit 3 removes Makar from his earlier locations. Equip the upgrades
    // without claiming those story milestones. The game's equipment refresh restores
    // the earned equipment on reload, including a portable-state load.
    st8(s + kSelectEquip + 0, 0x3E);
    st8(s + kSelectEquip + 1, 0x3C);
}

void max_stats(uint32_t s) {
    st16(s + kMaxLife, 80);  // 20 hearts
    st16(s + kLife, 80);
    st8(s + kWallet, 2);     // 5000
    st16(s + kRupee, 5000);
    st8(s + kMaxMagic, 32);  // double magic
    st8(s + kMagic, 32);
}

// Fill the bags: both bags (if missing), every kind of spoils x99, the bait bag's empty slots with
// All-Purpose Bait (first half) and Hyoi Pears; counts as the game gives them (dSv_player_bag_item_c::
// setBaitItem: 3)
void fill_bags(uint32_t s) {
    if (ld8(s + kItems + 4) == 0xFF) give(s, 4, 0x24);
    if (ld8(s + kItems + 11) == 0xFF) give(s, 11, 0x2C);
    for (uint8_t item : kSpoils) {
        bool have = false;
        for (int k = 0; k < 8; k++) have |= ld8(s + kBeast + k) == item;
        for (int k = 0; k < 8 && !have; k++)
            if (ld8(s + kBeast + k) == 0xFF) {
                st8(s + kBeast + k, item);
                have = true;
            }
    }
    for (int i = 0; i < 8; i++) st8(s + kBeastNum + i, 99);
    st8(s + kBeastFlags, 0xFF);
    for (int k = 0; k < 8; k++)
        if (ld8(s + kBait + k) == 0xFF) {
            st8(s + kBait + k, k < 4 ? kBirdBait : kHyoiPear);
            st8(s + kBaitNum + k, 3);
        }
    st8(s + kBaitFlags, ld8(s + kBaitFlags) | 3);
    // a test of the ring's delivery bag icons (WWHD_DELIVERY_TEST, debug builds by intent extra): each
    // fill puts the next 8 of the bag's 19 items in it (letters, cards, flowers, flags, statues),
    // replacing what it held; story items, so only in a save for testing
    static const bool delivery_test = getenv("WWHD_DELIVERY_TEST") != nullptr;
    if (delivery_test) {
        static const uint8_t kDelivery[19] = {0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x8C, 0x8D, 0x8E,
                                              0x8F, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97};
        static int next = 0;
        for (int k = 0; k < 8; k++) st8(s + kBait + 8 + k, next + k < 19 ? kDelivery[next + k] : 0xFF);
        LOG("[cheats] delivery bag test: items %d..%d of 19", next + 1, std::min(next + 8, 19));
        next = next + 8 >= 19 ? 0 : next + 8;
    }
}

// Infinite bait and spoils, each frame: spoils counts stay at 99; bait that was used is put back. A
// Hyoi Pear (or the last bait) empties its slot and the button it was on (setBaitItemEmpty): both are
// restored, the button also in the play's copy of the item numbers (dComIfGp_setSelectItem; play +
// 0x5BBB, JPN 0x5BB7: GameCube play + 0x4933, next to the action bytes as in touch_state.cpp)
void keep_bags(uint32_t s) {
    static uint8_t prev_item[8], prev_num[8], prev_sel[3];
    static uint32_t prev_s = 0;
    for (int i = 0; i < 8; i++)
        if (ld8(s + kBeastNum + i) < 99) st8(s + kBeastNum + i, 99);
    const uint32_t copy = kPlay + (release::id() == release::Id::JPN ? 0x5BB7 : 0x5BBB);
    if (prev_s == s) {
        for (int k = 0; k < 8; k++) {
            if (prev_item[k] == 0xFF) continue;
            if (ld8(s + kBait + k) == 0xFF) st8(s + kBait + k, prev_item[k]);
            if (ld8(s + kBait + k) == prev_item[k] && ld8(s + kBaitNum + k) < prev_num[k]) st8(s + kBaitNum + k, prev_num[k]);
        }
        for (int b = 0; b < 3; b++) {
            const uint8_t was = prev_sel[b];
            if (was >= 36 && was < 44 && ld8(s + kSelectItems + b) == 0xFF && ld8(s + kBait + (was - 36)) != 0xFF) {
                st8(s + kSelectItems + b, was);
                st8(copy + b, ld8(s + kBait + (was - 36)));
            }
        }
    }
    prev_s = s;
    for (int k = 0; k < 8; k++) {
        prev_item[k] = ld8(s + kBait + k);
        prev_num[k] = ld8(s + kBaitNum + k);
    }
    for (int b = 0; b < 3; b++) prev_sel[b] = ld8(s + kSelectItems + b);
}

int parse_env(const char* var, std::initializer_list<std::pair<const char*, int>> names) {
    const char* e = getenv(var);
    int w = 0;
    for (auto& [n, bit] : names)
        if (e && strstr(e, n)) w |= bit;
    return w;
}

std::atomic<int> g_pending{parse_env("WWHD_CHEAT", {{"items", kCheatItems}, {"sword", kCheatSword}, {"stats", kCheatStats},
                                                    {"songs", kCheatSongs}, {"triforce", kCheatTriforce},
                                                    {"dungeon", kCheatDungeon}, {"key", kCheatKey}, {"bags", kCheatBags}})};
std::atomic<int> g_infinite{parse_env("WWHD_CHEAT_INFINITE",
                                      {{"health", kInfHealth}, {"magic", kInfMagic}, {"ammo", kInfAmmo}, {"moonjump", kMoonJump}, {"nofall", kMoonNoFall},
                                       {"allanims", kMoonAllAnims}, {"rupees", kInfRupees}, {"bags", kInfBags}, {"breath", kInfBreath}, {"onehit", kOneHit}})};
}  // namespace

uint32_t loaded_save() {
    uint32_t s = save_addr();
    return save_loaded(s) ? s : 0;
}

uint32_t save_info() { return save_addr(); }

void request_cheat(int which) { g_pending |= which; }
bool infinite(int which) { return g_infinite.load(std::memory_order_relaxed) & which; }
void set_infinite(int which, bool on) {
    if (on) g_infinite |= which;
    else g_infinite &= ~which;
    LOG("[cheats] %s %s",
        which == kInfHealth  ? "infinite health"
        : which == kInfMagic ? "infinite magic"
        : which == kInfAmmo  ? "infinite ammo"
        : which == kMoonJump ? "moon jump"
        : which == kMoonNoFall ? "no fall damage after flying"
        : which == kInfRupees ? "infinite rupees"
        : which == kInfBags   ? "infinite bait and spoils"
        : which == kInfBreath ? "infinite swimming"
        : which == kOneHit    ? "one hit kills (enemies)"
                             : "random lift-off animations",
        on ? "on" : "off");
}

// Moon jump. fopAc_ac_c fields (WWHD: base fields +0x11C; docs/decomp-notes.md): current.angle at
// +0x320, shape_angle +0x328, speed +0x33C; daPy_lk_c::mModeFlg +0x6A70 (bit 2: in the air), mCurProc
// +0x65F0. Speeds per 30 Hz step, like the game's. While L (the shoulder button) is held:
//  - he's put into the game's fall, procFall_init (proc 0x27), also from swimming: on the ground his procedures ignore
//    speed.y and the ground snapping undoes a lift; the jump off a ledge (proc 0x24) moves him along
//    its own take-off line. His collision's ground flags are cleared too, or the fall lands at once;
//  - speed.y is set upward every step.
// Until he lands, also after L is let go, the left stick steers as when he runs: he turns towards the
// stick's direction relative to the camera at a limited rate, and the horizontal part of his
// `current.pos += speed` (true60_link.cpp, moon_jump_move) is the way he faces; stick released: no
// horizontal move. His angles are written before and after his step. The
// fall itself sets his speed and direction every step and pins his position, so neither speed fields
// nor a position written from outside steer him. The collision after the move still applies (walls).
// No fall damage after flying (a switch): his fall start height (+0x6A48: posMove stores the height at
// the top of a jump, 023FD38C; landing compares it with his height for the hard landing, 0241C8B8) is
// kept at his height before and after every step of the flight, until he lands.
constexpr uint32_t kAngleY = 0x320 + 2, kShapeAngleY = 0x328 + 2;
constexpr uint32_t kCurProc = 0x65F0, kProcFall = 0x27;  // daPy_lk_c::mCurProc, daPy_PROC_FALL
constexpr uint32_t kSpeedY = 0x33C + 4, kPosY = 0x314 + 4, kFallStartY = 0x6A48;
uint32_t g_move_link = 0;        // the step being steered (moon_jump_move), and its move
float g_move_x = 0, g_move_z = 0;
bool g_flight = false;           // a moon jump began and Link hasn't landed yet: the stick steers
int16_t g_face = 0;              // the way he faces and flies (the fall resets his angles every step)
bool g_face_set = false;
float env_float(const char* name, float def) {
    const char* e = getenv(name);
    return e ? (float)atof(e) : def;
}
std::atomic<int> g_rise_pct{100}, g_fly_pct{100};  // the menu's speeds (set_moon_jump_speeds)
float rise_speed() {
    static const float base = env_float("WWHD_MOON_JUMP_SPEED", 25.0f);
    return base * g_rise_pct.load(std::memory_order_relaxed) / 100.0f;
}
float fly_speed() {
    static const float base = env_float("WWHD_MOON_JUMP_FLY", 40.0f);
    return base * g_fly_pct.load(std::memory_order_relaxed) / 100.0f;
}
void face(uint32_t link) {
    st16(link + kAngleY, (uint16_t)g_face);
    st16(link + kShapeAngleY, (uint16_t)g_face);
}
constexpr uint32_t kModeFlg = 0x6A70, kModeMidair = 0x2, kModeSwim = 0x40000;
// daPy_lk_c::mAcch (dBgS_Acch) at +0x80C: its m_flags (+0x28) with GROUND_HIT 0x20, GROUND_FIND 0x40,
// GROUND_LANDING 0x80 (autoGroundHit, 023FE2F0, tests 0x20 there). Left set, the fall lands at once.
constexpr uint32_t kAcchFlags = 0x80C + 0x28, kAcchGround = 0x20 | 0x40 | 0x80;
const release::Code kFallInit{0x023F6564};
// daPy_lk_c::setSingleMoveAnime(anm r4, f32 rate f1, f32 start f2, s16 end r5, f32 morf f3)
const release::Code kSetAnime{0x023E0A04};
// the pose at each lift-off (each new L press), chosen at random: the two the user liked best (the
// game's animations for the jump off a ledge and its neighbour), or with the switch "random lift-off
// animations" any of the ones tried (from the airborne procedures and the Deku leaf ones)
const int kLiftAnims[] = {0x3C, 0x3D};
const int kAllLiftAnims[] = {0x0D, 0x3C, 0x3D, 0x3E, 0x30, 0xD3, 0x4C, 0x55, 0x56, 0x57, 0x5D, 0x5E, 0x5F, 0x60};
bool g_l_was = false;
uint32_t g_rand = 0x9E3779B9u;
int random_below(int n) {  // xorshift seeded by the clock; any spread will do for a pose
    static bool seeded = false;
    if (!seeded) {
        seeded = true;
        g_rand ^= (uint32_t)std::chrono::steady_clock::now().time_since_epoch().count() | 1u;
    }
    g_rand ^= g_rand << 13;
    g_rand ^= g_rand >> 17;
    g_rand ^= g_rand << 5;
    return (int)(g_rand % (uint32_t)n);
}  // daPy_lk_c::procFall_init(int, f32) (calls commonProcInit(0x27))
// the stick's horizontal move for this step (moon_jump_move uses it), as when he runs: the stick's
// direction relative to the camera, Link turns towards it at a limited rate and flies the way he faces
void moon_jump_steer(uint32_t link) {
    const float fly = fly_speed();
    static const int turn = (int)env_float("WWHD_MOON_JUMP_TURN", 3000.0f);  // per 30 Hz step (0x8000 = 180 degrees)
    if (!g_face_set) {
        g_face = (int16_t)ld16(link + kShapeAngleY);
        g_face_set = true;
    }
    float sx, sy;
    left_stick(sx, sy);
    const float mag = std::min(1.0f, std::sqrt(sx * sx + sy * sy));
    float vx = 0, vz = 0;
    uint32_t cam = interp::last_camera();
    if (mag >= 0.15f && cam) {
        // camera forward on the ground plane (center - eye); right = forward x up = (-fz, fx)
        float fx = (float)(ldf32(cam + 0xE8) - ldf32(cam + 0xDC)), fz = (float)(ldf32(cam + 0xF0) - ldf32(cam + 0xE4));
        const float fl = std::sqrt(fx * fx + fz * fz);
        if (fl > 1e-3f) {
            fx /= fl;
            fz /= fl;
            const float wx = fx * sy - fz * sx, wz = fz * sy + fx * sx;
            const int16_t target = (int16_t)std::lround(std::atan2(wx, wz) * (32768.0 / 3.14159265358979));
            const int step = std::max(1, (int)std::lround(turn * true60::dt()));
            const int16_t diff = (int16_t)(target - g_face);
            g_face = (int16_t)(g_face + std::clamp<int>(diff, -step, step));
            const float a = g_face * (3.14159265358979f / 32768.0f);
            vx = std::sin(a) * fly * mag;
            vz = std::cos(a) * fly * mag;
        }
    }
    face(link);
    g_move_link = link;
    g_move_x = vx;
    g_move_z = vz;
}

void moon_jump_step(Cpu* c, uint32_t link, bool before) {
    if (before) g_move_link = 0;
    if (!link || !(g_infinite.load(std::memory_order_relaxed) & kMoonJump)) {
        g_flight = g_face_set = false;
        return;
    }
    if (g_flight && (g_infinite.load(std::memory_order_relaxed) & kMoonNoFall))
        st32(link + kFallStartY, ld32(link + kPosY));  // no fall height: no hard landing
    const uint32_t mode = ld32(link + kModeFlg);
    if (!moon_held()) {
        if (before) g_l_was = false;
        // let go: gravity takes over, the stick still steers until he lands (or swims)
        if (g_flight && (!(mode & kModeMidair) || (mode & kModeSwim))) g_flight = g_face_set = false;
        if (g_flight) {
            if (before) moon_jump_steer(link);
            else if (g_face_set) face(link);
        }
        return;
    }
    const float rise = rise_speed();
    if (before) {
        // into the game's fall: from the ground, from swimming, and from the jump off a ledge (it
        // keeps its own take-off line)
        const bool press = !g_l_was;
        if (ld32(link + kCurProc) != kProcFall) {
            const Cpu saved = *c;
            c->f[1].ps0 = c->f[1].ps1 = 5.0;  // animation blend
            guest_call(c, kFallInit, {link, 0});
            *c = saved;
        }
        if (press) {
            const bool all = g_infinite.load(std::memory_order_relaxed) & kMoonAllAnims;
            const int anm = all ? kAllLiftAnims[random_below((int)(sizeof kAllLiftAnims / sizeof kAllLiftAnims[0]))]
                                : kLiftAnims[random_below((int)(sizeof kLiftAnims / sizeof kLiftAnims[0]))];
            const Cpu saved = *c;
            c->f[1].ps0 = c->f[1].ps1 = 1.0;  // rate
            c->f[2].ps0 = c->f[2].ps1 = 0.0;  // start
            c->f[3].ps0 = c->f[3].ps1 = 5.0;  // blend
            guest_call(c, kSetAnime, {link, (uint32_t)anm, 0xFFFFFFFFu});
            *c = saved;
        }
        st32(link + kAcchFlags, ld32(link + kAcchFlags) & ~kAcchGround);  // not standing: the rise carries him off
        g_flight = true;
    }
    stf32(link + kSpeedY, rise);
    if (before) g_l_was = true;
    if (!g_flight) return;
    if (before) moon_jump_steer(link);
    else if (g_face_set) face(link);
}

void set_moon_jump_speeds(int rise, int fly) {
    g_rise_pct = std::clamp(rise, 10, 1000);
    g_fly_pct = std::clamp(fly, 10, 1000);
    LOG("[cheats] moon jump speeds: rise %d%%, flight %d%%", g_rise_pct.load(), g_fly_pct.load());
}
int moon_jump_rise() { return g_rise_pct.load(); }
int moon_jump_fly() { return g_fly_pct.load(); }

bool moon_jump_move(uint32_t link, float& x, float& z) {
    if (!link || link != g_move_link) return false;
    x = g_move_x;
    z = g_move_z;
    // his facing once more: the fall resets the angles in its procedure (before this move), and the
    // model matrix is built after it (setWorldMatrix, end of his step), so he's drawn turned
    if (g_face_set) face(link);
    return true;
}

void cheats_service() {
    int inf = g_infinite.load(std::memory_order_relaxed) & ~(kMoonJump | kMoonNoFall | kMoonAllAnims);
    if (!g_pending.load(std::memory_order_relaxed) && !inf) return;
    uint32_t s = save_addr();
    if (!save_loaded(s)) return;  // stays pending until a file is loaded
    // ponytail: topped up once per frame, so a single hit bigger than your whole health still kills
    if (inf & kInfHealth) st16(s + kLife, ld16(s + kMaxLife));
    if (inf & kInfMagic) st8(s + kMagic, ld8(s + kMaxMagic));
    if (inf & kInfAmmo) {
        st8(s + kArrowNum, ld8(s + kArrowMax));
        st8(s + kBombNum, ld8(s + kBombMax));
    }
    if (inf & kInfRupees) {
        // through the HUD counter, so the shown number stays right: a charge not taken yet is dropped;
        // one the counter already took (it counts down) is followed by a change back up to the full wallet
        const int full = ld8(s + kWallet) == 0 ? 200 : ld8(s + kWallet) == 1 ? 1000 : 5000;
        int32_t change = (int32_t)ld32(rupee_change());
        if (change < 0) st32(rupee_change(), change = 0);
        if (change == 0 && ld16(s + kRupee) < full) st32(rupee_change(), (uint32_t)(full - ld16(s + kRupee)));
    }
    if (inf & kInfBags) keep_bags(s);
    if (inf & kInfBreath) {  // only while the meter runs (0: not swimming, the meter is hidden)
        const uint32_t air = air_meter();
        const int32_t left = (int32_t)ld32(air), max = (int32_t)ld32(air + 4);
        if (left > 0 && left < max) st32(air, (uint32_t)max);
    }
    if (!g_pending.load(std::memory_order_relaxed)) return;
    int w = g_pending.exchange(0);
    if (w & kCheatItems) all_items(s);
    if (w & kCheatSword) best_sword(s);
    if (w & kCheatStats) max_stats(s);
    if (w & kCheatSongs) st8(s + kTact, 0x3F);  // Wind's Requiem .. Song of Passing
    if (w & kCheatTriforce) st8(s + kTriforce, 0xFF);
    if (w & kCheatDungeon) st8(s + kStageDungeonItems, ld8(s + kStageDungeonItems) | 7);
    if (w & kCheatKey && ld8(s + kStageKeys) < 99) st8(s + kStageKeys, ld8(s + kStageKeys) + 1);
    if (w & kCheatBags) fill_bags(s);
    LOG("[cheats] applied %d in %s: life %u/%u, rupees %u, magic %u, items %02X..%02X, collect %02X %02X, songs %02X, triforce %02X, keys %u, dungeon items %02X",
        w, stage().c_str(), ld16(s + kLife), ld16(s + kMaxLife), ld16(s + kRupee), ld8(s + kMagic), ld8(s + kItems), ld8(s + kItems + 20),
        ld8(s + kCollect), ld8(s + kCollect + 1), ld8(s + kTact), ld8(s + kTriforce), ld8(s + kStageKeys), ld8(s + kStageDungeonItems));
}

}  // namespace mods

// One hit kills: every actor's execute goes through fopAc_Execute (025D475C; the matcher named it after its
// CHECK_VEC3_RANGE assert); an enemy (fopAc_ac_c group, GameCube 0x1BE -> WWHD 0x2DA, == fopAc_ENEMY_e 2) keeps
// its health (0x285 -> 0x3A1) at 1, so any hit takes the last point. Bosses that count hits themselves are
// not changed by it.
extern "C" void f_025D475C_orig(Cpu* c);
extern "C" void hook_025D475C(Cpu* c) {
    const uint32_t a = c->r[3];
    if (mods::infinite(mods::kOneHit) && ld8(a + 0x2DA) == 2 && (int8_t)ld8(a + 0x3A1) > 1) st8(a + 0x3A1, 1);
    f_025D475C_orig(c);
}
