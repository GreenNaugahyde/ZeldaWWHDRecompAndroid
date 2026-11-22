// Loot swords (notes/loot.md): a sword won in an online quest, switchable, with its own model in Link's hand and its
// own strength. This part: the strength. Every sword swing's attack power goes through daPy_lk_c::setAtParam
// (023E5600: r4 the attack type, r5 its power; the game doubles it there after Grandma's soup, then writes it into
// Link's 3 attack shapes as a byte): for a sword attack (type 2) it is scaled by the loot sword's percentage first.
// 100 = the game's own sword. Set by the sword switch (set_loot_power); for tests WWHD_LOOT_POWER (percent).
#include <algorithm>
#include <atomic>
#include <cstdlib>

#include <cmath>
#include <cstring>

#include "mods.h"
#include "coop_names.h"
#include "../guest_addr.h"
#include "../runtime.h"

namespace mods {
namespace {
std::atomic<int> g_power{[] {
    const char* e = getenv("WWHD_LOOT_POWER");
    return e ? std::clamp(atoi(e), 10, 1000) : 100;
}()};
}  // namespace
void set_loot_power(int percent) { g_power = std::clamp(percent, 10, 1000); }
int loot_power() { return g_power; }

// Reach: the sword's hit shapes run from its root joint to its tip, the tip worked out each frame as the root joint
// times a constant (daPy_lk_c's static msword_top {57, 48, 0}, right after the Hero's Sword's nsword_top {45, -31, 0};
// USA .rodata 10035B00 / 10035B0C). Lengthened for a longer loot sword: the hits, the swing's trail and the sparks
// reach out to its tip. Only the Master Sword's (the loot swords take its place).
namespace {
float g_reach = 1.0f, g_tilt = 0.0f;
uint32_t g_top = 0;  // msword_top in guest memory (0: not looked for yet / not found)
bool g_topTried = false;
uint32_t sword_top() {
    if (g_top || g_topTried) return g_top;
    g_topTried = true;
    static const float want[6] = {45, -31, 0, 57, 48, 0};
    uint8_t pat[24];
    for (int i = 0; i < 6; i++) {
        uint32_t u;
        memcpy(&u, &want[i], 4);
        pat[4 * i] = (uint8_t)(u >> 24), pat[4 * i + 1] = (uint8_t)(u >> 16), pat[4 * i + 2] = (uint8_t)(u >> 8), pat[4 * i + 3] = (uint8_t)u;
    }
    const uint32_t a = GD(0x10035B00);
    if (memcmp(mem::ptr(a), pat, 24) == 0) return g_top = a + 12;
    // (the release's own place, if the data map doesn't know it: looked for in the game's data)
    for (uint32_t s = 0x10000000; s + 24 <= 0x10400000; s += 4)
        if (memcmp(mem::ptr(s), pat, 24) == 0) {
            LOG("[loot] the sword's tip constant at %08X", s + 12);
            return g_top = s + 12;
        }
    LOG("[loot] the sword's tip constant not found: the reach stays the game's");
    return 0;
}
void put_f(uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint8_t* p = mem::ptr(a);
    p[0] = (uint8_t)(u >> 24), p[1] = (uint8_t)(u >> 16), p[2] = (uint8_t)(u >> 8), p[3] = (uint8_t)u;
}
}  // namespace
float get_f(uint32_t a) {
    const uint8_t* p = mem::ptr(a);
    const uint32_t u = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
    float f;
    memcpy(&f, &u, 4);
    return f;
}
// (each frame: a save state may have brought the game's constants back); the Hero's Sword's tip (nsword_top, just
// before) too: a loot sword takes its place as well
// (tilt: degrees the sword is turned up in the hand, the tips turned with it round the joint's z axis, as the mesh is)
void set_loot_reach(float factor, float hero, float tilt) {
    factor = std::clamp(factor, 0.5f, 4.0f), hero = std::clamp(hero, 0.5f, 4.0f);
    if (factor == 1.0f && hero == 1.0f && tilt == 0.0f && !g_top) return;  // (never changed: nothing to put back)
    const uint32_t a = sword_top();
    if (!a) return;
    const float t = tilt * 3.14159265f / 180.0f, cs = std::cos(t), sn = std::sin(t);
    const float mx = 57.0f * factor, my = 48.0f * factor, hx = 45.0f * hero, hy = -31.0f * hero;
    const float m[2] = {mx * cs - my * sn, mx * sn + my * cs}, h[2] = {hx * cs - hy * sn, hx * sn + hy * cs};
    if (get_f(a) == m[0] && get_f(a + 4) == m[1] && get_f(a - 12) == h[0] && get_f(a - 8) == h[1]) return;
    put_f(a, m[0]), put_f(a + 4, m[1]);
    put_f(a - 12, h[0]), put_f(a - 8, h[1]);
    if (factor != g_reach || tilt != g_tilt) LOG("[loot] sword reach x%.2f (Hero's Sword x%.2f), tilted %.1f degrees", factor, hero, tilt);
    g_reach = factor, g_tilt = tilt;
}
float loot_reach() { return g_reach; }
}  // namespace mods

