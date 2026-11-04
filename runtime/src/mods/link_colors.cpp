// Mods > Link's colours, per save file: Link's coloured textures (body, casual hair, hands, mouth, eyes) put into the
// game's memory, where the renderer reads textures from (written pages are uploaded again, mem_writes.h), for the save
// file played and the clothes he wears. The game's own pack stays as it is.
//
// The app makes <dir>/file<N>.bin for each save file with colours (rando/link_colors_make.h: per texture level its
// place in Link.bfres, the game's bytes, the island clothes' and, for the hands, mouth and eyes, which both clothes draw
// with, the hero's own).
// Where the game keeps them is found by their bytes, all levels and versions at once in one pass over MEM2, MEM1 and
// the foreground bucket (on a thread of its own; 256-byte steps, a key of 16 bytes per version): Link.bfres's start
// from the places that agree with their offsets, each level at start + offset (mouthS3TC.10's bytes are three textures
// in the file), and copies of a level outside the file as well (the game may draw from those).
// Each frame start the save file (dSv_info_c::mDataNum, progress.cpp) and the clothes flag (Link + 0x3BC bit 8,
// clothes.cpp) decide what goes in; every half second the bytes are checked (a save state or a reload may have put
// others back).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "mods.h"
#include "../mem_writes.h"
#include "../packs.h"
#include "../runtime.h"

