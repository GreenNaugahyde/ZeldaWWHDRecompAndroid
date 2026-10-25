// Pictures of Link in his own animations' poses, for the touch controls (link_icons.h): the model "cl" of
// Link.bfres (skeleton, skinned shapes, textures) posed by a skeletal animation of LkAnm.bfres and drawn by a
// small software rasterizer with the model's own toon ramp (ZBtoonEX), then outlined like the game's item icons.
// Wii U BFRES version 3.4: big-endian, offsets relative to the field that holds them.
#include "link_icons.h"
#include "game_archive.h"
#include "ui_icons.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../runtime.h"

namespace link_icons {
namespace {
using game_archive::be16;
using game_archive::be32;
using game_archive::Bytes;

// ---- a BFRES file
struct Res {
    Bytes d;
    uint32_t u32(size_t o) const { return o + 4 <= d.size() ? be32(&d[o]) : 0; }
    uint16_t u16(size_t o) const { return o + 2 <= d.size() ? be16(&d[o]) : 0; }
    int16_t s16(size_t o) const { return (int16_t)u16(o); }
    uint8_t u8(size_t o) const { return o < d.size() ? d[o] : 0; }
    float f32(size_t o) const {
        uint32_t v = u32(o);
        float f;
        memcpy(&f, &v, 4);
        return f;
    }
    // the offset stored at o, made absolute (0: none)
    size_t ptr(size_t o) const {
        int32_t v = (int32_t)u32(o);
        return v ? o + v : 0;
    }
    std::string str(size_t o) const {
        size_t p = ptr(o);
        if (!p || p >= d.size()) return {};
        size_t e = p;
        while (e < d.size() && d[e]) e++;
        return std::string((const char*)&d[p], e - p);
    }
    // an index group at g: (name, data) of each entry
    std::vector<std::pair<std::string, size_t>> group(size_t g) const {
        std::vector<std::pair<std::string, size_t>> out;
        if (!g) return out;
        int32_t n = (int32_t)u32(g + 4);
        for (int32_t i = 1; i <= n; i++) {
            size_t e = g + 8 + 16 * i;
            out.emplace_back(str(e + 8), ptr(e + 12));
        }
        return out;
    }
    // the file's index group i (0 models, 1 textures, 2 skeletal animations)
    size_t find(int i, const std::string& name) const {
        for (auto& [n, p] : group(ptr(0x20 + 4 * i)))
            if (n == name && p + 0x100 <= d.size()) return p;
        return 0;
    }
};

// a BFRES out of the game's 3D pack (permanent_3d.pack: a SARC of Yaz0 SARCs)
bool load_res(const Bytes& pack, const char* arc, const char* file, Res& out) {
    Bytes packed, sarc;
    return game_archive::sarc_file(pack, arc, packed) && game_archive::yaz0(packed, sarc) &&
           game_archive::sarc_file(sarc, file, out.d) && out.d.size() > 0x60 && !memcmp(out.d.data(), "FRES", 4);
}

// ---- 3x4 matrices (rows), vectors
struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-12f ? a * (1 / l) : a;
}

struct M34 {
    float m[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
    V3 point(V3 v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z + m[3], m[4] * v.x + m[5] * v.y + m[6] * v.z + m[7],
                m[8] * v.x + m[9] * v.y + m[10] * v.z + m[11]};
    }
    V3 dir(V3 v) const {
        return {m[0] * v.x + m[1] * v.y + m[2] * v.z, m[4] * v.x + m[5] * v.y + m[6] * v.z, m[8] * v.x + m[9] * v.y + m[10] * v.z};
    }
};
M34 mul(const M34& a, const M34& b) {
    M34 r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 4; j++)
            r.m[i * 4 + j] = a.m[i * 4] * b.m[j] + a.m[i * 4 + 1] * b.m[4 + j] + a.m[i * 4 + 2] * b.m[8 + j] + (j == 3 ? a.m[i * 4 + 3] : 0);
    return r;
}
M34 inverse(const M34& a) {
    const float* m = a.m;
    float det = m[0] * (m[5] * m[10] - m[6] * m[9]) - m[1] * (m[4] * m[10] - m[6] * m[8]) + m[2] * (m[4] * m[9] - m[5] * m[8]);
    M34 r;
    if (std::fabs(det) < 1e-20f) return r;
    float id = 1 / det;
    r.m[0] = (m[5] * m[10] - m[6] * m[9]) * id;
    r.m[1] = (m[2] * m[9] - m[1] * m[10]) * id;
    r.m[2] = (m[1] * m[6] - m[2] * m[5]) * id;
    r.m[4] = (m[6] * m[8] - m[4] * m[10]) * id;
    r.m[5] = (m[0] * m[10] - m[2] * m[8]) * id;
    r.m[6] = (m[2] * m[4] - m[0] * m[6]) * id;
    r.m[8] = (m[4] * m[9] - m[5] * m[8]) * id;
    r.m[9] = (m[1] * m[8] - m[0] * m[9]) * id;
    r.m[10] = (m[0] * m[5] - m[1] * m[4]) * id;
    for (int i = 0; i < 3; i++) r.m[i * 4 + 3] = -(r.m[i * 4] * m[3] + r.m[i * 4 + 1] * m[7] + r.m[i * 4 + 2] * m[11]);
    return r;
}
// T * Rz * Ry * Rx * S (the skeleton's Euler XYZ rotation)
M34 srt(V3 s, V3 r, V3 t) {
    float cx = std::cos(r.x), sx = std::sin(r.x), cy = std::cos(r.y), sy = std::sin(r.y), cz = std::cos(r.z), sz = std::sin(r.z);
    M34 o;
    o.m[0] = cy * cz * s.x;
    o.m[1] = (sx * sy * cz - cx * sz) * s.y;
    o.m[2] = (cx * sy * cz + sx * sz) * s.z;
    o.m[3] = t.x;
    o.m[4] = cy * sz * s.x;
    o.m[5] = (sx * sy * sz + cx * cz) * s.y;
    o.m[6] = (cx * sy * sz - sx * cz) * s.z;
    o.m[7] = t.y;
    o.m[8] = -sy * s.x;
    o.m[9] = sx * cy * s.y;
    o.m[10] = cx * cy * s.z;
    o.m[11] = t.z;
    return o;
}