extern "C" void f_023E5600_orig(Cpu* c);  // daPy_lk_c::setAtParam(type, atp, spl, se, hitMark, cutType, radius)
extern "C" void hook_023E5600(Cpu* c) {
    const int p = mods::loot_power();
    if (c->r[4] == 2 && p != 100) {  // (AT_TYPE_SWORD)
        const int atp = (int)(c->r[5] & 0xFF);
        // (at least 1 more for a stronger sword; at most 127: the soup's doubling after must still fit the byte)
        int scaled = (atp * p + 50) / 100;
        if (p > 100) scaled = std::max(scaled, atp + 1);
        c->r[5] = (uint32_t)std::clamp(scaled, 1, 127);
        static int said = -1;  // (logged once per power: the hook runs for every swing)
        if (said != (atp << 8 | (int)c->r[5])) said = atp << 8 | (int)c->r[5], LOG("[loot] sword power %d -> %u (%d%%)", atp, c->r[5], p);
    }
    f_023E5600_orig(c);
}

// Loot armour. Defence: every change of Link's hearts goes through daPy_lk_c::setDamagePoint (023F51D4): damage (f1 < 0)
// is lowered by the armour's percentage; what is left of a quarter heart is carried to the next hit, so a weak hit
// still counts in the end.
namespace mods {
namespace {
std::atomic<int> g_defence{0};
std::atomic<bool> g_helmet{false}, g_chest{false};
double g_carry = 0;
}  // namespace
void set_loot_worn(bool helmet, bool chest, int defencePercent) {
    g_helmet = helmet, g_chest = chest;
    if (g_defence.exchange(std::clamp(defencePercent, 0, 75)) != std::clamp(defencePercent, 0, 75))
        LOG("[loot] armour: defence %d %%", std::clamp(defencePercent, 0, 75));
}
}  // namespace mods

extern "C" void f_023F51D4_orig(Cpu* c);
extern "C" void hook_023F51D4(Cpu* c) {
    const int d = mods::g_defence;
    if (d > 0 && c->f[1].ps0 < 0) {
        const double want = -c->f[1].ps0 * (100 - d) / 100.0 + mods::g_carry;
        const double take = std::floor(want);
        mods::g_carry = want - take;
        c->f[1].ps0 = -take;
    }
    f_023F51D4_orig(c);
}

