// The game's own GamePad screens as drawing orders for a GamePad device (drc_mirror.h).
//
// nw::lyt classes as found in memory (2026-11-03, EUR; WWHD_PANE_DUMP in aspect.cpp printed them, the layouts'
// bflyt files told the fields apart):
//   Pane: +0x08 vtable, +0x0C parent, +0x3C size (w, h), +0x44 flags (bit 0 visible), +0x46 alpha it is drawn with,
//     +0x47 base position (h: 0 centre, 1 left, 2 right; v << 2: 0 centre, 1 top, 2 bottom), +0x48 global matrix
//     3x4 (layout space: 1280 x 720 around the centre, y up), +0x80 name.
//   Picture: +0xA4 Material*, +0xA8 vertex colours [4] (RGBA: top left, top right, bottom left, bottom right),
//     +0xB8 texture coordinate sets (u8 capacity, u8 count; +0xBC their array: 4 x (u, v) each, the same corners).
//   Window: +0xA4 content inflation (s16 l r t b), +0xAC frame sizes (u16 l r t b), +0xB4 content vertex colours,
//     +0xC4 content texture coordinates (as a picture's), +0xCC u8 kind (0 around, 1 horizontal, 2 horizontal
//     without content), +0xCD frame count (1, 4 or 8: LT RT LB RB L R T B), +0xCE flags (2: the first frame's material
//     for all of them, 1: the frames take the content's vertex colours, 4: no content; the bflyt's bits 0, 1 and 4),
//     +0xD0 frames (8 bytes each: u8 flip, Material*), +0xD4 content Material*.
//   TextBox: +0xAC / +0xB0 text colours (top, bottom), +0xD8 shadow offset, +0xE0 shadow scale, +0xE8 / +0xEC shadow
//     colours, +0xF4 flags (0x10 shadow), +0xF8 Material*, +0xFC the glyphs as the game built them (+0x04 u16 glyph
//     count, +0x14 / +0x18 two vertex buffers, +0x1C which one is drawn (0 the first), +0x20 the font's sheets: a
//     texture object, its GX2Texture at +0x10). A glyph is 4 vertices (top left, top right, bottom left, bottom right;
//     y down) of 0x1C bytes: x, y, z, colour RGBA, u, v (the sheets are stored upside down), w (the sheet; -1 - the
//     sheet: drawn in the font shadow's colours).
//   Material: +0x00 black colour, +0x04 white colour (the texture's 0 and 1 go to these), +0x0C what it has (+0x08
//     what it has room for; bits from the top: texture maps 2, texture SRTs 2, coordinate generators 2, TEV stages 3,
//     alpha compare 1, blend mode 1, texture only 1, alpha blend mode 1, indirect 1, projections 2), +0x10 those, one
//     after the other: texture maps (0x1C: +0x04 u16 w, h, +0x0C GX2 sampler word 0 (clamp x bits 0-2, y 3-5),
//     +0x18 GX2Texture*), SRTs (0x14: translate x y, rotate (degrees), scale x y), generators (8: u8 kind, u8 source
//     (0-2 a coordinate set, 3 / 4 a projection), +4 its parameters: translate x y, scale x y, flags), TEV stages (4:
//     u8 colour mode, u8 alpha mode), alpha compare (8), blend mode (4: u8 operation, source, destination factor),
//     alpha blend (4).
//   GX2Texture: GX2Surface (+0x04 w, +0x08 h, +0x0C depth, +0x14 format, +0x20 image size, +0x24 image, +0x30 tile
//     mode, +0x34 swizzle), +0x84 component selection.
// The colours the game's layout shaders make of these (their GLSL, WWHD_DUMP_SHADERS): MirrorView.
// Pane kinds are told apart by their vtables, learnt from the panes' fields (kind_by_fields): the addresses differ
// between the game's versions, and the game has its own kinds of text boxes.
//
// Serialized frame (little-endian): u32 'MIR1', u32 quads, u32 textures, the texture ids, then per quad: u8 texture
// count | 4 texture only | 8 a glyph (its font's sheet: 0.5 its outline, 1 the letter), u8 blend (0 normal, 1 add,
// 2 multiply), u8 TEV colour mode, u8 alpha mode (the second texture with the first), f32 x y [4] (screen pixels of
// the 1280 x 720 picture: top left, top right, bottom left, bottom right), u32 ARGB colours [4], u32 ARGB black,
// white, then per texture: u32 id, u8 wrap x, y (GX2: 0 repeat, 1 mirror, 2 clamp), u16 0, f32 u v [4].
#include "drc_mirror.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>