// ---- the model
struct Texture {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
    bool clampU = false, clampV = false, mirrorU = false, mirrorV = false;
    void sample(float u, float v, float out[4]) const;
};

float wrap(float c, bool clamp, bool mirror) {
    if (clamp) return std::min(1.f, std::max(0.f, c));
    float f = c - std::floor(c);
    if (mirror && ((int)std::floor(c) & 1)) f = 1 - f;
    return f;
}

void Texture::sample(float u, float v, float out[4]) const {
    if (!w) {
        out[0] = out[1] = out[2] = out[3] = 1;
        return;
    }
    // bilinear
    float x = wrap(u, clampU, mirrorU) * w - 0.5f, y = wrap(v, clampV, mirrorV) * h - 0.5f;
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    auto at = [&](int xi, int yi, int c) {
        xi = std::min(w - 1, std::max(0, xi));
        yi = std::min(h - 1, std::max(0, yi));
        return rgba[((size_t)yi * w + xi) * 4 + c] / 255.f;
    };
    for (int c = 0; c < 4; c++)
        out[c] = (at(x0, y0, c) * (1 - fx) + at(x0 + 1, y0, c) * fx) * (1 - fy) + (at(x0, y0 + 1, c) * (1 - fx) + at(x0 + 1, y0 + 1, c) * fx) * fy;
}

struct Bone {
    std::string name;
    int parent = -1;
    V3 s{1, 1, 1}, r, t;
};

struct Vertex {
    V3 p, n;
    float u = 0, v = 0;
    int idx[4] = {0, 0, 0, 0};
    float w[4] = {1, 0, 0, 0};
};

struct Shape {
    std::string name, material;
    int bone = 0, skin = 0;
    std::vector<Vertex> verts;
    std::vector<uint32_t> tris;
    const Texture* tex = nullptr;   // _a0
    const Texture* tex2 = nullptr;  // _a1 (the eyes' pupil)
    bool overlay = false;           // eyes and brows: drawn over the hair, as the game does
};

// a model of the file: Link's body, or an item hung on one of its joints
struct Part {
    std::vector<Bone> bones;
    std::vector<int> matrixBone;  // skinning matrix index -> bone
    std::vector<M34> rest;        // per bone: its rest world matrix
    std::vector<M34> invBind;     // ... and that one's inverse
    std::vector<Shape> shapes;
    int joint = -1;  // an item: the body's joint it hangs on and its place there
    M34 offset;
    int joint2 = -1;  // a carried thing: held midway between joint and this one (his hands), upright
    std::string tag;  // a thing that goes with him (Request.with)
};

struct Model {
    std::vector<Part> parts;  // [0] the body
    std::map<std::string, Texture> textures;
    const Texture* ramp = nullptr;
};

