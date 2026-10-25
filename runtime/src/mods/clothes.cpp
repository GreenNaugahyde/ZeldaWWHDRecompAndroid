// Link's clothes (Game menu): the story's choice, the Hero's clothes or the casual clothes (the lobster shirt)
// at any time.
//
// As on the GameCube (decomp daPy_lk_c::playerInit, d_a_player_main.cpp) the casual clothes are Link's model
// with another texture and a flag: daPyFlg1_CASUAL_CLOTHES, in WWHD bit 8 of Link + 0x3BC (it also hides his
// cap and belt buckle and draws his hair). Link's creation (02410BE8, at 024126B0) sets it unless the Hero's
// clothes are his (event bit 0x2A80) and the second quest's byte (save + 0x1C0) is 0, and swaps the textures to
// match. WWHD keeps up to 10 texture headers of 0x24 bytes for the clothes: the model's list at Link + 0x454
// (count, at +8 the headers' addresses) and the other clothes' copies at Link + 0x48C; the cutscene that changes
// his clothes (023F25FC) swaps the two and flips the flag, nothing more (the renderer reads the headers at each
// draw). This does the same: each frame start, with a choice made, Link in play, no event and the flag not the
// choice. A new room makes Link again with the story's clothes: changed back at once, quietly. A change of the
// choice shows a puff of smoke at Link first (dPa_name::ID_AK_JN_SHOTENPOFU 0x1A, a spirit orb vanishing; WWHD's
// dPa_control_c::set 025A847C, the particle control at play + 0x5AB0, as d_a_disappear's set_disappear) and
// changes his clothes 10 frames later, inside the cloud (the app sends a menu's choice once the menu has closed:
// the game runs behind it).
// Test switches: WWHD_CLOTHES=1 (Hero's) / 2 (casual), WWHD_CLOTHES_PUFF, WWHD_CLOTHES_TINT (below).
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "mods.h"
#include "../guest_addr.h"
#include "../release.h"
#include "../runtime.h"
#include "../savestate.h"

namespace mods {
namespace {
constexpr uint32_t kFlags = 0x3BC, kCasual = 0x8;  // mNoResetFlg1, daPyFlg1_CASUAL_CLOTHES
constexpr uint32_t kTexList = 0x454, kTexOther = 0x48C, kTexHeader = 0x24, kTexMax = 10;
constexpr uint32_t kPos = 0x314;  // current.pos
uint16_t g_puff_id = 0x1A;  // ID_AK_JN_SHOTENPOFU, at once (WWHD_CLOTHES_PUFF=[group:]hex: another one; 14 ID_AK_JN_SIBOUPOFU starts ~1.3 s late)
uint32_t g_puff_group = 0, g_puff_alpha = 0xFF;  // dPtclGroup_Normal_e
// its colours (GXColor RGBA: prim, env; 0 the particle's own); WWHD_CLOTHES_TINT=RRGGBBAA,RRGGBBAA
uint32_t g_puff_prim = 0, g_puff_env = 0;  // (WWHD ignores them for this emitter, tested)

std::atomic<int> g_mode{0};       // 0 the story's, 1 Hero's, 2 casual
std::atomic<bool> g_puff{false};  // the choice changed: a puff with the next change

bool guest_ptr(uint32_t a) { return a >= 0x10000000 && a < 0x50000000; }

// the other clothes' texture headers into the model and the model's into the copies; the flag flipped
bool swap(uint32_t link) {
    uint32_t list = ld32(link + kTexList);
    if (!guest_ptr(list)) return false;
    uint32_t n = ld32(list), heads = ld32(list + 8);
    if (n == 0 || n > kTexMax || !guest_ptr(heads)) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t cur = ld32(heads + 4 * i), other = link + kTexOther + i * kTexHeader;
        if (!guest_ptr(cur)) continue;
        uint8_t tmp[kTexHeader];
        memcpy(tmp, mem::ptr(cur), kTexHeader);
        memcpy(mem::ptr(cur), mem::ptr(other), kTexHeader);
        memcpy(mem::ptr(other), tmp, kTexHeader);
    }
    st32(link + kFlags, ld32(link + kFlags) ^ kCasual);
    return true;
}

// a puff of smoke at Link's middle
void puff(Cpu* c, uint32_t link) {
    uint32_t play = guest_call(c, GC(0x025200D4));
    // the control's offset from d_a_disappear's own load (lwz r3, 0x5AB0(r3)), in case another release moved it
    uint32_t insn = release::code(0x021247F0) ? ld32(release::code(0x021247F0)) : 0;
    uint32_t off = (insn >> 26) == 32 ? (insn & 0xFFFF) : 0x5AB0;
    uint32_t ctrl = guest_ptr(play) ? ld32(play + off) : 0;
    if (!guest_ptr(ctrl)) return;
    // guest_call's frame (0x40 below the stack pointer): the stack arguments at +8, the vectors after them
    uint32_t f = c->r[1] - 0x40, pos = f + 0x18, scale = f + 0x24;
    st32(f + 0x08, 0xFFFFFFFF);  // setupInfo -1
    st32(f + 0x30, g_puff_prim);
    st32(f + 0x34, g_puff_env);
    st32(f + 0x0C, g_puff_prim ? f + 0x30 : 0);  // prim colour
    st32(f + 0x10, g_puff_env ? f + 0x34 : 0);   // env colour
    st32(f + 0x14, 0);           // 2D scale
    st32(pos, ld32(link + kPos));
    st32(pos + 4, f32_as_u32(u32_as_f32(ld32(link + kPos + 4)) + 50.0f));
    st32(pos + 8, ld32(link + kPos + 8));
    for (int i = 0; i < 3; i++) st32(scale + 4 * i, f32_as_u32(1.2f));
    uint32_t emitter = guest_call(c, GC(0x025A847C), {ctrl, g_puff_group, g_puff_id, pos, 0, scale, g_puff_alpha, 0});
    LOG("[clothes] puff of smoke: emitter %08X", emitter);
}
}  // namespace

