// The mini map indoors (map_plan.h): a stage's files are content/Common/Stage/<stage>_Stage.szs and
// <stage>_Room<n>.szs (Yaz0 SARCs; only sea rooms are kept in the szs_permanent packs instead, never needed here),
// each with one BFRES (Stage.bfres / Room<n>.bfres) whose embedded files hold the GameCube stage data: stage.dzs,
// room.dzr, room.dzb (collision, cBgD_t as on the GameCube: c_bg_w.h).
//   DMAP (dStage_DMap_dt_c {originX, originZ, scale, offsetY}): the GamePad dungeon map's world origin and scale (a
//   picture texel of DungeonMapFloor_0x is 0.382 x scale world units, its layout's (0, 0) is the origin; found by
//   laying the pictures over floor plans of the collision, all six dungeons within a few texels; MiniMap.java).
//   room.dzb faces: {vtx0, vtx1, vtx2, info, group} u16; info entries 0x10 bytes, the attribute code (dBgS_AttributeCode,
//   d_bg_s.h) bits 16..20 of the second word: lava 6, void 8, water 0x13 and waterfalls 0x17 are no floor on the map.
//   TRES (stage_actor_data_class, 0x20: name[8], params, pos, angle, setID): treasure chests; in stage.dzs the room
//   is angle.x, the chest's number params >> 7 & 0x1F (daTbox_c::getTboxNo). TGDR (stage_tgsc_data_class, 0x24):
//   doors and shutters, angle.x the rooms on both sides (6 bits each, as d_menu_dmap.cpp reads them).
//   FLOR (dStage_FloorInfo_dt_c, 0x14 bytes {height f32, floorNo u8 (0x80 = 1F)}): where each floor starts.
#include "map_plan.h"
#include "game_archive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "../runtime.h"

