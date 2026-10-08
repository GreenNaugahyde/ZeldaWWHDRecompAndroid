// What the context-aware touch controls show: the items on X / Y / R, the sword, and the actions
// of A / B / R (the game's own button prompts). Read from game memory each logic step.
//   Save data (dSv_player_c, layout as on the GameCube; addresses from pull request #8 by
//   SSunnKing): pointer at 10114B90, player data at +0xC92C; +0x02 life, +0x09 select items (X, Y,
//   R: inventory slots or item ids), +0x0E select equipment (sword, shield, ...), +0x3C inventory.
//   Actions (dComIfG_play_c mRStatus / mAStatus / mDoStatus, GameCube play + 0x492D..0x492F; HD
//   play at 1046F0B0, event controller moved from +0x3F38 to +0x51D0): WWHD_TOUCHSTATE_LOG=1 logs
//   the bytes of a window around the expected place when they change, to confirm the offsets.
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "mods.h"
#include "runtime.h"
#include "../hud.h"
#include "../release.h"

namespace mods {
namespace {
const release::Data kInfoPointer{0x10114B90}, kPlay{0x1046F0B0};
constexpr uint32_t kSaveOffset = 0xC92C, kSelectItems = 0x09, kSelectEquip = 0x0E, kInventory = 0x3C;
constexpr uint32_t kActWindow = 0x5BB0, kActWindowLen = 0x30;  // around GameCube 0x492F + 0x1298
// confirmed on the tablet (2026-10-08): R/ZR action, B action, A action (mRStatus, mAStatus, mDoStatus)
constexpr uint32_t kRStatus = 0x5BB1, kBStatus = 0x5BB2, kAStatus = 0x5BB3;

uint32_t save_address() {
    uint32_t base = ld32(kInfoPointer);
    if (base < 0x10000000 || base > 0x50000000) return 0;
    uint32_t s = base + kSaveOffset;
    uint16_t maxLife = ld16(s), life = ld16(s + 2);
    if (maxLife < 12 || maxLife > 80 || maxLife % 4 || life > maxLife) return 0;
    return s;
}
}  // namespace

void touch_state(int32_t out[12]) {
    for (int i = 0; i < 12; i++) out[i] = 0;
    uint32_t s = save_address();
    // the title screen and the file select have placeholder save data
    const char* stage = (const char*)mem::ptr(kPlay + 0x5134);
    size_t n = strnlen(stage, 8);
    if (!s || n == 0 || (n == 5 && !memcmp(stage, "sea_T", 5)) || (n == 4 && !memcmp(stage, "Name", 4))) return;
    out[0] = 1;
    out[1] = ld8(kPlay + kAStatus);
    out[2] = ld8(s + kSelectEquip);  // sword (0xFF: none)
    for (int i = 0; i < 3; i++) {
        uint8_t v = ld8(s + kSelectItems + i);
        out[3 + i] = v == 0xFF ? 0xFF : v < 21 ? ld8(s + kInventory + v) : v;
    }
    out[6] = ld8(kPlay + kBStatus);
    out[7] = ld8(kPlay + kRStatus);
    out[8] = event_mode_now();
    out[9] = hud_menu_open();
    for (int i = 0; i < 21; i++)  // the Wind Waker (d-pad up conducts)
        if (ld8(s + kInventory + i) == 0x22) out[10] = 1;
    out[11] = ld8(s + kSelectEquip + 1);  // shield (0x3B Hero's, 0x3C Mirror)
}

// hearts only in combat (hud.h): shown while the sword is in Link's hand (daPy_lk_c::mEquipItem,
// GameCube 0x3560 -> HD 0x69B0; daPyItem_SWORD 0x103) and for a while after he took damage
void hearts_step() {
    constexpr uint32_t kEquipItem = 0x69B0;
    constexpr uint16_t kSword = 0x103;
    static uint16_t last_life = 0, last_equip = 0xFFFF, last_rupees = 0xFFFF;
    static uint64_t rupees_until = 0;
    static uint64_t shown_until = 0;
    uint64_t now = step();
    uint32_t s = save_address();
    uint32_t link = link_actor();
    if (!s || !link) {
        hud_set_fade_wanted(kFadeHearts, true);
        hud_set_fade_wanted(kFadeRupees, true);
        return;
    }
    // rupees: shown for 4.2 s after the count changed (picked up, spent); not at the first look
    uint16_t rupees = ld16(s + 4);
    if (rupees != last_rupees && last_rupees != 0xFFFF) rupees_until = now + 126;
    last_rupees = rupees;
    hud_set_fade_wanted(kFadeRupees, now < rupees_until);
    uint16_t life = ld16(s + 2), equip = ld16(link + kEquipItem);
    if (equip != last_equip) {
        LOG("[hud] item in Link's hand %04X", equip);
        last_equip = equip;
    }
    // they stay 2.2 s after the sword is put away and 4.2 s after damage (logic steps at 30 per second)
    if (equip == kSword) shown_until = std::max(shown_until, now + 66);
    if (life < last_life) shown_until = std::max(shown_until, now + 126);
    last_life = life;
    hud_set_fade_wanted(kFadeHearts, now < shown_until);
}

void touch_state_step() {
    hearts_step();
    static const bool log = getenv("WWHD_TOUCHSTATE_LOG") != nullptr;
    if (!log) return;
    uint32_t s = save_address();
    static uint8_t last_items[3] = {0xEE, 0xEE, 0xEE}, last_sword = 0xEE;
    if (s) {
        uint8_t items[3], resolved[3];
        for (int i = 0; i < 3; i++) {
            items[i] = ld8(s + kSelectItems + i);
            resolved[i] = items[i] < 21 ? ld8(s + kInventory + items[i]) : items[i];
        }
        uint8_t sword = ld8(s + kSelectEquip);
        if (memcmp(items, last_items, 3) || sword != last_sword) {
            LOG("[touch] X %02X (%02X)  Y %02X (%02X)  R %02X (%02X)  sword %02X  life %u/%u", items[0], resolved[0], items[1],
                resolved[1], items[2], resolved[2], sword, ld16(s + 2), ld16(s));
            memcpy(last_items, items, 3);
            last_sword = sword;
        }
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
