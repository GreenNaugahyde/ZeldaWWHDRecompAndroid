// Game progress (the app's GameProgress.java counts it and the checklist shows it): the player's save data as it is
// now, read on demand by the app (any thread), as touch_state / minimap_state.
//   dSv_save_c as on the GameCube (decomp d_save.h): dSv_player_c at +0x000 (life, items, collect +0xB4, map +0xC4),
//   the stages' dSv_memBit_c table at +0x380 (16 x 0x24: chests, switches, ..., mDungeonItem +0x21), the events at
//   +0x624; then dSv_info_c's working copy of the stage's dSv_memBit_c at +0x778 (written back to the table when the
//   stage is left) and dSv_danBit_c at +0x79C (mStageNo, s8: which table entry the copy belongs to).
// After it the play's __d_timer_info_c (dComIfG_play_c + 0x4A40 on the GameCube, 0x10 later in HD as the player status
// and minigame fields before it: play + 0x5CF0): the time since the timer started, its limit, its mode (2 the boating
// course, 3 the Fire Mountain / Ice Ring timer that goes on across stages, -1 none).
// Last the save file's number (dSv_info_c::mDataNum, +0x1290 after the save as on the GameCube: 0..2, the file select's
// slot it was loaded from and is saved to), so the app can tell the save games apart.
// WWHD_PROGRESS_LOG=1 logs the stage number and dungeon item bytes when they change, and the timer's mode.
#include <cstdlib>
#include <cstring>

#include "mods.h"
#include "../release.h"
#include "../runtime.h"

namespace mods {
namespace {
const release::Data kPlay{0x1046F0B0};
constexpr uint32_t kStartStage = 0x5134, kStageNo = 0x79C, kTimerInfo = 0x5CF0, kDataNum = 0x1290;
}  // namespace

bool save_new_game(uint32_t save) {
    if (!save) return false;
    const uint8_t* ev = (const uint8_t*)mem::ptr(save + 0x624);
    for (int i = 0; i < 0x100; i++)
        if (ev[i] && i != 0x7E && i != 0xBE) return false;
    return true;
}

int progress_data(uint8_t out[kProgressLen]) {
    uint32_t s = player_save();
    char stage[9] = {};
    memcpy(stage, mem::ptr(kPlay + kStartStage), 8);
    if (!s || !stage[0] || !strcmp(stage, "sea_T") || !strcmp(stage, "Name")) return 0;  // (title, file select)
    memcpy(out, mem::ptr(s), kProgressSave);  // (guest memory is big-endian, as the app reads it)
    memcpy(out + kProgressSave, mem::ptr(kPlay + kTimerInfo), 0x10);
    out[kProgressSave + 0x10] = *(const uint8_t*)mem::ptr(s + kDataNum);
    static const bool log = getenv("WWHD_PROGRESS_LOG") != nullptr;
    if (log) {
        static int last = -2;
        static uint8_t lastItems[16];
        uint8_t items[16];
        for (int i = 0; i < 16; i++) items[i] = out[0x380 + 0x24 * i + 0x21];
        int no = (int8_t)out[kStageNo];
        if (no != last || memcmp(items, lastItems, 16)) {
            last = no;
            memcpy(lastItems, items, 16);
            char l[64];
            for (int i = 0; i < 16; i++) snprintf(l + 2 * i, 3, "%02X", items[i]);
            LOG("[progress] stage %s no %d, its copy's dungeon items %02X, table %s", stage, no, out[0x778 + 0x21], l);
        }
        static int lastFile = -1;
        if (out[kProgressSave + 0x10] != lastFile) {
            lastFile = out[kProgressSave + 0x10];
            char l[3 * 0x18 + 1];
            for (int i = 0; i < 0x18; i++) snprintf(l + 3 * i, 4, "%02X ", *(const uint8_t*)mem::ptr(s + 0x1288 + i));
            LOG("[progress] file number %d (save + 0x1288: %s), name %.16s", lastFile, l, (const char*)mem::ptr(s + 0x148 + 0x14));
        }
        static int32_t lastMode = -2;
        int32_t mode = (int32_t)ld32(kPlay + kTimerInfo + 0xC);
        if (mode != lastMode) {
            lastMode = mode;
            LOG("[progress] timer mode %d, %d of %d ms", mode, (int32_t)ld32(kPlay + kTimerInfo + 4), (int32_t)ld32(kPlay + kTimerInfo + 8));
        }
    }
    return kProgressLen;
}
}  // namespace mods