namespace map_plan {
namespace {
using game_archive::be16;
using game_archive::be32;
using game_archive::Bytes;

float bef(const uint8_t* p) {
    uint32_t v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}

// a BFRES's embedded file by name (index group 11; offsets relative to the field holding them)
bool fres_file(const Bytes& b, const char* name, Bytes& out) {
    if (b.size() < 0x50 || memcmp(b.data(), "FRES", 4) != 0) return false;
    auto rel = [&](size_t at) -> size_t {
        int32_t v = (int32_t)be32(&b[at]);
        return v ? at + v : 0;
    };
    size_t g = rel(0x20 + 11 * 4);
    if (!g || g + 8 > b.size()) return false;
    uint32_t n = be32(&b[g + 4]);
    for (uint32_t i = 1; i <= n; i++) {
        size_t e = g + 8 + 16 * i;
        if (e + 16 > b.size()) return false;
        size_t nm = rel(e + 8), data = rel(e + 12);
        if (!nm || !data || nm >= b.size() || strcmp((const char*)&b[nm], name) != 0) continue;
        size_t off = rel(data), size = be32(&b[data + 4]);
        if (!off || off + size > b.size()) return false;
        out.assign(b.begin() + off, b.begin() + off + size);
        return true;
    }
    return false;
}

// <stage>_<part>.szs -> <bfres> -> its embedded file
bool stage_file(const std::string& gameDir, const std::string& stage, const std::string& part, const char* bfres,
                const char* file, Bytes& out) {
    Bytes packed, sarc, fres;
    std::string path = gameDir + "/content/Common/Stage/" + stage + "_" + part + ".szs";
    return game_archive::read_file(path, packed) && game_archive::yaz0(packed, sarc) &&
           game_archive::sarc_file(sarc, bfres, fres) && fres_file(fres, file, out);
}

// a DZS / DZR chunk: count and offset of its entries
bool chunk(const Bytes& d, const char* tag, uint32_t& count, uint32_t& off) {
    if (d.size() < 4) return false;
    uint32_t n = be32(&d[0]);
    for (uint32_t i = 0; i < n && 4 + 12 * i + 12 <= d.size(); i++) {
        const uint8_t* e = &d[4 + 12 * i];
        if (memcmp(e, tag, 4)) continue;
        count = be32(e + 4);
        off = be32(e + 8);
        return true;
    }
    return false;
}

// cheap smooth noise over world positions (the pictures' mottling): 0..1
float noise(float x, float z) {
    auto h = [](int32_t i, int32_t j) {
        uint32_t v = (uint32_t)i * 374761393u + (uint32_t)j * 668265263u;
        v = (v ^ (v >> 13)) * 1274126177u;
        return ((v ^ (v >> 16)) & 0xFFFF) / 65535.f;
    };
    int32_t i = (int32_t)std::floor(x), j = (int32_t)std::floor(z);
    float fx = x - i, fz = z - j;
    fx = fx * fx * (3 - 2 * fx);
    fz = fz * fz * (3 - 2 * fz);
    float a = h(i, j) + (h(i + 1, j) - h(i, j)) * fx, b = h(i, j + 1) + (h(i + 1, j + 1) - h(i, j + 1)) * fx;
    return a + (b - a) * fz;
}
}  // namespace

bool stage_info(const std::string& gameDir, const std::string& stage, std::vector<float>& out) {
    out.clear();
    Bytes dzs;
    if (!stage_file(gameDir, stage, "Stage", "Stage.bfres", "stage.dzs", dzs)) return false;
    uint32_t n, off;
    if (chunk(dzs, "DMAP", n, off) && n && off + 16 <= dzs.size())
        for (int i = 0; i < 3; i++) out.push_back(bef(&dzs[off + 4 * i]));
    else
        out.insert(out.end(), {0, 0, 0});
    if (chunk(dzs, "FLOR", n, off))
        for (uint32_t i = 0; i < n && off + 0x14 * (i + 1) <= dzs.size(); i++) {
            out.push_back(bef(&dzs[off + 0x14 * i]));
            out.push_back((float)((int)dzs[off + 0x14 * i + 4] - 0x80));
        }
    return true;
}

bool chart_list(const std::string& gameDir, std::vector<uint8_t>& out) {
    Bytes packed, sarc, fres;
    return game_archive::read_file(gameDir + "/content/Common/Misc/Misc.szs", packed) && game_archive::yaz0(packed, sarc) &&
           game_archive::sarc_file(sarc, "Misc.bfres", fres) && fres_file(fres, "cmapdat.bin", out) && out.size() >= 4;
}

bool marks(const std::string& gameDir, const std::string& stage, std::vector<float>& out) {
    out.clear();
    auto chests = [&](const Bytes& d, int room) {
        uint32_t n, off;
        if (!chunk(d, "TRES", n, off)) return;
        for (uint32_t i = 0; i < n && off + 0x20 * (i + 1) <= d.size(); i++) {
            const uint8_t* e = &d[off + 0x20 * i];
            uint32_t prm = be32(e + 8);
            int r = room >= 0 ? room : (int)(be16(e + 0x18) & 0x3F);
            out.insert(out.end(), {0, bef(e + 0xC), bef(e + 0x10), bef(e + 0x14), (float)(prm >> 7 & 0x1F), (float)r, 0});
        }
    };
    Bytes dzs;
    if (!stage_file(gameDir, stage, "Stage", "Stage.bfres", "stage.dzs", dzs)) return false;
    chests(dzs, -1);
    uint32_t n, off;
    if (chunk(dzs, "TGDR", n, off))
        for (uint32_t i = 0; i < n && off + 0x24 * (i + 1) <= dzs.size(); i++) {
            const uint8_t* e = &dzs[off + 0x24 * i];
            uint16_t ax = be16(e + 0x18);
            float ay = (int16_t)be16(e + 0x1A) * (float)(M_PI / 32768.0);
            out.insert(out.end(), {1, bef(e + 0xC), bef(e + 0x10), bef(e + 0x14), ay, (float)(ax & 0x3F), (float)(ax >> 6 & 0x3F)});
        }
    for (int room = 0; room < 64; room++) {
        Bytes dzr;
        if (stage_file(gameDir, stage, "Room" + std::to_string(room), ("Room" + std::to_string(room) + ".bfres").c_str(), "room.dzr", dzr))
            chests(dzr, room);
    }
    return true;
}

}  // namespace map_plan