// a texture of the file by name, decoded (GX2 surface: mip 0, the view's channel selection)
bool load_texture(const Res& r, const std::string& name, Texture& t) {
    size_t f = r.find(1, name);
    if (!f || memcmp(&r.d[f], "FTEX", 4) != 0) return false;
    uint32_t w = r.u32(f + 8), h = r.u32(f + 0xC), fmt = r.u32(f + 0x18), size = r.u32(f + 0x24);
    uint32_t tile = r.u32(f + 0x34), swz = r.u32(f + 0x38);
    size_t data = r.ptr(f + 0xB0);
    if (!data || data + size > r.d.size()) return false;
    Bytes raw;
    uint32_t ch = 0;
    if (!ui_icons::decode_surface(&r.d[data], size, w, h, fmt, tile, swz & 0x700, raw, ch)) {
        LOG("[link_icons] %s: format %x not decoded", name.c_str(), fmt);
        return false;
    }
    uint8_t sel[4] = {r.u8(f + 0x88), r.u8(f + 0x89), r.u8(f + 0x8A), r.u8(f + 0x8B)};
    t.w = (int)w;
    t.h = (int)h;
    t.rgba.resize((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; i++)
        for (int c = 0; c < 4; c++) {
            uint8_t s = sel[c];
            t.rgba[i * 4 + c] = s < ch ? raw[i * ch + s] : s == 4 ? 0 : 255;
        }
    return true;
}

// a vertex attribute (GX2AttribFormat: the float and uint32 ones this model uses) as up to 4 floats
int read_attr(const Res& r, size_t p, uint32_t fmt, float out[4]) {
    int n;
    switch (fmt) {
        case 0x80D: n = 2; break;  // float 32_32
        case 0x811: n = 3; break;
        case 0x813: n = 4; break;
        case 0x806: n = 1; break;
        case 0x10C: n = -2; break;  // uint 32_32
        case 0x110: n = -3; break;
        case 0x112: n = -4; break;
        case 0x105: n = -1; break;
        case 0x100: out[0] = r.u8(p); return 1;  // uint8
        case 0x104: out[0] = r.u8(p); out[1] = r.u8(p + 1); return 2;
        case 0x10A: for (int i = 0; i < 4; i++) out[i] = r.u8(p + i); return 4;
        case 0x00A: for (int i = 0; i < 4; i++) out[i] = r.u8(p + i) / 255.f; return 4;  // unorm 8_8_8_8
        default: return 0;
    }
    for (int i = 0; i < std::abs(n); i++) out[i] = n > 0 ? r.f32(p + 4 * i) : (float)r.u32(p + 4 * i);
    return std::abs(n);
}

bool load_model(const Res& r, const char* name, Model& m, Part& p) {
    size_t f = r.find(0, name);
    if (!f || memcmp(&r.d[f], "FMDL", 4) != 0) return false;
    // skeleton
    size_t skl = r.ptr(f + 0xC);
    int nb = r.u16(skl + 8), nsmooth = r.u16(skl + 10), nrigid = r.u16(skl + 12);
    size_t ba = r.ptr(skl + 0x14), mtb = r.ptr(skl + 0x18);
    for (int i = 0; i < nb; i++) {
        size_t b = ba + 0x40 * i;
        Bone bone;
        bone.name = r.str(b);
        bone.parent = r.s16(b + 6);
        if (bone.parent >= i) bone.parent = -1;  // (parents come first)
        bone.s = {r.f32(b + 0x14), r.f32(b + 0x18), r.f32(b + 0x1C)};
        bone.r = {r.f32(b + 0x20), r.f32(b + 0x24), r.f32(b + 0x28)};
        bone.t = {r.f32(b + 0x30), r.f32(b + 0x34), r.f32(b + 0x38)};
        p.bones.push_back(bone);
    }
    for (int i = 0; i < nsmooth + nrigid; i++) p.matrixBone.push_back(r.u16(mtb + 2 * i));
    std::vector<M34> world(nb);
    for (int i = 0; i < nb; i++) {
        M34 l = srt(p.bones[i].s, p.bones[i].r, p.bones[i].t);
        world[i] = p.bones[i].parent >= 0 ? mul(world[p.bones[i].parent], l) : l;
        p.rest.push_back(world[i]);
        p.invBind.push_back(inverse(world[i]));
    }
    // materials: their textures (_a0 colour, _a1 the toon ramp or the pupil) and samplers' wrap modes
    struct Mat { std::string name, tex0, tex1; uint32_t samp0 = 0; };
    std::vector<Mat> mats;
    for (auto& [mn, mp] : r.group(r.ptr(f + 0x18))) {
        Mat mt;
        mt.name = mn;
        int ns = r.u8(mp + 0x10), nt = r.u8(mp + 0x11);
        size_t tr = r.ptr(mp + 0x28), sm = r.ptr(mp + 0x2C);
        for (int k = 0; k < std::min(ns, nt); k++) {
            std::string samp = r.str(sm + 0x18 * k + 0x10), tex = r.str(tr + 8 * k);
            if (samp == "_a0") {
                mt.tex0 = tex;
                mt.samp0 = r.u32(sm + 0x18 * k);
            } else if (samp == "_a1") {
                mt.tex1 = tex;
            }
        }
        mats.push_back(mt);
    }
    auto texture = [&](const std::string& n, uint32_t samp) -> const Texture* {
        if (n.empty()) return nullptr;
        auto it = m.textures.find(n);
        if (it == m.textures.end()) {
            Texture t;
            if (!load_texture(r, n, t)) return nullptr;
            // GX2 sampler word 0: clamp X bits 0-2, Y bits 3-5 (0 wrap, 1 mirror, 2 clamp...)
            uint32_t cx = samp & 7, cy = (samp >> 3) & 7;
            t.clampU = cx >= 2 && cx != 3;
            t.clampV = cy >= 2 && cy != 3;
            t.mirrorU = cx == 1;
            t.mirrorV = cy == 1;
            it = m.textures.emplace(n, std::move(t)).first;
        }
        return &it->second;
    };
    // shapes and their vertex buffers
    size_t vtxArr = r.ptr(f + 0x10);
    for (auto& [sn, sp] : r.group(r.ptr(f + 0x14))) {
        Shape s;
        s.name = sn;
        int mat = r.u16(sp + 0xE), vb = r.u16(sp + 0x12);
        s.bone = r.u16(sp + 0x10);
        s.skin = r.u8(sp + 0x16);
        if (mat < (int)mats.size()) {
            s.material = mats[mat].name;
            s.tex = texture(mats[mat].tex0, mats[mat].samp0);
            if (mats[mat].tex1 == "ZBtoonEX") {
                if (!m.ramp) m.ramp = texture("ZBtoonEX", 2 | 2 << 3);
            } else {
                s.tex2 = texture(mats[mat].tex1, 2 | 2 << 3);
            }
        }
        size_t v = vtxArr + 0x20 * vb;
        int na = r.u8(v + 4);
        uint32_t count = r.u32(v + 8);
        size_t att = r.ptr(v + 0x10), buf = r.ptr(v + 0x18);
        s.verts.resize(count);
        for (int a = 0; a < na; a++) {
            size_t e = att + 12 * a;
            std::string an = r.str(e);
            int bi = r.u8(e + 4);
            uint32_t off = r.u16(e + 6), fmt = r.u32(e + 8);
            size_t bd = r.ptr(buf + 0x18 * bi + 0x14);
            uint32_t stride = r.u16(buf + 0x18 * bi + 0xC);
            for (uint32_t i = 0; i < count; i++) {
                float x[4] = {0, 0, 0, 1};
                int n = read_attr(r, bd + (size_t)stride * i + off, fmt, x);
                Vertex& vx = s.verts[i];
                if (an == "_p0") vx.p = {x[0], x[1], x[2]};
                else if (an == "_n0") vx.n = {x[0], x[1], x[2]};
                else if (an == "_u0") { vx.u = x[0]; vx.v = x[1]; }
                else if (an == "_i0") for (int k = 0; k < 4; k++) vx.idx[k] = k < n ? (int)x[k] : 0;
                else if (an == "_w0") {
                    float sum = 0;
                    for (int k = 0; k < 4; k++) sum += vx.w[k] = k < n ? x[k] : 0;
                    if (n < s.skin) vx.w[n] = std::max(0.f, 1 - sum);  // (the last weight is implied)
                }
            }
        }
        // the first level of detail: triangles
        size_t mesh = r.ptr(sp + 0x24);
        uint32_t prim = r.u32(mesh), ifmt = r.u32(mesh + 4), npts = r.u32(mesh + 8);
        size_t ib = r.ptr(mesh + 0x14), idata = ib ? r.ptr(ib + 0x14) : 0;
        uint32_t first = r.u32(mesh + 0x18);
        if (prim != 4 || !idata) {
            LOG("[link_icons] %s: primitive %u not drawn", sn.c_str(), prim);
            continue;
        }
        for (uint32_t i = 0; i < npts; i++) {
            uint32_t ix;
            switch (ifmt) {
                case 0: ix = __builtin_bswap16(r.u16(idata + 2 * i)); break;  // u16 LE
                case 1: ix = __builtin_bswap32(r.u32(idata + 4 * i)); break;  // u32 LE
                case 9: ix = r.u32(idata + 4 * i); break;
                default: ix = r.u16(idata + 2 * i); break;  // 4: u16 BE
            }
            s.tris.push_back(ix + first);
        }
        // eyes and brows: the open ones (not the damage faces) over the hair; the bone markers aren't drawn
        if (s.material.rfind("eye", 0) == 0 || s.material.rfind("mayu", 0) == 0) {
            if (s.material.find("dam") != std::string::npos) continue;
            s.overlay = true;
        }
        p.shapes.push_back(std::move(s));
    }
    return true;
}

// ---- an animation
struct Curve {
    int target = 0;  // byte offset in the bone's data: 0x04.. scale, 0x10.. translation, 0x20.. rotation
    int type = 0, keyElems = 4;
    std::vector<float> frames, keys;
    float eval(float fr) const;
};

float Curve::eval(float fr) const {
    size_t n = frames.size();
    if (!n) return 0;
    size_t i = 0;
    while (i + 1 < n && frames[i + 1] <= fr) i++;
    const float* k = &keys[i * keyElems];
    if (type == 2 || type >= 3) return k[0];  // baked / stepped
    float t = 0;
    if (i + 1 < n && frames[i + 1] > frames[i]) t = std::min(1.f, std::max(0.f, (fr - frames[i]) / (frames[i + 1] - frames[i])));
    if (type == 1) return k[0] + k[1] * t;  // linear
    return k[0] + k[1] * t + k[2] * t * t + k[3] * t * t * t;  // cubic
}

struct BoneAnim {
    V3 s{1, 1, 1}, r, t;
    bool hasS = false, hasR = false, hasT = false;
    std::vector<Curve> curves;
};

struct Anim {
    float frames = 0;
    std::map<std::string, BoneAnim> bones;
};

bool load_anim(const Res& r, const std::string& name, Anim& a) {
    size_t f = r.find(2, name);
    if (!f || memcmp(&r.d[f], "FSKA", 4) != 0) return false;
    a.frames = (float)(int32_t)r.u32(f + 0x10);
    int nb = r.u16(f + 0x14);
    size_t ba = r.ptr(f + 0x20);
    for (int i = 0; i < nb; i++) {
        size_t b = ba + 0x18 * i;
        uint32_t fl = r.u32(b);
        int nc = r.u8(b + 10);
        size_t cv = r.ptr(b + 0x10), base = r.ptr(b + 0x14);
        BoneAnim ba2;
        size_t p = base;
        if (fl & 0x08) { ba2.hasS = true; ba2.s = {r.f32(p), r.f32(p + 4), r.f32(p + 8)}; p += 12; }
        if (fl & 0x10) { ba2.hasR = true; ba2.r = {r.f32(p), r.f32(p + 4), r.f32(p + 8)}; p += 16; }
        if (fl & 0x20) { ba2.hasT = true; ba2.t = {r.f32(p), r.f32(p + 4), r.f32(p + 8)}; p += 12; }
        for (int c = 0; c < nc; c++) {
            size_t e = cv + 0x24 * c;
            uint16_t cf = r.u16(e);
            int kc = r.u16(e + 2);
            Curve cu;
            cu.target = (int)r.u32(e + 4);
            int ft = cf & 3, kt = (cf >> 2) & 3;
            cu.type = (cf >> 4) & 7;
            cu.keyElems = cu.type == 0 ? 4 : cu.type == 1 ? 2 : 1;
            float scale = r.f32(e + 0x10), offset = r.f32(e + 0x14);
            size_t fp = r.ptr(e + 0x1C), kp = r.ptr(e + 0x20);
            for (int k = 0; k < kc; k++)
                cu.frames.push_back(ft == 0 ? r.f32(fp + 4 * k) : ft == 1 ? r.s16(fp + 2 * k) / 32.f : (float)r.u8(fp + k));
            for (int k = 0; k < kc * cu.keyElems; k++) {
                float raw = kt == 0 ? r.f32(kp + 4 * k) : kt == 1 ? (float)r.s16(kp + 2 * k) : (float)(int8_t)r.u8(kp + k);
                cu.keys.push_back(kt == 0 ? raw : raw * scale + (k % cu.keyElems == 0 ? offset : 0));
            }
            ba2.curves.push_back(std::move(cu));
        }
        a.bones[r.str(b + 4)] = std::move(ba2);
    }
    return true;
}

// the bones' world matrices at frame fr
// (each bone's own scale, rotation (Euler) and translation at frame fr)
struct Local {
    V3 s, r, t;
};
std::vector<Local> locals(const Part& p, const Anim& a, float fr, bool inPlace) {
    std::vector<Local> out(p.bones.size());
    for (size_t i = 0; i < p.bones.size(); i++) {
        const Bone& b = p.bones[i];
        V3 s = b.s, r = b.r, t = b.t;
        auto it = a.bones.find(b.name);
        if (it != a.bones.end()) {
            const BoneAnim& ba = it->second;
            if (ba.hasS) s = ba.s;
            if (ba.hasR) r = ba.r;
            if (ba.hasT) t = ba.t;
            for (const Curve& c : ba.curves) {
                float v = c.eval(fr);
                float* dst = c.target < 0x10 ? &s.x + (c.target - 4) / 4 : c.target < 0x20 ? &t.x + (c.target - 0x10) / 4 : &r.x + (c.target - 0x20) / 4;
                if (c.target >= 4 && c.target < 0x2C) *dst = v;
            }
        }
        if (inPlace && b.parent < 0) t.x = t.z = 0;
        out[i] = {s, r, t};
    }
    return out;
}

std::vector<M34> world_of(const Part& p, const std::vector<Local>& l) {
    std::vector<M34> world(p.bones.size());
    for (size_t i = 0; i < p.bones.size(); i++) {
        M34 m = srt(l[i].s, l[i].r, l[i].t);
        world[i] = p.bones[i].parent >= 0 ? mul(world[p.bones[i].parent], m) : m;
    }
    return world;
}

std::vector<M34> pose(const Part& p, const Anim& a, float fr, bool inPlace) { return world_of(p, locals(p, a, fr, inPlace)); }

// a pose between two (w 0: a, 1: b), as the game blends one animation into the next; angles the short way round
std::vector<M34> blend(const Part& p, const std::vector<Local>& a, const std::vector<Local>& b, float w) {
    std::vector<Local> l(a.size());
    auto lerp = [w](V3 x, V3 y) { return x + (y - x) * w; };
    auto turn = [w](float x, float y) {
        float d = std::remainder(y - x, 2 * (float)M_PI);
        return x + d * w;
    };
    for (size_t i = 0; i < a.size(); i++) {
        l[i].s = lerp(a[i].s, b[i].s);
        l[i].t = lerp(a[i].t, b[i].t);
        l[i].r = {turn(a[i].r.x, b[i].r.x), turn(a[i].r.y, b[i].r.y), turn(a[i].r.z, b[i].r.z)};
    }
    return world_of(p, l);
}

// ---- drawing
struct Posed {
    V3 p, n;
};

std::vector<Posed> skin(const Part& m, const Shape& s, const std::vector<M34>& world) {
    std::vector<Posed> out(s.verts.size());
    for (size_t i = 0; i < s.verts.size(); i++) {
        const Vertex& v = s.verts[i];
        if (s.skin == 0 || s.skin == 1) {
            int b = s.skin == 0 || m.matrixBone.empty() ? s.bone : m.matrixBone[std::min((size_t)v.idx[0], m.matrixBone.size() - 1)];
            if (b < 0 || b >= (int)world.size()) continue;
            out[i] = {world[b].point(v.p), norm(world[b].dir(v.n))};
            continue;
        }
        V3 p, n;
        for (int k = 0; k < s.skin && k < 4; k++) {
            if (v.w[k] <= 0 || m.matrixBone.empty()) continue;
            int b = m.matrixBone[std::min((size_t)v.idx[k], m.matrixBone.size() - 1)];
            if (b < 0 || b >= (int)world.size()) continue;
            M34 sk = mul(world[b], m.invBind[b]);
            p = p + sk.point(v.p) * v.w[k];
            n = n + sk.dir(v.n) * v.w[k];
        }
        out[i] = {p, norm(n)};
    }
    return out;
}

struct Raster {
    int w, h;
    std::vector<float> depth;
    std::vector<float> col;  // RGBA, premultiplied
    Raster(int w_, int h_) : w(w_), h(h_), depth((size_t)w_ * h_, 1e30f), col((size_t)w_ * h_ * 4, 0) {}
};

struct Camera {
    V3 right, up, fwd;  // fwd points away from the viewer
    float scale = 1, cx = 0, cy = 0;
    V3 project(V3 p) const { return {dot(p, right) * scale + cx, cy - dot(p, up) * scale, dot(p, fwd)}; }
};

// one shape's triangles (light: towards the light, model space)
void draw(Raster& ras, const Camera& cam, const Model& m, const Shape& s, const std::vector<Posed>& pv, V3 light, float depthBias) {
    std::vector<V3> sp(pv.size());
    for (size_t i = 0; i < pv.size(); i++) sp[i] = cam.project(pv[i].p);
    for (size_t t = 0; t + 2 < s.tris.size(); t += 3) {
        uint32_t i0 = s.tris[t], i1 = s.tris[t + 1], i2 = s.tris[t + 2];
        if (i0 >= sp.size() || i1 >= sp.size() || i2 >= sp.size()) continue;
        V3 a = sp[i0], b = sp[i1], c = sp[i2];
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (std::fabs(area) < 1e-9f) continue;
        bool back = area > 0;  // (screen y points down)
        int x0 = std::max(0, (int)std::floor(std::min({a.x, b.x, c.x}))), x1 = std::min(ras.w - 1, (int)std::ceil(std::max({a.x, b.x, c.x})));
        int y0 = std::max(0, (int)std::floor(std::min({a.y, b.y, c.y}))), y1 = std::min(ras.h - 1, (int)std::ceil(std::max({a.y, b.y, c.y})));
        const Vertex &v0 = s.verts[i0], &v1 = s.verts[i1], &v2 = s.verts[i2];
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                float z = a.z * w0 + b.z * w1 + c.z * w2 - depthBias;
                size_t o = (size_t)y * ras.w + x;
                if (z >= ras.depth[o]) continue;
                float u = v0.u * w0 + v1.u * w1 + v2.u * w2, v = v0.v * w0 + v1.v * w1 + v2.v * w2;
                float tc[4];
                if (s.tex) s.tex->sample(u, v, tc);
                else tc[0] = tc[1] = tc[2] = tc[3] = 1;
                if (s.tex2) {  // the eye: its white (_a0) times the pupil (_a1, on white)
                    float pc[4];
                    s.tex2->sample(u, v, pc);
                    for (int k = 0; k < 3; k++) tc[k] *= pc[k];
                }
                if (tc[3] < 0.5f) continue;
                V3 n = norm(pv[i0].n * w0 + pv[i1].n * w1 + pv[i2].n * w2);
                if (back) n = n * -1;
                float l = dot(n, light) * 0.5f + 0.5f, shade = 1;
                if (m.ramp && !s.overlay) {
                    float rc[4];
                    m.ramp->sample(l, 0.5f, rc);
                    shade = 0.62f + 0.38f * rc[0];
                }
                float* d = &ras.col[o * 4];
                for (int k = 0; k < 3; k++) d[k] = tc[k] * shade;  // (vertex colours: white, or black on the face's parts; unused)
                d[3] = 1;
                ras.depth[o] = z;
            }
    }
}