#include "aspect.h"
#include "runtime.h"

namespace drc_mirror {
namespace {
std::atomic<bool> g_on{false};

constexpr uint32_t kParent = 0x0C, kSize = 0x3C, kFlags = 0x44, kAlpha = 0x46, kBase = 0x47, kMtx = 0x48, kName = 0x80;

bool mem_ok(uint32_t a, uint32_t n = 4) {
    uint64_t e = (uint64_t)a + n;
    return (a >= 0x10000000 && e <= 0x50000000) || (a >= 0xF4000000 && e <= 0xF6000000);
}
float f32(uint32_t a) { return (float)ldf32(a); }
uint32_t argb(uint32_t rgba) { return (rgba >> 8) | (rgba << 24); }
uint32_t with_alpha(uint32_t c, int a) { return (c & 0x00FFFFFF) | ((uint32_t)(((c >> 24) * a + 127) / 255) << 24); }
uint32_t mul(uint32_t x, uint32_t y) {  // ARGB * ARGB
    uint32_t o = 0;
    for (int s = 0; s < 32; s += 8) o |= (uint32_t)((((x >> s) & 255) * ((y >> s) & 255) + 127) / 255) << s;
    return o;
}

enum Kind { kOther, kPicture, kText, kWindow };
std::mutex g_kind_mu;
std::unordered_map<uint32_t, int> g_kinds;  // vtable -> Kind
// a material at m named `name` (+ suffix): how a pane's own material is named
bool mat_named(uint32_t m, const char* name, const char* suffix) {
    if (!mem_ok(m, 0x3C)) return false;
    char want[32];
    snprintf(want, sizeof want, "%.24s%s", name, suffix);
    return !strncmp((const char*)mem::ptr(m + 0x18), want, std::min<size_t>(strlen(want), 20));  // (some end in a space)
}
// what a pane of an unknown vtable is, by its fields: a picture's material (+0xA4) and a text box's (+0xF8) carry
// its name, a window's content material (+0xD4) its name and "C" (also the game's own kinds of them: they keep
// the fields)
int kind_by_fields(uint32_t pane) {
    const char* n = (const char*)mem::ptr(pane + kName);
    if (!n[0]) return kOther;
    if (mat_named(ld32(pane + 0xD4), n, "C") && mem_ok(ld32(pane + 0xD0), 8)) return kWindow;
    if (mat_named(ld32(pane + 0xF8), n, "") && mem_ok(ld32(pane + 0xFC), 0x24)) return kText;
    if (mat_named(ld32(pane + 0xA4), n, "")) return kPicture;
    return kOther;
}
int kind_of(uint32_t pane) {
    uint32_t vt = ld32(pane + 8);
    std::lock_guard<std::mutex> lk(g_kind_mu);
    auto it = g_kinds.find(vt);
    if (it != g_kinds.end()) return it->second;
    int k = kind_by_fields(pane);
    if (k == kOther) {  // the layouts' names: P_ pictures, T_ text, W_ windows
        const char* n = (const char*)mem::ptr(pane + kName);
        if (n[1] == '_' || (n[1] && n[2] == '_')) k = n[0] == 'P' ? kPicture : n[0] == 'T' ? kText : n[0] == 'W' ? kWindow : kOther;
    }
    if (k == kOther) {  // (a null pane, parts, a bounding box: decided on a few of them)
        static std::unordered_map<uint32_t, int> others;
        if (++others[vt] < 64) return kOther;
    }
    g_kinds[vt] = k;
    if (k != kOther) LOG("[drc-mirror] %s panes: vtable %08X ('%.24s')", k == kPicture ? "picture" : k == kText ? "text" : "window", vt, (const char*)mem::ptr(pane + kName));
    return k;
}

struct Tex { uint32_t id; uint8_t wrapS, wrapT; float uv[8]; };
struct Quad {
    uint8_t ntex, texOnly, font, blend, tevC, tevA;
    float pos[8];
    uint32_t col[4], black, white;
    Tex tex[2];
};

// ---- textures: an id per surface and content (a picture the game redraws into, the album's, gets a new one)
std::mutex g_tex_mu;
std::unordered_map<uint32_t, TexInfo> g_tex;                                   // id -> surface
std::unordered_map<uint32_t, std::pair<uint64_t, uint32_t>> g_sums;            // image -> (frame checked, sum)
uint32_t fnv(uint32_t h, uint32_t v) {
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 255)) * 16777619u;
    return h;
}
uint32_t content_sum(uint32_t image, uint32_t size) {
    uint64_t now = aspect::game_frame();
    auto it = g_sums.find(image);
    if (it != g_sums.end() && now - it->second.first < 10) return it->second.second;
    uint32_t h = 2166136261u, step = std::max<uint32_t>(4, (size / 97) & ~3u);
    for (uint32_t o = 0; o + 4 <= size && o < step * 97; o += step) h = fnv(h, ld32(image + o));
    if (g_sums.size() > 4096) g_sums.clear();
    g_sums[image] = {now, h};
    return h;
}
// the id of a GX2Texture's slice (0: not one to decode)
uint32_t tex_id(uint32_t t, uint32_t slice) {
    if (!mem_ok(t, 0x9C)) return 0;
    TexInfo i{};
    i.w = ld32(t + 0x04);
    i.h = ld32(t + 0x08);
    i.depth = std::max<uint32_t>(1, ld32(t + 0x0C));
    i.format = ld32(t + 0x14);
    i.size = ld32(t + 0x20);
    i.image = ld32(t + 0x24);
    i.tile = ld32(t + 0x30);
    i.swizzle = ld32(t + 0x34);
    i.compSel = ld32(t + 0x84);
    i.slice = slice < i.depth ? slice : 0;
    if (!i.w || !i.h || i.w > 4096 || i.h > 4096 || !i.size || !mem_ok(i.image, i.size)) return 0;
    std::lock_guard<std::mutex> lk(g_tex_mu);
    uint32_t h = 2166136261u;
    for (uint32_t v : {i.image, i.w, i.h, i.format, i.tile, i.swizzle, i.compSel, i.slice, content_sum(i.image, i.size)}) h = fnv(h, v);
    if (!h) h = 1;
    if (g_tex.size() > 8192) g_tex.clear();
    g_tex[h] = i;
    return h;
}

