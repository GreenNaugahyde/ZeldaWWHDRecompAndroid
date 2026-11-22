// Mods > Mod packs > Player models, per save file and live: a GameCube player model (gcmodel/, converted into a Link.szs
// of its pack) put into the game's memory over the Link.bfres the game loaded, when the save file played has one; HD
// Link put back when it has none. The game's pack stays as it is (no restart).
//
// Why this works (notes/player-models-per-save.md): the converter keeps every structure of Link.bfres where it was
// (shapes, materials, bones, texture headers); it replaces texture data in place and appends new vertex, index and
// skin-bone data at the file's end, pointed at by relative offsets. The game, loading the file, writes absolute data
// pointers into the vertex/index buffer and texture headers and binds the materials to their shaders: none of those
// words is one the converter changes. So a model goes in as
//   - the words where its Link.bfres differs from the game's (structure fields and texture data),
//   - its appended part copied to an area of its own (kArea, part of save states), the relative offsets that point
//     into it aimed there (32-bit offsets reach any guest address),
//   - each vertex/index buffer's data pointer (its +0, set by the game at load) aimed at its data there,
// and goes out by writing the game's words back (a data pointer: the file's address plus its own offset). Tried with
// Link on screen: the game draws the new model at once, animated; no moment without Link needed.
//
// Which model is in memory is noted in the area's header and checked against sample words (a save state may bring
// another, with or without the area).
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mods.h"
#include "../mem_writes.h"
#include "../runtime.h"
#include "../savestate.h"
#ifdef __ANDROID__
#include "../gcmodel/gcm_io.h"
#include "../gcmodel/hd_sword.h"
#endif