// The armour's models. Link's draw (023D9820) notes each model it has entered here (023D9340); once his hands are noted
// (his normal draw, the place where the game enters the casual clothes' hair) what he wears is entered too, straight
// through entryDLSetLight's original (the co-op hook would take them for his own in the pose it sends).
// The hair's model follows his head by the game's own doing; pring's first joint is put on his chest here.
extern "C" void f_023D9340_orig(Cpu* c);
extern "C" void f_023D8DF0_orig(Cpu* c);
namespace {
constexpr uint32_t kLinkCL = 0x448, kLinkHair = 0x44C, kLinkHands = 0xCF4, kLinkRing = 0x4434;
constexpr uint32_t kChestJoint = 4;  // chest_jnt
uint32_t rd32(uint32_t a) {
    const uint8_t* p = mem::ptr(a);
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
uint8_t rd8(uint32_t a) { return *mem::ptr(a); }
uint32_t g_clModel = 0, g_clLink = 0, g_play = 0;  // Link's body model, Link and the game's play data (seen at his draw)
void put_f_be(uint32_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    uint8_t* p = mem::ptr(a);
    p[0] = (uint8_t)(u >> 24), p[1] = (uint8_t)(u >> 16), p[2] = (uint8_t)(u >> 8), p[3] = (uint8_t)u;
}
void enter(Cpu* c, uint32_t link, uint32_t model) {
    c->r[3] = link, c->r[4] = model, c->r[5] = 0;
    f_023D8DF0_orig(c);
}
void draw_armor(Cpu* c, uint32_t link) {
    const bool helmet = mods::g_helmet, chest = mods::g_chest;
    if (guest_call(c, GC(0x023D85A4), {link})) return;  // (checkCaughtShapeHide)
    const uint32_t idx = rd32(link + 0x69BC);
    const uint32_t play = guest_call(c, GC(0x025200D4));
    if (rd32(play + idx * 0x34 + 0x5B00) & 0x20) return;  // (first person: as the game's hair)
    const bool casual = rd32(link + 0x3BC) & 8;
    if (helmet && !casual) enter(c, link, rd32(link + kLinkHair));  // (in casual clothes the game enters it itself)
    if (chest) {
        const uint32_t cl = rd32(rd32(link + kLinkCL) + 0x2C), ring = rd32(link + kLinkRing), block = rd32(ring + 0x2C);
        const uint32_t from = rd32(cl + 0x10) + 48 * kChestJoint, to = rd32(block + 0x10) + 48;
        memcpy(mem::ptr(to), mem::ptr(from), 48);
        // (with the Power Bracelets on the game enters this model itself, right after)
        if (rd8(rd32(GD(0x101F84DC)) + 0x30) != 0x28) enter(c, link, ring);
    }
}
}  // namespace
namespace mods { void loot_item_draw(Cpu* c, uint32_t link); }
extern "C" void hook_023D9340(Cpu* c) {
    const uint32_t link = c->r[3], model = c->r[5];
    f_023D9340_orig(c);
    g_clModel = rd32(link + kLinkCL), g_clLink = link;
    if (model && model == rd32(link + kLinkHands)) {
        const Cpu saved = *c;
        mods::loot_item_draw(c, link);  // (a loot item in real use: loot_item.cpp)
        *c = saved;
    }
    if ((mods::g_helmet || mods::g_chest || !g_play) && model && model == rd32(link + kLinkHands)) {
        const Cpu saved = *c;  // (its callers know this small function leaves their registers alone)
        if (!g_play) g_play = guest_call(c, GC(0x025200D4));
        if (mods::g_helmet || mods::g_chest) draw_armor(c, link);
        *c = saved;
    }
}

// An outfit at its own proportions. Link's body is posed by J3DModel::calc (027F4D5C); straight after, before his hands, sword
// and hair are placed from it, each joint's matrix is made again from its parent's with its distance from the parent stretched
// (the character's bone length over Link's). The rotations stay Link's animation; the body is raised so the feet stay on the ground.
namespace {
constexpr int kLinkJoints = 42;
constexpr uint32_t kSwimFirst = 0x35, kSwimLast = 0x37;  // daPy_lk_c: swim up, wait, move
const int8_t kParent[kLinkJoints] = {-1, 0, 1, 2, 3, 4, 5, 6, 7, 4, 9, 10, 11, 4, 4, 14, 15, 15, 15, 15, 15, 15, 15, 22, 15, 24, 15, 26, 27, 1, 29, 30, 31, 32, 33, 34, 30, 36, 37, 38, 39, 0};
std::atomic<bool> g_stretch{false};
float g_ratio[kLinkJoints], g_raise, g_offset[kLinkJoints * 3];
bool g_hasOffset = false, g_adjusted = false;
bool g_hasHands = false;
float g_handFrame[18], g_handScale = 1;
float g_armOut = 0;  // radians his arms are swung away from the body
float g_len[kLinkJoints];  // his joints' distances from their parents, as the game poses them
// is a fist shown on that side: his fists' model has materials 0..5 for the left hand, 6..9 for the right (each its shape's visible flag)
bool fist_shown(uint32_t hands, int side) {
    namespace j3d = mods::coop::names::j3d;
    const uint32_t data = rd32(hands + j3d::mModelData);
    if (!data) return false;
    const uint32_t mats = rd32(data + j3d::mMaterials), count = rd32(data + j3d::mMaterialNum);
    for (uint32_t i = side ? 6 : 0; i < (side ? 10u : 6u) && i < count; i++) {
        const uint32_t shape = rd32(mats + i * j3d::kMaterial + j3d::mShape);
        if (!shape) {  // (seen on the first frames after a save state's load)
            static std::atomic<bool> told{false};
            if (!told.exchange(true)) LOG("[loot] fists: material %u of %u has no shape (hands %08X data %08X)", i, count, hands, data);
            continue;
        }
        if (rd8(shape + j3d::mVisible)) return true;
    }
    return false;
}
struct M34 { float m[3][4]; };
M34 read34(uint32_t a) {
    M34 r;
    for (int i = 0; i < 12; i++) {
        const uint32_t u = rd32(a + 4 * i);
        memcpy(&r.m[i / 4][i % 4], &u, 4);
    }
    return r;
}
void write34(uint32_t a, const M34& r) {
    for (int i = 0; i < 12; i++) put_f_be(a + 4 * i, r.m[i / 4][i % 4]);
}
M34 mul(const M34& a, const M34& b) {
    M34 r;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
        r.m[i][3] = a.m[i][0] * b.m[0][3] + a.m[i][1] * b.m[1][3] + a.m[i][2] * b.m[2][3] + a.m[i][3];
    }
    return r;
}
bool invert(const M34& a, M34& out) {
    const float* x = a.m[0], *y = a.m[1], *z = a.m[2];
    const float c0 = y[1] * z[2] - y[2] * z[1], c1 = y[2] * z[0] - y[0] * z[2], c2 = y[0] * z[1] - y[1] * z[0];
    const float det = x[0] * c0 + x[1] * c1 + x[2] * c2;
    if (std::fabs(det) < 1e-9f) return false;
    const float d = 1.0f / det;
    out.m[0][0] = c0 * d, out.m[0][1] = (x[2] * z[1] - x[1] * z[2]) * d, out.m[0][2] = (x[1] * y[2] - x[2] * y[1]) * d;
    out.m[1][0] = c1 * d, out.m[1][1] = (x[0] * z[2] - x[2] * z[0]) * d, out.m[1][2] = (x[2] * y[0] - x[0] * y[2]) * d;
    out.m[2][0] = c2 * d, out.m[2][1] = (x[1] * z[0] - x[0] * z[1]) * d, out.m[2][2] = (x[0] * y[1] - x[1] * y[0]) * d;
    for (int i = 0; i < 3; i++) out.m[i][3] = -(out.m[i][0] * a.m[0][3] + out.m[i][1] * a.m[1][3] + out.m[i][2] * a.m[2][3]);
    return true;
}
void stretch_joints(uint32_t model) {
    const uint32_t block = rd32(model + 0x2C), mtx = block ? rd32(block + 0x10) : 0;
    if (!mtx) return;
    M34 was[kLinkJoints], now[kLinkJoints];
    for (int j = 0; j < kLinkJoints; j++) was[j] = read34(mtx + 48 * j);
    // looking through his eyes (aiming a bow or the like; the game hides Link's head then): the whole outfit folded away, as
    // it would stand round the camera, his joints as the game made them for what is held
    if (g_clLink && g_play && (rd32(g_play + rd32(g_clLink + 0x69BC) * 0x34 + 0x5B00) & 0x20)) {
        for (int j = 0; j < kLinkJoints; j++) {
            if (j == 0 || j == 1 || j == 8 || j == 12 || j == 13 || (j >= 19 && j <= 21) || j == 41) continue;
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 3; k++) was[j].m[i][k] = 0;
            write34(mtx + 48 * j, was[j]);
        }
        return;
    }
    for (int j = 0; j < kLinkJoints; j++) {
        const int p = kParent[j];
        if (p < 0) {
            now[j] = was[j];
            continue;
        }
        M34 inv;
        g_len[j] = std::sqrt(std::pow(was[j].m[0][3] - was[p].m[0][3], 2.0f) + std::pow(was[j].m[1][3] - was[p].m[1][3], 2.0f) + std::pow(was[j].m[2][3] - was[p].m[2][3], 2.0f));
        if (!invert(was[p], inv)) { now[j] = was[j]; continue; }
        M34 local = mul(inv, was[j]);
        for (int i = 0; i < 3; i++) local.m[i][3] *= g_ratio[j];
        now[j] = mul(now[p], local);
        // (the upper arms swung out round the chest's forward axis: a wide character's arms clear of its body)
        if ((j == 6 || j == 10) && g_armOut > 0) {
            const float a[3] = {now[4].m[0][1], now[4].m[1][1], now[4].m[2][1]};
            const float t = j == 6 ? g_armOut : -g_armOut, co = std::cos(t), si = std::sin(t);
            for (int k = 0; k < 3; k++) {
                const float v[3] = {now[j].m[0][k], now[j].m[1][k], now[j].m[2][k]};
                const float d = a[0] * v[0] + a[1] * v[1] + a[2] * v[2];
                const float c[3] = {a[1] * v[2] - a[2] * v[1], a[2] * v[0] - a[0] * v[2], a[0] * v[1] - a[1] * v[0]};
                for (int i = 0; i < 3; i++) now[j].m[i][k] = v[i] * co + c[i] * si + a[i] * d * (1 - co);
            }
        }
    }
    // the lower foot as far above the ground as Link's, plus how much higher its ankle sits (longer legs, bent as his, would not reach)
    if (g_ratio[34] != 1.0f || g_raise != 0.0f) {
        const float dy = std::min(was[34].m[1][3], was[39].m[1][3]) + g_raise - std::min(now[34].m[1][3], now[39].m[1][3]);
        for (int j = 0; j < kLinkJoints; j++) now[j].m[1][3] += dy;
    }
    // hanging, on a ladder, a vine or a rope: his hands where Link's are, so they grip what he holds, the longer body below them
    const uint32_t proc = g_clLink ? rd32(g_clLink + 0x65F0) : 0;
    const bool holding = (proc >= 0x2B && proc <= 0x31) || (proc >= 0x38 && proc <= 0x40) || (proc >= 0x76 && proc <= 0x7F);
    static float blend = 0;
    blend = holding ? std::min(1.0f, blend + 0.2f) : std::max(0.0f, blend - 0.2f);
    if (blend > 0) {
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = blend * ((was[8].m[i][3] + was[12].m[i][3]) - (now[8].m[i][3] + now[12].m[i][3])) / 2;
        for (int j = 0; j < kLinkJoints; j++)
            for (int i = 0; i < 3; i++) now[j].m[i][3] += d[i];
        // (a pose with the hands at different heights, the first grab of a ledge: each hand on Link's, its arm bent to reach it)
        static const int kArm[2][3] = {{6, 7, 8}, {10, 11, 12}};
        for (auto& arm : kArm) {
            float e[3];
            for (int i = 0; i < 3; i++) e[i] = blend * (was[arm[2]].m[i][3] - now[arm[2]].m[i][3]);
            for (int k = 0; k < 3; k++)
                for (int i = 0; i < 3; i++) now[arm[k]].m[i][3] += e[i] * (k + 1) / 3.0f;
        }
    }
    // swimming: not raised off the ground: his chest where Link's is across, his shoulders at the water
    const bool swimming = (proc >= kSwimFirst && proc <= kSwimLast);
    static float blendSwim = 0;
    blendSwim = swimming ? std::min(1.0f, blendSwim + 0.2f) : std::max(0.0f, blendSwim - 0.2f);
    if (blendSwim > 0) {
        float d[3];
        for (int i = 0; i < 3; i++) d[i] = blendSwim * (was[4].m[i][3] - now[4].m[i][3]);
        // (halfway between his chest and his shoulders at Link's: the long arms in the water, still to be seen: user)
        d[1] = blendSwim * ((was[4].m[1][3] - now[4].m[1][3]) + (was[5].m[1][3] - now[5].m[1][3])) / 2;
        for (int j = 0; j < kLinkJoints; j++)
            for (int i = 0; i < 3; i++) now[j].m[i][3] += d[i];
    }
    // (the root joint carries no mesh; the game centres Link's shadow on it: left at his place, so the feet stay inside the shadow's area)
    now[0] = was[0];
    static uint32_t saidProc = ~0u;
    if (holding && proc != saidProc) saidProc = proc, LOG("[loot] outfit: hands kept on what he holds (state %X)", proc);
    for (int j = 0; j < kLinkJoints; j++) write34(mtx + 48 * j, now[j]);
}
}  // namespace
namespace mods {
void set_loot_skeleton(const float* ratio, int count, float raise, const float* offset, float armAngle, const float* handFrame, float handScale) {
    if (!ratio || count != kLinkJoints) {
        g_stretch = false;
        return;
    }
    memcpy(g_ratio, ratio, sizeof g_ratio);
    g_raise = raise;
    g_armOut = armAngle * 3.14159265f / 180.0f;
    g_hasHands = handFrame != nullptr, g_handScale = handScale;
    if (handFrame) memcpy(g_handFrame, handFrame, sizeof g_handFrame);
    g_hasOffset = offset != nullptr;
    if (offset) memcpy(g_offset, offset, sizeof g_offset);
    g_stretch = true;
}
// Link's draw: each skinned joint moved along its own axes, so the skin (built on Link's rest pose) bends about the character's joints
// (for the drawing only: the game's logic has read the joints already; the next pose makes them again)
void loot_before_draw(uint32_t link) {
    g_clLink = link, g_clModel = rd32(link + kLinkCL);  // (this draw's own: what an earlier draw saw is stale after a save state's load)
    if (!g_stretch.load(std::memory_order_relaxed) || !g_hasOffset || !g_clModel || g_adjusted) return;
    if (g_clLink && g_play && (rd32(g_play + rd32(g_clLink + 0x69BC) * 0x34 + 0x5B00) & 0x20)) return;
    const uint32_t block = rd32(g_clModel + 0x2C), mtx = block ? rd32(block + 0x10) : 0;
    if (!mtx) return;
    g_adjusted = true;
    // the game bends his legs again after the pose is made (its foot placement, with Link's own shin and thigh lengths): the leg joints made
    // again from what it left, each at the character's length from its parent
    {
        static const int kLeg[10] = {31, 32, 33, 34, 35, 36, 37, 38, 39, 40};
        M34 cur[kLinkJoints], fresh[kLinkJoints];
        for (int j = 0; j < kLinkJoints; j++) fresh[j] = cur[j] = read34(mtx + 48 * j);
        for (int j : kLeg) {
            const int p = kParent[j];
            M34 inv;
            if (!invert(cur[p], inv)) continue;
            M34 local = mul(inv, cur[j]);
            const float len = std::sqrt(local.m[0][3] * local.m[0][3] + local.m[1][3] * local.m[1][3] + local.m[2][3] * local.m[2][3]);
            if (len > 1e-4f)
                for (int i = 0; i < 3; i++) local.m[i][3] *= g_len[j] * g_ratio[j] / len;
            fresh[j] = mul(fresh[p], local);
            write34(mtx + 48 * j, fresh[j]);
        }
    }
    for (int j = 0; j < kLinkJoints; j++) {
        if (j == 0 || j == 1 || j == 8 || j == 12 || j == 13 || (j >= 19 && j <= 21) || j == 41) continue;
        M34 a = read34(mtx + 48 * j);
        for (int i = 0; i < 3; i++) a.m[i][3] += a.m[i][0] * g_offset[3 * j] + a.m[i][1] * g_offset[3 * j + 1] + a.m[i][2] * g_offset[3 * j + 2];
        write34(mtx + 48 * j, a);
    }
    // the character's own hands, on two joints of the hair nothing else uses: on his wrists, gone while the game shows a fist there
    if (g_hasHands) {
        const uint32_t hands = g_clLink ? rd32(g_clLink + kLinkHands) : 0;
        for (int side = 0; side < 2; side++) {
            const M34 w = read34(mtx + 48 * (side ? 12 : 8));
            M34 a = w;
            for (int i = 0; i < 3; i++)
                for (int k = 0; k < 3; k++) a.m[i][k] = (w.m[i][0] * g_handFrame[side * 9 + k] + w.m[i][1] * g_handFrame[side * 9 + 3 + k] + w.m[i][2] * g_handFrame[side * 9 + 6 + k]);
            if (hands && fist_shown(hands, side))
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++) a.m[i][k] = 0;
            write34(mtx + 48 * (side ? 24 : 22), a);
        }
    }
}
// his fists as big as the character's hands, turning about the wrist
void loot_before_entry(uint32_t model) {
    // (a whole character does not wear Link's Power Bracelets: folded away, unless the breastplate sits on that model)
    if (g_stretch.load(std::memory_order_relaxed) && g_hasHands && !mods::g_chest && g_clLink && model == rd32(g_clLink + kLinkRing)) {
        const uint32_t b = rd32(model + 0x2C), m2 = b ? rd32(b + 0x10) : 0;
        if (m2)
            for (int j = 1; j <= 2; j++) {
                M34 a = read34(m2 + 48 * j);
                for (int i = 0; i < 3; i++)
                    for (int k = 0; k < 3; k++) a.m[i][k] = 0;
                write34(m2 + 48 * j, a);
            }
        return;
    }
    if (!g_stretch.load(std::memory_order_relaxed) || !g_hasHands || g_handScale == 1.0f || !g_clLink || model != rd32(g_clLink + kLinkHands)) return;
    const uint32_t block = rd32(model + 0x2C), mtx = block ? rd32(block + 0x10) : 0;
    if (!mtx) return;
    for (int j = 1; j <= 10; j++) {
        M34 a = read34(mtx + 48 * j);
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) a.m[i][k] *= g_handScale;
        write34(mtx + 48 * j, a);
    }
}
// (from model_probe.cpp's J3DModel::calc hook, right after the calc)
void loot_after_calc(uint32_t model) {
    if (g_stretch.load(std::memory_order_relaxed) && model == g_clModel && g_clModel) g_adjusted = false, stretch_joints(model);
}
}  // namespace mods
