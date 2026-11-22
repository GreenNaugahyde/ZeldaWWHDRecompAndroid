// SPDX-License-Identifier: MPL-2.0
// Seamless houses: an island's house files, made on the device from the player's game files (the C++ form of
// tools/seamless/build_island.py, build_room.py, build_env.py and merge_events.py; see there and
// mods/seamless_houses.cpp). Output in <game>/seamless/:
//   seamless_<host>_<stage>_<room>.szs   each house room as sea room 50 (its places moved, its collision groups
//                                        renumbered, its bfres named Room50.bfres; SARC in a stored Yaz0)
//   <stage>.env / <stage>.evn            the house stage's lighting tables (EnvR, Colo, Pale, Virt) and EVNT entries
//   sea_<host>.evt                       the sea's event list with the houses' appended (their numbers shifted, their
//                                        absolute places moved)
//   seamless_<host>.txt                  the runtime's table (house / file / in / out lines), written last
#include "seamless_build.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "game_archive.h"

namespace seamless_build {
namespace {
using game_archive::Bytes;
using game_archive::be16;
using game_archive::be32;

void put32(uint8_t* p, uint32_t v) { p[0] = v >> 24, p[1] = v >> 16, p[2] = v >> 8, p[3] = v; }
void put16(uint8_t* p, uint16_t v) { p[0] = v >> 8, p[1] = v; }
float getf(const uint8_t* p) {
    uint32_t v = be32(p);
    float f;
    memcpy(&f, &v, 4);
    return f;
}
void putf(uint8_t* p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}
int32_t gets32(const uint8_t* p) { return (int32_t)be32(p); }

// ---- finding the game's files: Common/Stage first, then the packs (only what is asked for is kept)
struct Files {
    std::string common;
    std::map<std::string, Bytes> cache;  // archive name -> its (packed) bytes
    bool get(const std::string& name, Bytes& out) {
        auto it = cache.find(name);
        if (it != cache.end()) return out = it->second, true;
        if (game_archive::read_file(common + "/Stage/" + name, out)) return cache[name] = out, true;
        DIR* d = opendir((common + "/Pack").c_str());
        if (!d) return false;
        std::vector<std::string> packs;
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 5 && n.compare(n.size() - 5, 5, ".pack") == 0) packs.push_back(n);
        }
        closedir(d);
        std::sort(packs.begin(), packs.end());
        for (const std::string& p : packs) {
            Bytes raw, pack;
            if (!game_archive::read_file(common + "/Pack/" + p, raw) || !game_archive::yaz0(raw, pack)) continue;
            if (game_archive::sarc_file(pack, name.c_str(), out)) return cache[name] = out, true;
        }
        return false;
    }
};

// an archive's (first) bfres, unpacked; its other files (by name, in order)
bool open_szs(Files& fs, const std::string& name, Bytes& bfres, std::vector<std::pair<std::string, Bytes>>* others = nullptr,
              std::string* bfres_name = nullptr) {
    Bytes raw, sarc;
    if (!fs.get(name, raw) || !game_archive::yaz0(raw, sarc)) return false;
    bool found = false;
    game_archive::sarc_each(sarc, [&](const char* n, const uint8_t* d, size_t size) {
        size_t len = strlen(n);
        if (!found && len > 6 && strcmp(n + len - 6, ".bfres") == 0) {
            bfres.assign(d, d + size), found = true;
            if (bfres_name) *bfres_name = n;
        } else if (others) {
            others->push_back({n, Bytes(d, d + size)});
        }
    });
    return found;
}

// bfres v3 embedded files (index group 11): name -> (offset, size)
std::map<std::string, std::pair<uint32_t, uint32_t>> ext_files(const Bytes& b) {
    std::map<std::string, std::pair<uint32_t, uint32_t>> out;
    if (b.size() < 0x68) return out;
    const uint32_t g = 0x4C + gets32(&b[0x4C]);
    if (g + 8 > b.size()) return out;
    const uint32_t cnt = be32(&b[g + 4]);
    for (uint32_t k = 1; k <= cnt && g + 8 + 16 * k + 16 <= b.size(); k++) {
        const uint32_t e = g + 8 + 16 * k;
        const uint32_t no = e + 8 + gets32(&b[e + 8]), dofs = e + 12 + gets32(&b[e + 12]);
        if (no >= b.size() || dofs + 8 > b.size()) continue;
        const uint32_t off = dofs + gets32(&b[dofs]), size = be32(&b[dofs + 4]);
        out[std::string((const char*)&b[no])] = {off, size};
    }
    return out;
}