namespace mods {
namespace {
constexpr uint32_t kDataNum = 0x1290;  // the save file's number 0..2 (progress.cpp)
constexpr int kFiles = 3;
constexpr uint32_t kArea = 0x58000000;      // unused guest memory between MEM2 and the runtime's objects
constexpr uint32_t kAreaSize = 0x00400000;  // 4 MiB (the biggest of 62 models adds 1.5 MiB)
static_assert(kSaveNameArea + 0x100 == kArea + kAreaSize, "save names: the area's last 0x100 bytes");
constexpr uint32_t kHeader = 0x1000;        // "WWPM", the model's folder; its appended part after it
constexpr uint32_t kMagic = 0x4D505757;     // "WWPM" as stored
constexpr uint32_t kStep = 0x100;
// a loot sword's corner of the area (loot_sword.cpp, gcmodel/hd_sword.h): put in over whatever Link.bfres holds (the
// game's Link or a model), the words it wrote over kept there (offset, length, bytes), so out = those back. Save
// states always keep it (its header tells what is in; a hash of the sword's words tells a stale one from an old state)
constexpr uint32_t kSwordArea = 0x582F0000, kSwordSize = 0x100000, kSwordHeader = 0x100;  // (1 MiB: four textures)
static_assert(kSwordArea + kSwordSize <= kSaveNameArea, "the sword's corner below the save names");
// the loot equipment's corners: the sword's and, below it, the shield's (the same way: notes/loot.md)
struct Corner { uint32_t area, size; char magic[5]; const char* kind; };
constexpr Corner kSwordCorner{kSwordArea, kSwordSize, "WWSW", "sword"}, kShieldCorner{0x581F0000, 0x100000, "WWSH", "shield"};
static_assert(kShieldCorner.area + kShieldCorner.size == kSwordArea, "the shield's corner right below the sword's");
// the armour's, below those (a picture of its own in the chest's: Corner tail, records, then the picture)
constexpr Corner kHelmetCorner{0x581C0000, 0x30000, "WWHM", "helmet"}, kChestCorner{0x58190000, 0x30000, "WWCH", "chest"};
// (an outfit's: above the save names, its picture and all of Link's body data in it)
constexpr Corner kOutfitCorner{0x58400000, 0x100000, "WWOF", "outfit"};
static_assert(kHelmetCorner.area + kHelmetCorner.size == kShieldCorner.area && kChestCorner.area + kChestCorner.size == kHelmetCorner.area, "the armour's corners");
constexpr int kRainbow = 36;  // a sword's rainbow pictures (gcmodel::kRainbowSteps)
#ifdef __ANDROID__
static_assert(kRainbow == gcmodel::kRainbowSteps, "the rainbow's steps");
#endif

using Bytes = std::vector<uint8_t>;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }

// a Link.bfres (version 3, big-endian) read as far as needed here
struct Bfres {
    const Bytes& d;
    explicit Bfres(const Bytes& b) : d(b) {}
    bool in(uint64_t o, uint64_t n = 4) const { return o + n <= d.size(); }
    uint32_t u32(uint32_t o) const { return in(o) ? be32(&d[o]) : 0; }
    uint16_t u16(uint32_t o) const { return in(o, 2) ? (uint16_t)(d[o] << 8 | d[o + 1]) : 0; }
    uint8_t u8(uint32_t o) const { return in(o, 1) ? d[o] : 0; }
    uint32_t rel(uint32_t o) const {
        int32_t x = (int32_t)u32(o);
        return x ? (uint32_t)((int64_t)o + x) : 0;
    }
    std::vector<std::pair<std::string, uint32_t>> group(uint32_t o) const {
        std::vector<std::pair<std::string, uint32_t>> out;
        if (!o || !in(o + 8)) return out;
        uint32_t n = u32(o + 4);
        for (uint32_t i = 1; i <= n && in(o + 8 + i * 16 + 16); i++) {
            uint32_t s = rel(o + 8 + i * 16 + 8);
            std::string name;
            while (s && s < d.size() && d[s] && name.size() < 128) name += (char)d[s++];
            out.push_back({name, rel(o + 8 + i * 16 + 12)});
        }
        return out;
    }
    uint32_t section(int i) const { return rel(0x20 + i * 4); }
};

struct Buffer { uint32_t at, data, size; };  // a vertex/index buffer struct, its data's offset and size
struct Layout {
    std::vector<Buffer> buffers;
    std::vector<std::pair<std::string, std::array<uint32_t, 4>>> textures;  // name, image/mip offset and size
    std::vector<std::pair<uint32_t, uint32_t>> data;                       // data ranges (textures, buffers)
};

Layout layout(const Bytes& d) {
    Bfres f(d);
    Layout l;
    for (auto& [name, o] : f.group(f.section(1))) {
        std::array<uint32_t, 4> t{f.rel(o + 0xB0), f.u32(o + 0x24), f.rel(o + 0xB4), f.u32(o + 0x2C)};
        l.textures.push_back({name, t});
        l.data.push_back({t[0], t[1]});
        if (t[3]) l.data.push_back({t[2], t[3]});
    }
    for (auto& [mname, fm] : f.group(f.section(0))) {
        uint32_t vbl = f.rel(fm + 16);
        for (uint32_t i = 0, n = f.u16(fm + 32); i < n; i++) {
            uint32_t o = vbl + i * 0x20, bl = f.rel(o + 0x18);
            for (uint32_t k = 0; k < f.u8(o + 5); k++) {
                uint32_t bb = bl + k * 0x18;
                l.buffers.push_back({bb, f.rel(bb + 0x14), f.u32(bb + 4)});
            }
        }
        for (auto& [sname, so] : f.group(f.rel(fm + 20))) {
            uint32_t mesh0 = f.rel(so + 0x24);
            for (uint32_t k = 0; k < f.u8(so + 0x17); k++) {
                uint32_t ib = f.rel(mesh0 + k * 0x1C + 0x14);
                l.buffers.push_back({ib, f.rel(ib + 0x14), f.u32(ib + 4)});
            }
        }
    }
    for (auto& b : l.buffers) l.data.push_back({b.data, b.size});
    return l;
}

struct Run { uint32_t off, len, at; };  // file offset, length, place in Model::bytes
// a word aimed into the appended part (target: offset in the model's file); a data pointer (not rel) goes back to the
// file's address plus `game`, the game's own data offset
struct Fix { uint32_t off, target; bool rel; uint32_t game; };
struct Probe { uint32_t off, model, game; };    // a word telling the model from the game's (raw big-endian)

struct Model {
    std::string folder;
    std::string key;  // its Link.szs's path, size and time (imported again: made again)
    std::vector<Run> runs;
    Bytes bytes;   // the model's words of the runs
    Bytes revert;  // the game's words of the runs
    std::vector<Fix> fixes;
    Bytes tail;          // the appended part
    uint32_t tail0 = 0;  // its offset in the model's file
    std::vector<Probe> probes;
    // a sword: its slots pointed at its picture (FTEX offsets: slot, picture), the picture's image / mip (offset, size,
    // offset, size), its own data and its colour tiers' (4 tiers, 12 rainbow steps)
    std::vector<std::pair<uint32_t, uint32_t>> links;
    std::array<uint32_t, 4> image{};
    Bytes ownImage;
    std::vector<Bytes> tiers;
    bool surface = false;  // the picture in the corner (SwordExtra::surface)
    std::vector<float> boneRatio, boneOffset;  // an outfit at its own proportions (SwordExtra)
    std::vector<Bytes> blink;  // its picture with the eyes half shut / shut
    float armAngle = 0, handScale = 1;
    std::vector<float> handFrame;
    float raise = 0;
};

struct Set {
    Bytes game;  // the game's Link.bfres
    std::vector<Buffer> buffers;  // its vertex/index buffers (the game sets their data pointers at load)
    std::string basePack;
    std::vector<std::shared_ptr<Model>> models;
    std::vector<std::shared_ptr<Model>> swords;   // folder "sword:<id>"
    std::vector<std::shared_ptr<Model>> shields;  // folder "shield:<id>"
    std::vector<std::shared_ptr<Model>> helmets, chests, outfits;  // folders "helmet:<id>", "chest:<id>", "outfit:<id>"
};

#ifdef __ANDROID__
// a file's path, size and time (the game's pack made again: read again)
std::string stamp(const std::string& p) {
    struct stat st {};
    if (stat(p.c_str(), &st) != 0) return p;
    return p + "\t" + std::to_string((long long)st.st_size) + "\t" + std::to_string((long long)st.st_mtime);
}

bool read_file(const std::string& p, Bytes& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

// Link.bfres out of a Link.szs
bool bfres_of_szs(const Bytes& szs, Bytes& out) {
    try {
        auto files = gcmodel::sarc_read(gcmodel::yaz0_decode(szs));
        auto it = files.find("Link.bfres");
        if (it == files.end()) return false;
        out = std::move(it->second);
        return out.size() > 0x40 && memcmp(out.data(), "FRES", 4) == 0;
    } catch (const std::exception&) {
        return false;
    }
}

bool game_bfres(const std::string& pack, Bytes& out) {
    Bytes p;
    if (!read_file(pack, p)) return false;
    try {
        auto files = gcmodel::sarc_read(p);
        p.clear();
        p.shrink_to_fit();
        auto it = files.find("Link.szs");
        return it != files.end() && bfres_of_szs(it->second, out);
    } catch (const std::exception&) {
        return false;
    }
}

// what the model changes against the game's file; null (and why in the log) if it doesn't fit the game's
std::shared_ptr<Model> make_model(const std::string& folder, const Bytes& game, const Layout& gl, const Bytes& m) {
    const uint32_t n = (uint32_t)game.size();
    auto fail = [&](const char* why) {
        LOG("[playermodel] %s: %s", folder.c_str(), why);
        return std::shared_ptr<Model>();
    };
    if (m.size() < n) return fail("smaller than the game's Link.bfres");
    Layout ml = layout(m);
    if (ml.textures.size() != gl.textures.size() || ml.buffers.size() != gl.buffers.size()) return fail("not made from this game's Link");
    for (size_t i = 0; i < gl.textures.size(); i++)
        if (ml.textures[i] != gl.textures[i]) return fail("its textures aren't where the game's are");
    for (size_t i = 0; i < gl.buffers.size(); i++)
        if (ml.buffers[i].at != gl.buffers[i].at) return fail("its buffers aren't where the game's are");
    auto model = std::make_shared<Model>();
    model->folder = folder;
    model->tail0 = n & ~(kStep - 1);  // (the data's alignment kept)
    if (m.size() - model->tail0 > kChestCorner.area - kArea - kHeader) return fail("too big");  // (the loot corners after it)
    model->tail.assign(m.begin() + model->tail0, m.end());
    // the game's data ranges: no relative offsets there (texture bytes may look like one)
    std::vector<uint8_t> isData((n + 3) / 4, 0);
    for (auto [o, s] : gl.data)
        for (uint64_t w = o / 4; w < ((uint64_t)o + s + 3) / 4 && w < isData.size(); w++) isData[w] = 1;
    std::vector<uint8_t> isPtr((n + 3) / 4, 0);
    for (size_t i = 0; i < ml.buffers.size(); i++) {
        const Buffer& b = ml.buffers[i];
        if (b.data != gl.buffers[i].data) {
            if (b.data < n) return fail("a buffer moved inside the file");
            model->fixes.push_back({b.at, b.data, false, gl.buffers[i].data});
            isPtr[b.at / 4] = 1;
        }
    }
    // the differing words (the header's file size stays the game's)
    std::vector<uint32_t> structural;
    for (uint32_t o = 0; o + 4 <= n; o += 4) {
        if (o == 0xC || memcmp(&m[o], &game[o], 4) == 0) continue;
        if (isPtr[o / 4]) continue;  // (the data pointers: Fix)
        if (!model->runs.empty() && model->runs.back().off + model->runs.back().len == o) model->runs.back().len += 4;
        else model->runs.push_back({o, 4, (uint32_t)model->bytes.size()});
        model->bytes.insert(model->bytes.end(), &m[o], &m[o] + 4);
        model->revert.insert(model->revert.end(), &game[o], &game[o] + 4);
        if (isData[o / 4]) continue;
        int32_t x = (int32_t)be32(&m[o]);
        int64_t t = (int64_t)o + x;
        if (x && (x & 3) == 0 && t >= n && t < (int64_t)m.size()) model->fixes.push_back({o, (uint32_t)t, true, 0});
        else structural.push_back(o);
    }
    // sample words to tell it in memory: structure fields, spread over the file
    for (size_t k = 0, want = std::min<size_t>(64, structural.size()); k < want; k++) {
        uint32_t o = structural[k * structural.size() / want];
        model->probes.push_back({o, be32(&m[o]), be32(&game[o])});
    }
    LOG("[playermodel] %s: %zu runs (%zu KiB), %zu offsets/pointers into its %zu KiB, %zu probes", folder.c_str(), model->runs.size(),
        model->bytes.size() / 1024, model->fixes.size(), model->tail.size() / 1024, model->probes.size());
    return model;
}

// a sword written whole where a player model may differ from the game's file too: its shapes' skin-bone lists, the
// two textures' data, the two materials' colours (runs over words the sword didn't change; they go in after its own)
void sword_force(Model& m, const Bytes& game, const Bytes& sword, const std::vector<std::string>& modelNames = {"swms", "swgripms", "swa", "swgripa"},
                 const std::vector<std::string>& textureNames = {"SWgripAMSCI4"}) {
    Bfres f(sword);
    std::vector<std::pair<uint32_t, uint32_t>> ranges;
    for (auto& [mname, fm] : f.group(f.section(0))) {
        if (std::find(modelNames.begin(), modelNames.end(), mname) == modelNames.end()) continue;
        for (auto& [sname, so] : f.group(f.rel(fm + 20))) {
            ranges.push_back({so + 0x14, 4});
            ranges.push_back({so + 0x28, 4});
            const uint32_t list = f.rel(so + 0x28), n = f.u16(so + 0x14);
            if (list && list < game.size()) ranges.push_back({list & ~3u, ((list & 3u) + n * 2 + 3) & ~3u});
        }
        for (auto& [mn, mo] : f.group(f.rel(fm + 24))) {
            if (mn != "SwMSAMAT" && mn != "SwGripMSAMAT" && mn != "SWA1" && mn != "SWgripA") continue;
            const uint32_t pl = f.rel(mo + 0x34), pd = f.rel(mo + 0x3C);
            for (uint32_t k = 0; k < f.u16(mo + 0x12); k++) {
                const uint32_t e = pl + k * 0x14;
                uint32_t sp = f.rel(e + 0x10);
                std::string pn;
                while (sp && sp < sword.size() && sword[sp] && pn.size() < 32) pn += (char)sword[sp++];
                if (pn == "mat_color0" || pn == "mat_color1") {
                    const uint32_t at = pd + f.u16(e + 2);
                    ranges.push_back({at, 16});

                }
            }
        }
    }
    for (auto& [name, o] : f.group(f.section(1))) {
        if (std::find(textureNames.begin(), textureNames.end(), name) == textureNames.end()) continue;  // (its one picture)
        ranges.push_back({f.rel(o + 0xB0), f.u32(o + 0x24)});
        if (f.u32(o + 0x2C)) ranges.push_back({f.rel(o + 0xB4), f.u32(o + 0x2C)});
    }
    // its own runs and these as one set of runs, each byte once (all from the sword's file)
    std::vector<std::pair<uint32_t, uint32_t>> all;
    for (const Run& r : m.runs) all.push_back({r.off, r.off + r.len});
    for (auto [off, len] : ranges)
        if (len && (uint64_t)off + len <= game.size()) all.push_back({off, off + len});
    std::sort(all.begin(), all.end());
    m.runs.clear(), m.bytes.clear(), m.revert.clear();
    for (size_t i = 0; i < all.size();) {
        uint32_t lo = all[i].first, hi = all[i].second;
        for (i++; i < all.size() && all[i].first <= hi; i++) hi = std::max(hi, all[i].second);
        m.runs.push_back({lo, hi - lo, (uint32_t)m.bytes.size()});
        m.bytes.insert(m.bytes.end(), sword.begin() + lo, sword.begin() + hi);
        m.revert.insert(m.revert.end(), game.begin() + lo, game.begin() + hi);
    }
    LOG("[loot] %s: %zu runs, %zu KiB", m.folder.c_str(), m.runs.size(), m.bytes.size() / 1024);
}
#endif

// what the app chose (set_player_models)
std::mutex g_mu;
std::string g_basePack, g_packsDir;
std::array<std::string, kFiles> g_folders;
std::atomic<uint32_t> g_gen{0};
std::atomic<bool> g_fileHasModel[kFiles];

// what the loader made; the main thread takes it
std::shared_ptr<Set> g_set;      // main thread's
std::shared_ptr<Set> g_pending;  // loader -> main thread (under g_mu)
std::atomic<int> g_loading{0};   // 1 while the loader runs

// Link.bfres in memory, found by its header (the game's first 64 bytes; the game doesn't write there at load)
std::atomic<uint32_t> g_base{0};
std::atomic<int> g_scan{0};  // 0 to do, 1 running, 2 found, 3 not found

// the game's Link.bfres with his picture as it is in memory now (Link's colours write there)
#ifdef __ANDROID__
bool link_picture(const Bytes& game, uint32_t& off, uint32_t& size, uint32_t& mipOff, uint32_t& mipSize) {
    Bfres f(game);
    for (auto& [n, o] : f.group(f.section(1)))
        if (n == "linktexS3TC") return off = f.rel(o + 0xB0), size = f.u32(o + 0x24), mipOff = f.rel(o + 0xB4), mipSize = f.u32(o + 0x2C), true;
    return false;
}
Bytes game_as_shown(const Bytes& game) {
    Bytes out = game;
    uint32_t off, size, mipOff, mipSize;
    const uint32_t base = g_base.load();
    if (base && link_picture(game, off, size, mipOff, mipSize) && (uint64_t)off + size <= out.size())
        memcpy(&out[off], mem::ptr(base + off), size);
    return out;
}
#endif

void loader(uint32_t gen, std::string basePack, std::string packsDir, std::vector<std::string> folders, std::string gameDir,
            std::vector<std::string> swords, std::vector<std::string> shields, std::vector<std::string> helmets, std::vector<std::string> chests, std::vector<std::string> outfits,
            std::shared_ptr<Set> had) {
    auto set = std::make_shared<Set>();
    set->basePack = basePack;
#ifdef __ANDROID__
    // (the game's file again only when its pack changed)
    set->basePack = stamp(basePack);
    if (had && had->basePack == set->basePack && !had->game.empty()) set->game = had->game;
    else if (!game_bfres(basePack, set->game)) LOG("[playermodel] cannot read Link.bfres of %s", basePack.c_str());
    if (!set->game.empty()) {
        Layout gl = layout(set->game);
        set->buffers = gl.buffers;
        for (const std::string& f : folders) {
            std::shared_ptr<Model> m;
            const std::string path = packsDir + "/" + f + "/model/Link.szs", key = stamp(path);
            if (had && had->basePack == set->basePack)
                for (auto& h : had->models)
                    if (h->folder == f && h->key == key) m = h;
            if (!m) {
                Bytes szs, b;
                if (!read_file(path, szs) || !bfres_of_szs(szs, b)) {
                    LOG("[playermodel] %s: no model/Link.szs", f.c_str());
                    continue;
                }
                m = make_model(f, set->game, gl, b);
                if (m) m->key = key;
            }
            if (m) set->models.push_back(m);
        }
        // the loot swords: made from the game's own files (gcmodel/hd_sword.h), then as a model of their own
        for (const std::string& id : swords) {
            const std::string folder = "sword:" + id;
            std::shared_ptr<Model> m;
            if (had && had->basePack == set->basePack)
                for (auto& h : had->swords)
                    if (h->folder == folder) m = h;
            if (!m) {
                const gcmodel::SwordDef* def = gcmodel::sword_def(id);
                Bytes src, b;
                std::string err, report;
                gcmodel::SwordExtra ex;
                if (!def) err = "not a sword";
                else if (gcmodel::sword_source(gameDir, *def, src, err) && gcmodel::make_sword(set->game, src, *def, b, err, &report, &ex))
                    if ((m = make_model(folder, set->game, gl, b))) {
                        sword_force(*m, set->game, b);
                        m->links = ex.links, m->image = ex.image, m->tiers = std::move(ex.tiers);
                        if ((uint64_t)ex.image[0] + ex.image[1] <= b.size()) m->ownImage.assign(b.begin() + ex.image[0], b.begin() + ex.image[0] + ex.image[1]);
                        if (ex.image[3] && (uint64_t)ex.image[2] + ex.image[3] <= b.size()) m->ownImage.insert(m->ownImage.end(), b.begin() + ex.image[2], b.begin() + ex.image[2] + ex.image[3]);
                    }
                LOG("[loot] sword %s: %s", id.c_str(), m ? report.c_str() : err.empty() ? "not made" : err.c_str());
            }
            if (m) set->swords.push_back(m);
        }
        // the loot shields: the same way (gcmodel/hd_sword.h make_shield)
        for (const std::string& id : shields) {
            const std::string folder = "shield:" + id;
            std::shared_ptr<Model> m;
            if (had && had->basePack == set->basePack)
                for (auto& h : had->shields)
                    if (h->folder == folder) m = h;
            if (!m) {
                const gcmodel::SwordDef* def = gcmodel::shield_def(id);
                Bytes src, b;
                std::string err, report;
                gcmodel::SwordExtra ex;
                if (!def) err = "not a shield";
                else if (gcmodel::sword_source(gameDir, *def, src, err) && gcmodel::make_shield(set->game, src, *def, b, err, &report, &ex))
                    if ((m = make_model(folder, set->game, gl, b))) {
                        sword_force(*m, set->game, b, {"sha", "shms"}, {"SHA"});
                        m->links = ex.links, m->image = ex.image, m->tiers = std::move(ex.tiers);
                        if ((uint64_t)ex.image[0] + ex.image[1] <= b.size()) m->ownImage.assign(b.begin() + ex.image[0], b.begin() + ex.image[0] + ex.image[1]);
                        if (ex.image[3] && (uint64_t)ex.image[2] + ex.image[3] <= b.size()) m->ownImage.insert(m->ownImage.end(), b.begin() + ex.image[2], b.begin() + ex.image[2] + ex.image[3]);
                    }
                LOG("[loot] shield %s: %s", id.c_str(), m ? report.c_str() : err.empty() ? "not made" : err.c_str());
            }
            if (m) set->shields.push_back(m);
        }
        // the loot armour: the same way (make_armor)
        for (int slot = 0; slot < 3; slot++) {
            const char* kind = slot == 2 ? "outfit" : slot ? "chest" : "helmet";
            for (const std::string& id : slot == 2 ? outfits : slot ? chests : helmets) {
                const std::string folder = std::string(kind) + ":" + id;
                std::shared_ptr<Model> m;
                if (had && had->basePack == set->basePack)
                    for (auto& h : slot == 2 ? had->outfits : slot ? had->chests : had->helmets)
                        if (h->folder == folder) m = h;
                if (!m) {
                    const gcmodel::ArmorDef* def = gcmodel::armor_def(slot, id.substr(0, id.find('@')));
                    Bytes src, b;
                    std::string err, report;
                    gcmodel::SwordExtra ex;
                    if (!def) err = "not known";
                    else if (gcmodel::armor_source(gameDir, *def, src, err) &&
                             (slot == 2 ? gcmodel::make_outfit(game_as_shown(set->game), src, *def, b, err, &report, &ex, id.find("@R") != std::string::npos) : gcmodel::make_armor(set->game, src, *def, slot, b, err, &report, &ex)))
                        if ((m = make_model(folder, set->game, gl, b))) {
                            if (slot == 2) {}
                            else if (slot) sword_force(*m, set->game, b, {"pring"}, {});
                            else sword_force(*m, set->game, b, {"katsura"}, {});
                            m->links = ex.links, m->image = ex.image, m->tiers = std::move(ex.tiers), m->surface = ex.surface;
                            m->boneRatio = ex.boneRatio, m->boneOffset = ex.boneOffset, m->raise = ex.raise, m->blink = std::move(ex.blink), m->armAngle = ex.armAngle, m->handScale = ex.handScale, m->handFrame = ex.handFrame;
                            if (ex.surface) m->ownImage = std::move(ex.own);
                            else {
                                if ((uint64_t)ex.image[0] + ex.image[1] <= b.size()) m->ownImage.assign(b.begin() + ex.image[0], b.begin() + ex.image[0] + ex.image[1]);
                                if (ex.image[3] && (uint64_t)ex.image[2] + ex.image[3] <= b.size()) m->ownImage.insert(m->ownImage.end(), b.begin() + ex.image[2], b.begin() + ex.image[2] + ex.image[3]);
                            }
                        }
                    LOG("[loot] %s %s: %s", kind, id.c_str(), m ? report.c_str() : err.empty() ? "not made" : err.c_str());
                }
                if (m) (slot == 2 ? set->outfits : slot ? set->chests : set->helmets).push_back(m);
            }
        }
    }
#else
    (void)packsDir, (void)folders, (void)had, (void)gameDir, (void)swords, (void)shields, (void)helmets, (void)chests, (void)outfits;
#endif
    std::lock_guard<std::mutex> lk(g_mu);
    if (gen == g_gen.load()) g_pending = set;
    g_loading = 0;
}

void scan(Bytes head) {
    uint32_t found = 0;
    for (uint32_t a = mem::kMem2Start; a + head.size() <= mem::kMem2End; a += kStep) {
        const uint8_t* p = mem::ptr(a);
        if (p[0] != 'F' || memcmp(p, head.data(), head.size()) != 0) continue;
        found = a;
        break;
    }
    if (found) LOG("[playermodel] Link.bfres at %08X", found);
    g_base = found;
    g_scan.store(found ? 2 : 3, std::memory_order_release);
}

bool holds(uint32_t base, const Model& m, bool model) {
    for (const Probe& p : m.probes)
        if (be32(mem::ptr(base + p.off)) != (model ? p.model : p.game)) return false;
    for (const Fix& f : m.fixes)
        if (!f.rel && be32(mem::ptr(base + f.off)) != (model ? kArea + kHeader + (f.target - m.tail0) : base + f.game)) return false;
    return true;
}

void write_out(uint32_t base, const Model& m) {
    for (const Run& r : m.runs) memcpy(mem::ptr(base + r.off), &m.revert[r.at], r.len);
    for (const Fix& f : m.fixes)
        if (!f.rel) put_be32(mem::ptr(base + f.off), base + f.game);
    // (the area's header: nothing in)
    memset(mem::ptr(kArea), 0, kHeader);
}

void write_in(uint32_t base, const Model& m) {
    uint8_t* h = mem::ptr(kArea);
    memset(h, 0, kHeader);
    memcpy(mem::ptr(kArea + kHeader), m.tail.data(), m.tail.size());
    memw::mark(kArea + kHeader, (uint32_t)m.tail.size());
    for (const Run& r : m.runs) memcpy(mem::ptr(base + r.off), &m.bytes[r.at], r.len);
    for (const Fix& f : m.fixes) {
        uint32_t to = kArea + kHeader + (f.target - m.tail0);
        put_be32(mem::ptr(base + f.off), f.rel ? to - (base + f.off) : to);
    }
    put_be32(h + 4, (uint32_t)m.tail.size());
    snprintf((char*)h + 8, 120, "%s", m.folder.c_str());
    memcpy(h, &kMagic, 4);
}

// a loot corner (Corner: the sword's "WWSW", the shield's "WWSH"): header magic, Link.bfres's address, tail size, records
// size, hash, id; tail; records
uint32_t sword_hash(uint32_t base, const uint8_t* rec, uint32_t size) {
    uint32_t h = 2166136261u;
    for (uint32_t o = 0; o + 8 <= size;) {
        const uint32_t off = be32(rec + o), len = be32(rec + o + 4);
        const uint8_t* p = mem::ptr(base + off);
        for (uint32_t i = 0; i < len; i++) h = (h ^ p[i]) * 16777619u;
        o += 8 + len;
    }
    return h;
}
// the sword in memory: its id, "" none (a header whose hash doesn't fit the memory is let go)
std::string sword_in_memory(const Corner& c = kSwordCorner) {
    uint8_t* h = mem::ptr(c.area);
    if (memcmp(h, c.magic, 4) != 0) return "";
    const uint32_t base = be32(h + 4), tail = be32(h + 8), size = be32(h + 12);
    const uint32_t rec = (c.area + kSwordHeader + tail + kStep - 1) & ~(kStep - 1);
    if (rec + size > c.area + c.size || sword_hash(base, mem::ptr(rec), size) != be32(h + 16)) {
        LOG("[loot] the %s noted in memory is not there (an older save state): let go", c.kind);
        memset(h, 0, kSwordHeader);
        return "";
    }
    return std::string((const char*)h + 20, strnlen((const char*)h + 20, 32));
}
void sword_out(const Corner& c = kSwordCorner) {
    uint8_t* h = mem::ptr(c.area);
    if (memcmp(h, c.magic, 4) != 0) return;
    const uint32_t base = be32(h + 4), tail = be32(h + 8), size = be32(h + 12);
    const uint32_t rec = (c.area + kSwordHeader + tail + kStep - 1) & ~(kStep - 1);
    const uint8_t* r = mem::ptr(rec);
    std::vector<uint32_t> at;  // (back in reverse: overlapping records)
    for (uint32_t o = 0; o + 8 <= size; o += 8 + be32(r + o + 4)) at.push_back(o);
    for (size_t i = at.size(); i-- > 0;) memcpy(mem::ptr(base + be32(r + at[i])), r + at[i] + 8, be32(r + at[i] + 4));
    memset(h, 0, kSwordHeader);
}
// a picture's place in its corner: after the records, aligned as a texture's data must be (image, then its mips)
constexpr uint32_t kPicAlign = 0x2000;
uint32_t corner_picture(uint32_t records_end) { return (records_end + kPicAlign - 1) & ~(kPicAlign - 1); }
uint32_t mip_offset(uint32_t imageSize) { return (imageSize + kPicAlign - 1) & ~(kPicAlign - 1); }
bool sword_in(uint32_t base, const Model& m, const std::string& id, const Corner& c = kSwordCorner) {
    const uint32_t tail = c.area + kSwordHeader;
    const uint32_t rec = (tail + (uint32_t)m.tail.size() + kStep - 1) & ~(kStep - 1);
    uint64_t need = 0;
    for (const Run& r : m.runs) need += 8 + r.len;
    need += 12ull * m.fixes.size() + (8ull + 0x9C) * m.links.size();
    if (m.surface) need += m.ownImage.size() + 2ull * kPicAlign;
    if (rec + need > c.area + c.size) {
        LOG("[loot] %s %s: too big for its corner (%llu bytes)", c.kind, id.c_str(), (unsigned long long)(rec + need - c.area));
        return false;
    }
    memcpy(mem::ptr(tail), m.tail.data(), m.tail.size());
    memw::mark(tail, (uint32_t)m.tail.size());
    uint8_t* r = mem::ptr(rec);
    uint32_t o = 0;
    auto keep = [&](uint32_t off, uint32_t len) {
        put_be32(r + o, off), put_be32(r + o + 4, len);
        memcpy(r + o + 8, mem::ptr(base + off), len);
        o += 8 + len;
    };
    for (const Run& run : m.runs) {
        keep(run.off, run.len);
        memcpy(mem::ptr(base + run.off), &m.bytes[run.at], run.len);
    }
    for (const Fix& f : m.fixes) {
        keep(f.off, 4);
        const uint32_t to = tail + (f.target - m.tail0);
        put_be32(mem::ptr(base + f.off), f.rel ? to - (base + f.off) : to);
    }
    // the other sword slots shown as its picture: their texture's surface (GX2Texture: FTEX +4 .. +0xA0, with the
    // pointers and registers the game set at load) the picture's
    for (auto [slot, pic] : m.links) {
        keep(slot + 4, 0x9C);
        memcpy(mem::ptr(base + slot + 4), mem::ptr(base + pic + 4), 0x9C);
    }
    // (its picture kept in the corner: the slots' surfaces point there)
    if (m.surface && m.ownImage.size() == (size_t)m.image[1] + m.image[3]) {
        const uint32_t pic = corner_picture(rec + o), mips = pic + mip_offset(m.image[1]);
        memcpy(mem::ptr(pic), m.ownImage.data(), m.image[1]);
        if (m.image[3]) memcpy(mem::ptr(mips), m.ownImage.data() + m.image[1], m.image[3]);
        memw::mark(pic, m.image[1]);
        if (m.image[3]) memw::mark(mips, m.image[3]);
        for (auto [slot, unused] : m.links) {
            (void)unused;
            put_be32(mem::ptr(base + slot + 0x28), pic);
            put_be32(mem::ptr(base + slot + 0x30), m.image[3] ? mips : 0);
        }
    }
    uint8_t* h = mem::ptr(c.area);
    memset(h, 0, kSwordHeader);
    put_be32(h + 4, base), put_be32(h + 8, (uint32_t)m.tail.size()), put_be32(h + 12, o);
    put_be32(h + 16, sword_hash(base, r, o));
    snprintf((char*)h + 20, 32, "%s", id.c_str());
    memcpy(h, c.magic, 4);
    return true;
}

// a corner's id by its header only
std::string loot_header_id(const Corner& c) {
    return memcmp(mem::ptr(c.area), c.magic, 4) == 0 ? std::string((const char*)mem::ptr(c.area) + 20, strnlen((const char*)mem::ptr(c.area) + 20, 32)) : "";
}

// what the app chose for the loot swords (set_loot_swords)
std::string g_gameDir;
std::array<std::string, kFiles> g_swords, g_shields, g_helmets, g_chests, g_outfits;
std::atomic<uint32_t> g_outfitHash{0};  // Link's picture in memory, when last looked at
// the app's "id" (a whole character at its own proportions) or "id#1" (at Link's size); made with the picture's hash in its name
std::string outfit_id(const std::string& chosen) {
    const size_t h = chosen.find('#');
    return chosen.substr(0, h) + "@" + std::to_string(g_outfitHash.load()) + (h == std::string::npos ? "@R" : "");
}

std::atomic<uint32_t> g_areaUsed{0};
}  // namespace

void set_player_models(const std::string& basePack, const std::string& packsDir, const std::vector<std::string>& folders) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_basePack = basePack;
    g_packsDir = packsDir;
    for (int f = 0; f < kFiles; f++) {
        g_folders[f] = f < (int)folders.size() ? folders[f] : "";
        g_fileHasModel[f] = !g_folders[f].empty();
    }
    g_gen++;
}

