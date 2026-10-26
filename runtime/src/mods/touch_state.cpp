// What the context-aware touch controls show: the items on X / Y / R, the sword, and the actions
// of A / B / R (the game's own button prompts). Read from game memory each logic step.
//   Save data (dSv_player_c, layout as on the GameCube; addresses from pull request #8 by
//   SSunnKing): pointer at 10114B90, player data at +0xC92C; +0x02 life, +0x09 select items (X, Y,
//   R: inventory slots or item ids), +0x0E select equipment (sword, shield, ...), +0x3C inventory.
//   Actions (dComIfG_play_c mRStatus / mAStatus / mDoStatus, GameCube play + 0x492D..0x492F; HD
//   play at 1046F0B0, event controller moved from +0x3F38 to +0x51D0): WWHD_TOUCHSTATE_LOG=1 logs
//   the bytes of a window around the expected place when they change, to confirm the offsets.
//   Link's mode (dComIfGp mPlayerStatus[0][0..1], GameCube play + 0x4A28: bow / hookshot / boomerang
//   aim, first person, ship, swim, telescope, crawl, ... and Picto Box aim, Deku Leaf, conducting):
//   HD play + 0x5CD8 / 0x5CDC, found in the recompiled code by the bits tested on it (0x10000 ship,
//   0x100000 swim, 0x200000 telescope on the first word; 0x8 Picto Box, 0x1 conducting on the
//   second). mMiniGameType follows them as on the GameCube (+0x12 -> 0x5CEA, to confirm).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "mods.h"
#include "runtime.h"
#include "../hud.h"
#include "../release.h"