namespace map_plan {
namespace {
// a file of a large SARC (the szs_permanent packs, ~46 MB) without reading all of it: its tables, then the file
bool pack_file(const std::string& path, const char* name, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t h[0x14];
    bool ok = false;
    if (fread(h, 1, sizeof h, f) == sizeof h && memcmp(h, "SARC", 4) == 0) {
        uint32_t data = be32(&h[0xC]);
        Bytes t(data);
        fseek(f, 0, SEEK_SET);
        if (data >= 0x14 && fread(t.data(), 1, data, f) == data) {
            size_t p = be16(&h[4]);
            uint32_t n = p + 12 <= t.size() ? be16(&t[p + 6]) : 0;
            size_t nodes = p + 12, names = nodes + 16 * n + 8;
            for (uint32_t i = 0; i < n && names <= t.size() && !ok; i++) {
                const uint8_t* e = &t[nodes + 16 * i];
                uint32_t attr = be32(e + 4), start = be32(e + 8), end = be32(e + 12);
                size_t o = names + (attr & 0xFFFFFF) * 4;
                if (!(attr >> 24) || o >= t.size() || strncmp((const char*)&t[o], name, t.size() - o) != 0 || end < start) continue;
                out.resize(end - start);
                ok = fseek(f, (long)(data + start), SEEK_SET) == 0 && fread(out.data(), 1, out.size(), f) == out.size();
                break;
            }
        }
    }
    fclose(f);
    return ok;
}

bool sea_dzb(const std::string& gameDir, int room, Bytes& out) {
    std::string part = "Room" + std::to_string(room), bfres = part + ".bfres";
    if (stage_file(gameDir, "sea", part, bfres.c_str(), "room.dzb", out)) return true;
    std::string szs = "sea_" + part + ".szs";
    for (const char* pack : {"szs_permanent1.pack", "szs_permanent2.pack", "szs_permanent0.pack", "first_szs_permanent.pack"}) {
        Bytes packed, sarc, fres;
        if (pack_file(gameDir + "/content/Common/Pack/" + pack, szs.c_str(), packed))
            return game_archive::yaz0(packed, sarc) && game_archive::sarc_file(sarc, bfres.c_str(), fres) &&
                   fres_file(fres, "room.dzb", out);
    }
    return false;
}

// the ground's colour by its attribute code (dBgS_AttributeCode; 7 rock and trees, 0xD rock ledges, 0x10 roofs:
// seen on the islands)
uint32_t ground_colour(uint32_t attr) {
    switch (attr) {
    case 0x01: return 0xA8804E;  // dirt
    case 0x02: return 0x9C6C3A;  // wood
    case 0x03: return 0x9E988C;  // stone
    case 0x04: return 0x6A9E42;  // grass
    case 0x05: return 0xD86A9A;  // giant flower
    case 0x06: return 0xE0501A;  // lava
    case 0x07: return 0x8C7A62;  // rock, trees
    case 0x0B: return 0xE6D08C;  // sand
    case 0x0D: return 0x7A7468;  // rock ledges
    case 0x0F: return 0xDCEBF5;  // ice
    case 0x10: return 0xA8503C;  // roofs
    case 0x13: case 0x17: return 0x5A9BD8;  // water, waterfall
    case 0x14: return 0x8890A0;  // metal
    default: return 0xB88A48;
    }
}
}  // namespace

bool sea_terrain(const std::string& gameDir, int room, Plan& out) {
    Bytes dzb;
    if (room < 1 || room > 49 || !sea_dzb(gameDir, room, dzb) || dzb.size() < 0x34) return false;
    uint32_t vn = be32(&dzb[0]), vo = be32(&dzb[4]), tn = be32(&dzb[8]), to = be32(&dzb[0xC]);
    uint32_t in = be32(&dzb[0x28]), io = be32(&dzb[0x2C]);
    if ((size_t)vo + 12ull * vn > dzb.size() || (size_t)to + 10ull * tn > dzb.size() || (size_t)io + 16ull * in > dzb.size()) return false;
    const float SHALLOW = -1500;  // deepest ground shown as shallow water
    struct Tri { float x[3], y[3], z[3]; uint32_t attr; bool wall; };
    std::vector<Tri> tris;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (uint32_t t = 0; t < tn; t++) {
        const uint8_t* e = &dzb[to + 10 * t];
        uint32_t k[3] = {be16(e), be16(e + 2), be16(e + 4)};
        if (k[0] >= vn || k[1] >= vn || k[2] >= vn) continue;
        uint32_t id = be16(e + 6);
        Tri tr;
        tr.attr = id < in ? be32(&dzb[io + 16 * id + 4]) >> 16 & 0x1F : 0;
        float ymax = -1e30f, xa = 1e30f, xb = -1e30f, za = 1e30f, zb = -1e30f;
        for (int i = 0; i < 3; i++) {
            tr.x[i] = bef(&dzb[vo + 12 * k[i]]);
            tr.y[i] = bef(&dzb[vo + 12 * k[i] + 4]);
            tr.z[i] = bef(&dzb[vo + 12 * k[i] + 8]);
            ymax = std::max(ymax, tr.y[i]);
            xa = std::min(xa, tr.x[i]); xb = std::max(xb, tr.x[i]);
            za = std::min(za, tr.z[i]); zb = std::max(zb, tr.z[i]);
        }
        // (the sea floor and the square-wide planes some rooms have are no island)
        if (ymax <= SHALLOW || xb - xa > 30000 || zb - za > 30000) continue;
        float bx = tr.x[1] - tr.x[0], by = tr.y[1] - tr.y[0], bz = tr.z[1] - tr.z[0];
        float cx = tr.x[2] - tr.x[0], cy = tr.y[2] - tr.y[0], cz = tr.z[2] - tr.z[0];
        float nx = by * cz - bz * cy, ny = bz * cx - bx * cz, nz = bx * cy - by * cx;
        float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len <= 0) continue;
        tr.wall = std::fabs(ny) / len < 0.3f;
        if (ymax > 0) {
            x0 = std::min(x0, xa); x1 = std::max(x1, xb);
            z0 = std::min(z0, za); z1 = std::max(z1, zb);
        }
        tris.push_back(tr);
    }
    if (x1 < x0) return false;
    float upp = std::max(12.f, std::max(x1 - x0, z1 - z0) / 1000.f);
    int pad = (int)(1500 / upp) + 4;  // room for the shallows round the land
    x0 -= upp * pad;
    z0 -= upp * pad;
    int w = (int)((x1 - x0) / upp) + pad + 1, h = (int)((z1 - z0) / upp) + pad + 1;
    const float NONE = -1e30f;
    std::vector<float> Y((size_t)w * h, NONE);
    std::vector<uint8_t> A((size_t)w * h, 0), wall((size_t)w * h, 0);
    for (const Tri& t : tris) {
        float px[3], pz[3];
        for (int i = 0; i < 3; i++) {
            px[i] = (t.x[i] - x0) / upp;
            pz[i] = (t.z[i] - z0) / upp;
        }
        if (t.wall) {
            // cliffs (above the water): their lines seen from above, for the rock with no ground on top
            if (std::max({t.y[0], t.y[1], t.y[2]}) <= 0) continue;
            for (int i = 0; i < 3; i++) {
                float ax = px[i], az = pz[i], bx = px[(i + 1) % 3], bz = pz[(i + 1) % 3];
                int steps = (int)std::ceil(std::max(std::fabs(bx - ax), std::fabs(bz - az))) + 1;
                for (int s = 0; s <= steps; s++) {
                    int xx = (int)(ax + (bx - ax) * s / steps), zz = (int)(az + (bz - az) * s / steps);
                    if (xx >= 0 && zz >= 0 && xx < w && zz < h) wall[(size_t)zz * w + xx] = 1;
                }
            }
            continue;
        }
        int bx0 = std::max(0, (int)std::floor(std::min({px[0], px[1], px[2]}))),
            bx1 = std::min(w - 1, (int)std::ceil(std::max({px[0], px[1], px[2]})));
        int bz0 = std::max(0, (int)std::floor(std::min({pz[0], pz[1], pz[2]}))),
            bz1 = std::min(h - 1, (int)std::ceil(std::max({pz[0], pz[1], pz[2]})));
        float area = (px[1] - px[0]) * (pz[2] - pz[0]) - (px[2] - px[0]) * (pz[1] - pz[0]);
        if (std::fabs(area) < 1e-6f) continue;
        for (int y = bz0; y <= bz1; y++)
            for (int x = bx0; x <= bx1; x++) {
                float qx = x + 0.5f, qz = y + 0.5f;
                float w0 = ((px[1] - qx) * (pz[2] - qz) - (px[2] - qx) * (pz[1] - qz)) / area;
                float w1 = ((px[2] - qx) * (pz[0] - qz) - (px[0] - qx) * (pz[2] - qz)) / area;
                float w2 = 1 - w0 - w1;
                const float e = -0.02f;
                if (w0 < e || w1 < e || w2 < e) continue;
                float yy = w0 * t.y[0] + w1 * t.y[1] + w2 * t.y[2];
                size_t i = (size_t)y * w + x;
                if (yy > Y[i]) {
                    Y[i] = yy;
                    A[i] = (uint8_t)t.attr;
                }
            }
    }
    auto land = [&](size_t i) { return Y[i] > 0 && A[i] != 0x13 && A[i] != 0x17; };
    // rock with only its cliffs: what the sea can't reach past the cliff lines (from the picture's edge), not ground
    std::vector<uint8_t> sea((size_t)w * h, 0);
    std::vector<int> q;
    for (int x = 0; x < w; x++) q.insert(q.end(), {x, (h - 1) * w + x});
    for (int y = 0; y < h; y++) q.insert(q.end(), {y * w, y * w + w - 1});
    for (size_t k = 0; k < q.size(); k++) {
        int i = q[k];
        if (sea[i] || wall[i] || Y[i] > 0) continue;
        sea[i] = 1;
        int x = i % w, y = i / w;
        if (x > 0) q.push_back(i - 1);
        if (x < w - 1) q.push_back(i + 1);
        if (y > 0) q.push_back(i - w);
        if (y < h - 1) q.push_back(i + w);
    }
    std::vector<uint8_t> rock((size_t)w * h, 0);
    for (size_t i = 0; i < rock.size(); i++)
        if (!sea[i] && !(Y[i] > 0)) rock[i] = 1;
    out.argb.assign((size_t)w * h, 0);
    auto at = [&](int x, int y) { return Y[(size_t)std::max(0, std::min(h - 1, y)) * w + std::max(0, std::min(w - 1, x))]; };
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            size_t i = (size_t)y * w + x;
            bool isLand = land(i) || rock[i];
            if (!isLand) {
                if (Y[i] > SHALLOW && Y[i] <= 0) {  // shallow water, fading out with the depth
                    float f = 1 + Y[i] / -SHALLOW;
                    out.argb[i] = (uint32_t)(0x30 + 0x80 * f) << 24 | 0x9CCFEA;
                } else if (Y[i] > 0)  // ponds, waterfalls
                    out.argb[i] = 0xFF000000u | ground_colour(A[i]);
                continue;
            }
            uint32_t c = rock[i] ? 0x8C7A62 : ground_colour(A[i]);
            float shade = 0.92f;
            if (!rock[i]) {
                // light from the north-west: the slope's normal against it
                auto h4 = [&](int xx, int yy) { float v = at(xx, yy); return v > 0 ? v : 0.f; };
                float gx = (h4(x + 1, y) - h4(x - 1, y)) / (2 * upp), gz = (h4(x, y + 1) - h4(x, y - 1)) / (2 * upp);
                float nx = -gx, ny = 1, nz = -gz, nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                float d = (-nx + 1.5f * ny - nz) / (nl * 2.0616f);
                shade = 0.5f + 0.62f * std::max(0.f, d);
                shade *= 0.94f + 0.08f * std::min(1.f, Y[i] / 5000.f);
            } else
                shade = 0.8f + 0.15f * noise(x * upp / 400.f, y * upp / 400.f);
            // the coast: dark where the land meets water
            bool coast = false;
            for (int dy = -1; dy <= 1 && !coast; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    int xx = x + dx, yy = y + dy;
                    if (xx < 0 || yy < 0 || xx >= w || yy >= h) continue;
                    size_t j = (size_t)yy * w + xx;
                    if (!land(j) && !rock[j]) { coast = true; break; }
                }
            int r = (int)((c >> 16 & 0xFF) * shade), g = (int)((c >> 8 & 0xFF) * shade), b = (int)((c & 0xFF) * shade);
            if (coast) { r = r * 3 / 10 + 0x28 * 7 / 10; g = g * 3 / 10 + 0x1E * 7 / 10; b = b * 3 / 10 + 0x05 * 7 / 10; }
            out.argb[i] = 0xFF000000u | (uint32_t)std::min(255, r) << 16 | (uint32_t)std::min(255, g) << 8 | (uint32_t)std::min(255, b);
        }
    out.w = w;
    out.h = h;
    out.x0 = x0;
    out.z0 = z0;
    out.upp = upp;
    LOG("[minimap] terrain sea %d: %zu faces, %dx%d at %.1f", room, tris.size(), w, h, upp);
    return true;
}