struct Chunk {
    std::string tag;
    uint32_t count, at;  // entries at bfres offset `at`
};
std::vector<Chunk> chunks(const Bytes& b, uint32_t base) {
    std::vector<Chunk> v;
    if (base + 4 > b.size()) return v;
    const uint32_t n = be32(&b[base]);
    for (uint32_t k = 0; k < n && base + 16 + 12 * k <= b.size(); k++) {
        const uint8_t* e = &b[base + 4 + 12 * k];
        v.push_back({std::string((const char*)e, 4), be32(e + 4), base + be32(e + 8)});
    }
    return v;
}

struct Spawn {
    float x, y, z;
    int16_t angle;
};
std::map<int, Spawn> spawns(const Bytes& b, uint32_t dzr) {
    std::map<int, Spawn> out;
    for (const Chunk& c : chunks(b, dzr))
        if (c.tag == "PLYR")
            for (uint32_t i = 0; i < c.count; i++) {
                const uint8_t* e = &b[c.at + 0x20 * i];
                out[e[0x1D]] = {getf(e + 12), getf(e + 16), getf(e + 20), (int16_t)be16(e + 0x1A)};
            }
    return out;
}
struct Exit {
    std::string stage;
    int spawn, room;
};
std::vector<Exit> exits(const Bytes& b, uint32_t dzr) {
    std::vector<Exit> out;
    for (const Chunk& c : chunks(b, dzr))
        if (c.tag == "SCLS")
            for (uint32_t i = 0; i < c.count; i++) {
                const uint8_t* e = &b[c.at + 12 * i];
                out.push_back({std::string((const char*)e, strnlen((const char*)e, 8)), e[8], e[9]});
            }
    return out;
}

