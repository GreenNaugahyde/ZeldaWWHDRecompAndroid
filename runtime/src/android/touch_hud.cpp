// Game state for the touch controls: what A, B and ZR do right now (the game's own action prompts),
// the items on X/Y/R, and whether Link is on the boat, has the sword out or a target locked.
// Not read from the game yet: the addresses of these fields in the HD executable still have to be
// found (dComIfGp_getDoStatus / getRStatus, the select-item slots of the save data, dComIfGp_event...),
// e.g. with tools/decomp against code/cking.rpx. Until then the state is "unknown" (flags = 0) and
// the controls show their default icons and every button.
#include "touch_hud.h"

#include <cstring>

namespace touch_hud {
void state(int32_t out[kSize]) { memset(out, 0, sizeof(int32_t) * kSize); }
}  // namespace touch_hud