std::string player_model_in_memory() {
    const uint8_t* h = mem::ptr(kArea);
    if (memcmp(h, &kMagic, 4) != 0) return "";
    return std::string((const char*)h + 8, strnlen((const char*)h + 8, 120));
}
void player_model_dirs(std::string& basePack, std::string& packsDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    basePack = g_basePack, packsDir = g_packsDir;
}

bool player_model_file(int file) { return file >= 0 && file < kFiles && g_fileHasModel[file].load(std::memory_order_relaxed); }

uint32_t player_model_area(uint32_t* size) {
    *size = g_areaUsed.load();
    return kArea;
}

// game main thread, frame start (interp.cpp), before Link's colours
void player_models_service() {
    static uint32_t gen = 0;
    static unsigned loads = ~0u;
    static int frame = 0, lastScan = -100000;
    static int current = -2;  // -2 not known, -1 the game's Link, else g_set->models' index
    static std::string extra;  // a model a save state brought that no file has (to take it out again)
    frame++;
    // the app's choice changed: models made (on a thread; those made before are kept)
    uint32_t g = g_gen.load();
    if (g != gen && !g_loading.load()) {
        gen = g;
        std::vector<std::string> folders, swords, shields, helmets, chests, outfits;
        std::string basePack, packsDir, gameDir;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            basePack = g_basePack, packsDir = g_packsDir, gameDir = g_gameDir;
            for (auto& f : g_folders)
                if (!f.empty() && std::find(folders.begin(), folders.end(), f) == folders.end()) folders.push_back(f);
            for (auto& f : g_swords) {
                const std::string id = f.substr(0, f.find('#'));  // ("id#tier": one sword, coloured in memory)
                if (!id.empty() && std::find(swords.begin(), swords.end(), id) == swords.end()) swords.push_back(id);
            }
            for (auto& f : g_shields) {
                const std::string id = f.substr(0, f.find('#'));
                if (!id.empty() && std::find(shields.begin(), shields.end(), id) == shields.end()) shields.push_back(id);
            }
            for (int slot = 0; slot < 3; slot++)
                for (auto& f : slot == 2 ? g_outfits : slot ? g_chests : g_helmets) {
                    std::string id = f.substr(0, f.find('#'));
                    if (slot == 2 && !id.empty()) id = outfit_id(f);
                    auto& l = slot == 2 ? outfits : slot ? chests : helmets;
                    if (!id.empty() && std::find(l.begin(), l.end(), id) == l.end()) l.push_back(id);
                }
        }
        // (the sword in memory stays loaded until it's taken out)
        if (g_base.load()) {
            const std::string in = std::string(memcmp(mem::ptr(kSwordArea), "WWSW", 4) == 0 ? (const char*)mem::ptr(kSwordArea) + 20 : "");
            if (!in.empty() && std::find(swords.begin(), swords.end(), in) == swords.end()) swords.push_back(in);
            const uint32_t sa = kShieldCorner.area;
            const std::string ins = std::string(memcmp(mem::ptr(sa), "WWSH", 4) == 0 ? (const char*)mem::ptr(sa) + 20 : "");
            if (!ins.empty() && std::find(shields.begin(), shields.end(), ins) == shields.end()) shields.push_back(ins);
            for (int slot = 0; slot < 3; slot++) {
                const std::string inA = loot_header_id(slot == 2 ? kOutfitCorner : slot ? kChestCorner : kHelmetCorner);
                auto& l = slot == 2 ? outfits : slot ? chests : helmets;
                if (!inA.empty() && std::find(l.begin(), l.end(), inA) == l.end()) l.push_back(inA);
            }
        }
        // (the model in memory stays loaded until it's taken out; one only a save state had, too)
        for (const std::string& keep : {g_set && current >= 0 && current < (int)g_set->models.size() ? g_set->models[current]->folder : std::string(), extra})
            if (!keep.empty() && std::find(folders.begin(), folders.end(), keep) == folders.end()) folders.push_back(keep);
        // nothing chosen and nothing in: nothing to do
        if (basePack.empty() || (folders.empty() && swords.empty() && shields.empty() && helmets.empty() && chests.empty() && outfits.empty() && current < 0 && !g_set)) return;
        g_loading = 1;
        std::thread(loader, g, basePack, packsDir, folders, gameDir, swords, shields, helmets, chests, outfits, g_set).detach();
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (g_pending) {
            // the model in memory, if any, stays known under the new set (same folder)
            std::string was = g_set && current >= 0 && current < (int)g_set->models.size() ? g_set->models[current]->folder : "";
            g_set = std::move(g_pending);
            int now = -2;
            if (current == -1) now = -1;
            for (size_t i = 0; i < g_set->models.size(); i++)
                if (g_set->models[i]->folder == was) now = (int)i;
            if (current >= 0 && now == -2) LOG("[playermodel] %s in memory but no longer loaded", was.c_str());
            current = now;
        }
    }
    if (!g_set || g_set->game.empty()) return;
    // Link.bfres in memory (loaded with the game's first files; a save state keeps it where it was)
    const uint32_t base = g_base.load();
    int s = g_scan.load(std::memory_order_acquire);
    if (s == 1) return;
    if (s == 2 && memcmp(mem::ptr(base), g_set->game.data(), 64) != 0) s = 0;
    if (s != 2) {
        if (frame - lastScan < 60) return;
        lastScan = frame;
        g_scan = 1;
        std::thread(scan, Bytes(g_set->game.begin(), g_set->game.begin() + 64)).detach();
        current = -2;
        return;
    }
    // the game done loading it: its data pointers set (unpacked but not yet set up: the game would write over a model)
    if (g_set->buffers.empty()) return;
    for (const Buffer* b : {&g_set->buffers.front(), &g_set->buffers.back()})
        if (be32(mem::ptr(base + b->at)) == 0) return;
    // after a save state: whatever it brought
    unsigned lc = ss::load_count();
    if (lc != loads) {
        loads = lc;
        current = -2;
    }
    auto& models = g_set->models;
    if (current == -2) {
        // a sword in first taken out (it writes over the model's words; put in again below)
        if (!sword_in_memory(kOutfitCorner).empty()) sword_out(kOutfitCorner);
        if (!sword_in_memory(kChestCorner).empty()) sword_out(kChestCorner);
        if (!sword_in_memory(kHelmetCorner).empty()) sword_out(kHelmetCorner);
        if (!sword_in_memory(kShieldCorner).empty()) sword_out(kShieldCorner);
        if (!sword_in_memory().empty()) sword_out();
        memw::mark(base, (uint32_t)g_set->game.size());
        // the area's note first, then the game's own, then any model loaded
        std::string noted;
        const uint8_t* h = mem::ptr(kArea);
        if (memcmp(h, &kMagic, 4) == 0) noted.assign((const char*)h + 8, strnlen((const char*)h + 8, 120));
        int found = -2;
        for (size_t i = 0; i < models.size() && found == -2; i++)
            if (models[i]->folder == noted && holds(base, *models[i], true)) found = (int)i;
        if (found == -2 && std::all_of(models.begin(), models.end(), [&](auto& m) { return holds(base, *m, false); })) found = -1;
        for (size_t i = 0; i < models.size() && found == -2; i++)
            if (holds(base, *models[i], true)) found = (int)i;
        if (found == -2 && !noted.empty() && extra != noted) {
            // (a model no file has: loaded too, to take it out)
            LOG("[playermodel] memory holds %s: loading it", noted.c_str());
            extra = noted;
            g_gen++;
            return;
        }
        if (found == -2) {
            LOG("[playermodel] what Link.bfres holds is not known: taken as the game's");
            found = -1;
        }
        current = found;
        if (current >= 0) g_areaUsed = kHeader + (uint32_t)models[current]->tail.size();
        LOG("[playermodel] in memory: %s", current >= 0 ? models[current]->folder.c_str() : "the game's Link");
    }
    // the save file played decides
    uint32_t save = player_save();
    if (!save) return;
    int file = *(const uint8_t*)mem::ptr(save + kDataNum);
    if (file < 0 || file >= kFiles) return;
    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        want = g_folders[file];
    }
    if (save_new_game(save)) want.clear();  // (a new game in that file: the game's Link; the app forgets the choice)
    int target = -1;
    if (!want.empty()) {
        target = -2;
        for (size_t i = 0; i < models.size(); i++)
            if (models[i]->folder == want) target = (int)i;
        if (target == -2) {
            if (g_loading.load() || g_gen.load() != gen) return;  // (still being made)
            target = -1;  // (it couldn't be made: the log says why)
        }
    }
    if (target != current) {
        // (a sword in comes out first: it lies over the model's words; it goes in again below)
        if (!sword_in_memory(kOutfitCorner).empty()) sword_out(kOutfitCorner);
        if (!sword_in_memory(kChestCorner).empty()) sword_out(kChestCorner);
        if (!sword_in_memory(kHelmetCorner).empty()) sword_out(kHelmetCorner);
        if (!sword_in_memory(kShieldCorner).empty()) sword_out(kShieldCorner);
        if (!sword_in_memory().empty()) sword_out();
        if (current >= 0) write_out(base, *models[current]);
        if (target >= 0) write_in(base, *models[target]);
        memw::mark(base, (uint32_t)g_set->game.size());
        g_areaUsed = target >= 0 ? kHeader + (uint32_t)models[target]->tail.size() : 0;
        LOG("[playermodel] save file %d: %s (was %s)", file + 1, target >= 0 ? models[target]->folder.c_str() : "the game's Link",
            current >= 0 ? models[current]->folder.c_str() : "the game's Link");
        current = target;
        // (a model only a save state had: let go once out)
        if (!extra.empty() && target < 0) extra.clear();
    }
    // the loot sword and shield of the save file played, over the model (or the game's Link): each in its corner,
    // "id#tier" (the colour tier 1..5, 0 its own colours); false: still being made (wait)
    auto equip = [&](const Corner& c, const std::array<std::string, kFiles>& chosen, const std::vector<std::shared_ptr<Model>>& made,
                     int& shownTier, int& shownStep, std::string& want, int& tier) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            want = chosen[file];
        }
        if (save_new_game(save)) want.clear();
        tier = 0;
        if (const size_t h = want.find('#'); h != std::string::npos) tier = std::clamp(atoi(want.c_str() + h + 1), 0, 5), want.resize(h);
        const std::string in = sword_in_memory(c);
        const Model* sm = nullptr;
        if (!want.empty()) {
            for (auto& m : made)
                if (m->folder == std::string(c.kind) + ":" + want) sm = m.get();
            if (!sm) {
                if (g_loading.load() || g_gen.load() != gen) return false;  // (still being made)
                want.clear();                                               // (it couldn't be made: the log says why)
            }
        }
        if (want != in) {
            if (!in.empty()) sword_out(c);
            if (sm && !sword_in(base, *sm, want, c)) want.clear();
            memw::mark(base, (uint32_t)g_set->game.size());
            LOG("[loot] save file %d: %s %s (was %s)", file + 1, c.kind, want.empty() ? "the game's" : want.c_str(),
                in.empty() ? "the game's" : in.c_str());
            shownTier = 0, shownStep = -1;  // (in with its own colours)
        }
        // (a picture kept in the corner: its place)
        uint32_t picAt = sm ? base + sm->image[0] : 0, mipAt = sm ? base + sm->image[2] : 0;
        if (sm && sm->surface && !want.empty()) {
            const uint8_t* ch = mem::ptr(c.area);
            const uint32_t rec = (c.area + kSwordHeader + be32(ch + 8) + kStep - 1) & ~(kStep - 1);
            picAt = corner_picture(rec + be32(ch + 12)), mipAt = picAt + mip_offset(sm->image[1]);
        }
        // its colour tier: the picture's data (its slot's image + mips) written in memory, the tier's or its own
        // (inside its kept words: out puts the game's back); the rainbow drifting along it, one round in 8 s of play
        if (sm && !want.empty() && (tier || shownTier) && sm->tiers.size() >= 4 + (size_t)kRainbow) {
            const int step = tier == 5 ? 4 + (int)(std::fmod(game_time() / 8.0, 1.0) * kRainbow) % kRainbow : tier ? tier - 1 : -1;
            if (step != shownStep) {
                const Bytes& d = step < 0 ? sm->ownImage : sm->tiers[step];
                if (d.size() == (size_t)sm->image[1] + sm->image[3]) {
                    memcpy(mem::ptr(picAt), d.data(), sm->image[1]);
                    if (sm->image[3]) memcpy(mem::ptr(mipAt), d.data() + sm->image[1], sm->image[3]);
                    memw::mark(picAt, sm->image[1]);
                    if (sm->image[3]) memw::mark(mipAt, sm->image[3]);
                }
                shownStep = step;
            }
            if (tier != shownTier) LOG("[loot] save file %d: %s colour tier %d", file + 1, c.kind, tier);
            shownTier = tier;
        }
        return true;
    };
    static int swordTier = 0, swordStep = -1, shieldTier = 0, shieldStep = -1, helmetTier = 0, helmetStep = -1, chestTier = 0, chestStep = -1, outfitTier = 0, outfitStep = -1;
    std::string wantSword, wantShield;
    int tier = 0, tierShield = 0;
    if (!equip(kSwordCorner, g_swords, g_set->swords, swordTier, swordStep, wantSword, tier)) return;
    equip(kShieldCorner, g_shields, g_set->shields, shieldTier, shieldStep, wantShield, tierShield);
    std::string wantHelmet, wantChest;
    int tierHelmet = 0, tierChest = 0;
    equip(kHelmetCorner, g_helmets, g_set->helmets, helmetTier, helmetStep, wantHelmet, tierHelmet);
    equip(kChestCorner, g_chests, g_set->chests, chestTier, chestStep, wantChest, tierChest);
    // (an outfit is made against the game's body: none over a player model)
    static const std::array<std::string, kFiles> kNoOutfits;
    std::string wantOutfit;
    int tierOutfit = 0;
    // (made with his picture as it is now: Link's colours changing it make it again; the id carries the picture's hash)
    static std::array<std::string, kFiles> outfitIds;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (int f2 = 0; f2 < kFiles; f2++) outfitIds[f2] = g_outfits[f2].empty() ? "" : outfit_id(g_outfits[f2]);
    }
    equip(kOutfitCorner, current >= 0 ? kNoOutfits : outfitIds, g_set->outfits, outfitTier, outfitStep, wantOutfit, tierOutfit);