// ---- a house room as sea room 50 (build_room.py)
void move(uint8_t* p, float dx, float dy, float dz) {
    putf(p, getf(p) + dx), putf(p + 4, getf(p + 4) + dy), putf(p + 8, getf(p + 8) + dz);
}
bool position_chunk(const std::string& t, uint32_t& size, uint32_t& off) {
    if (t == "ACTR" || t == "PLYR" || t == "TRES" || (t.compare(0, 3, "ACT") == 0) || t.compare(0, 3, "TRE") == 0)
        return size = 0x20, off = 0xC, true;
    if (t == "SCOB" || t.compare(0, 3, "SCO") == 0) return size = 0x24, off = 0xC, true;
    if (t == "RPPN") return size = 0x10, off = 4, true;
    if (t == "SOND") return size = 0x1C, off = 8, true;
    if (t == "RARO") return size = 0x14, off = 0, true;
    if (t == "LGTV") return size = 0x1C, off = 0, true;
    return false;
}
uint32_t sarc_hash(const std::string& n) {
    uint32_t h = 0;
    for (unsigned char ch : n) h = h * 0x65 + ch;
    return h;
}
Bytes write_sarc(std::vector<std::pair<std::string, Bytes>> files) {
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return sarc_hash(a.first) < sarc_hash(b.first); });
    Bytes names;
    std::vector<uint32_t> name_at;
    for (auto& f : files) {
        name_at.push_back((uint32_t)names.size());
        names.insert(names.end(), f.first.begin(), f.first.end());
        names.push_back(0);
        while (names.size() % 4) names.push_back(0);
    }
    constexpr uint32_t kAlign = 0x2000;
    const uint32_t hdr = 0x14 + 0xC + 16 * (uint32_t)files.size() + 8 + (uint32_t)names.size();
    const uint32_t data_start = (hdr + kAlign - 1) / kAlign * kAlign;
    Bytes data, fat;
    for (size_t i = 0; i < files.size(); i++) {
        while (data.size() % kAlign) data.push_back(0);
        const uint32_t s = (uint32_t)data.size();
        data.insert(data.end(), files[i].second.begin(), files[i].second.end());
        uint8_t e[16];
        put32(e, sarc_hash(files[i].first)), put32(e + 4, 0x01000000 | (name_at[i] / 4)), put32(e + 8, s), put32(e + 12, (uint32_t)data.size());
        fat.insert(fat.end(), e, e + 16);
    }
    Bytes out(0x14 + 0xC);
    memcpy(&out[0], "SARC", 4), put16(&out[4], 0x14), put16(&out[6], 0xFEFF), put32(&out[8], data_start + (uint32_t)data.size());
    put32(&out[12], data_start), put32(&out[16], 0x01000000);
    memcpy(&out[0x14], "SFAT", 4), put16(&out[0x18], 0xC), put16(&out[0x1A], (uint16_t)files.size()), put32(&out[0x1C], 0x65);
    out.insert(out.end(), fat.begin(), fat.end());
    const uint8_t sfnt[8] = {'S', 'F', 'N', 'T', 0, 8, 0, 0};
    out.insert(out.end(), sfnt, sfnt + 8);
    out.insert(out.end(), names.begin(), names.end());
    out.resize(data_start, 0);
    out.insert(out.end(), data.begin(), data.end());
    return out;
}
Bytes yaz0_store(const Bytes& d) {  // a valid Yaz0 without compression (every block literal)
    Bytes out(16, 0);
    memcpy(&out[0], "Yaz0", 4), put32(&out[4], (uint32_t)d.size());
    out.reserve(16 + d.size() + d.size() / 8 + 1);
    for (size_t i = 0; i < d.size(); i += 8) {
        out.push_back(0xFF);
        out.insert(out.end(), d.begin() + i, d.begin() + std::min(d.size(), i + 8));
    }
    return out;
}
bool write_file(const std::string& path, const Bytes& b) {
    const std::string tmp = path + ".part";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(b.data(), 1, b.size(), f) == b.size();
    fclose(f);
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}
bool build_room(Files& fs, const std::string& src, float dx, float dy, float dz, int host, const std::string& out) {
    Bytes b;
    std::vector<std::pair<std::string, Bytes>> files;
    if (!open_szs(fs, src, b, &files)) return false;
    auto ext = ext_files(b);
    if (!ext.count("room.dzr") || !ext.count("room.dzb")) return false;
    for (const Chunk& c : chunks(b, ext["room.dzr"].first)) {
        uint32_t size, off;
        if (!position_chunk(c.tag, size, off)) continue;
        for (uint32_t i = 0; i < c.count && c.at + size * (i + 1) <= b.size(); i++) move(&b[c.at + size * i + off], dx, dy, dz);
    }
    const uint32_t zb = ext["room.dzb"].first;
    const uint32_t vn = be32(&b[zb]), vo = be32(&b[zb + 4]);
    for (uint32_t i = 0; i < vn && zb + vo + 12 * i + 12 <= b.size(); i++) move(&b[zb + vo + 12 * i], dx, dy, dz);
    const uint32_t gn = be32(&b[zb + 0x20]), go = be32(&b[zb + 0x24]);
    for (uint32_t i = 0; i < gn && zb + go + 0x34 * i + 0x34 <= b.size(); i++) {
        uint8_t* g = &b[zb + go + 0x34 * i];
        put32(g + 0x30, (be32(g + 0x30) & ~0xFFu) | (uint32_t)host);  // the room list to load: the island's
        if (be16(g + 0x2A) != 0xFFFF) put16(g + 0x2A, 50);              // the room Link stays in: 50
    }
    files.push_back({"Room50.bfres", b});
    return write_file(out, yaz0_store(write_sarc(files)));
}