namespace mods {
namespace {
const release::Data kInfoPointer{0x10114B90}, kPlay{0x1046F0B0};
const release::Data kOverlap{0x101F36CC};  // l_fopOvlpM_overlap[0]: a fade / wipe is under way (turbo.cpp)
constexpr uint32_t kSaveOffset = 0xC92C, kSelectItems = 0x09, kSelectEquip = 0x0E, kInventory = 0x3C;
// the bags' contents (dSv_player_bag_item_c at +0x76: spoils mBeast[8], bait mBait[8], delivery
// mReserve[8]); on a button they are inventory slots 24..31, 36..43, 48..55 (dInvSlot_BeastFirst_e ...)
constexpr uint32_t kBagItems = 0x76;
constexpr uint32_t kActWindow = 0x5BB0, kActWindowLen = 0x30;  // around GameCube 0x492F + 0x1298
// R/ZR action, B action, A action (mRStatus, mAStatus, mDoStatus) at +0x5BB5..7 in EUR (its log
// 2026-10-12: 36 / 35 / 0C) and USA (its log 2026-10-14 on land: 0F / 35 / 08, Crouch / sword /
// Put away; +0x5BB1 put B's byte on A and R's on B); JPN keeps them 4 bytes earlier (its log
// 2026-10-14: 36 / 35 / 08 at +0x5BB1..3, the telescope's forced B 07 at +0x5BB5).
uint32_t act_base() { return release::id() == release::Id::JPN ? 0x5BB1 : 0x5BB5; }
uint32_t r_status() { return kPlay + act_base(); }
uint32_t b_status() { return kPlay + act_base() + 1; }
uint32_t a_status() { return kPlay + act_base() + 2; }
// the forced actions follow (mRStatusForce, mAStatusForce, mDoStatusForce): set by modes such as the
// Picto Box (ZR 0x49 album, B return, A take photo) and its album (B return, A choose); the HUD shows
// a forced action instead of the normal one (dMeter_weponMove)
uint8_t action(uint32_t normal) {
    uint8_t forced = ld8(normal + 3);
    return forced ? forced : ld8(normal);
}
constexpr uint32_t kPlayerStatus0 = 0x5CD8, kPlayerStatus1 = 0x5CDC, kMiniGameType = 0x5CEA;

// the item in an inventory slot, also the bags' (dComIfGs_getItem); 0xFF none
uint8_t inv_item(uint32_t s, uint8_t slot) {
    if (slot < 21) return ld8(s + kInventory + slot);
    for (int bag = 0; bag < 3; bag++)
        if (slot >= 24 + 12 * bag && slot < 32 + 12 * bag) return ld8(s + kBagItems + 8 * bag + (slot - 24 - 12 * bag));
    return 0xFF;
}

// Lock-on (ZL): dAttention_c, as on the GameCube (decomp d_attention.h), in HD at play + 0x5804 (USA, its
// constructor's call 025B254C) and kept by Link at daPy_lk_c::mpAttention (+0x6894, GameCube 0x3480;
// setAtnList 023DCD30), which also holds the locked actor each step (mpAttnActorLockOn +0x65B0, GameCube
// 0x319C; only while the lock holds). In it mLockOnState +0x18 (1 lock, 2 release), mLockonCount +0xD4
// (the targets in reach while none is locked). An actor (fopAc_ac_c, HD): process id +0x4 (fpcSch_JudgeByID
// 025E1234), attention flags +0x39C (chaseAttention 024ECB08; 4 = an enemy's lock), health +0x3A1 (s8).
constexpr uint32_t kLinkAttention = 0x6894, kLinkLockOnActor = 0x65B0;
// daPy_lk_c::mpAttnEntryA (+0x6898, GameCube 0x3484, next to mpAttention): the attention entry A acts on; its
// mType (+8) tells a door (5) from a chest (6) behind the same Open
constexpr uint32_t kLinkAttnEntryA = 0x6898;
constexpr uint32_t kAttLockState = 0x18, kAttLockCount = 0xD4;
constexpr uint32_t kActorBsType = 0x0, kActorProcId = 0x4, kActorAttnFlags = 0x39C, kActorHealth = 0x3A1;
constexpr uint32_t kAttnLockonBattle = 0x4;
constexpr int kLostWatchSteps = 30;  // 1 s
std::atomic<int> g_defeats{0};
std::atomic<int> g_locked{0}, g_lock_reach{0};

uint32_t attention(uint32_t link) {
    if (!link) return 0;
    uint32_t a = ld32(link + kLinkAttention);
    if (a < 0x10000000 || a > 0x50000000 || ld32(a) != link) return 0;  // mpPlayer
    return a;
}

// each logic step: the lock's target is remembered; when the lock ends and it was an enemy that is now
// dead (health 0, deleted, or no longer lockable as an enemy), a defeat is counted (the app lets go of
// a latched ZL, or locks the next target). A lock ended by distance, a cutscene or ZL keeps the count.
void lockon_step() {
    static uint32_t target = 0, target_id = 0;
    static bool had_health = false;  // some enemies keep 0 throughout: then their health says nothing
    static int lost_steps = 0;  // watching a lost target
    uint32_t link = link_actor(), a = attention(link);
    static const bool log = getenv("WWHD_LOCKON_LOG") != nullptr;
    if (log) {  // the lock's state when it changes (finding why a defeat isn't seen)
        static uint32_t last[4] = {1, 1, 1, 1};
        uint32_t raw = link ? ld32(link + kLinkAttention) : 0;
        uint32_t cur[4] = {raw, raw >= 0x10000000 && raw < 0x50000000 ? (uint32_t)ld8(raw + kAttLockState) : 0xFFu,
                           link ? ld32(link + kLinkLockOnActor) : 0u, a ? ld32(a + kAttLockCount) : 0xFFu};
        if (memcmp(cur, last, sizeof cur)) {
            uint32_t t = cur[2];
            bool ok = t >= 0x10000000 && t < 0x50000000;
            LOG("[lockon] link %08X att %08X (owner %08X, %s) state %u target %08X (id %08X flags %08X health %d) count %u", link, raw,
                raw >= 0x10000000 && raw < 0x50000000 ? ld32(raw) : 0, a ? "ok" : "rejected", cur[1], t, ok ? ld32(t + kActorProcId) : 0,
                ok ? ld32(t + kActorAttnFlags) : 0, ok ? (int8_t)ld8(t + kActorHealth) : 0, cur[3]);
            memcpy(last, cur, sizeof cur);
        }
    }
    if (!a) {
        target = 0;
        lost_steps = 0;
        g_locked = 0;
        g_lock_reach = 0;
        return;
    }
    uint8_t st = ld8(a + kAttLockState);
    uint32_t t = ld32(link + kLinkLockOnActor);
    bool locked = st == 1 || (st == 2 && t);
    g_locked = locked;
    g_lock_reach = locked ? 0 : (int)ld32(a + kAttLockCount);
    if (locked && t >= 0x10000000 && t < 0x50000000) {
        if (ld32(t + kActorAttnFlags) & kAttnLockonBattle) {
            if (t != target) had_health = false;
            lost_steps = 0;
            target = t;
            target_id = ld32(t + kActorProcId);
            had_health = had_health || (int8_t)ld8(t + kActorHealth) > 0;  // (it may reach 0 while still locked)
        } else if (t != target) {
            target = 0;  // a person, a sign, ...
            lost_steps = 0;
        }  // (the same enemy no longer lockable while the lock still holds: dying, seen when the lock ends)
        return;
    }
    if (!target) return;
    if (locked) return;  // still locked, Link's pointer empty for a step (around a hit): the target stays
    // the lock ended: the enemy may still be dying or be deleted a few steps later (fpcBs_Delete sets
    // mBsType +0x0 to 0), so it is watched for kLostWatchSteps
    if (lost_steps == 0) lost_steps = kLostWatchSteps;
    int8_t health = (int8_t)ld8(target + kActorHealth);
    bool gone = ld32(target + kActorBsType) == 0 || ld32(target + kActorProcId) != target_id;
    bool unlockable = !(ld32(target + kActorAttnFlags) & kAttnLockonBattle);
    if (gone || (had_health && health <= 0) || unlockable) {
        g_defeats++;
        LOG("[touch] lock-on target defeated (%s, %d steps after the lock ended); %d in reach",
            gone ? "deleted" : unlockable ? "not lockable" : "health 0", kLostWatchSteps - lost_steps, (int)ld32(a + kAttLockCount));
    } else if (--lost_steps > 0) {
        return;
    } else if (log) {
        LOG("[lockon] lost target %08X alive: health %d flags %08X", target, health, ld32(target + kActorAttnFlags));
    }
    lost_steps = 0;
    target = 0;
}

uint32_t save_address() {
    uint32_t base = ld32(kInfoPointer);
    if (base < 0x10000000 || base > 0x50000000) return 0;
    uint32_t s = base + kSaveOffset;
    uint16_t maxLife = ld16(s), life = ld16(s + 2);
    if (maxLife < 12 || maxLife > 80 || maxLife % 4 || life > maxLife) return 0;
    return s;
}
}  // namespace

uint32_t player_save() { return save_address(); }

void touch_state(int32_t out[kTouchStateLen]) {
    static bool texts_logged = false;  // the HUD's action table once (it may differ between the releases)
    if (!texts_logged) {
        int32_t t[kActionCodes];
        if (action_texts(t)) {
            texts_logged = true;
            std::string l;
            for (int i = 0; i < kActionCodes; i++) l += std::to_string(t[i]) + (i + 1 < kActionCodes ? "," : "");
            LOG("[touch] %s action texts: %s", release::name(), l.c_str());
        }
    }
    for (int i = 0; i < kTouchStateLen; i++) out[i] = 0;
    out[30] = g_defeats;  // also on the title screen: the count only grows
    for (int i = 0; i < kClusterCount; i++) out[15 + i] = hud_cluster_alpha(i);
    for (int i = 0; i < 3; i++) out[20 + i] = hud_cluster_command(i);
    hud_zoom_bar(out + 23);
    out[33] = ld32(kOverlap) != 0;
    uint32_t s = save_address();
    // the title screen and the file select have placeholder save data
    const char* stage = (const char*)mem::ptr(kPlay + 0x5134);
    size_t n = strnlen(stage, 8);
    if (!s || n == 0 || (n == 5 && !memcmp(stage, "sea_T", 5)) || (n == 4 && !memcmp(stage, "Name", 4))) return;
    out[0] = 1;
    out[1] = action(a_status());
    out[2] = ld8(s + kSelectEquip);  // sword (0xFF: none)
    for (int i = 0; i < 3; i++) {
        uint8_t v = ld8(s + kSelectItems + i);
        out[3 + i] = v == 0xFF ? 0xFF : inv_item(s, v);
    }
    out[6] = action(b_status());
    out[7] = action(r_status());
    out[8] = event_mode_now();
    out[9] = hud_menu_open();
    for (int i = 0; i < 21; i++)  // the Wind Waker (d-pad up conducts)
        if (ld8(s + kInventory + i) == 0x22) out[10] = 1;
    out[11] = ld8(s + kSelectEquip + 1);  // shield (0x3B Hero's, 0x3C Mirror)
    out[12] = (int32_t)ld32(kPlay + kPlayerStatus0);
    out[13] = (int32_t)ld32(kPlay + kPlayerStatus1);
    out[14] = ld8(kPlay + kMiniGameType);
    uint32_t link = link_actor();
    out[29] = link ? ld16(link + 0x69B0) : -1;  // daPy_lk_c::mEquipItem: in his hand (0x22 the Wind Waker, 0x103 sword)
    out[31] = g_locked;
    out[32] = g_lock_reach;
    uint32_t e = link ? ld32(link + kLinkAttnEntryA) : 0;
    out[34] = e >= 0x10000000 && e < 0x50000000 ? (int32_t)ld32(e + 8) : -1;
    // A's Jump is the jump attack: the game shows Jump (0x12) while locked on for the side hop, the back flip and,
    // with the sword in his hand and the stick idle or forward, the jump attack (daPy_lk_c::setDoStatus: dActStts
    // 0x43, shown as Jump); forward as getDirectionFromShapeAngle: m34E8 (+0x6930, steer.cpp) - shape_angle.y (+0x32A) within
    // +-0x2000, mStickDistance (+0x6A08) <= 0.05 idle
    if (link && out[1] == 0x12 && out[29] == 0x103) {
        int16_t dir = (int16_t)(ld16(link + 0x6930) - ld16(link + 0x32A));
        float dist = u32_as_f32(ld32(link + 0x6A08));
        out[35] = dist <= 0.05f || (dir > -0x2000 && dir < 0x2000);
    }
    out[36] = link ? (int32_t)ld32(link + 0x6A70) : 0;  // daPy_lk_c::mModeFlg (0x400000 on a ladder, 0x10000 climbing, 0x20 hanging)
    for (int i = 0; i < 21; i++)  // the Swift Sail (HD, 0x77) in place of the sail (0x78)
        if (ld8(s + kInventory + i) == 0x77) out[37] = 1;
}

bool inventory(uint8_t out[21]) {
    uint32_t s = save_address();
    if (!s) return false;
    for (int i = 0; i < 21; i++) out[i] = ld8(s + kInventory + i);
    return true;
}

// the HUD's action names: a table by action code of {label, length?} (8 bytes), the label one of its message
// file's (CommandGuide_00) "T_ACT_Command_NN"; filled at start (USA 0264AB20)
bool action_texts(int32_t out[kActionCodes]) {
    static const release::Data kTable{0x10491350};
    for (int i = 0; i < kActionCodes; i++) {
        uint32_t p = ld32((uint32_t)kTable + 8 * i);
        if (p < 0x10000000 || p >= 0x10400000) return false;
        const char* s = (const char*)mem::ptr(p);
        if (strncmp(s, "T_ACT_Command_", 14) || s[14] < '0' || s[14] > '9' || s[15] < '0' || s[15] > '9') return false;
        out[i] = (s[14] - '0') * 10 + (s[15] - '0');
    }
    return out[1] != 0;  // Look
}

// hearts only in combat (hud.h): shown while the sword is in Link's hand (daPy_lk_c::mEquipItem,
// GameCube 0x3560 -> HD 0x69B0; daPyItem_SWORD 0x103) and for a while after he took damage; the
// magic bar on its own, while magic is used (the Deku Leaf, Fire / Ice Arrows) or refilled
void hearts_step() {
    constexpr uint32_t kEquipItem = 0x69B0;
    constexpr uint16_t kSword = 0x103;
    static uint16_t last_life = 0, last_max_life = 0, last_equip = 0xFFFF, last_rupees = 0xFFFF;
    static uint64_t rupees_until = 0, magic_until = 0;
    static uint8_t last_magic = 0xFF;
    static uint64_t shown_until = 0;
    uint64_t now = step();
    uint32_t s = save_address();
    uint32_t link = link_actor();
    if (!s || !link) {
        hud_set_fade_wanted(kFadeHearts, true);
        hud_set_fade_wanted(kFadeRupees, true);
        hud_set_fade_wanted(kFadeMagic, true);
        return;
    }
    // rupees: shown for 4.2 s after the count changed (picked up, spent); not at the first look
    uint16_t rupees = ld16(s + 4);
    if (rupees != last_rupees && last_rupees != 0xFFFF) rupees_until = now + 126;
    last_rupees = rupees;
    hud_set_fade_wanted(kFadeRupees, now < rupees_until);
    // magic (save + 0x14, mods/cheats.cpp): shown while it changes and 2.2 s after; not at the first look
    uint8_t magic = ld8(s + 0x14);
    if (magic != last_magic && last_magic != 0xFF) magic_until = now + 66;
    last_magic = magic;
    hud_set_fade_wanted(kFadeMagic, now < magic_until);
    uint16_t max_life = ld16(s), life = ld16(s + 2), equip = ld16(link + kEquipItem);
    if (equip != last_equip) {
        LOG("[hud] item in Link's hand %04X", equip);
        last_equip = equip;
    }
    // they stay 2.2 s after the sword is put away and 4.2 s after damage or healing (a heart picked
    // up, a potion, a fairy, a new heart container; logic steps at 30 per second); not at the first look
    if (equip == kSword) shown_until = std::max(shown_until, now + 66);
    if (life < last_life) shown_until = std::max(shown_until, now + 126);
    if (last_max_life && (life > last_life || max_life > last_max_life)) shown_until = std::max(shown_until, now + 126);
    last_life = life;
    last_max_life = max_life;
    hud_set_fade_wanted(kFadeHearts, now < shown_until);
}

// ---- the item ring (app: ItemRing.java): the inventory, and an item put on X / Y / R ----
// As the game's item screen does it (dMenu_Item_c::itemDecide): the save data's select item (an
// inventory slot) and the play's copy of its item id (dComIfGp_setSelectItem; GameCube play + 0x4933,
// right after the forced actions: HD act_base() + 6), which the HUD and Link's item use read. An item
// already on another button swaps with the old one. The play's copy is written only when it matches
// the buttons' items before (a check of the offset); the request is applied on the game thread.
namespace {
std::atomic<int> g_equip_req{-1};  // button << 8 | slot
std::atomic<int> g_conduct_req{-1};
uint32_t select_cache() { return kPlay + act_base() + 6; }
// puts the slot on the button (0xFF: empties it, to give a button its emptiness back)
bool equip(int btn, int slot) {
    uint32_t s = save_address();
    if (!s || btn < 0 || btn > 2 || (slot != 0xFF && inv_item(s, (uint8_t)slot) == 0xFF)) return false;
    // the play's copy must match the buttons' items (a check of its offset); the Picto Boxes are left
    // out of the check: with the Deluxe Picto Box (0x26) in the inventory the copy can hold 0x23
    uint8_t before[3], cache[3];
    int matched = 0, checked = 0;
    for (int i = 0; i < 3; i++) {
        before[i] = ld8(s + kSelectItems + i);
        cache[i] = ld8(select_cache() + i);
        uint8_t item = inv_item(s, before[i]);
        if (item == 0xFF || item == 0x23 || item == 0x26) continue;
        checked++;
        if (cache[i] == item) matched++;
    }
    const bool cache_ok = matched == checked;
    const uint8_t old = before[btn];
    for (int i = 0; i < 3; i++)
        if (i != btn && slot != 0xFF && before[i] == slot) st8(s + kSelectItems + i, old);  // swap
    st8(s + kSelectItems + btn, (uint8_t)slot);
    if (cache_ok)  // only the buttons that changed
        for (int i = 0; i < 3; i++) {
            uint8_t v = ld8(s + kSelectItems + i);
            if (v != before[i]) st8(select_cache() + i, inv_item(s, v));
        }
    LOG("[ring] %c: slot %d (item %02X); before: slots %02X %02X %02X, play copy %02X %02X %02X%s", "XYR"[btn], slot,
        inv_item(s, (uint8_t)slot), before[0], before[1], before[2], cache[0], cache[1], cache[2],
        cache_ok ? "" : "; the play's copy didn't match, left alone");
    return true;
}
void apply_equip() {
    int req = g_equip_req.exchange(-1);
    if (req >= 0) equip(req >> 8, req & 0xFF);
}

// The Wind Waker picked in the ring conducts instead of staying on the button: it goes on the button
// for the app's one press (ItemRing.java), and the button's own item comes back as soon as Link conducts
// (conducting runs on the item in his hand, daPy_lk_c::mEquipItem, and reads no button; decomp
// d_a_player_tact.inc), or when he didn't start within kConductStartSteps (no place to conduct)
constexpr int kConductStartSteps = 45;
struct {
    int btn = -1;
    uint8_t old = 0xFF;
    int steps = 0;
} g_tact;
void conduct_step() {
    int req = g_conduct_req.exchange(-1);
    if (req >= 0 && g_tact.btn < 0) {
        uint32_t s = save_address();
        const int btn = req >> 8, slot = req & 0xFF;
        if (s && btn >= 0 && btn <= 2) {
            const uint8_t old = ld8(s + kSelectItems + btn);
            if (equip(btn, slot)) g_tact = {btn, old, 0};
        }
    }
    if (g_tact.btn < 0) return;
    uint32_t s = save_address();
    if (!s) {
        g_tact.btn = -1;
        return;
    }
    const bool conducting = ld32(kPlay + kPlayerStatus1) & 1;
    if (!conducting && ++g_tact.steps < kConductStartSteps) return;
    // only if the button still has the baton (dItemNo_WIND_WAKER; nothing else was put on it meanwhile)
    if (inv_item(s, ld8(s + kSelectItems + g_tact.btn)) == 0x22) equip(g_tact.btn, g_tact.old);
    LOG("[ring] %c: conducting %s, slot %02X back", "XYR"[g_tact.btn], conducting ? "started" : "didn't start", g_tact.old);
    g_tact.btn = -1;
}

// The Picto Box lent to a button for the postbox (postbox.cpp: the game's picto handover reads the shown item from
// X / Y / R): the button's own item comes back once the postbox has answered the picto (lend_return), or after
// 20 s without (the chooser cancelled; its album screen isn't an event, so the wait goes on while it is drawn)
std::atomic<int> g_lend_req{-1};
std::atomic<bool> g_lend_back{false};
struct {
    int btn = -1;
    uint8_t old = 0xFF;
    int steps = 0;
    bool started = false;
} g_lend;
void lend_step() {
    int req = g_lend_req.exchange(-1);
    if (req >= 0 && g_lend.btn < 0) {
        uint32_t s = save_address();
        if (s) {
            const uint8_t old = ld8(s + kSelectItems + req);
            if (equip(req, 8)) g_lend = {req, old, 0, false};  // the Picto Box's inventory slot
        }
    }
    if (g_lend.btn < 0) return;
    uint32_t s = save_address();
    if (!s) {
        g_lend.btn = -1;
        return;
    }
    const bool back = g_lend_back.exchange(false);
    if (back) g_lend.started = true;
    if (!back && (++g_lend.steps < 600 || event_mode_now() != 0 || hud_album_screen())) return;
    uint8_t now = inv_item(s, ld8(s + kSelectItems + g_lend.btn));
    if (now == 0x23 || now == 0x26) equip(g_lend.btn, g_lend.old);  // (unless something else was put there meanwhile)
    LOG("[postbox] %c: the Picto Box lent %s, slot %02X back", "XYR"[g_lend.btn], g_lend.started ? "and shown" : "but not shown", g_lend.old);
    g_lend.btn = -1;
}

// With the item ring the Wind Waker is never kept on X / Y / R (put there in the game's item screen,
// a save from before, a conduct whose old item didn't come back): once no menu is open the button
// gets its last other item again (if no other button has it now), else nothing. Only the ring's own
// press (g_tact above) has the baton on a button
std::atomic<bool> g_no_baton{false};
void baton_guard_step() {
    static uint8_t last_other[3] = {0xFF, 0xFF, 0xFF};
    uint32_t s = save_address();
    const char* stage = (const char*)mem::ptr(kPlay + 0x5134);
    size_t n = strnlen(stage, 8);
    if (!s || n == 0 || (n == 5 && !memcmp(stage, "sea_T", 5)) || (n == 4 && !memcmp(stage, "Name", 4))) return;
    uint8_t slots[3];
    for (int i = 0; i < 3; i++) slots[i] = ld8(s + kSelectItems + i);
    const bool busy = !g_no_baton || g_tact.btn >= 0 || g_conduct_req.load() >= 0 || hud_menu_open();
    for (int i = 0; i < 3; i++) {
        if (inv_item(s, slots[i]) != 0x22) {
            last_other[i] = slots[i];
            continue;
        }
        if (busy) continue;
        uint8_t back = last_other[i];
        for (int j = 0; j < 3; j++)
            if (j != i && slots[j] == back) back = 0xFF;
        if (back != 0xFF && (inv_item(s, back) == 0xFF || inv_item(s, back) == 0x22)) back = 0xFF;
        LOG("[ring] %c: the Wind Waker off the button, slot %02X back", "XYR"[i], back);
        equip(i, back);
        slots[i] = back;
        last_other[i] = back;
    }
}
}  // namespace

void set_ring_no_baton(bool on) { g_no_baton = on; }

void request_equip(int button, int slot) { g_equip_req = (button << 8) | (slot & 0xFF); }
void lend_picto_box(int button) { g_lend_req = button; }
void lend_return() { g_lend_back = true; }
void request_conduct(int button, int slot) { g_conduct_req = (button << 8) | (slot & 0xFF); }

// the item button (0 X, 1 Y, 2 R) with the Picto Box on it; -1 none, -2 no Picto Box at all
int picto_box_button() {
    uint32_t s = save_address();
    if (!s) return -2;
    for (int i = 0; i < 3; i++) {
        uint8_t it = inv_item(s, ld8(s + kSelectItems + i));
        if (it == 0x23 || it == 0x26) return i;
    }
    uint8_t own = ld8(s + kInventory + 8);  // the Picto Box's inventory slot
    return own == 0x23 || own == 0x26 ? -1 : -2;
}

void item_ring_state(int32_t out[kItemRingLen]) {
    for (int i = 0; i < kItemRingLen; i++) out[i] = -1;
    uint32_t s = save_address();
    const char* stage = (const char*)mem::ptr(kPlay + 0x5134);
    size_t n = strnlen(stage, 8);
    if (!s || n == 0 || (n == 5 && !memcmp(stage, "sea_T", 5)) || (n == 4 && !memcmp(stage, "Name", 4))) return;
    for (int i = 0; i < 21; i++) out[i] = ld8(s + kInventory + i);
    for (int i = 0; i < 3; i++) out[21 + i] = ld8(s + kSelectItems + i);
    uint32_t link = link_actor();
    out[24] = link ? ld16(link + 0x69B0) : -1;  // daPy_lk_c::mEquipItem: 0x100 none, 0x103 sword
    for (int i = 0; i < 24; i++) out[25 + i] = ld8(s + kBagItems + i);  // spoils, bait, delivery bag
}

void touch_state_step() {
    apply_equip();
    conduct_step();
    lend_step();
    baton_guard_step();
    hearts_step();
    lockon_step();
    static const bool log = getenv("WWHD_TOUCHSTATE_LOG") != nullptr;
    if (!log) return;
    uint32_t s = save_address();
    static uint8_t last_items[3] = {0xEE, 0xEE, 0xEE}, last_sword = 0xEE;
    if (s) {
        uint8_t items[3], resolved[3];
        for (int i = 0; i < 3; i++) {
            items[i] = ld8(s + kSelectItems + i);
            resolved[i] = inv_item(s, items[i]);
        }
        uint8_t sword = ld8(s + kSelectEquip);
        if (memcmp(items, last_items, 3) || sword != last_sword) {
            LOG("[touch] X %02X (%02X)  Y %02X (%02X)  R %02X (%02X)  sword %02X  life %u/%u", items[0], resolved[0], items[1],
                resolved[1], items[2], resolved[2], sword, ld16(s + 2), ld16(s));
            memcpy(last_items, items, 3);
            last_sword = sword;
        }
    }
    // Link's mode, by the decompilation's names
    static uint32_t last_st[2] = {0xFFFFFFFF, 0xFFFFFFFF};
    static uint8_t last_mg = 0xFF;
    uint32_t st[2] = {ld32(kPlay + kPlayerStatus0), ld32(kPlay + kPlayerStatus1)};
    uint8_t mg = ld8(kPlay + kMiniGameType);
    if (st[0] != last_st[0] || st[1] != last_st[1] || mg != last_mg) {
        static const char* const n0[32] = {"1", "2", "4", "8", "10", "20", "40", "80", "hang", "200", "400", "800",
                                           "bow-aim", "first-person", "hookshot-aim", "sword-swing", "ship", "rope-aim",
                                           "40000", "boomerang-aim", "swim", "telescope", "boomerang-wait", "800000",
                                           "1000000", "2000000", "4000000", "crawl", "10000000", "20000000",
                                           "spin-attack", "80000000"};
        static const char* const n1[32] = {"conduct", "2", "4", "picto-aim", "10", "deku-fly", "deku-fan", "80", "100",
                                           "200", "sail", "800", "1000", "2000", "4000", "8000", "10000", "20000",
                                           "40000", "80000", "100000", "200000", "400000", "800000", "1000000",
                                           "2000000", "4000000", "8000000", "10000000", "20000000", "40000000",
                                           "80000000"};
        std::string names;
        for (int b = 0; b < 32; b++)
            if (st[0] >> b & 1) names += std::string(" ") + n0[b];
        for (int b = 0; b < 32; b++)
            if (st[1] >> b & 1) names += std::string(" 1:") + n1[b];
        LOG("[touch] mode %08X %08X minigame %u:%s", st[0], st[1], mg, names.c_str());
        last_st[0] = st[0];
        last_st[1] = st[1];
        last_mg = mg;
    }
    // the buttons' actions and what Link holds / does (daPy_lk_c::mCurProc, GameCube 0x31D8 + 0x3450 as mEquipItem:
    // not it in HD, reads 0 throughout; kept to find the right one)
    static int32_t last_act[6] = {-2, -2, -2, -2, -2, -2};
    uint32_t lk = link_actor(), ea = lk ? ld32(lk + kLinkAttnEntryA) : 0;
    int32_t act[6] = {action(a_status()), action(b_status()), action(r_status()), lk ? ld16(lk + 0x69B0) : -1,
                      lk ? (int32_t)ld32(lk + 0x6628) : -1, ea >= 0x10000000 && ea < 0x50000000 ? (int32_t)ld32(ea + 8) : -1};
    if (memcmp(act, last_act, sizeof act)) {
        LOG("[touch] A %02X  B %02X  R %02X  hand %04X  proc %d  attention %d", act[0], act[1], act[2], act[3] & 0xFFFF, act[4],
            act[5]);
        memcpy(last_act, act, sizeof act);
    }
    static uint8_t last[kActWindowLen];
    static bool init = false;
    uint8_t cur[kActWindowLen];
    for (uint32_t i = 0; i < kActWindowLen; i++) cur[i] = ld8(kPlay + kActWindow + i);
    if (!init || memcmp(cur, last, kActWindowLen)) {
        char buf[256];
        int n = snprintf(buf, sizeof buf, "[touch] play+%X:", kActWindow);
        for (uint32_t i = 0; i < kActWindowLen; i++) n += snprintf(buf + n, sizeof buf - n, "%s%02X", i % 4 ? "" : " ", cur[i]);
        LOG("%s", buf);
        memcpy(last, cur, kActWindowLen);
        init = true;
    }
}

}  // namespace mods