// ---- materials
struct Mat {
    uint32_t black = 0, white = 0xFFFFFFFF;  // ARGB
    int tm = 0, srt = 0, gen = 0;
    bool texOnly = false;
    uint8_t blend = 0, tevC = 1, tevA = 1;
    uint32_t map[3]{}, gens[3]{}, srts[3]{};  // their addresses
};
bool read_mat(uint32_t m, Mat& o) {
    if (!mem_ok(m, 0x3C)) return false;
    o.black = argb(ld32(m));
    o.white = argb(ld32(m + 4));
    uint32_t bits = ld32(m + 0xC), mem = ld32(m + 0x10);  // (+0x08: what it has room for)
    o.tm = bits >> 30;
    o.srt = (bits >> 28) & 3;
    o.gen = (bits >> 26) & 3;
    int tev = (bits >> 23) & 7, ac = (bits >> 22) & 1, bm = (bits >> 21) & 1;
    o.texOnly = (bits >> 20) & 1;
    if (o.tm > 3) o.tm = 3;
    if (!o.tm && !o.srt && !o.gen && !tev && !ac && !bm) return true;
    if (!mem_ok(mem, 0x100)) return false;
    uint32_t off = 0;
    for (int i = 0; i < o.tm; i++) o.map[i] = mem + off + 0x1C * i;
    off += 0x1C * o.tm;
    for (int i = 0; i < o.srt; i++) o.srts[i] = mem + off + 0x14 * i;
    off += 0x14 * o.srt;
    for (int i = 0; i < o.gen; i++) o.gens[i] = mem + off + 8 * i;
    off += 8 * o.gen;
    if (tev) {
        o.tevC = ld8(mem + off);
        o.tevA = ld8(mem + off + 1);
    }
    off += 4 * tev + 8 * ac;
    if (bm) {
        uint8_t op = ld8(mem + off), src = ld8(mem + off + 1), dst = ld8(mem + off + 2);
        // (nw::lyt blend factors: 0 zero, 1 one, 2 destination colour, 3 1 - it, 4 source alpha, 5 1 - it, 6
        // destination alpha, 7 1 - it, 8 source colour, 9 1 - it)
        if (op == 1 && dst == 1) o.blend = 1;
        else if (op == 1 && ((src == 2 && dst == 0) || (src == 0 && dst == 8))) o.blend = 2;
        else o.blend = 0;
        static std::mutex mu;
        static std::set<uint32_t> seen;
        std::lock_guard<std::mutex> lk(mu);
        uint32_t key = op << 16 | src << 8 | dst;
        if (seen.insert(key).second) LOG("[drc-mirror] blend mode op %u src %u dst %u -> %u ('%.20s')", op, src, dst, o.blend, (const char*)mem::ptr(m + 0x18));
    }
    return true;
}