// ---- event lists (merge_events.py)
constexpr uint32_t kSec[7] = {0xB0, 0x50, 0x50, 0x40, 4, 4, 1};
struct EvList {
    Bytes sec[7];
    uint32_t n[7] = {};
    Bytes tail;  // the header's last 8 bytes
};
bool parse_ev(const Bytes& b, EvList& e) {
    if (b.size() < 0x40) return false;
    for (int k = 0; k < 7; k++) {
        const uint32_t at = be32(&b[8 * k]), n = be32(&b[8 * k + 4]);
        if ((uint64_t)at + (uint64_t)n * kSec[k] > b.size()) return false;
        e.sec[k].assign(b.begin() + at, b.begin() + at + n * kSec[k]), e.n[k] = n;
    }
    e.tail.assign(b.begin() + 0x38, b.begin() + 0x40);
    return true;
}
int32_t flags_max(const EvList& e) {
    int32_t m = -1;
    for (uint32_t i = 0; i < e.n[0]; i++)
        for (int j = 0; j < 5; j++) m = std::max(m, gets32(&e.sec[0][i * 0xB0 + 0x80 + 4 * j]));
    for (uint32_t i = 0; i < e.n[2]; i++)
        for (int j = 0; j < 4; j++) m = std::max(m, gets32(&e.sec[2][i * 0x50 + 0x28 + 4 * j]));
    return m;
}
void add(Bytes& b, size_t at, int32_t d) {
    const int32_t v = gets32(&b[at]);
    if (v >= 0) put32(&b[at], (uint32_t)(v + d));
}
// the house's absolute places moved (its 'pos' and the camera's Eye / Center / StartEye / StartCenter without RelActor)
void move_places(EvList& e, float dx, float dy, float dz) {
    std::set<int32_t> done;
    for (uint32_t c = 0; c < e.n[2]; c++) {
        struct Item { std::string name; int32_t type, idx; };
        std::vector<Item> items;
        int32_t di = gets32(&e.sec[2][c * 0x50 + 0x38]);
        while (di >= 0 && (uint32_t)di < e.n[3] && items.size() < 64) {
            const uint8_t* d = &e.sec[3][di * 0x40];
            items.push_back({std::string((const char*)d, strnlen((const char*)d, 32)), gets32(d + 0x24), gets32(d + 0x28)});
            di = gets32(d + 0x30);
        }
        bool relative = false;
        for (auto& it : items) relative |= it.name == "RelActor";
        for (auto& it : items) {
            if (it.type != 1 || it.idx < 0 || (uint32_t)(it.idx + 3) * 4 > e.sec[4].size() || done.count(it.idx)) continue;
            if (it.name == "pos" || (!relative && (it.name == "Eye" || it.name == "Center" || it.name == "StartEye" || it.name == "StartCenter"))) {
                move(&e.sec[4][it.idx * 4], dx, dy, dz);
                done.insert(it.idx);
            }
        }
    }
}
void shift(EvList& e, const uint32_t base[7], int32_t flag0) {
    for (uint32_t i = 0; i < e.n[0]; i++) {
        const size_t o = i * 0xB0;
        add(e.sec[0], o + 0x20, base[0]);
        for (int j = 0; j < 20; j++) add(e.sec[0], o + 0x2C + 4 * j, base[1]);
        for (int j = 0; j < 5; j++) add(e.sec[0], o + 0x80 + 4 * j, flag0);
    }
    for (uint32_t i = 0; i < e.n[1]; i++) add(e.sec[1], i * 0x50 + 0x24, base[1]), add(e.sec[1], i * 0x50 + 0x30, base[2]);
    for (uint32_t i = 0; i < e.n[2]; i++) {
        const size_t o = i * 0x50;
        add(e.sec[2], o + 0x24, base[2]);
        for (int j = 0; j < 4; j++) add(e.sec[2], o + 0x28 + 4 * j, flag0);
        add(e.sec[2], o + 0x38, base[3]), add(e.sec[2], o + 0x3C, base[2]);
    }
    for (uint32_t i = 0; i < e.n[3]; i++) {
        const size_t o = i * 0x40;
        add(e.sec[3], o + 0x20, base[3]);
        const int32_t t = gets32(&e.sec[3][o + 0x24]);
        add(e.sec[3], o + 0x28, (int32_t)(t == 0 || t == 1 ? base[4] : t == 3 ? base[5] : t == 4 ? base[6] : 0));
        add(e.sec[3], o + 0x30, base[3]);
    }
}

// a stage's stage.dzs
bool stage_dzs(Files& fs, const std::string& stage, Bytes& b, uint32_t& dzs) {
    if (!open_szs(fs, stage + "_Stage.szs", b)) return false;
    auto ext = ext_files(b);
    if (!ext.count("stage.dzs")) return false;
    dzs = ext["stage.dzs"].first;
    return true;
}
}  // namespace

