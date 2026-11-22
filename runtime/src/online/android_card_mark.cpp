// (fork: Android) the online mark on the file select's own save file cards: a save file that plays online (the Online
// page's "Plays online", online/coop_sync_android.cpp) shows a small badge in its card's top right corner.
//
// The card is the layout BtnSaveData_00 (content/Common/Layout), placed three times by SaveData_00 as the parts
// L_SaveData_00 / 01 / 02 (files 1..3). It has a picture of its own for a mark, P_ExtraMark_00 (80 x 80, child of the
// card's P_Base_00, at its left edge; the game shows it through the animation BtnSaveData_00_Extra). Here, right after
// a card's P_Base_00 (and so its mark) is drawn (Pane::Draw, hook_02877100 in aspect.cpp), that picture is drawn once
// more for an online file: made visible, put at the top right corner, a size of its own and its material's texture
// aimed at the badge (a GX2Texture of ours in unused guest memory), then everything put back as it was. The game's
// state is not changed: the card, its own mark (a Hero Mode file keeps it) and its animations are the game's.
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>

#include "cemu_shim.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "gx2_surface_calc.h"
#include "gx2_texture_regs.h"
#include "runtime.h"
#include "online/android_scope.h"

extern "C" void f_02877100_orig(Cpu* c);

namespace android_card_mark {
namespace {
constexpr uint32_t kPaneParent = 0x0C, kPaneChildren = 0x14, kPaneSize = 0x3C, kPaneFlags = 0x44, kPaneAlpha = 0x46,
                   kPaneGlobal = 0x48, kPaneName = 0x80, kPictureMaterial = 0xA4;
constexpr uint32_t kMatTexMaps = 0x10, kTexMapTexture = 0x18;  // the material's parts (a pointer), the first texture map's GX2Texture*
// guest memory nobody uses (below the player model area, player_models.cpp): the texture object, then its image
constexpr uint32_t kArea = 0x57FF0000, kTexture = kArea, kImage = kArea + 0x1000, kMagic = 0x574F4D4B;  // "WOMK"
constexpr uint32_t kMagicAt = kArea + 0x200;
constexpr int kSide = 64;
// where the badge goes in the card (P_Base_00's units: 1010 x 150 around its centre) and how big
constexpr float kX = 466.0f, kY = 46.0f, kSize = 52.0f;  // (above the name line's curl)

bool name_is(uint32_t p, const char* n) { return !strncmp((const char*)mem::ptr(p + kPaneName), n, 24); }
bool ok(uint32_t a) { return a >= 0x10000000 && a < 0x50000000; }

// the badge's pixels (RGBA, top row first): a round paper-coloured badge with a dark blue rim and the sea's teal
// waves of a signal over a dot, as the game's own marks are drawn (soft edges: 4 x 4 samples a pixel)
void paint(uint8_t* out, uint32_t pitch) {
    const float c = kSide / 2.0f;
    for (int y = 0; y < kSide; y++)
        for (int x = 0; x < kSide; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    const float px = x + (sx + 0.5f) / 4 - c, py = y + (sy + 0.5f) / 4 - c;
                    const float r = std::sqrt(px * px + py * py);
                    float col[4] = {0, 0, 0, 0};
                    if (r <= 31.0f) {
                        if (r > 27.0f) col[0] = 0x1F, col[1] = 0x3A, col[2] = 0x5C, col[3] = 255;  // rim
                        else {
                            col[0] = 0xF7, col[1] = 0xEE, col[2] = 0xCF, col[3] = 255;  // paper
                            // the signal: centre low in the badge, arcs opening upwards (60 degrees each side)
                            const float qx = px, qy = py - 9.0f, d = std::sqrt(qx * qx + qy * qy);
                            const bool up = qy < 0 && std::fabs(qx) <= -qy * 1.25f;
                            const bool dot = d <= 4.2f;
                            const bool arc1 = up && d >= 9.0f && d <= 13.5f, arc2 = up && d >= 18.0f && d <= 22.5f;
                            if (dot || arc1 || arc2) col[0] = 0x0E, col[1] = 0x8C, col[2] = 0x8F;  // teal
                        }
                    }
                    for (int k = 0; k < 4; k++) acc[k] += col[k] * (k < 3 ? col[3] : 1.0f);
                }
            uint8_t* d = out + (size_t)y * pitch * 4 + (size_t)x * 4;
            const float a = acc[3] / 16.0f;
            for (int k = 0; k < 3; k++) d[k] = a > 0 ? (uint8_t)std::lround(acc[k] / 16.0f / a) : 0;
            d[3] = (uint8_t)std::lround(a);
        }
}

// our texture, made like the card mark's own (its format, sampler) but 64 x 64 and linear; again after a save state
// put other bytes there
bool make_texture(uint32_t like) {
    if (ld32(kMagicAt) == kMagic) return true;
    GX2::GX2Texture t;
    memcpy(&t, mem::ptr(like), sizeof t);
    t.surface.dim = Latte::E_DIM::DIM_2D;
    t.surface.width = kSide, t.surface.height = kSide, t.surface.depth = 1, t.surface.numLevels = 1;
    t.surface.aa = 0;
    t.surface.tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED;
    t.surface.swizzle = 0;
    t.surface.mipPtr = 0, t.surface.mipSize = 0;
    gx2calc::CalcSurfaceSizeAndAlignment(&t.surface);
    if ((uint32_t)t.surface.imageSize > 0x10000 - 0x1000 || (uint32_t)t.surface.pitch < kSide) return false;
    t.surface.imagePtr = kImage;
    t.viewFirstMip = 0, t.viewNumMips = 1, t.viewFirstSlice = 0, t.viewNumSlices = 1;
    GX2::GX2InitTextureRegs(&t);
    memset(mem::ptr(kImage), 0, (uint32_t)t.surface.imageSize);
    paint((uint8_t*)mem::ptr(kImage), (uint32_t)t.surface.pitch);
    memcpy(mem::ptr(kTexture), &t, sizeof t);
    st32(kMagicAt, kMagic);
    return true;
}

// the save file (1..3) whose card this pane is in, 0 none
int card_file(uint32_t pane) {
    for (int i = 0; i < 8 && ok(pane); i++, pane = ld32(pane + kPaneParent)) {
        const char* n = (const char*)mem::ptr(pane + kPaneName);
        if (!strncmp(n, "L_SaveData_0", 12) && n[12] >= '0' && n[12] <= '2' && n[13] == 0) return n[12] - '0' + 1;
    }
    return 0;
}

uint32_t child_named(uint32_t pane, const char* name) {
    const uint32_t sentinel = pane + kPaneChildren;
    int k = 0;
    for (uint32_t n = ld32(sentinel); n && n != sentinel && k < 32; n = ld32(n), k++)
        if (name_is(n, name)) return n;
    return 0;
}
}  // namespace

