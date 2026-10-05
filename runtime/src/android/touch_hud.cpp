// Game state for the touch controls, read from the save data in game memory (dSv_player_c, as in the
// GameCube version's decompilation): which equipment and items Link has, and the items on X/Y/R.
//   g_dComIfG_gameInfo's block: pointer at 0x10114B90; the player save data at +0xC92C
//   dSv_player_status_a: +0x00 max life, +0x02 life (u16, quarter hearts), +0x04 rupees,
//     +0x09 select items (X, Y, R, ...), +0x0E select equipment (sword, shield, bracelets, ...)
//   dSv_player_item: +0x3C, 21 inventory slots (item ids, 0xFF = empty)
// Item ids as in the GameCube version (0x20 telescope ... 0x38 Hero's Sword, 0x3B Hero's Shield).
// What A, B and ZR do right now (the game's action prompts) is not read yet: B and ZR follow the
// equipment, A keeps its default icon.
#include "touch_hud.h"

#include <cstring>

#include "../runtime.h"

namespace touch_hud {
namespace {
constexpr uint32_t kInfoPointer = 0x10114B90, kSaveOffset = 0xC92C;
constexpr uint32_t kSelectItems = 0x09, kSelectEquip = 0x0E, kInventory = 0x3C;
constexpr int kSlots = 21;
constexpr uint8_t kNone = 0xFF;

// TouchIcons.ITEMS codes for item ids (0 = unknown, 1 = empty)
int item_code(uint8_t id) {
    switch (id) {
        case 0xFF: return 1;
        case 0x20: return 2;   // telescope
        case 0x78: return 3;   // sail
        case 0x77: return 4;   // swift sail
        case 0x22: return 5;   // Wind Waker
        case 0x25: return 6;   // grappling hook
        case 0x24: return 7;   // spoils bag
        case 0x2D: return 8;   // boomerang
        case 0x34: return 9;   // Deku Leaf
        case 0x21: return 10;  // Tingle Bottle (the Tingle Tuner's slot)
        case 0x23: return 11;  // Picto Box
        case 0x26: return 12;  // Deluxe Picto Box
        case 0x29: return 13;  // Iron Boots
        case 0x2A: return 14;  // Magic Armor
        case 0x2C: return 15;  // bait bag
        case 0x27: return 16;  // Hero's Bow
        case 0x35: return 17;  // fire and ice arrows
        case 0x36: return 19;  // light arrows
        case 0x31: return 20;  // bombs
        case 0x50: return 21;  // empty bottle
        case 0x51: return 22;  // red potion
        case 0x52: return 23;  // green potion
        case 0x53: return 24;  // blue potion
        case 0x55: return 25;  // Elixir Soup
        case 0x54: return 26;  // half Elixir Soup
        case 0x56: return 27;  // water
        case 0x59: return 28;  // Forest Water
        case 0x57: return 29;  // fairy
        case 0x58: return 30;  // forest firefly
        case 0x30: return 31;  // delivery bag
        case 0x2F: return 32;  // hookshot
        case 0x33: return 33;  // Skull Hammer
        default: return 0;
    }
}

// the save data if it looks valid (life within its maximum, sane maximum), else 0
uint32_t save_address() {
    uint32_t base = ld32(kInfoPointer);
    if (base < 0x10000000 || base > 0x50000000) return 0;
    uint32_t s = base + kSaveOffset;
    uint16_t maxLife = ld16(s), life = ld16(s + 2);
    if (maxLife < 12 || maxLife > 80 || maxLife % 4 || life > maxLife) return 0;
    return s;
}

bool has(uint32_t s, uint8_t id) {
    for (int i = 0; i < kSlots; i++)
        if (ld8(s + kInventory + i) == id) return true;
    return false;
}
}  // namespace

void state(int32_t out[kSize]) {
    memset(out, 0, sizeof(int32_t) * kSize);
    uint32_t s = save_address();
    if (!s) return;  // not in a game yet: unknown, the controls show everything
    uint8_t sword = ld8(s + kSelectEquip), shield = ld8(s + kSelectEquip + 1);
    int32_t flags = kKnown;
    if (sword != kNone) flags |= kHasSword;
    if (shield != kNone) flags |= kHasShield;
    if (has(s, 0x22)) flags |= kHasBaton;
    if (has(s, 0x25)) flags |= kHasGrapple;
    if (has(s, 0x31)) flags |= kHasBombs;
    out[0] = flags;
    out[1] = 0;                                    // A: default icon until the action prompts are read
    out[2] = sword != kNone ? 2 : 1;               // B: sword, or nothing
    out[3] = shield != kNone ? 2 : 3;              // ZR: shield, or crouch
    // X, Y, R: the selected items, as inventory slots (or item ids)
    for (int i = 0; i < 3; i++) {
        uint8_t v = ld8(s + kSelectItems + i);
        uint8_t id = v == kNone ? kNone : v < kSlots ? ld8(s + kInventory + v) : v;
        out[4 + i] = item_code(id);
    }
}
}  // namespace touch_hud
