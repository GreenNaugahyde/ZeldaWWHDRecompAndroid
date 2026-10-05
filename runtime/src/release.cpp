// Game releases and their address maps (release.h).
#include "release.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#ifdef WWHD_RELEASE_MAPS
extern "C" const char wwhd_release_eur[], wwhd_release_eur_end[];  // recomp_data.S: tools/recomp/release_eur.txt
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
    std::vector<std::pair<uint32_t, uint32_t>> funcs;  // entries of changed functions (USA, other)
};

Id g_id = Id::USA;

const Map& eur() {
    static Map m;
    static std::once_flag once;
    std::call_once(once, [] {
#ifdef WWHD_RELEASE_MAPS
        std::string text(wwhd_release_eur, wwhd_release_eur_end);
        size_t p = 0;
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
            else if (!strcmp(kind, "func")) m.funcs.push_back({a, b});
            else if (!strcmp(kind, "code")) m.code.push_back({a, b, d});
            else if (!strcmp(kind, "data")) m.data.push_back({a, b, d});
        }
        std::sort(m.funcs.begin(), m.funcs.end());
#endif
    });
    return m;
}

const Range* find(const std::vector<Range>& v, uint32_t a) {
    auto it = std::upper_bound(v.begin(), v.end(), a, [](uint32_t x, const Range& r) { return x < r.lo; });
    if (it == v.begin()) return nullptr;
    --it;
    return a < it->hi ? &*it : nullptr;
}

}  // namespace

bool known_entry(uint32_t entryPoint) { return entryPoint == kUsaEntry || (eur().entry && entryPoint == eur().entry); }

bool select(uint32_t entryPoint) {
    if (entryPoint == kUsaEntry) g_id = Id::USA;
    else if (eur().entry && entryPoint == eur().entry) g_id = Id::EUR;
    else return false;
    return true;
}

Id id() { return g_id; }
const char* name() { return g_id == Id::EUR ? "EUR" : "USA"; }

uint32_t code(uint32_t usa) {
    if (g_id == Id::USA) return usa;
    const Map& m = eur();
    if (const Range* r = find(m.code, usa)) return usa + r->delta;
    auto it = std::lower_bound(m.funcs.begin(), m.funcs.end(), std::make_pair(usa, 0u));
    if (it != m.funcs.end() && it->first == usa) return it->second;
    return 0;
}

uint32_t data(uint32_t usa) {
    if (g_id == Id::USA) return usa;
    const Range* r = find(eur().data, usa);
    return r ? usa + r->delta : usa;
}

uint32_t usa_code(uint32_t addr) {
    if (g_id == Id::USA) return addr;
    const Map& m = eur();
    for (const Range& r : m.code)  // few ranges: a scan in this direction is fine
        if (addr >= r.lo + r.delta && addr < r.hi + r.delta) return addr - r.delta;
    for (auto& [u, o] : m.funcs)
        if (o == addr) return u;
    return 0;
}

std::string map_text() {
#ifdef WWHD_RELEASE_MAPS
    return std::string(wwhd_release_eur, wwhd_release_eur_end);
#else
    return {};
#endif
}

}  // namespace release