namespace {
// the last stage's rooms' collision (a dungeon's plan is made again as rooms are visited): {room, room.dzb}
std::mutex dzbLock;
std::string dzbStage;
std::vector<std::pair<int, Bytes>> dzbRooms;
}  // namespace

bool room_plan(const std::string& gameDir, const std::string& stage, float yMin, float yMax, Plan& out, int style,
               uint64_t lit, uint64_t dim, const float* window, float fineUpp) {
    std::lock_guard<std::mutex> lock(dzbLock);
    if (dzbStage != gameDir + "|" + stage) {
        dzbRooms.clear();
        for (int room = 0; room < 64; room++) {
            Bytes dzb;
            if (stage_file(gameDir, stage, "Room" + std::to_string(room), ("Room" + std::to_string(room) + ".bfres").c_str(),
                           "room.dzb", dzb) && dzb.size() >= 0x34)
                dzbRooms.emplace_back(room, std::move(dzb));
        }
        dzbStage = gameDir + "|" + stage;
    }
    const bool terrain = style == PLAN_TERRAIN;
    struct Tri { float x[3], y[3], z[3]; uint8_t attr, room; };
    std::vector<Tri> tris;
    float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
    for (const auto& rd : dzbRooms) {
        int room = rd.first;
        if (!(lit >> room & 1) && !(dim >> room & 1)) continue;
        const Bytes& dzb = rd.second;
        uint32_t vn = be32(&dzb[0]), vo = be32(&dzb[4]), tn = be32(&dzb[8]), to = be32(&dzb[0xC]);
        uint32_t in = be32(&dzb[0x28]), io = be32(&dzb[0x2C]);
        if ((size_t)vo + 12ull * vn > dzb.size() || (size_t)to + 10ull * tn > dzb.size() || (size_t)io + 16ull * in > dzb.size()) continue;
        auto v = [&](uint32_t k, int c) { return bef(&dzb[vo + 12 * k + 4 * c]); };
        for (uint32_t t = 0; t < tn; t++) {
            const uint8_t* e = &dzb[to + 10 * t];
            uint32_t k[3] = {be16(e), be16(e + 2), be16(e + 4)};
            if (k[0] >= vn || k[1] >= vn || k[2] >= vn) continue;
            uint32_t id = be16(e + 6), attr = id < in ? be32(&dzb[io + 16 * id + 4]) >> 16 & 0x1F : 0;
            // void is no floor; lava and water only on the terrain (in their colours), never on the plain plan
            if (attr == 8 || !terrain && (attr == 6 || attr == 0x13 || attr == 0x17)) continue;
            float ax = v(k[0], 0), ay = v(k[0], 1), az = v(k[0], 2);
            float bx = v(k[1], 0) - ax, by = v(k[1], 1) - ay, bz = v(k[1], 2) - az;
            float cx = v(k[2], 0) - ax, cy = v(k[2], 1) - ay, cz = v(k[2], 2) - az;
            float nx = by * cz - bz * cy, ny = bz * cx - bx * cz, nz = bx * cy - by * cx;
            float len = std::sqrt(nx * nx + ny * ny + nz * nz);
            float my = ay + (by + cy) / 3;
            if (len <= 0 || ny / len < 0.5f || my < yMin || my > yMax) continue;  // floors only, of this height
            // (the stage-wide planes round some dungeons are no floor)
            float ex = std::max({std::fabs(bx), std::fabs(cx), std::fabs(bx - cx)}), ez = std::max({std::fabs(bz), std::fabs(cz), std::fabs(bz - cz)});
            if (ex > 30000 || ez > 30000) continue;
            if (window) {
                // (a window: only faces reaching into it, a margin round it for the edges)
                float m = 12 * fineUpp;
                float fx0 = std::min({ax, ax + bx, ax + cx}), fx1 = std::max({ax, ax + bx, ax + cx});
                float fz0 = std::min({az, az + bz, az + cz}), fz1 = std::max({az, az + bz, az + cz});
                if (fx1 < window[0] - m || fx0 > window[2] + m || fz1 < window[1] - m || fz0 > window[3] + m) continue;
            }
            Tri tr;
            tr.attr = (uint8_t)attr;
            tr.room = (uint8_t)room;
            for (int i = 0; i < 3; i++) {
                tr.x[i] = v(k[i], 0);
                tr.y[i] = v(k[i], 1);
                tr.z[i] = v(k[i], 2);
                x0 = std::min(x0, tr.x[i]); x1 = std::max(x1, tr.x[i]);
                z0 = std::min(z0, tr.z[i]); z1 = std::max(z1, tr.z[i]);
            }
            tris.push_back(tr);
        }
    }
    if (tris.empty()) return false;
    // about 8 pixels per metre-ish, at most 1024 across; a margin for the edge (a window: its own scale, made a little
    // larger than it and cut back to it at the end, so its edges are no floor edges)
    float upp = std::max(10.f, std::max(x1 - x0, z1 - z0) / 1000.f);
    if (window) {
        upp = fineUpp;
        float m = 8 * upp;
        x0 = std::max(x0, window[0] - m); z0 = std::max(z0, window[1] - m);
        x1 = std::min(x1, window[2] + m); z1 = std::min(z1, window[3] + m);
        if (x1 <= x0 || z1 <= z0 || (x1 - x0) / upp > 4096 || (z1 - z0) / upp > 4096) return false;
    }
    const int edge = 3;
    x0 -= upp * (edge + 1);
    z0 -= upp * (edge + 1);
    int w = (int)((x1 - x0) / upp) + edge + 3, h = (int)((z1 - z0) / upp) + edge + 3;
    // per pixel: the highest floor's height, its ground and room
    const float NONE = -1e30f;
    std::vector<float> Y((size_t)w * h, NONE);
    std::vector<uint8_t> A((size_t)w * h, 0), R((size_t)w * h, 0);
    for (const Tri& t : tris) {
        float px[3], pz[3];
        for (int i = 0; i < 3; i++) {
            px[i] = (t.x[i] - x0) / upp;
            pz[i] = (t.z[i] - z0) / upp;
        }
        int bx0 = std::max(0, (int)std::floor(std::min({px[0], px[1], px[2]}))),
            bx1 = std::min(w - 1, (int)std::ceil(std::max({px[0], px[1], px[2]})));
        int bz0 = std::max(0, (int)std::floor(std::min({pz[0], pz[1], pz[2]}))),
            bz1 = std::min(h - 1, (int)std::ceil(std::max({pz[0], pz[1], pz[2]})));
        float area = (px[1] - px[0]) * (pz[2] - pz[0]) - (px[2] - px[0]) * (pz[1] - pz[0]);
        if (std::fabs(area) < 1e-6f) continue;
        for (int y = bz0; y <= bz1; y++)
            for (int x = bx0; x <= bx1; x++) {
                float qx = x + 0.5f, qz = y + 0.5f;
                float w0 = ((px[1] - qx) * (pz[2] - qz) - (px[2] - qx) * (pz[1] - qz)) / area;
                float w1 = ((px[2] - qx) * (pz[0] - qz) - (px[0] - qx) * (pz[2] - qz)) / area;
                float w2 = 1 - w0 - w1;
                const float e = -0.02f;  // (seams between faces closed)
                if (w0 < e || w1 < e || w2 < e) continue;
                float yy = w0 * t.y[0] + w1 * t.y[1] + w2 * t.y[2];
                size_t i = (size_t)y * w + x;
                if (yy > Y[i]) {
                    Y[i] = yy;
                    A[i] = t.attr;
                    R[i] = t.room;
                }
            }
    }
    auto floor = [&](int x, int y) { return x >= 0 && y >= 0 && x < w && y < h && Y[(size_t)y * w + x] > NONE; };
    out.argb.assign((size_t)w * h, 0);
    if (style == PLAN_MASK) {
        // the floor alone, white (MiniMap.redraw draws it anew)
        for (size_t i = 0; i < Y.size(); i++)
            if (Y[i] > NONE) out.argb[i] = 0xFFFFFFFFu;
    } else if (terrain) {
        // the ground's colour (ground_colour), lit from the north-west by its slope, higher floors a little lighter,
        // ledges (a step down of more than 80 units) dark, a dark edge line round the floor and a soft shadow outside;
        // rooms only seen on the dungeon's map grey and faint
        float lo = 1e30f, hi = -1e30f;
        for (float v : Y)
            if (v > NONE) { lo = std::min(lo, v); hi = std::max(hi, v); }
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                if (Y[i] <= NONE) {
                    int d = 99;
                    for (int dy = -2; dy <= 2; dy++)
                        for (int dx = -2; dx <= 2; dx++)
                            if (floor(x + dx, y + dy)) d = std::min(d, dx * dx + dy * dy);
                    if (d <= 8) out.argb[i] = (uint32_t)(d <= 2 ? 0x70 : 0x38) << 24 | 0x281E05;
                    continue;
                }
                auto at = [&](int xx, int yy) { return floor(xx, yy) ? Y[(size_t)yy * w + xx] : Y[i]; };
                float gx = (at(x + 1, y) - at(x - 1, y)) / (2 * upp), gz = (at(x, y + 1) - at(x, y - 1)) / (2 * upp);
                gx = std::max(-3.f, std::min(3.f, gx));
                gz = std::max(-3.f, std::min(3.f, gz));
                float nx = -gx, ny = 1, nz = -gz, nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                float shade = 0.5f + 0.62f * std::max(0.f, (-nx + 1.5f * ny - nz) / (nl * 2.0616f));
                if (hi > lo) shade *= 0.88f + 0.16f * (Y[i] - lo) / (hi - lo);
                bool border = false, ledge = false;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!floor(x + dx, y + dy)) border = true;
                        else if (Y[i] - Y[(size_t)(y + dy) * w + x + dx] > 80) ledge = true;
                    }
                if (ledge) shade *= 0.72f;
                uint32_t c = ground_colour(A[i]);
                int r = (int)((c >> 16 & 0xFF) * shade), g = (int)((c >> 8 & 0xFF) * shade), b = (int)((c & 0xFF) * shade);
                if (border) { r = r * 3 / 10 + 0x28 * 7 / 10; g = g * 3 / 10 + 0x1E * 7 / 10; b = b * 3 / 10 + 0x05 * 7 / 10; }
                uint32_t a = 0xFF;
                if (!(lit >> R[i] & 1)) {
                    int l = (r * 3 + g * 6 + b) / 10;
                    r = g = b = l * 6 / 10 + 0x20;
                    a = 0x8C;
                }
                out.argb[i] = a << 24 | (uint32_t)std::min(255, r) << 16 | (uint32_t)std::min(255, g) << 8 | (uint32_t)std::min(255, b);
            }
    } else {
        // the edge: pixels within `edge` of the floor; colours as DungeonMapFloor's P_Room materials (black #FFDD55,
        // white #A04B00, alpha 0xB4): the floor's grey levels low (yellow, mottled), its outline light (brown)
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                size_t i = (size_t)y * w + x;
                float l;
                if (Y[i] > NONE) {
                    float wx = x0 + x * upp, wz = z0 + y * upp;
                    l = 0.08f + 0.22f * noise(wx / 260.f, wz / 260.f) + 0.08f * noise(wx / 70.f, wz / 70.f);
                } else {
                    bool near = false;
                    for (int dy = -edge; dy <= edge && !near; dy++)
                        for (int dx = -edge; dx <= edge && !near; dx++)
                            if (dx * dx + dy * dy <= edge * edge + 1 && floor(x + dx, y + dy)) near = true;
                    if (!near) continue;
                    l = 1;
                }
                uint32_t r = (uint32_t)(0xFF + (0xA0 - 0xFF) * l), g = (uint32_t)(0xDD + (0x4B - 0xDD) * l), b = (uint32_t)(0x55 * (1 - l));
                out.argb[i] = 0xB4u << 24 | r << 16 | g << 8 | b;
            }
    }
    if (window) {
        int cx0 = std::max(0, (int)std::floor((window[0] - x0) / upp)), cz0 = std::max(0, (int)std::floor((window[1] - z0) / upp));
        int cx1 = std::min(w, (int)std::ceil((window[2] - x0) / upp)), cz1 = std::min(h, (int)std::ceil((window[3] - z0) / upp));
        if (cx1 <= cx0 || cz1 <= cz0) return false;
        std::vector<uint32_t> cut((size_t)(cx1 - cx0) * (cz1 - cz0));
        for (int y = cz0; y < cz1; y++)
            std::copy(out.argb.begin() + (size_t)y * w + cx0, out.argb.begin() + (size_t)y * w + cx1, cut.begin() + (size_t)(y - cz0) * (cx1 - cx0));
        out.argb.swap(cut);
        x0 += cx0 * upp;
        z0 += cz0 * upp;
        w = cx1 - cx0;
        h = cz1 - cz0;
    }
    out.w = w;
    out.h = h;
    out.x0 = x0;
    out.z0 = z0;
    out.upp = upp;
    LOG("[minimap] plan %s y %.0f..%.0f style %d%s: %zu faces, %dx%d at %.1f", stage.c_str(), yMin, yMax, style,
        window ? " (window)" : "", tris.size(), w, h, upp);
    return true;
}
}  // namespace map_plan