// ss x ss supersampled raster -> size x size picture with an outline (the item icons' dark rim)
std::vector<uint8_t> finish(const Raster& ras, int size, int ss) {
    // coverage per output pixel
    std::vector<float> img((size_t)size * size * 4, 0);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < ss; j++)
                for (int i = 0; i < ss; i++) {
                    const float* c = &ras.col[(((size_t)y * ss + j) * ras.w + x * ss + i) * 4];
                    for (int k = 0; k < 4; k++) acc[k] += c[k];
                }
            for (int k = 0; k < 4; k++) img[((size_t)y * size + x) * 4 + k] = acc[k] / (ss * ss);
        }
    // outline: the coverage grown by a disc, in a dark ink under the picture
    float rad = std::max(1.5f, size / 40.f);
    int ir = (int)std::ceil(rad);
    std::vector<uint8_t> out((size_t)size * size * 4);
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++) {
            float grow = 0;
            for (int j = -ir; j <= ir; j++)
                for (int i = -ir; i <= ir; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= size || yy >= size) continue;
                    float dist = std::sqrt((float)(i * i + j * j));
                    float wgt = std::min(1.f, std::max(0.f, rad + 0.5f - dist));
                    grow = std::max(grow, img[((size_t)yy * size + xx) * 4 + 3] * wgt);
                }
            const float* c = &img[((size_t)y * size + x) * 4];
            static const float ink[3] = {0.08f, 0.1f, 0.16f};
            float a = c[3] + grow * (1 - c[3]);
            uint8_t* o = &out[((size_t)y * size + x) * 4];
            for (int k = 0; k < 3; k++) {
                float v = a > 0 ? (c[k] + ink[k] * grow * (1 - c[3])) / a : 0;
                o[k] = (uint8_t)std::lround(std::min(1.f, v) * 255);
            }
            o[3] = (uint8_t)std::lround(std::min(1.f, a) * 255);
        }
    return out;
}

}  // namespace