int link_clothes() { return g_mode; }
void set_link_clothes(int mode) {
    if (mode < 0 || mode > 2) mode = 0;
    if (mode != g_mode.exchange(mode)) g_puff = true;
    LOG("[clothes] %s", mode == 0 ? "the story's" : mode == 1 ? "Hero's clothes" : "casual clothes");
}

// game main thread, frame start (interp.cpp)
void clothes_service(Cpu* c) {
    static bool env = [] {
        if (const char* e = getenv("WWHD_CLOTHES")) set_link_clothes(atoi(e));
        if (const char* e = getenv("WWHD_CLOTHES_TINT")) {
            g_puff_prim = (uint32_t)strtoul(e, nullptr, 16);
            const char* comma = strchr(e, ',');
            g_puff_env = comma ? (uint32_t)strtoul(comma + 1, nullptr, 16) : 0;
        }
        if (const char* e = getenv("WWHD_CLOTHES_PUFF")) {
            const char* colon = strchr(e, ':');
            if (colon) g_puff_group = (uint32_t)atoi(e), g_puff_alpha = g_puff_group >= 2 ? 0xB9 : 0xFF;
            g_puff_id = (uint16_t)strtoul(colon ? colon + 1 : e, nullptr, 16);
        }
        return true;
    }();
    (void)env;
    int mode = g_mode;
    uint32_t link = link_actor();
    // the same Link for a few frames (a new room's Link is set up over more than one)
    static uint32_t seen = 0;
    static int frames = 0;
    if (link != seen) seen = link, frames = 0;
    else if (frames < 10) frames++;
    if (!mode || !link || frames < 10 || ss::front_screen() != 0 || event_mode_now() != 0) return;
    bool casual = ld32(link + kFlags) & kCasual;
    static int puffed = -1;  // frames since the puff: the clothes change once it hides him
    if (casual == (mode == 2)) {
        g_puff = false;
        puffed = -1;
        return;
    }
    if (g_puff.exchange(false)) {
        puff(c, link);
        puffed = 0;
    }
    if (puffed >= 0 && ++puffed < 10) return;  // (the cloud is full after ~0.3 s)
    puffed = -1;
    if (!swap(link)) return;
    LOG("[clothes] Link now in his %s clothes", mode == 2 ? "casual" : "Hero's");
}

}  // namespace mods