#ifdef __ANDROID__
    // (a few bytes of the picture each second: enough to see a colour change)
    if (static int tick = 0; ++tick >= 30) {
        tick = 0;
        uint32_t off, size, mo, ms;
        if (link_picture(g_set->game, off, size, mo, ms)) {
            uint32_t h = 2166136261u;
            const uint8_t* p = mem::ptr(base + off);
            for (uint32_t i = 0; i < size; i += 64) h = (h ^ p[i]) * 16777619u;
            if (h != g_outfitHash.load()) {
                g_outfitHash = h;
                std::lock_guard<std::mutex> lk(g_mu);
                if (std::any_of(g_outfits.begin(), g_outfits.end(), [](const std::string& x) { return !x.empty(); })) g_gen++;
            }
        }
    }
#endif
#ifdef __ANDROID__
    const gcmodel::SwordDef* def = wantSword.empty() ? nullptr : gcmodel::sword_def(wantSword);
    // (the Hero's Sword hits half as hard as the Master Sword: a loot sword as strong in either save file)
    const bool hero = *(const uint8_t*)mem::ptr(save + 0x0E) == 0x38;
    static const int kTierPower[6] = {100, 110, 120, 135, 150, 175};  // (percent of the sword's own)
    const int power = def ? def->power * (hero ? 2 : 1) * kTierPower[tier] / 100 : 100;
    const float reach = def ? gcmodel::sword_reach(*def) : 1.0f, heroReach = def ? gcmodel::sword_reach_hero(*def) : 1.0f;
    const float tilt = def ? gcmodel::sword_tilt(*def) : 0.0f;