int render(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests) {
    Bytes pack;
    Res link, anims;
    if (!game_archive::read_file(gameDir + "/content/Common/Pack/permanent_3d.pack", pack) ||
        !load_res(pack, "Link.szs", "Link.bfres", link) || !load_res(pack, "LkAnm.szs", "LkAnm.bfres", anims)) {
        LOG("[link_icons] Link.bfres / LkAnm.bfres not found in %s", gameDir.c_str());
        return 0;
    }
    Model m;
    m.parts.resize(1);
    if (!load_model(link, "cl", m, m.parts[0])) {
        LOG("[link_icons] model cl not loaded");
        return 0;
    }
    // his sword (its grip: the blade is in the scabbard) and shield on his back, placed on the scabbard's joint
    // as daPy_lk_c::setItemModel does
    int podA = -1;
    for (size_t i = 0; i < m.parts[0].bones.size(); i++)
        if (m.parts[0].bones[i].name == "cl_podA") podA = (int)i;
    struct Gear { const char* model; V3 t; float rz; };
    static const Gear kGear[] = {{"swgripa", {-11.25f, 4.5f, 0.45f}, -40}, {"sha", {15.5f, 4.75f, 0}, -90}};
    for (const Gear& g : kGear) {
        Part p;
        if (podA < 0 || !load_model(link, g.model, m, p)) continue;
        p.joint = podA;
        p.offset = srt({1, 1, 1}, {0, 0, g.rz * (float)M_PI / 180}, g.t);
        m.parts.push_back(std::move(p));
    }
    // the things that go with him (Request.with): the small pot he carries midway between his hands, as
    // daPy_lk_c places the grabbed actor; a chest and a door (their files also hold the animations they move in
    // with his: boxopenshortbox / boxopenshortlink, dooropenadoor / dooropenalink); a Bokoblin's stick; a block
    int lhand = -1, rhand = -1;
    for (size_t i = 0; i < m.parts[0].bones.size(); i++) {
        if (m.parts[0].bones[i].name == "cl_LhandA") lhand = (int)i;
        if (m.parts[0].bones[i].name == "cl_RhandA") rhand = (int)i;
    }
    // (root: a name for the model's root bone, the one its animation moves: the house door door_c hangs on the
    // knob doors' DoorDummy)
    struct Thing { const char* tag; const char* arc; const char* file; const char* model; const char* root; };
    static const Thing kThings[] = {{"pot", "Always.szs", "Always.bfres", "obm_kotubo1", nullptr},
                                    {"chest", "Dalways.szs", "Dalways.bfres", "boxa", nullptr},
                                    {"door", "Knob.szs", "Knob.bfres", "door_c", "DoorDummy"},
                                    {"stick", "Boko.szs", "Boko.bfres", "boko", nullptr},
                                    {"postbox", "Toripost.szs", "Toripost.bfres", "vpost", nullptr}};
    // (the postbox is in the first pack, the others in the 3D pack)
    Bytes firstPack;
    game_archive::read_file(gameDir + "/content/Common/Pack/first_szs_permanent.pack", firstPack);
    std::map<std::string, Res> thingRes;
    for (const Thing& t : kThings) {
        Part p;
        Res& r = thingRes[t.tag];
        const Bytes& from = std::string(t.tag) == "postbox" ? firstPack : pack;
        if (!load_res(from, t.arc, t.file, r) || !load_model(r, t.model, m, p)) {
            LOG("[link_icons] %s (%s) not loaded", t.model, t.arc);
            continue;
        }
        p.tag = t.tag;
        if (t.root && !p.bones.empty()) p.bones[0].name = t.root;
        if (p.tag == "pot") {
            if (lhand < 0 || rhand < 0) continue;
            p.joint = lhand;
            p.joint2 = rhand;
        }
        m.parts.push_back(std::move(p));
    }
    pack.clear();
    firstPack.clear();
    int written = 0;
    for (const Request& rq : requests) {
        Anim a;
        if (!load_anim(anims, rq.anim, a)) {
            LOG("[link_icons] animation %s not found", rq.anim.c_str());
            continue;
        }
        Anim from;  // a pose to blend from (frames 0: none)
        if (!rq.from.empty() && !load_anim(anims, rq.from, from)) LOG("[link_icons] animation %s not found", rq.from.c_str());
        Anim withAnim;  // the thing's own animation (frames 0: none)
        if (!rq.withAnim.empty() && !load_anim(thingRes[rq.with], rq.withAnim, withAnim))
            LOG("[link_icons] animation %s not found", rq.withAnim.c_str());
        float last = rq.last >= 0 ? rq.last : rq.noLink && withAnim.frames > 0 ? std::max(0.f, withAnim.frames - 1.f) : std::max(0.f, a.frames - 1);
        // the camera: around Link (y up), looking at him
        float yaw = rq.yaw * (float)M_PI / 180, pitch = rq.pitch * (float)M_PI / 180;
        Camera cam;
        cam.fwd = norm({-std::sin(yaw) * std::cos(pitch), -std::sin(pitch), -std::cos(yaw) * std::cos(pitch)});
        cam.right = norm(cross(cam.fwd, {0, 1, 0}));
        cam.up = cross(cam.right, cam.fwd);
        V3 light = norm(cam.right * -0.45f + cam.up * 0.7f - cam.fwd * 0.55f);
        // the frames' poses: [frame][part][shape]; each frame centred, one scale for all (he keeps his size)
        struct Frame {
            std::vector<std::vector<std::vector<Posed>>> parts;
            float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        };
        std::vector<Frame> frames;
        float extent = 0;
        for (int i = 0; i < rq.frames + (int)rq.extra.size(); i++) {
            float fr = i >= rq.frames ? rq.extra[i - rq.frames] : rq.frames > 1 ? rq.first + (last - rq.first) * i / (rq.frames - 1) : rq.first;
            std::vector<M34> body;
            if (from.frames > 0) {  // from the other pose into this one: fr is how far (0..1), eased in and out
                float w = std::min(1.f, std::max(0.f, fr));
                w = w * w * (3 - 2 * w);
                body = blend(m.parts[0], locals(m.parts[0], from, rq.fromFrame, rq.inPlace), locals(m.parts[0], a, rq.toFrame, rq.inPlace), w);
            } else {
                body = pose(m.parts[0], a, fr, rq.inPlace);
            }
            Frame f;
            for (size_t pi = 0; pi < m.parts.size(); pi++) {
                const Part& p = m.parts[pi];
                if (!p.tag.empty() ? p.tag != rq.with : rq.noLink || (pi > 0 && !rq.gear)) {
                    f.parts.emplace_back();
                    continue;
                }
                std::vector<M34> world = body;
                if (!p.tag.empty() && p.joint < 0) {  // standing in front of him, in its own animation (or at rest)
                    M34 at = srt({1, 1, 1}, {0, rq.withYaw * (float)M_PI / 180, 0}, {rq.withAt[0], rq.withAt[1], rq.withAt[2]});
                    world = withAnim.frames > 0 ? pose(p, withAnim, fr, false) : p.rest;
                    for (M34& w : world) w = mul(at, w);
                } else if (p.joint >= 0) {
                    M34 base = mul(body[p.joint], p.offset);
                    if (p.joint2 >= 0) {
                        base = p.offset;
                        for (int k = 3; k < 12; k += 4) base.m[k] += (body[p.joint].m[k] + body[p.joint2].m[k]) / 2;
                    }
                    world.resize(p.rest.size());
                    for (size_t b = 0; b < p.rest.size(); b++) world[b] = mul(base, p.rest[b]);
                }
                std::vector<std::vector<Posed>> shapes;
                for (const Shape& s : p.shapes) {
                    shapes.push_back(skin(p, s, world));
                    for (const Posed& v : shapes.back()) {
                        float x = dot(v.p, cam.right), y = dot(v.p, cam.up);
                        f.lo[0] = std::min(f.lo[0], x), f.hi[0] = std::max(f.hi[0], x);
                        f.lo[1] = std::min(f.lo[1], y), f.hi[1] = std::max(f.hi[1], y);
                    }
                }
                f.parts.push_back(std::move(shapes));
            }
            extent = std::max({extent, f.hi[0] - f.lo[0], f.hi[1] - f.lo[1]});
            frames.push_back(std::move(f));
        }
        const int ss = 4, n = rq.size * ss;
        float margin = rq.size / 16.f * ss;  // room for the outline
        cam.scale = (n - 2 * margin) / std::max(extent, 1e-3f);
        for (size_t i = 0; i < frames.size(); i++) {
            const Frame& f = frames[i];
            cam.cx = n / 2.f - (f.lo[0] + f.hi[0]) / 2 * cam.scale;
            cam.cy = n / 2.f + (f.lo[1] + f.hi[1]) / 2 * cam.scale;
            Raster ras(n, n);
            for (int overlay = 0; overlay < 2; overlay++)
                for (size_t pi = 0; pi < m.parts.size(); pi++)
                    for (size_t k = 0; k < f.parts[pi].size(); k++) {
                        const Shape& s = m.parts[pi].shapes[k];
                        if (s.overlay == (overlay == 1)) draw(ras, cam, m, s, f.parts[pi][k], light, overlay ? 12 : 0);
                    }
            std::vector<uint8_t> px = finish(ras, rq.size, ss);
            std::string path = outDir + "/" + rq.name + "_" + std::to_string(i) + ".rgba";
            if (FILE* o = fopen(path.c_str(), "wb")) {
                uint32_t hd[2] = {(uint32_t)rq.size, (uint32_t)rq.size};
                fwrite(hd, 4, 2, o);
                fwrite(px.data(), 1, px.size(), o);
                fclose(o);
                written++;
            }
        }
    }
    LOG("[link_icons] %d pictures", written);
    return written;
}