// ---- a pane's quads
struct Xf {
    float m00, m01, m03, m10, m11, m13;
    void to(float x, float y, float* out) const {
        out[0] = 640.0f + m00 * x + m01 * y + m03;
        out[1] = 360.0f - (m10 * x + m11 * y + m13);
    }
};

// a quad of the pane: local corners (top left, top right, bottom left, bottom right; y up), coordinate sets (4 x (u, v)
// each, the same corners), colours (ARGB) with the pane's alpha
void emit(std::vector<Quad>& out, const Xf& xf, const float* loc, const Mat& m, const float sets[][8], int nsets,
          const uint32_t* col, int alpha, float paneW, float paneH) {
    Quad q{};
    for (int k = 0; k < 4; k++) xf.to(loc[2 * k], loc[2 * k + 1], &q.pos[2 * k]);
    for (int k = 0; k < 4; k++) q.col[k] = with_alpha(col[k], alpha);
    if ((q.col[0] | q.col[1] | q.col[2] | q.col[3]) >> 24 == 0) return;  // (all transparent)
    q.black = m.black;
    q.white = m.white;
    q.texOnly = m.texOnly;
    q.blend = m.blend;
    q.tevC = m.tevC;
    q.tevA = m.tevA;
    for (int i = 0; i < m.tm && q.ntex < 2; i++) {
        uint32_t t = m.map[i];
        uint32_t id = tex_id(ld32(t + 0x18), 0);
        if (!id) continue;
        Tex& x = q.tex[q.ntex++];
        x.id = id;
        uint32_t s0 = ld32(t + 0x0C);
        x.wrapS = s0 & 7;
        x.wrapT = (s0 >> 3) & 7;
        int src = i;
        uint32_t pp = 0;
        if (i < m.gen) {
            src = ld8(m.gens[i] + 1);
            if (src >= 3) pp = ld32(m.gens[i] + 4);
        }
        if (src >= 3 && mem_ok(pp, 0x14)) {
            // projected: the texture over layout space (3) or over the pane from its origin (4), at its own size times
            // the scale, or fitted to the layout (flag 1) or the pane (flag 2)
            float tw = ld16(t + 4), th = ld16(t + 6);
            float tx = f32(pp), ty = f32(pp + 4), sx = f32(pp + 8), sy = f32(pp + 0xC);
            uint32_t fl = ld32(pp + 0x10);
            float w = tw * sx, h = th * sy;
            if (fl & 1) { w = 1280.0f * sx; h = 720.0f * sy; }
            if (fl & 2) { w = paneW * sx; h = paneH * sy; }
            for (int k = 0; k < 4; k++) {
                float X = q.pos[2 * k] - 640.0f, Y = 360.0f - q.pos[2 * k + 1];
                if (src != 3) { X -= xf.m03; Y -= xf.m13; }
                x.uv[2 * k] = (w != 0 ? X / w : 0) + 0.5f + tx;
                x.uv[2 * k + 1] = (h != 0 ? -Y / h : 0) + 0.5f + ty;
            }
        } else {
            const float* s = nsets ? sets[std::min(src, nsets - 1)] : nullptr;
            static const float kUnit[8] = {0, 0, 1, 0, 0, 1, 1, 1};
            memcpy(x.uv, s ? s : kUnit, sizeof x.uv);
        }
        if (i < m.srt) {  // the texture's SRT: scaled and turned around the middle, moved
            uint32_t a = m.srts[i];
            float tx = f32(a), ty = f32(a + 4), r = f32(a + 8) * 0.017453293f, sx = f32(a + 0xC), sy = f32(a + 0x10);
            float c = std::cos(r), s = std::sin(r);
            float a0 = c * sx, a1 = -s * sy, a2 = s * sx, a3 = c * sy;
            for (int k = 0; k < 4; k++) {
                float u = x.uv[2 * k] - 0.5f, v = x.uv[2 * k + 1] - 0.5f;
                x.uv[2 * k] = a0 * u + a1 * v + 0.5f + tx;
                x.uv[2 * k + 1] = a2 * u + a3 * v + 0.5f + ty;
            }
        }
    }
    out.push_back(q);
}

