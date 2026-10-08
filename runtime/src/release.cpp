// Game releases and their address maps (release.h).
#include "release.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef WWHD_RELEASE_MAPS
// recomp_data.S: tools/recomp/release_eur.txt, release_jpn.txt
extern "C" const char wwhd_release_eur[], wwhd_release_eur_end[], wwhd_release_jpn[], wwhd_release_jpn_end[];
#endif

namespace release {
namespace {

constexpr uint32_t kUsaEntry = 0x028EA120;

struct Range {
    uint32_t lo, hi;
    int32_t delta;
};
struct Map {
    uint32_t entry = 0;
    std::vector<Range> code, data;
    // entries of changed functions and hook addresses inside them (USA, other), sorted by USA address
    std::vector<std::pair<uint32_t, uint32_t>> funcs, rfuncs;  // rfuncs: (other, USA), sorted
    std::string text;
};

Id g_id = Id::USA;

Map parse(const char* begin, const char* end) {
    Map m;
    m.text.assign(begin, end);
    size_t p = 0;
    const std::string& text = m.text;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(p, e - p);
        p = e + 1;
        // "KIND HEX HEX [SIGNED-HEX]"; '#' comments
        char kind[8] = {};
        const char* c = line.c_str();
        if (sscanf(c, "%7s", kind) != 1 || kind[0] == '#') continue;
        char* q = (char*)c + strlen(kind);
        uint32_t a = (uint32_t)strtoul(q, &q, 16), b = (uint32_t)strtoul(q, &q, 16);
        int32_t d = (int32_t)strtol(q, &q, 16);
        if (!strcmp(kind, "entry")) m.entry = b;
        else if (!strcmp(kind, "func") || !strcmp(kind, "site")) m.funcs.push_back({a, b});
        else if (!strcmp(kind, "code")) m.code.push_back({a, b, d});
        else if (!strcmp(kind, "data")) m.data.push_back({a, b, d});
    }
    std::sort(m.funcs.begin(), m.funcs.end());
    for (auto& [u, o] : m.funcs) m.rfuncs.push_back({o, u});
    std::sort(m.rfuncs.begin(), m.rfuncs.end());
    return m;
}

// the address map of a release other than USA (empty for USA or without maps)
const Map& map_of(Id id) {
    static Map maps[3];
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef WWHD_RELEASE_MAPS
        maps[(int)Id::EUR] = parse(wwhd_release_eur, wwhd_release_eur_end);
        maps[(int)Id::JPN] = parse(wwhd_release_jpn, wwhd_release_jpn_end);
#endif
    });
    return maps[(int)id];
}

const Range* find(const std::vector<Range>& v, uint32_t a) {
    auto it = std::upper_bound(v.begin(), v.end(), a, [](uint32_t x, const Range& r) { return x < r.lo; });
    if (it == v.begin()) return nullptr;
    --it;
    return a < it->hi ? &*it : nullptr;
}

// the release with this executable entry point
bool release_of(uint32_t entryPoint, Id& out) {
    if (entryPoint == kUsaEntry) {
        out = Id::USA;
        return true;
    }
    for (Id id : {Id::EUR, Id::JPN})
        if (map_of(id).entry && entryPoint == map_of(id).entry) {
            out = id;
            return true;
        }
    return false;
}

}  // namespace

bool known_entry(uint32_t entryPoint) {
    Id id;
    return release_of(entryPoint, id);
}

bool select(uint32_t entryPoint) { return release_of(entryPoint, g_id); }

Id id() { return g_id; }
const char* name() { return name_of(g_id); }
const char* name_of(Id id) { return id == Id::EUR ? "EUR" : id == Id::JPN ? "JPN" : "USA"; }

const char* name_of_entry(uint32_t entryPoint) {
    Id id;
    return release_of(entryPoint, id) ? name_of(id) : "";
}

uint32_t code(uint32_t usa) {
    if (g_id == Id::USA) return usa;
    const Map& m = map_of(g_id);
    if (const Range* r = find(m.code, usa)) return usa + r->delta;
    auto it = std::lower_bound(m.funcs.begin(), m.funcs.end(), std::make_pair(usa, 0u));
    if (it != m.funcs.end() && it->first == usa) return it->second;
    return 0;
}

uint32_t data(uint32_t usa) {
    if (g_id == Id::USA) return usa;
    const Range* r = find(map_of(g_id).data, usa);
    return r ? usa + r->delta : usa;
}

uint32_t usa_code(uint32_t addr) {
    if (g_id == Id::USA) return addr;
    const Map& m = map_of(g_id);
    for (const Range& r : m.code)  // few ranges: a scan in this direction is fine
        if (addr >= r.lo + r.delta && addr < r.hi + r.delta) return addr - r.delta;
    auto it = std::lower_bound(m.rfuncs.begin(), m.rfuncs.end(), std::make_pair(addr, 0u));
    return it != m.rfuncs.end() && it->first == addr ? it->second : 0;
}

std::string map_text(uint32_t entryPoint) {
    Id id;
    if (!release_of(entryPoint, id) || id == Id::USA) return {};
    return map_of(id).text;
}

}  // namespace release
