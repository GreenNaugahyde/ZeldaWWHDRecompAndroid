// Lenzo's Picto Box test in GameCube saves converted to HD (#306).
//
// Both games keep the test's progress in event register 0xC407 (event byte 0xC4, mask 0x07):
// 0 no order, 1/3/5 order 1/2/3 given, 2/4 picto 1/2 accepted, 6 picto 3 accepted.
//   GameCube (tww d_a_npc_photo.cpp): the third picto gives the Joy Pendant; the Deluxe Picto Box
//     comes later, when a Forest Firefly in a bottle is shown to Lenzo (register >= 6, no Deluxe).
//   HD (WWHD 022CFA00 executeTalk, 022CC690 XyEventCB): the third picto gives the Deluxe Picto
//     Box; the firefly event is gone, and with register >= 6 nothing gives the Deluxe Picto Box.
// So a GameCube file saved between the two steps (register >= 6 with the black-and-white Picto
// Box) is stuck in HD. HD's own files never hold that combination: the register reaches 6 in the
// talk that hands out the Deluxe Picto Box. Converters (upstream's tools/savegame/gc2hd.py before
// its fix, our app's GcSave.java) copy the state as it is; when the game reads cking.sav, such a Quest Log gets what HD gives
// for the test: item_func_camera2 (WWHD 0254E3F0), inventory slot 8 = Deluxe Picto Box (0x26)
// and get-item slot 8 bit 1. The item buttons hold the slot number (8), so they need no change.
// The game keeps the result with its next save. From the original project (devel 42f6e3d3).
//
// Pure; the hook is the cking.sav read in hle/fs.cpp.
#pragma once
#include <cstdint>

namespace picto_quest {

// cking.sav file block: the packed save data (0x768 bytes) from +0, then at +0xA8C the u32 byte sum
// and complement sum over the first 0xA8C bytes (SaveMgr 02721AFC), big-endian
constexpr uint32_t kSaveBlock = 0xA94;
constexpr uint32_t kSaveSummed = 0xA8C;
// offsets in the packed save data (dSv_save_c_PACKED; tools/savegame/wwsave.py)
constexpr uint32_t kCameraItem = 0x44;     // dSv_player_item_c slot 8 (dInvSlot_CAMERA_e)
constexpr uint32_t kCameraGetItem = 0x59;  // dSv_player_get_item_c::mItemFlags[8]
constexpr uint32_t kLenzoReg = 0x618 + 0xC4;  // dSv_event_c::mFlags[0xC4], register 0xC407
constexpr uint8_t kLenzoRegMask = 0x07;
constexpr uint8_t kTestPassed = 6;
constexpr uint8_t kPictoBox = 0x23, kDeluxePictoBox = 0x26, kDeluxeGetBit = 0x02;

inline uint32_t load_be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
inline void store_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24), p[1] = uint8_t(v >> 16), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v);
}

// the GameCube-only state: test passed, black-and-white Picto Box, no Deluxe Picto Box
inline bool stuck(const uint8_t* save) {
    return (save[kLenzoReg] & kLenzoRegMask) >= kTestPassed && save[kCameraItem] == kPictoBox &&
           !(save[kCameraGetItem] & kDeluxeGetBit);
}

// One file block read from cking.sav (`block`, kSaveBlock bytes): when its checksum is valid and it
// is stuck, gives the Deluxe Picto Box and writes the matching checksum. True if the block changed.
// A damaged or unused Quest Log stays as it is (an unused one has no Picto Box).
inline bool repair(uint8_t* block) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < kSaveSummed; i++) sum += block[i];
    if (load_be32(block + kSaveSummed) != sum || load_be32(block + kSaveSummed + 4) != kSaveSummed * 0xFFFFFFFFu - sum)
        return false;
    if (!stuck(block)) return false;
    sum -= block[kCameraItem] + block[kCameraGetItem];
    block[kCameraItem] = kDeluxePictoBox;
    block[kCameraGetItem] |= kDeluxeGetBit;
    sum += block[kCameraItem] + block[kCameraGetItem];
    store_be32(block + kSaveSummed, sum);
    store_be32(block + kSaveSummed + 4, kSaveSummed * 0xFFFFFFFFu - sum);
    return true;
}

}  // namespace picto_quest