void pane_rect(uint32_t pane, float& L, float& R, float& T, float& B) {
    float w = f32(pane + kSize), h = f32(pane + kSize + 4);
    uint8_t base = ld8(pane + kBase);
    int hp = base & 3, vp = (base >> 2) & 3;
    L = hp == 1 ? 0.0f : hp == 2 ? -w : -w * 0.5f;
    T = vp == 1 ? 0.0f : vp == 2 ? h : h * 0.5f;
    R = L + w;
    B = T - h;
}
void corners(float L, float R, float T, float B, float* loc) {
    float c[8] = {L, T, R, T, L, B, R, B};
    memcpy(loc, c, sizeof c);
}
// coordinates of a set (+0xB8 / +0xC4 kind: u8 capacity, u8 count, then (4 bytes on) the array)
int read_sets(uint32_t ary, float sets[3][8]) {
    int n = std::min<int>(ld8(ary + 1), 3);
    uint32_t p = ld32(ary + 4);
    if (n <= 0 || !mem_ok(p, 0x20 * n)) return 0;
    for (int s = 0; s < n; s++)
        for (int k = 0; k < 8; k++) sets[s][k] = f32(p + 0x20 * s + 4 * k);
    return n;
}

void picture(std::vector<Quad>& out, uint32_t pane, const Xf& xf, int alpha) {
    Mat m;
    if (!read_mat(ld32(pane + 0xA4), m)) return;
    float L, R, T, B, loc[8], sets[3][8];
    pane_rect(pane, L, R, T, B);
    corners(L, R, T, B, loc);
    uint32_t col[4];
    for (int k = 0; k < 4; k++) col[k] = argb(ld32(pane + 0xA8 + 4 * k));
    int n = read_sets(pane + 0xB8, sets);
    emit(out, xf, loc, m, sets, n, col, alpha, R - L, T - B);
}

// a window's frame piece: its rectangle (local), its texture turned / mirrored (nw::lyt TextureFlip: 0 none, 1
// horizontally, 2 vertically, 3 90°, 4 180°, 5 270°), stretched past its size by the texture's clamped edge
void frame_piece(std::vector<Quad>& out, const Xf& xf, uint32_t matAddr, uint32_t texMat, int flip, float L, float R, float T,
                 float B, const uint32_t* col, int alpha, float paneW, float paneH) {
    Mat m;
    if (!read_mat(matAddr, m)) return;
    if (texMat != matAddr) {  // (the window's one material, the frame's own shape: its first texture)
        Mat t;
        if (read_mat(texMat, t) && t.tm > 0) {
            m.map[0] = t.map[0];
            if (!m.tm) m.tm = 1;
        }
    }
    float tw = m.tm ? ld16(m.map[0] + 4) : 1, th = m.tm ? ld16(m.map[0] + 6) : 1;
    if (tw <= 0) tw = 1;
    if (th <= 0) th = 1;
    bool turned = flip == 3 || flip == 5;
    float U = (turned ? (T - B) : (R - L)) / tw, V = (turned ? (R - L) : (T - B)) / th;
    float s[8];
    switch (flip) {
        case 1: { float t[8] = {U, 0, 0, 0, U, V, 0, V}; memcpy(s, t, sizeof s); break; }
        case 2: { float t[8] = {0, V, U, V, 0, 0, U, 0}; memcpy(s, t, sizeof s); break; }
        case 3: { float t[8] = {0, V, 0, 0, U, V, U, 0}; memcpy(s, t, sizeof s); break; }
        case 4: { float t[8] = {U, V, 0, V, U, 0, 0, 0}; memcpy(s, t, sizeof s); break; }
        case 5: { float t[8] = {U, 0, U, V, 0, 0, 0, V}; memcpy(s, t, sizeof s); break; }
        default: { float t[8] = {0, 0, U, 0, 0, V, U, V}; memcpy(s, t, sizeof s); break; }
    }
    float loc[8], sets[1][8];
    memcpy(sets[0], s, sizeof s);
    corners(L, R, T, B, loc);
    emit(out, xf, loc, m, sets, 1, col, alpha, paneW, paneH);
}