#else
    const int power = 100;
    const float reach = 1.0f, heroReach = 1.0f, tilt = 0.0f;
#endif
    if (loot_power() != power) set_loot_power(power);
    {
        const Model* worn = nullptr;
        const std::string in = loot_header_id(kOutfitCorner);
        if (!in.empty())
            for (auto& m : g_set->outfits)
                if (m->folder == "outfit:" + in) worn = m.get();
        // blinking: every 4 s the eyes half shut, shut, half shut (a frame of the picture each, written in its place in the corner)
        static const Model* blinkModel = nullptr;
        static int shownBlink = -1;
        if (worn != blinkModel) blinkModel = worn, shownBlink = -1;
        if (worn && !worn->blink.empty() && worn->surface && worn->ownImage.size() == (size_t)worn->image[1] + worn->image[3]) {
            const double t = std::fmod(game_time(), 4.0);
            const int n = (int)worn->blink.size();
            const int frame = t < 0.08 ? 0 : t < 0.16 ? n - 1 : t < 0.24 ? 0 : -1;
            if (frame != shownBlink) {
                shownBlink = frame;
                const Bytes& d = frame < 0 ? worn->ownImage : worn->blink[frame];
                if (d.size() == worn->ownImage.size()) {
                    const uint8_t* ch = mem::ptr(kOutfitCorner.area);
                    const uint32_t rec = (kOutfitCorner.area + kSwordHeader + be32(ch + 8) + kStep - 1) & ~(kStep - 1);
                    const uint32_t pic = corner_picture(rec + be32(ch + 12)), mips = pic + mip_offset(worn->image[1]);
                    memcpy(mem::ptr(pic), d.data(), worn->image[1]);
                    if (worn->image[3]) memcpy(mem::ptr(mips), d.data() + worn->image[1], worn->image[3]);
                    memw::mark(pic, worn->image[1]);
                    if (worn->image[3]) memw::mark(mips, worn->image[3]);
                }
            }
        }
        set_loot_skeleton(worn && !worn->boneRatio.empty() ? worn->boneRatio.data() : nullptr, worn ? (int)worn->boneRatio.size() : 0, worn ? worn->raise : 0,
                          worn && worn->boneOffset.size() == worn->boneRatio.size() * 3 ? worn->boneOffset.data() : nullptr, worn ? worn->armAngle : 0, worn && worn->handFrame.size() == 18 ? worn->handFrame.data() : nullptr, worn ? worn->handScale : 1);
    }
    {
        const bool hasHelmet = !sword_in_memory(kHelmetCorner).empty(), hasChest = !sword_in_memory(kChestCorner).empty();
        int defence = 0;
#ifdef __ANDROID__
        static const int kTierDefence[6] = {100, 110, 120, 135, 150, 175};  // (percent of the piece's own, as a sword's strength)
        if (const gcmodel::ArmorDef* d = hasHelmet ? gcmodel::armor_def(0, wantHelmet) : nullptr) defence += d->defence * kTierDefence[tierHelmet] / 100;
        if (const gcmodel::ArmorDef* d = hasChest ? gcmodel::armor_def(1, wantChest) : nullptr) defence += d->defence * kTierDefence[tierChest] / 100;
        if (const gcmodel::ArmorDef* d = !wantOutfit.empty() ? gcmodel::armor_def(2, wantOutfit.substr(0, wantOutfit.find('@'))) : nullptr) defence += d->defence;
#endif
        set_loot_worn(hasHelmet, hasChest, defence);
    }
    set_loot_reach(reach, heroReach, tilt);  // (checks the game's constants itself)
}