int render_buttons(const std::string& gameDir, const std::string& outDir) {
    std::vector<Request> rq(6);
    // jump: the auto jump's leap (its last frame); facing right, three quarters
    rq[0].name = "link_jump";
    rq[0].anim = "jmpst";
    rq[0].first = rq[0].last = 6;
    rq[0].yaw = -60;
    // crouch: crouched down
    rq[1].name = "link_crouch";
    rq[1].anim = "crouch";
    rq[1].first = rq[1].last = 19;
    rq[1].yaw = -60;
    // ... and animated (the game's crouch is only the crouched pose; it blends into it): from standing down to the
    // crouch, and back up (ZR while crouched: stand up)
    Request down;
    down.name = "link_kneel";
    down.anim = "crouch";
    down.toFrame = 19;
    down.from = "wait";
    down.first = 0;
    down.last = 1;
    down.frames = 8;
    down.yaw = -60;
    rq.push_back(down);
    Request up = down;
    up.name = "link_rise";
    up.first = 1;
    up.last = 0;
    rq.push_back(up);
    // roll: from his lean forward to standing up again, from the side (the button plays it in a loop)
    rq[2].name = "link_roll";
    rq[2].anim = "rollf";
    rq[2].first = 4;
    rq[2].last = 16;
    rq[2].frames = 12;
    rq[2].extra = {5.65f};  // link_roll_12: the button's resting picture, diving in (the lean before looks like a jump)
    rq[2].yaw = -90;
    // stand up (ZR while crouched): standing as he waits
    rq[3].name = "link_stand";
    rq[3].anim = "wait";
    rq[3].yaw = -60;
    // third person (ZL while aiming: back to the camera behind him): standing, seen from behind and above
    rq[4].name = "link_back";
    rq[4].anim = "wait";
    rq[4].yaw = 150;
    rq[4].pitch = 30;
    // sidle (A at a wall): his back to the wall, stepping to his left; from the front, a little to the side. One step
    // of his cycle (30 frames), every 2nd frame from the stepping pose (6) round to it again (the button plays them,
    // then keeps the last)
    rq[5].name = "link_sidle";
    rq[5].anim = "wallwl";
    rq[5].first = rq[5].last = 6;
    for (int f = 8; f <= 36; f += 2) rq[5].extra.push_back((float)(f % 30));
    rq[5].yaw = 25;
    rq[5].pitch = 15;
    // carrying a pot (A Lift / Throw, B Throw, ZR Drop), 8 pictures each (the button plays them, then keeps the
    // last): lifting it from the ground over his head, from over his head lunging forward as he throws it, and the
    // lift backwards as he sets it down whole
    static const struct { const char* name; const char* anim; float first, last; } kCarry[] = {
        {"link_lift", "grabup", 0, 7}, {"link_throw", "grabthrow", 0, 2.5f}, {"link_drop", "grabup", 7, 0}};
    for (const auto& c : kCarry) {
        Request r;
        r.name = c.name;
        r.anim = c.anim;
        r.first = c.first;
        r.last = c.last;
        r.frames = 8;
        r.yaw = -60;
        r.with = "pot";
        rq.push_back(r);
    }
    // the other actions, played by the button the same way:
    // A Open at a door: pushing the house door open (until he walks through), from his side, behind him
    Request door;
    door.name = "link_door";
    door.anim = "dooropenalink";
    door.last = 56;
    door.frames = 12;
    door.yaw = -120;
    door.inPlace = false;
    door.with = "door";
    door.withAnim = "dooropenadoor";
    rq.push_back(door);
    // A Open at a chest: opening the small chest, from the side
    Request chest;
    chest.name = "link_chest";
    chest.anim = "boxopenshortlink";
    chest.last = 29;
    chest.frames = 10;
    chest.yaw = -95;
    chest.pitch = 20;
    chest.inPlace = false;
    chest.with = "chest";
    chest.withAnim = "boxopenshortbox";
    chest.withAt[2] = 55;
    chest.withYaw = 180;
    rq.push_back(chest);
    // ZR Grab at a block: pushing (one step of the cycle, every 2nd frame; the block is drawn by GameIcons)
    Request push;
    push.name = "link_push";
    push.anim = "walkpush";
    push.last = 22;
    push.frames = 12;
    push.yaw = -90;
    rq.push_back(push);
    // A Pick up at a Bokoblin's stick: crouching down to it
    Request pick;
    pick.name = "link_pickup";
    pick.anim = "grabp";
    pick.last = 5;
    pick.frames = 8;
    pick.yaw = -70;
    pick.pitch = 25;
    pick.with = "stick";
    pick.withAt[2] = 80;
    rq.push_back(pick);
    // A at a postbox (postbox.cpp): the postbox itself swallowing a letter, from the front, a little to the side
    Request box;
    box.name = "link_postbox";
    box.anim = "wait";
    box.noLink = true;
    box.frames = 10;
    box.yaw = 25;
    box.pitch = 12;
    box.with = "postbox";
    box.withAnim = "post_get";
    rq.push_back(box);
    for (Request& r : rq) r.size = 160;
    return render(gameDir, outDir, rq);
}
}  // namespace link_icons