void window(std::vector<Quad>& out, uint32_t pane, const Xf& xf, int alpha) {
    float L, R, T, B;
    pane_rect(pane, L, R, T, B);
    float W = R - L, H = T - B;
    int kind = ld8(pane + 0xCC), nf = ld8(pane + 0xCD);
    uint8_t wf = ld8(pane + 0xCE);
    bool oneMaterial = wf & 2, frameColors = wf & 1, noContent = wf & 4;
    uint32_t frames = ld32(pane + 0xD0);
    float fl = (int16_t)ld16(pane + 0xAC), fr = (int16_t)ld16(pane + 0xAE), ft = (int16_t)ld16(pane + 0xB0), fb = (int16_t)ld16(pane + 0xB2);
    uint32_t col[4], white[4] = {~0u, ~0u, ~0u, ~0u};
    for (int k = 0; k < 4; k++) col[k] = argb(ld32(pane + 0xB4 + 4 * k));
    const uint32_t* fcol = frameColors ? col : white;
    auto content = [&](float cl, float cr, float ct, float cb) {
        if (noContent) return;
        Mat m;
        if (!read_mat(ld32(pane + 0xD4), m)) return;
        float il = (int16_t)ld16(pane + 0xA4), ir = (int16_t)ld16(pane + 0xA6), it = (int16_t)ld16(pane + 0xA8), ib = (int16_t)ld16(pane + 0xAA);
        float loc[8], sets[3][8];
        corners(cl - il, cr + ir, ct + it, cb - ib, loc);
        int n = read_sets(pane + 0xC4, sets);
        emit(out, xf, loc, m, sets, n, col, alpha, W, H);
    };
    if (nf < 1 || nf > 8 || !mem_ok(frames, 8 * nf)) { content(L, R, T, B); return; }
    auto own = [&](int i) { return ld32(frames + 8 * std::min(i, nf - 1) + 4); };
    auto fm = [&](int i) { return ld32(frames + 8 * (oneMaterial ? 0 : std::min(i, nf - 1)) + 4); };
    auto ff = [&](int i, int one) { return nf == 1 ? one : (int)ld8(frames + 8 * i); };
    if (kind == 1 || kind == 2) {  // horizontal: a frame left and right (mirrored), the content between (kind 1)
        if (kind == 1) {
            content(L + fl, R - fr, T, B);
            frame_piece(out, xf, fm(0), own(0), ff(0, 0), L, L + fl, T, B, fcol, alpha, W, H);
        } else {
            frame_piece(out, xf, fm(0), own(0), ff(0, 0), L, R - fr, T, B, fcol, alpha, W, H);
        }
        frame_piece(out, xf, fm(1), own(1), ff(1, 1), R - fr, R, T, B, fcol, alpha, W, H);
        return;
    }
    content(L, R, T, B);
    if (nf >= 8) {  // corners at their sizes, the sides between them
        frame_piece(out, xf, fm(0), own(0), ff(0, 0), L, L + fl, T, T - ft, fcol, alpha, W, H);
        frame_piece(out, xf, fm(1), own(1), ff(1, 1), R - fr, R, T, T - ft, fcol, alpha, W, H);
        frame_piece(out, xf, fm(2), own(2), ff(2, 2), L, L + fl, B + fb, B, fcol, alpha, W, H);
        frame_piece(out, xf, fm(3), own(3), ff(3, 4), R - fr, R, B + fb, B, fcol, alpha, W, H);
        frame_piece(out, xf, fm(4), own(4), ff(4, 0), L, L + fl, T - ft, B + fb, fcol, alpha, W, H);
        frame_piece(out, xf, fm(5), own(5), ff(5, 0), R - fr, R, T - ft, B + fb, fcol, alpha, W, H);
        frame_piece(out, xf, fm(6), own(6), ff(6, 0), L + fl, R - fr, T, T - ft, fcol, alpha, W, H);
        frame_piece(out, xf, fm(7), own(7), ff(7, 0), L + fl, R - fr, B + fb, B, fcol, alpha, W, H);
        return;
    }
    // one or four frames: four pieces around, each a corner and the side after it (clockwise), the corner's texture
    // stretched along the side by its clamped edge
    frame_piece(out, xf, fm(0), own(0), ff(0, 0), L, R - fr, T, T - ft, fcol, alpha, W, H);
    frame_piece(out, xf, fm(1), own(1), ff(1, 1), R - fr, R, T, B + fb, fcol, alpha, W, H);
    frame_piece(out, xf, fm(3), own(3), ff(3, 4), L + fl, R, B + fb, B, fcol, alpha, W, H);
    frame_piece(out, xf, fm(2), own(2), ff(2, 2), L, L + fl, T - ft, B, fcol, alpha, W, H);
}

