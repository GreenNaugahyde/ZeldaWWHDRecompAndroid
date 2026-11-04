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
};

struct Set {
    Bytes game;  // the game's Link.bfres
    std::vector<Buffer> buffers;  // its vertex/index buffers (the game sets their data pointers at load)
    std::string basePack;
    std::vector<std::shared_ptr<Model>> models;
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
    if (m.size() - model->tail0 > kSaveNameArea - kArea - kHeader) return fail("too big");  // (save names after it)
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

void loader(uint32_t gen, std::string basePack, std::string packsDir, std::vector<std::string> folders, std::shared_ptr<Set> had) {
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
    }
#else
    (void)packsDir, (void)folders, (void)had;
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
        std::vector<std::string> folders;
        std::string basePack, packsDir;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            basePack = g_basePack, packsDir = g_packsDir;
            for (auto& f : g_folders)
                if (!f.empty() && std::find(folders.begin(), folders.end(), f) == folders.end()) folders.push_back(f);
        }
        // (the model in memory stays loaded until it's taken out; one only a save state had, too)
        for (const std::string& keep : {g_set && current >= 0 && current < (int)g_set->models.size() ? g_set->models[current]->folder : std::string(), extra})
            if (!keep.empty() && std::find(folders.begin(), folders.end(), keep) == folders.end()) folders.push_back(keep);
        // nothing chosen and nothing in: nothing to do
        if (basePack.empty() || (folders.empty() && current < 0 && !g_set)) return;
        g_loading = 1;
        std::thread(loader, g, basePack, packsDir, folders, g_set).detach();
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
    if (target == current) return;
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

}  // namespace mods
