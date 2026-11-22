// Upstream's mods/coop_pose.cpp (online co-op: Link's pose and the other players' puppets), built through this file
// so the fork can add one thing without changing upstream's file (online/compat/README.md): each puppet's copies of
// Link's models are made from that player's own player model when this game has it (online/android_models.cpp).
//
// How: the file's guest calls go through android_guest_call (a macro over the name for this file only). It passes
// every call on unchanged, except mDoExt_J3DModel__create (025E38E0) of one of Link's model data while a puppet's
// own pool heap is the current heap (make_kit): that data is swapped for the same file of the player's model archive.
// (the JIT stubs it calls, named here for the stub scan: f_023D4BB8_orig f_023D5548_orig f_023D8454_orig f_023D8DF0_orig
// f_023D9820_orig f_025D4654_orig f_025E2DE0_orig f_025E2E5C_orig f_025E38E0_orig f_026066C4_orig f_027F50F8_orig
// f_027F583C_orig)
#include <initializer_list>

#include "guest_addr.h"
#include "runtime.h"

namespace mods::coop {
namespace {
uint32_t android_guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> a = {});
}  // namespace
}  // namespace mods::coop

#define guest_call android_guest_call
#include "../mods/coop_pose.cpp"
#undef guest_call

namespace android_models {
uint32_t kit_data(Cpu* c, int slot, uint32_t link_data);
}

namespace mods::coop {
namespace {
uint32_t g_current_heap = 0;  // the heap the last JKRHeap::becomeCurrentHeap of this file made current
uint32_t android_guest_call(Cpu* c, uint32_t fn, std::initializer_list<uint32_t> a) {
    if (fn == GC(0x027EC220) && a.size() == 1) {  // JKRHeap::becomeCurrentHeap
        const uint32_t was = guest_call(c, fn, a);
        g_current_heap = *a.begin();
        return was;
    }
    if (fn == GC(0x025E38E0) && a.size() == 3 && g_current_heap) {  // mDoExt_J3DModel__create(data, flags, diff)
        const uint32_t* v = a.begin();
        for (int i = 0; i < kPuppets; i++)
            if (g_slot_mem[i] && g_slot_mem[i] == g_current_heap) {  // (a pool heap is made at its block's start)
                if (const uint32_t data = android_models::kit_data(c, i, v[0])) return guest_call(c, fn, {data, v[1], v[2]});
                break;
            }
    }
    return guest_call(c, fn, a);
}
}  // namespace
}  // namespace mods::coop

// the puppet slots' players (upstream's coop_sync.cpp: slot = id < my id ? id : id - 1)
namespace android_models {
bool kit_made(int slot) { return slot >= 0 && slot < mods::coop::kPuppets && mods::coop::g_kit[slot].heap; }
void kit_again(Cpu* c, int slot) {  // its copies made again at the next show (another model)
    if (slot < 0 || slot >= mods::coop::kPuppets) return;
    mods::coop::free_kit(c, slot);
    mods::coop::g_kit[slot].models.clear(), mods::coop::g_slot_mem[slot] = 0;
}
}  // namespace android_models