void text(std::vector<Quad>& out, uint32_t pane, const Xf& xf, int alpha) {
    uint32_t d = ld32(pane + 0xFC);
    if (!mem_ok(d, 0x24)) return;
    // (two vertex buffers, for the GPU to draw one while the other is written: +0x1C which one is drawn)
    uint32_t vb = ld32(d + (ld32(d + 0x1C) == 0 ? 0x14 : 0x18)), n = ld16(d + 4), font = ld32(d + 0x20);
    if (!n || n > 2048 || !mem_ok(vb, n * 4 * 0x1C) || !mem_ok(font, 0x20)) return;
    Mat m;
    if (!read_mat(ld32(pane + 0xF8), m)) m = Mat{};
    m.tm = 0;  // (the glyphs' texture is the font's)
    m.blend = 0;
    uint32_t top = argb(ld32(pane + 0xAC)), bottom = argb(ld32(pane + 0xB0));
    bool shadow = ld8(pane + 0xF4) & 0x10;
    float sdx = f32(pane + 0xD8), sdy = f32(pane + 0xDC), ssx = f32(pane + 0xE0), ssy = f32(pane + 0xE4);
    uint32_t stop = argb(ld32(pane + 0xE8)), sbottom = argb(ld32(pane + 0xEC));
    float L, R, T, B;
    pane_rect(pane, L, R, T, B);
    for (int pass = shadow ? 0 : 1; pass < 2; pass++)
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = vb + i * 4 * 0x1C;
            static const int kOrder[4] = {0, 1, 2, 3};  // (top left, top right, bottom left, bottom right, y down)
            float loc[8], uv[1][8];
            uint32_t col[4];
            float cx = 0, cy = 0;
            for (int k = 0; k < 4; k++) {
                uint32_t e = v + kOrder[k] * 0x1C;
                loc[2 * k] = f32(e);
                loc[2 * k + 1] = -f32(e + 4);
                uv[0][2 * k] = f32(e + 0x10);
                uv[0][2 * k + 1] = f32(e + 0x14);
                cx += loc[2 * k] * 0.25f;
                cy += loc[2 * k + 1] * 0.25f;
                uint32_t c = argb(ld32(e + 0xC));
                col[k] = pass ? mul(c, k < 2 ? top : bottom) : (c & 0xFF000000) | 0x00FFFFFF;
                if (!pass) col[k] = mul(col[k], k < 2 ? stop : sbottom);
            }
            if (!pass)
                for (int k = 0; k < 4; k++) {
                    loc[2 * k] = cx + (loc[2 * k] - cx) * ssx + sdx;
                    loc[2 * k + 1] = cy + (loc[2 * k + 1] - cy) * ssy + sdy;
                }
            Mat g = m;
            if (!pass) { g.black = stop & 0x00FFFFFF; g.white = 0xFFFFFFFF; g.texOnly = false; }
            size_t before = out.size();
            emit(out, xf, loc, g, uv, 1, col, alpha, R - L, T - B);
            if (out.size() == before) continue;
            Quad& q = out.back();
            float w = f32(v + 0x18);  // the sheet (-1 - it: in the font shadow's colours, taken as the material's here)
            int sheet = (int)std::lround(w < 0 ? -w - 1 : w);
            uint32_t id = tex_id(font + 0x10, sheet > 0 ? (uint32_t)sheet : 0);
            if (!id) { out.pop_back(); continue; }
            q.ntex = 1;
            q.font = 1;
            q.tex[0].id = id;
            q.tex[0].wrapS = q.tex[0].wrapT = 2;
            memcpy(q.tex[0].uv, uv[0], sizeof uv[0]);
        }
}

// ---- recording: per thread the root being drawn; per root its last quads; the order the render thread drew them
thread_local uint32_t t_root = 0;
thread_local std::vector<Quad> t_quads;
std::mutex g_mu;
std::unordered_map<uint32_t, std::vector<Quad>> g_roots;
std::vector<uint32_t> g_order_cur, g_order;
uint32_t g_sent_hash = 0;
}  // namespace