void set_loot_swords(const std::string& gameDir, const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_gameDir = gameDir;
    if (g_basePack.empty()) g_basePack = gameDir + "/content/Common/Pack/permanent_3d.pack";
    for (int f = 0; f < kFiles; f++) g_swords[f] = f < (int)ids.size() ? ids[f] : "";
    g_gen++;
}
uint32_t loot_sword_area(uint32_t* size) {
    *size = kSwordSize + kShieldCorner.size;  // (the shield's corner right below: both in one region)
    return kShieldCorner.area;
}
void set_loot_armor(int slot, const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lk(g_mu);
    auto& a = slot == 2 ? g_outfits : slot ? g_chests : g_helmets;
    for (int f = 0; f < kFiles; f++) a[f] = f < (int)ids.size() ? ids[f] : "";
    g_gen++;
}
std::string loot_outfit_in_memory() {
    const std::string id = loot_header_id(kOutfitCorner);
    return id.substr(0, id.find('@'));
}
uint32_t loot_outfit_area(uint32_t* size) {
    *size = kOutfitCorner.size;
    return kOutfitCorner.area;
}
uint32_t loot_armor_area(uint32_t* size) {
    *size = kChestCorner.size + kHelmetCorner.size;
    return kChestCorner.area;
}
// the sword's / shield's id in memory now, "" the game's (its header only: for polling, any thread)
std::string loot_in_memory(bool shield) {
    const Corner& c = shield ? kShieldCorner : kSwordCorner;
    const uint8_t* h = mem::ptr(c.area);
    if (memcmp(h, c.magic, 4) != 0) return "";
    return std::string((const char*)h + 20, strnlen((const char*)h + 20, 32));
}
void set_loot_shields(const std::vector<std::string>& ids) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int f = 0; f < kFiles; f++) g_shields[f] = f < (int)ids.size() ? ids[f] : "";
    g_gen++;
}

}  // namespace mods