// after Pane::Draw(pane) (aspect.cpp hook_02877100), c as it was called
void after_draw(Cpu* c, uint32_t pane) {
    if (!name_is(pane, "P_Base_00")) return;
    const int file = card_file(pane);
    if (!file || !(mods::coop::sync::android_online_files() >> (file - 1) & 1)) return;
    const uint32_t mark = child_named(pane, "P_ExtraMark_00");
    const uint32_t mat = mark ? ld32(mark + kPictureMaterial) : 0;
    const uint32_t parts = ok(mat) ? ld32(mat + kMatTexMaps) : 0;
    const uint32_t texAt = parts + kTexMapTexture, tex = ok(parts) ? ld32(texAt) : 0;
    static int why_told = 0;
    auto why = [&](const char* w) {
        if (why_told++ < 2) LOG("[cardmark] not drawn: %s (mark %08X material %08X texture %08X)", w, mark, mat, tex);
    };
    if (!ok(mat) || ld32(mat + 0x0C) >> 30 == 0) return why("no material / texture map");
    if (!ok(tex) && tex != kTexture) return why("no texture");
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    if (!make_texture(tex == kTexture ? kTexture : tex)) return why("texture not made");
    // the mark's fields as they are, then ours
    uint8_t saved[0x80];
    memcpy(saved, mem::ptr(mark + kPaneSize), sizeof saved);  // (+0x3C size .. +0xBC: flags, alpha, global matrix)
    st8(mark + kPaneFlags, ld8(mark + kPaneFlags) | 1);
    st8(mark + kPaneAlpha, ld8(pane + kPaneAlpha));
    stf32(mark + kPaneSize, kSize), stf32(mark + kPaneSize + 4, kSize);
    float m[12];
    for (int i = 0; i < 12; i++) m[i] = (float)ldf32(pane + kPaneGlobal + 4 * i);
    m[3] += m[0] * kX + m[1] * kY;
    m[7] += m[4] * kX + m[5] * kY;
    m[11] += m[8] * kX + m[9] * kY;
    for (int i = 0; i < 12; i++) stf32(mark + kPaneGlobal + 4 * i, m[i]);
    st32(texAt, kTexture);
    const Cpu before = *c;
    c->r[3] = mark;
    f_02877100_orig(c);
    *c = before;
    st32(texAt, tex);
    memcpy(mem::ptr(mark + kPaneSize), saved, sizeof saved);
}
}  // namespace android_card_mark