void set_on(bool on) {
    if (g_on.exchange(on) == on) return;
    LOG("[drc-mirror] %s", on ? "on" : "off");
    std::lock_guard<std::mutex> lk(g_mu);
    g_roots.clear();
    g_order.clear();
    g_order_cur.clear();
    g_sent_hash = 0;
}
bool on() { return g_on.load(std::memory_order_relaxed); }

void root_begin(uint32_t root, bool drc) {
    t_root = g_on.load(std::memory_order_relaxed) && drc ? root : 0;
    t_quads.clear();
}

void pane(uint32_t pane) {
    if (!t_root || !(ld8(pane + kFlags) & 1)) return;
    int alpha = ld8(pane + kAlpha);
    if (!alpha) return;
    int k = kind_of(pane);
    if (k == kOther) return;
    Xf xf{f32(pane + kMtx), f32(pane + kMtx + 4), f32(pane + kMtx + 0xC), f32(pane + kMtx + 0x10), f32(pane + kMtx + 0x14), f32(pane + kMtx + 0x1C)};
    if (t_quads.size() > 6000) return;
    if (k == kPicture) picture(t_quads, pane, xf, alpha);
    else if (k == kWindow) window(t_quads, pane, xf, alpha);
    else text(t_quads, pane, xf, alpha);
}

void root_done(uint32_t root) {
    if (!t_root || t_root != root) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_roots[root].swap(t_quads);
    t_quads.clear();
    t_root = 0;
}

void root_drawn(uint32_t root, bool drc) {
    if (!drc || !g_on.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    for (uint32_t r : g_order_cur)
        if (r == root) {  // a new frame: the last one's order is complete
            g_order.swap(g_order_cur);
            g_order_cur.clear();
            break;
        }
    g_order_cur.push_back(root);
}

namespace {
template <class T> void put(std::vector<uint8_t>& o, T v) {
    size_t s = o.size();
    o.resize(s + sizeof v);
    memcpy(&o[s], &v, sizeof v);  // (the device is little-endian like this one)
}
}  // namespace

bool frame(std::vector<uint8_t>& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<const Quad*> qs;
    std::vector<uint32_t> ids;
    std::set<uint32_t> seen;
    for (uint32_t r : g_order) {
        auto it = g_roots.find(r);
        if (it == g_roots.end()) continue;
        for (const Quad& q : it->second) {
            qs.push_back(&q);
            for (int i = 0; i < q.ntex; i++)
                if (seen.insert(q.tex[i].id).second) ids.push_back(q.tex[i].id);
        }
    }
    out.clear();
    put<uint32_t>(out, 0x3152494D);  // 'MIR1'
    put<uint32_t>(out, (uint32_t)qs.size());
    put<uint32_t>(out, (uint32_t)ids.size());
    for (uint32_t id : ids) put(out, id);
    for (const Quad* q : qs) {
        put<uint8_t>(out, q->ntex | (q->texOnly ? 4 : 0) | (q->font ? 8 : 0));
        put<uint8_t>(out, q->blend);
        put<uint8_t>(out, q->tevC);
        put<uint8_t>(out, q->tevA);
        for (float f : q->pos) put(out, f);
        for (uint32_t c : q->col) put(out, c);
        put(out, q->black);
        put(out, q->white);
        for (int i = 0; i < q->ntex; i++) {
            put(out, q->tex[i].id);
            put<uint8_t>(out, q->tex[i].wrapS);
            put<uint8_t>(out, q->tex[i].wrapT);
            put<uint16_t>(out, 0);
            for (float f : q->tex[i].uv) put(out, f);
        }
    }
    uint32_t h = 2166136261u;
    for (uint8_t b : out) h = (h ^ b) * 16777619u;
    if (h == g_sent_hash) return false;
    g_sent_hash = h;
    // roots no longer drawn
    for (auto it = g_roots.begin(); it != g_roots.end();) {
        bool used = false;
        for (uint32_t r : g_order) used |= r == it->first;
        for (uint32_t r : g_order_cur) used |= r == it->first;
        it = used ? std::next(it) : g_roots.erase(it);
    }
    return true;
}

bool texture(uint32_t id, TexInfo& info) {
    std::lock_guard<std::mutex> lk(g_tex_mu);
    auto it = g_tex.find(id);
    if (it == g_tex.end()) return false;
    info = it->second;
    return mem_ok(info.image, info.size);
}
}  // namespace drc_mirror
