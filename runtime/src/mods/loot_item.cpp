// Loot items on X / Y / R in real use (android LootQuick, notes/loot.md): the press creates one of the game's own carried
// weapons (daBoko_c, the enemies' sticks, machetes, ... with its own swings, hits and throw) and Link picks it up as he
// would off the ground; the weapon itself is kept hidden and the loot model (Link's sword models, which the loot layer
// has just made into the thing: player_models.cpp) is drawn in his hand where it would be. When it leaves his hands
// (thrown, dropped, or the press again) the weapon is deleted: the thing goes back on its button.
// No hooks of its own: the frame hook (interp.cpp) runs it and the hook on his hands' entry draws (loot_sword.cpp).
#include <atomic>
#include <cmath>
#include <cstdlib>

#include "coop_items.h"
#include "coop_names.h"
#include "mods.h"
#include "../guest_addr.h"
#include "../runtime.h"
#include "../savestate.h"

extern "C" void f_023D8DF0_orig(Cpu* c);

namespace mods {
namespace {
namespace py = coop::names::py;
namespace j3d = coop::names::j3d;
namespace ac = coop::names::ac;
// daBoko_c (HD, read from its create and checkNoDraw): the fpcNm, its model, its mode (3: carried by the player), its "not drawn" byte
constexpr int16_t kBokoProc = 0x1CF;
constexpr uint32_t kBokoModel = 0x3B4, kBokoHide = 0x431, kLinkCL = 0x448;
constexpr int kHandJoint = 12;  // the hand a sword is held in
constexpr uint16_t kEquipBoko = 0x101;  // daPyItem_BOKO_e
constexpr uint32_t kFileBlade = 37, kFileHilt = 36;  // swms, swgripms in Link's archive (measured)
constexpr int kGiveUp = 150;  // steps until he has it

std::atomic<int> g_want{-1}, g_state{0};
std::atomic<bool> g_drop{false};
uint32_t g_id = 0, g_link = 0;
int g_waited = 0;
unsigned g_loads = ~0u;
uint32_t g_models[2] = {0, 0};

uint32_t actor(Cpu* c) { return g_id ? guest_call(c, GC(0x025DE50C), {g_id}) : 0; }  // fopAcM_SearchByID
void remove(Cpu* c) {
    if (g_id && actor(c)) guest_call(c, GC(0x025D57E4), {g_id});  // fopAcM_delete
    g_id = 0;
}

uint32_t make_model(Cpu* c, uint32_t file) {
    static const uint32_t arc = [] {  // "Link", as initModel passes it (the text, then the string class's vtable)
        const uint32_t b = mem::host_alloc(8);
        st32(b, GD(0x101CEB48)), st32(b + 4, GD(0x10034B24));
        return b;
    }();
    const uint32_t data = guest_call(c, GC(0x026066C4), {ld32(GD(0x101F4F28)), arc, file});  // dComIfG_getObjectRes
    const uint32_t heap = data ? guest_call(c, GC(0x025E3528), {0, 0}) : 0;                  // mDoExt_createSolidHeapFromGame
    if (!heap) return 0;
    const uint32_t was = guest_call(c, GC(0x027EC220), {heap});  // JKRHeap::becomeCurrentHeap
    const uint32_t model = guest_call(c, GC(0x025E38E0), {data, 0x80000, 0x11000002});  // mDoExt_J3DModel__create
    guest_call(c, GC(0x027EC220), {was});
    guest_call(c, GC(0x025E3678), {heap});  // mDoExt_adjustSolidHeap
    if (!model) guest_call(c, GC(0x025E3868), {heap});
    return model;
}

void step(Cpu* c) {
    const unsigned lc = ss::load_count();
    uint32_t link = link_actor();
    if (lc != g_loads || link != g_link) {  // (a save state or a new Link: nothing we made is there any more)
        g_loads = lc, g_link = link;
        g_id = 0, g_state = 0, g_models[0] = g_models[1] = 0;
    }
    const int want = g_want.exchange(-1);
    const bool drop = g_drop.exchange(false);
    if (!link) return;
    const uint32_t equip = ld16(link + py::mEquipItem);
    if (g_state == 0) {
        if (want < 0 || want > 5) return;
        if (!g_models[0]) g_models[0] = make_model(c, kFileBlade), g_models[1] = make_model(c, kFileHilt);
        float pos[3];
        const float yaw = (float)((int16_t)ld16(link + ac::kAngleY) * M_PI / 32768);
        for (int i = 0; i < 3; i++) pos[i] = (float)ldf32(link + ac::kPos + 4 * i);
        pos[0] += 30 * std::sin(yaw), pos[2] += 30 * std::cos(yaw);
        const uint32_t a = coop::items::spawn(c, link, kBokoProc, (uint32_t)want, pos);
        if (!a) return;
        g_id = ld32(a + 4), g_waited = 0, g_state = 1;
        st8(a + kBokoHide, 1);
        LOG("[loot] item: weapon type %d made (models %08X %08X)", want, g_models[0], g_models[1]);
        return;
    }
    const uint32_t a = actor(c);
    if (g_state == 1) {
        if (!a || ++g_waited > kGiveUp) return remove(c), g_state = 0, void();
        st8(a + kBokoHide, 1);
        if (equip == kEquipBoko) g_state = 2, LOG("[loot] item: in his hand");
        return;
    }
    if (!a || equip != kEquipBoko || drop) {  // (thrown or let go: back on its button)
        remove(c);
        g_state = 0;
        LOG("[loot] item: back on its button");
        return;
    }
    st8(a + kBokoHide, 1);
}
}  // namespace

void loot_item_take(int type) { g_want = type; }
void loot_item_drop() { g_drop = true; }
int loot_item_state() { return g_state.load(std::memory_order_relaxed); }

void loot_item_service(Cpu* c) {
    if (g_state == 0 && g_want.load(std::memory_order_relaxed) < 0 && !g_drop.load(std::memory_order_relaxed)) return;
    const Cpu saved = *c;
    step(c);
    *c = saved;
}

// the loot model in his hand, where the weapon would be drawn (with his hands' entry in his draw)
void loot_item_draw(Cpu* c, uint32_t link) {
    if (g_state != 2 || !g_models[0]) return;
    const uint32_t a = actor(c);
    const uint32_t m = a ? ld32(a + kBokoModel) : 0, mb = m ? ld32(m + j3d::mpMtxBuffer) : 0, bk = mb ? ld32(mb + j3d::mpAnmMtx) : 0;
    const uint32_t cl = ld32(link + kLinkCL), block = cl ? ld32(cl + j3d::mpMtxBuffer) : 0, base = block ? ld32(block + j3d::mpAnmMtx) : 0;
    if (!bk || !base) return;
    // turned as the game's weapon is turned (its length along its -z, its width along -x), placed at his hand (the sword models lie in the hand's frame, their
    // length along kAx: hd_sword.cpp); the weapon's own place is the middle of its stick
    static const double kAx[3] = {1.0 / std::hypot(1.0, 0.84), 0.84 / std::hypot(1.0, 0.84), 0}, kAcross[3] = {-0.84 / std::hypot(1.0, 0.84), 1.0 / std::hypot(1.0, 0.84), 0}, kThin[3] = {0, 0, 1};
    const uint32_t hand = base + 48 * kHandJoint;
    float out[12];
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++)
            out[r * 4 + col] = (float)(-ldf32(bk + 4 * (r * 4 + 0)) * kAcross[col] + ldf32(bk + 4 * (r * 4 + 1)) * kThin[col] - ldf32(bk + 4 * (r * 4 + 2)) * kAx[col]);
        out[r * 4 + 3] = (float)ldf32(hand + 4 * (r * 4 + 3));
    }
    for (uint32_t model : g_models) {
        const uint32_t jnt = model ? ld32(model + j3d::mpMtxBuffer) : 0, mtx = jnt ? ld32(jnt + j3d::mpAnmMtx) : 0;
        if (!mtx) continue;
        const uint32_t joints = ld16(jnt + j3d::kJointCount);
        for (uint32_t j = 0; j < joints && j < 8; j++)
            for (uint32_t e = 0; e < 12; e++) stf32(mtx + 48 * j + 4 * e, out[e]);
        for (uint32_t e = 0; e < 12; e++) stf32(model + j3d::mBaseTransformMtx + 4 * e, out[e]);
        c->r[3] = link, c->r[4] = model, c->r[5] = 0;
        f_023D8DF0_orig(c);
    }
}
}  // namespace mods