std::string build_island(const std::string& game_dir, const std::string& out_dir, int host) {
    Files fs;
    fs.common = game_dir + "/content/Common";
    mkdir(out_dir.c_str(), 0775);
    // the island: its room's exits and spawns, its MULT place
    Bytes hb;
    if (!open_szs(fs, "sea_Room" + std::to_string(host) + ".szs", hb)) return "no sea_Room" + std::to_string(host);
    auto hext = ext_files(hb);
    if (!hext.count("room.dzr")) return "no room.dzr in the island's room";
    const auto host_spawns = spawns(hb, hext["room.dzr"].first);
    const auto host_exits = exits(hb, hext["room.dzr"].first);
    Bytes sb;
    uint32_t sdzs;
    if (!stage_dzs(fs, "sea", sb, sdzs)) return "no sea stage";
    float cx = NAN, cz = NAN;
    for (const Chunk& c : chunks(sb, sdzs))
        if (c.tag == "MULT")
            for (uint32_t i = 0; i < c.count; i++)
                if (sb[c.at + 12 * i + 10] == host) cx = getf(&sb[c.at + 12 * i]), cz = getf(&sb[c.at + 12 * i + 4]);
    if (std::isnan(cx)) return "no MULT entry for the island";
    // its house rooms
    std::vector<std::pair<std::string, int>> houses;
    for (const Exit& x : host_exits) {
        if (std::find(houses.begin(), houses.end(), std::make_pair(x.stage, x.room)) != houses.end()) continue;
        Bytes b;
        uint32_t dzs;
        if (!stage_dzs(fs, x.stage, b, dzs)) continue;
        for (const Chunk& c : chunks(b, dzs))
            if (c.tag == "STAG" && c.at + 0x10 <= b.size()) {
                const int table = b[c.at + 9] >> 1, kind = (be32(&b[c.at + 0xC]) >> 16) & 7;
                if (table == 11 && kind == 2) houses.push_back({x.stage, x.room});
            }
    }
    static const float kSlots[][2] = {{-45000, -30000}, {-15000, -30000}, {15000, -30000}, {45000, -30000}, {-45000, 30000},
                                      {-15000, 30000},  {15000, 30000},   {45000, 30000},  {-30000, 0},     {30000, 0}};
    std::string txt = "host " + std::to_string(host) + "\n";
    std::map<std::string, std::array<float, 3>> stage_offset;
    std::vector<std::string> stages;
    char line[160];
    for (size_t hi = 0; hi < houses.size() && hi < sizeof kSlots / sizeof *kSlots; hi++) {
        const auto& [stage, room] = houses[hi];
        auto it = stage_offset.find(stage);
        if (it == stage_offset.end()) {
            it = stage_offset.emplace(stage, std::array<float, 3>{cx + kSlots[hi][0], 150000.0f, cz + kSlots[hi][1]}).first;
            stages.push_back(stage);
        }
        const float dx = it->second[0], dy = it->second[1], dz = it->second[2];
        const std::string file = "seamless_" + std::to_string(host) + "_" + stage + "_" + std::to_string(room) + ".szs";
        const std::string src = stage + "_Room" + std::to_string(room) + ".szs";
        if (!build_room(fs, src, dx, dy, dz, host, out_dir + "/" + file)) return "could not build " + src;
        Bytes stage_b;
        uint32_t sd;
        if (!stage_dzs(fs, stage, stage_b, sd)) continue;
        int table = 11;
        for (const Chunk& c : chunks(stage_b, sd))
            if (c.tag == "STAG") table = stage_b[c.at + 9] >> 1;
        snprintf(line, sizeof line, "house %s %d 50 save %d dx %.1f dy %.1f dz %.1f\nfile %s\n", stage.c_str(), room, table, dx, dy, dz, file.c_str());
        txt += line;
        Bytes rb;
        open_szs(fs, src, rb);
        auto rext = ext_files(rb);
        for (auto& [p, s] : spawns(rb, rext["room.dzr"].first)) {
            snprintf(line, sizeof line, "in %d %.1f %.1f %.1f %d\n", p, s.x + dx, s.y + dy, s.z + dz, s.angle);
            txt += line;
        }
        for (const Exit& x : exits(rb, rext["room.dzr"].first))
            if (x.stage == "sea" && x.room == host && host_spawns.count(x.spawn)) {
                const Spawn& s = host_spawns.at(x.spawn);
                snprintf(line, sizeof line, "out %d %.1f %.1f %.1f %d\n", x.spawn, s.x, s.y, s.z, s.angle);
                txt += line;
            }
    }
    // each stage's lighting tables and EVNT, then the merged event list
    EvList merged;
    {
        auto sext = ext_files(sb);
        if (!sext.count("event_list.dat")) return "no sea event list";
        const auto [o, n] = sext["event_list.dat"];
        if ((uint64_t)o + n > sb.size()) return "bad sea event list";
        Bytes ev(sb.begin() + o, sb.begin() + o + n);
        if (!parse_ev(ev, merged)) return "bad sea event list";
    }
    int32_t flag0 = flags_max(merged) + 1;
    std::sort(stages.begin(), stages.end());  // (as build_island.py: by name)
    for (const std::string& stage : stages) {
        Bytes b;
        uint32_t dzs;
        if (!stage_dzs(fs, stage, b, dzs)) continue;
        Bytes env = {'S', 'H', 'E', 'V', 0, 0, 0, 1};
        static const char* kEnv[4] = {"EnvR", "Colo", "Pale", "Virt"};
        static const uint32_t kEnvSize[4] = {8, 12, 108, 56};
        for (int t = 0; t < 4; t++) {
            uint32_t cnt = 0, at = 0;
            for (const Chunk& c : chunks(b, dzs))
                if (c.tag == kEnv[t]) cnt = c.count, at = c.at;
            uint8_t h[8];
            put32(h, cnt), put32(h + 4, kEnvSize[t]);
            env.insert(env.end(), h, h + 8);
            if (cnt && at + cnt * kEnvSize[t] <= b.size()) env.insert(env.end(), b.begin() + at, b.begin() + at + cnt * kEnvSize[t]);
        }
        write_file(out_dir + "/" + stage + ".env", env);
        for (const Chunk& c : chunks(b, dzs))
            if (c.tag == "EVNT" && c.at + 0x18 * c.count <= b.size()) {
                Bytes e(4);
                put32(&e[0], c.count);
                e.insert(e.end(), b.begin() + c.at, b.begin() + c.at + 0x18 * c.count);
                write_file(out_dir + "/" + stage + ".evn", e);
            }
        auto ext = ext_files(b);
        if (!ext.count("event_list.dat")) continue;
        Bytes evb(b.begin() + ext["event_list.dat"].first, b.begin() + ext["event_list.dat"].first + ext["event_list.dat"].second);
        EvList h;
        if (!parse_ev(evb, h)) continue;
        const auto& d = stage_offset[stage];
        move_places(h, d[0], d[1], d[2]);
        const int32_t fmax = flags_max(h);
        const int32_t f0 = flag0 + fmax < 320 * 32 ? flag0 : 0;
        shift(h, merged.n, f0);
        for (int k = 0; k < 7; k++) merged.sec[k].insert(merged.sec[k].end(), h.sec[k].begin(), h.sec[k].end()), merged.n[k] += h.n[k];
        if (f0) flag0 += fmax + 1;
    }
    Bytes ev(0x40);
    uint32_t at = 0x40;
    for (int k = 0; k < 7; k++) {
        put32(&ev[8 * k], at), put32(&ev[8 * k + 4], merged.n[k]);
        at += (uint32_t)merged.sec[k].size();
    }
    memcpy(&ev[0x38], merged.tail.data(), 8);
    for (int k = 0; k < 7; k++) ev.insert(ev.end(), merged.sec[k].begin(), merged.sec[k].end());
    if (!write_file(out_dir + "/sea_" + std::to_string(host) + ".evt", ev)) return "could not write the event list";
    // the table last: its presence means the island is complete
    if (!write_file(out_dir + "/seamless_" + std::to_string(host) + ".txt", Bytes(txt.begin(), txt.end()))) return "could not write the table";
    return "";
}
}  // namespace seamless_build