namespace mods {
namespace {
constexpr uint32_t kFlags = 0x3BC, kCasual = 0x8;  // mNoResetFlg1, daPyFlg1_CASUAL_CLOTHES (clothes.cpp)
constexpr uint32_t kDataNum = 0x1290;              // the save file's number 0..2 (progress.cpp)
constexpr int kFiles = 3;
constexpr uint32_t kStep = 0x100;                  // texture data alignment (GX2: 256 bytes and up)

struct Level {
    std::string name;
    uint32_t size = 0, offset = 0;         // its bytes and place in Link.bfres
    const uint8_t* game = nullptr;         // the game's bytes
    const uint8_t* casual[kFiles] = {};    // per save file: the island clothes' (null: the game's)
    const uint8_t* hero[kFiles] = {};      // ... the hero's
    std::vector<uint32_t> addrs;           // where the game has it (empty: not found)
    std::vector<uint8_t> shown;            // what was put there last (a file made again no longer has it)
};
std::vector<std::vector<uint8_t>> g_files(kFiles);  // the files' bytes (the levels point into them)
std::vector<Level> g_levels;
std::atomic<int> g_scan{0};  // 0 to do, 1 running, 2 found, 3 not found

void parse(int file, std::vector<Level>& levels) {
    const std::vector<uint8_t>& b = g_files[file];
    auto u32 = [&](size_t p) { uint32_t v; memcpy(&v, &b[p], 4); return v; };
    if (b.size() < 12 || memcmp(b.data(), "WWLC", 4) != 0 || u32(4) != 3) return;
    size_t p = 12;
    for (uint32_t i = 0, count = u32(8); i < count; i++) {
        if (p + 2 > b.size()) return;
        uint16_t len;
        memcpy(&len, &b[p], 2);
        if (p + 2 + len + 9 > b.size()) return;
        std::string name((const char*)&b[p + 2], len);
        p += 2 + len;
        uint32_t size = u32(p), offset = u32(p + 4);
        bool ownHero = b[p + 8] != 0;
        p += 9;
        if (size < 16 || p + (ownHero ? 3ull : 2ull) * size > b.size()) return;
        auto it = std::find_if(levels.begin(), levels.end(), [&](const Level& l) { return l.name == name; });
        if (it == levels.end()) {
            Level l;
            l.name = name;
            l.size = size;
            l.offset = offset;
            l.game = &b[p];
            levels.push_back(l);
            it = levels.end() - 1;
        }
        if (it->size == size) {
            it->casual[file] = &b[p + size];
            it->hero[file] = ownHero ? &b[p + 2ull * size] : &b[p + size];
        }
        p += (ownHero ? 3ull : 2ull) * size;
    }
}

// the files of `dir` (any of them may be missing: that save file has the game's colours); places found before stay
void load(const std::string& dir) {
    std::map<std::string, std::pair<std::vector<uint32_t>, std::vector<uint8_t>>> known;
    for (Level& l : g_levels) known[l.name] = {std::move(l.addrs), std::move(l.shown)};
    g_levels.clear();
    for (int f = 0; f < kFiles; f++) {
        g_files[f].clear();
        if (dir.empty()) continue;
        if (FILE* h = fopen((dir + "/file" + std::to_string(f + 1) + ".bin").c_str(), "rb")) {
            uint8_t buf[65536];
            size_t n;
            while ((n = fread(buf, 1, sizeof buf, h)) > 0) g_files[f].insert(g_files[f].end(), buf, buf + n);
            fclose(h);
        }
    }
    for (int f = 0; f < kFiles; f++) parse(f, g_levels);
    bool all = !g_levels.empty();
    for (Level& l : g_levels) {
        auto it = known.find(l.name);
        if (it != known.end()) l.addrs = std::move(it->second.first), l.shown = std::move(it->second.second);
        all = all && !l.addrs.empty();
    }
    if (!all) g_scan = 0;
    int files = 0;
    for (auto& f : g_files) files += !f.empty();
    if (!dir.empty()) LOG("[linkcolors] %d save file(s) with colours, %zu texture levels", files, g_levels.size());
}

// every version of a level the game's memory may hold
std::vector<const uint8_t*> versions(const Level& l) {
    std::vector<const uint8_t*> v{l.game};
    if (l.shown.size() == l.size) v.push_back(l.shown.data());
    for (int f = 0; f < kFiles; f++)
        for (const uint8_t* p : {l.casual[f], l.hero[f]})
            if (p && std::none_of(v.begin(), v.end(), [&](const uint8_t* q) { return memcmp(p, q, l.size) == 0; })) v.push_back(p);
    return v;
}

bool holds_version(const Level& l, uint32_t a) {
    const uint8_t* p = mem::ptr(a);
    for (const uint8_t* v : versions(l))
        if (memcmp(p, v, l.size) == 0) return true;
    return false;
}

// (its own thread) every place of every level's versions
void scan() {
    // per version a key: 16 bytes at a multiple of kStep that aren't all alike (zero pages are skipped at once)
    struct Key { uint32_t level, at; const uint8_t* bytes; };
    std::unordered_multimap<uint64_t, Key> keys;
    auto key_of = [](const uint8_t* p) { uint64_t a, b; memcpy(&a, p, 8); memcpy(&b, p + 8, 8); return a * 0x9E3779B97F4A7C15ull ^ b; };
    for (uint32_t i = 0; i < g_levels.size(); i++)
        for (const uint8_t* v : versions(g_levels[i])) {
            for (uint32_t at = 0; at + 16 <= g_levels[i].size; at += kStep) {
                const uint8_t* k = v + at;
                if (std::all_of(k, k + 16, [&](uint8_t c) { return c == k[0]; })) continue;
                keys.emplace(key_of(k), Key{i, at, v});
                break;
            }
        }
    std::vector<std::vector<uint32_t>> found(g_levels.size());
    struct Range { uint32_t from, size; };
    for (Range r : {Range{mem::kMem2Start, mem::kMem2End - mem::kMem2Start}, Range{mem::kMem1, mem::kMem1Size},
                    Range{mem::kFgBucket, mem::kFgBucketSize}}) {
        for (uint64_t a = r.from; a + 16 <= (uint64_t)r.from + r.size; a += kStep) {
            const uint8_t* p = mem::ptr((uint32_t)a);
            uint64_t w0, w1;
            memcpy(&w0, p, 8);
            memcpy(&w1, p + 8, 8);
            if (w0 == 0 && w1 == 0) continue;
            auto [lo, hi] = keys.equal_range(key_of(p));
            for (auto it = lo; it != hi; ++it) {
                const Key& k = it->second;
                const Level& l = g_levels[k.level];
                if (a < r.from + k.at || a - k.at + l.size > (uint64_t)r.from + r.size) continue;
                uint32_t start = (uint32_t)(a - k.at);
                if (memcmp(mem::ptr(start), k.bytes, l.size) == 0) found[k.level].push_back(start);
            }
        }
    }
    // Link.bfres's start: the one most places agree on
    std::map<uint32_t, int> votes;
    for (uint32_t i = 0; i < g_levels.size(); i++)
        for (uint32_t a : found[i])
            if (a >= g_levels[i].offset) votes[a - g_levels[i].offset]++;
    uint32_t base = 0;
    int best = 0;
    for (auto [b, n] : votes)
        if (n > best) base = b, best = n;
    uint32_t end = 0;
    for (const Level& l : g_levels) end = std::max(end, l.offset + l.size);
    size_t placed = 0, copies = 0;
    for (uint32_t i = 0; i < g_levels.size(); i++) {
        Level& l = g_levels[i];
        std::sort(found[i].begin(), found[i].end());
        found[i].erase(std::unique(found[i].begin(), found[i].end()), found[i].end());
        l.addrs.clear();
        bool inFile = false;
        for (uint32_t a : found[i])
            if (best >= 2 && a == base + l.offset) inFile = true;
        if (inFile) l.addrs.push_back(base + l.offset);
        // a copy of it outside the file (what the game may draw from); inside it: another texture alike
        int others = 0;
        for (uint32_t a : found[i])
            if (!(best >= 2 && a >= base && a < base + end)) l.addrs.push_back(a), others++;
        if (others) LOG("[linkcolors] %s: %d place(s) outside Link.bfres, first %08X", l.name.c_str(), others, l.addrs.back());
        placed += !l.addrs.empty();
        copies += others;
    }
    LOG("[linkcolors] Link.bfres at %08X (%d levels agree); %zu of %zu texture levels found, %zu copies elsewhere", base, best, placed,
        g_levels.size(), copies);
    g_scan.store(placed ? 2 : 3, std::memory_order_release);
}
}  // namespace

// game main thread, frame start (interp.cpp)
void link_colors_service() {
    static uint32_t gen = ~0u;
    static int frame = 0, last = -1, lastScan = -100000;
    frame++;
    if (g_scan.load(std::memory_order_acquire) == 1) return;  // (the thread is reading g_levels)
    if ((frame & 15) == 1) {
        uint32_t g = packs::link_colors_gen();
        if (g != gen) {
            gen = g;
            load(packs::link_colors_used());
            last = -1;
        }
    }
    if (g_levels.empty()) return;
    int s = g_scan.load(std::memory_order_acquire);
    // (none found yet: again every 2 s, Link.bfres may not be loaded; some missing: every 10 s)
    bool missing = std::any_of(g_levels.begin(), g_levels.end(), [](const Level& l) { return l.addrs.empty(); });
    if (s == 0 || s == 3 || (missing && frame - lastScan > 600)) {
        if (frame - lastScan < 120) return;
        lastScan = frame;
        g_scan = 1;
        std::thread(scan).detach();
        return;
    }
    uint32_t link = link_actor(), save = player_save();
    if (!link || !save) return;
    int file = *(const uint8_t*)mem::ptr(save + kDataNum);
    if (file < 0 || file >= kFiles) return;
    if (player_model_file(file)) {  // (a player model's textures: its own)
        last = -1;
        return;
    }
    bool casual = (ld32(link + kFlags) & kCasual) != 0;
    const bool fresh = save_new_game(save);  // (a new game in that file: the game's colours; the app forgets them)
    int now = fresh ? 6 + file : file * 2 + (casual ? 1 : 0);
    if (now == last && frame % 30 != 0) return;
    last = now;
    // what each place is to hold and whether it still holds one of its versions
    size_t written = 0, places = 0, known = 0;
    std::vector<std::pair<Level*, uint32_t>> todo, unknown;
    for (Level& l : g_levels) {
        const uint8_t* want = fresh ? nullptr : casual ? l.casual[file] : l.hero[file];
        if (!want) want = l.game;
        for (uint32_t a : l.addrs) {
            places++;
            if (memcmp(mem::ptr(a), want, l.size) == 0) {
                known++;
                continue;
            }
            if (holds_version(l, a)) known++, todo.push_back({&l, a});
            else unknown.push_back({&l, a});
        }
    }
    // bytes no version has (a save state from colours made since): still there while most of the others are (Link.bfres
    // stays where it was loaded), else it moved
    bool lost = false;
    if (!unknown.empty()) {
        if (2 * known >= places) todo.insert(todo.end(), unknown.begin(), unknown.end());
        else lost = true;
    }
    for (auto [l, a] : todo) {
        const uint8_t* want = fresh ? nullptr : casual ? l->casual[file] : l->hero[file];
        if (!want) want = l->game;
        memcpy(mem::ptr(a), want, l->size);
        memw::mark(a, l->size);
        l->shown.assign(want, want + l->size);
        written++;
    }
    if (written) LOG("[linkcolors] save file %d, %s clothes: %zu places written", file + 1, casual ? "island" : "hero's", written);
    if (lost) {
        LOG("[linkcolors] the textures moved: looking again");
        g_scan = 0;
    }
}

}  // namespace mods
