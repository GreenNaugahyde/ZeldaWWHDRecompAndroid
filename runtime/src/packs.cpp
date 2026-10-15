// Mod packs (see packs.h).
#include "packs.h"

#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>
#include <chrono>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <mutex>
#include <sstream>
#include <tuple>
#include <unordered_map>

#include "disc/crypto.h"
#include "recomp/program.h"
#include "runtime.h"
#include "wwhd/gpack.h"

namespace packs {
namespace {

std::mutex g_mu;
std::string g_gameDir, g_cfg;
bool g_done = false;
recomp::CodePatch g_patch;
std::vector<Status> g_status;
uint32_t g_caveEnd = kCodeCave;
// content replacement: lower-case path below content/ -> host path (files; folders); per lower-case
// folder its entries
std::unordered_map<std::string, std::string> g_contentFiles, g_contentDirs;
std::unordered_map<std::string, std::vector<DirEntry>> g_contentEntries;
// shader replacements: (base hash, aux hash, type) -> GLSL
struct ShaderKey {
    uint64_t base, aux;
    char type;
    bool operator<(const ShaderKey& o) const { return std::tie(base, aux, type) < std::tie(o.base, o.aux, o.type); }
};
std::map<ShaderKey, std::string> g_customShaders;
std::vector<gpack::TextureRule> g_textureRules;  // of all enabled packs, in list order

// Cemu's ApplyShaderPresets: each $name becomes its value (integers for "$name:int" variables)
std::string fill_variables(std::string src, const std::map<std::string, double>& vars, const std::vector<std::string>& ints) {
    std::string out;
    for (size_t i = 0; i < src.size();) {
        if (src[i] != '$') {
            out += src[i++];
            continue;
        }
        size_t j = i + 1;
        while (j < src.size() && (isalnum((unsigned char)src[j]) || src[j] == '_')) j++;
        std::string name = src.substr(i, j - i);
        auto it = vars.find(name);
        if (it == vars.end()) {
            out += name;  // unknown: left as it is (the compile reports it)
        } else if (std::find(ints.begin(), ints.end(), name) != ints.end()) {
            out += std::to_string((long long)it->second);
        } else {
            char b[64];
            snprintf(b, sizeof b, "%f", it->second);
            out += b;
        }
        i = j;
    }
    return out;
}


std::string lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

// a pack's content/ folder into the index; returns the number of files
size_t index_content(const std::string& root, const std::string& rel = "") {
    size_t n = 0;
    const std::string dir = rel.empty() ? root : root + "/" + rel;
    if (DIR* h = opendir(dir.c_str())) {
        std::vector<std::pair<std::string, bool>> items;
        while (dirent* e = readdir(h)) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            struct stat st;
            if (stat((dir + "/" + name).c_str(), &st) != 0) continue;
            items.push_back({name, S_ISDIR(st.st_mode)});
        }
        closedir(h);
        auto& entries = g_contentEntries[lower(rel)];
        for (auto& [name, isDir] : items) {
            std::string r = rel.empty() ? name : rel + "/" + name, host = dir + "/" + name;
            auto same = std::find_if(entries.begin(), entries.end(), [&](const DirEntry& d) { return lower(d.name) == lower(name); });
            if (same == entries.end()) entries.push_back({name, host});
            else same->host = host;
            if (isDir) {
                g_contentDirs[lower(r)] = host;
                n += index_content(root, r);
            } else {
                g_contentFiles[lower(r)] = host;
                n++;
            }
        }
    }
    return n;
}

std::string read_file(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return {};
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    size_t i = 0;
    for (;;) {
        size_t e = s.find(sep, i);
        out.push_back(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
        if (e == std::string::npos) return out;
        i = e + 1;
    }
}

std::string parent(const std::string& dir) {
    std::string d = dir;
    while (d.size() > 1 && d.back() == '/') d.pop_back();
    size_t s = d.find_last_of('/');
    return s == std::string::npos ? "." : d.substr(0, s);
}

bool ends_with_i(const std::string& s, const char* suf) {
    size_t n = strlen(suf);
    return s.size() >= n && strcasecmp(s.c_str() + s.size() - n, suf) == 0;
}
bool starts_with_i(const std::string& s, const char* pre) { return strncasecmp(s.c_str(), pre, strlen(pre)) == 0; }

struct FileEntry {
    std::string name;
    bool dir;
};
std::vector<FileEntry> list_dir(const std::string& d) {
    std::vector<FileEntry> out;
    if (DIR* h = opendir(d.c_str())) {
        while (dirent* e = readdir(h)) {
            std::string n = e->d_name;
            if (n == "." || n == "..") continue;
            struct stat st;
            bool isDir = stat((d + "/" + n).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
            out.push_back({n, isDir});
        }
        closedir(h);
    }
    std::sort(out.begin(), out.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    return out;
}

// the patch files (Cemu: patch_*.asm; a pack with none may have a Cemuhook patches.txt) and the parts
// of the pack this port doesn't handle yet
struct PackFiles {
    std::vector<std::pair<std::string, std::string>> patches;
    std::vector<std::string> unsupported;
    size_t contentFiles = 0;  // game files in content/
    size_t pixelShaders = 0;  // <hash>_<hash>_ps.txt and _vs.txt
};
size_t count_files(const std::string& dir) {
    size_t n = 0;
    for (auto& e : list_dir(dir)) n += e.dir ? count_files(dir + "/" + e.name) : 1;
    return n;
}
PackFiles pack_files(const std::string& dir, const gpack::Rules& rules) {
    PackFiles pf;
    bool cemuhook = false;
    int shaders = 0;
    for (auto& e : list_dir(dir)) {
        if (e.dir) {
            if (!strcasecmp(e.name.c_str(), "content")) pf.contentFiles += count_files(dir + "/" + e.name);
            else if (!strcasecmp(e.name.c_str(), "aoc")) pf.unsupported.push_back("DLC file replacements (aoc/)");
            continue;
        }
        if (starts_with_i(e.name, "patch_") && ends_with_i(e.name, ".asm")) pf.patches.push_back({e.name, read_file(dir + "/" + e.name)});
        else if (!strcasecmp(e.name.c_str(), "patches.txt")) cemuhook = true;
        else if (ends_with_i(e.name, "_gs.txt")) shaders++;
        else if (ends_with_i(e.name, "_ps.txt") || ends_with_i(e.name, "_vs.txt")) pf.pixelShaders++;
    }
    if (cemuhook && pf.patches.empty()) pf.patches.push_back({"patches.txt", read_file(dir + "/patches.txt")});  // Cemu: only without patch_*.asm
    if (shaders) pf.unsupported.push_back(std::to_string(shaders) + " geometry shader replacements");
    if (pf.pixelShaders && rules.version <= 3) pf.unsupported.push_back("shaders for Cemu's OpenGL renderer (pack version 3 or older)");
    std::map<std::string, int> secs;
    for (auto& s : rules.sections) secs[s]++;
    for (const char* s : {"Output", "Control", "RAM"})
        for (auto& [n, c] : secs)
            if (!strcasecmp(n.c_str(), s)) pf.unsupported.push_back(std::string("[") + s + "] rules");
    // texture rules: sampling (anisotropy, LOD bias) works; resolution and format changes don't yet
    // (evaluated with the default settings: their expressions use the preset variables)
    for (auto& r : gpack::texture_rules(read_file(dir + "/rules.txt"), gpack::resolve(rules, {}).vars)) {
        if (r.overwriteWidth != -1 || r.overwriteHeight != -1 || r.overwriteDepth != -1 || r.overwriteFormat != -1) {
            pf.unsupported.push_back("texture resolution/format rules");
            break;
        }
    }
    return pf;
}

// the pack's shader files (Cemu: <hex>_<hex>_ps|vs|gs[...].txt; packs of rules version 3 and older have
// pre-Vulkan GLSL, which Cemu's Vulkan renderer skips); how many
size_t index_shaders(const std::string& dir, const gpack::Rules& rules, const std::map<std::string, double>& vars) {
    if (rules.version <= 3) return 0;
    size_t n = 0;
    for (auto& e : list_dir(dir)) {
        if (e.dir) continue;
        unsigned long long base, aux;
        char type[16] = {};
        if (sscanf(e.name.c_str(), "%16llx_%16llx_%15[a-z_.]", &base, &aux, type) != 3) continue;
        if (strncmp(type, "ps", 2) && strncmp(type, "vs", 2) && strncmp(type, "gs", 2)) continue;
        if (strstr(type, "_msl")) continue;  // Metal sources
        ShaderKey k{base, aux, type[0]};
        if (g_customShaders.count(k)) continue;  // the first pack in the list wins
        g_customShaders[k] = fill_variables(read_file(dir + "/" + e.name), vars, rules.intVars);
        n++;
    }
    return n;
}

std::vector<uint8_t> read_bytes(const std::string& p) {
    std::string s = read_file(p);
    return std::vector<uint8_t>(s.begin(), s.end());
}

}  // namespace

void init(const std::string& gameDir) {
    std::lock_guard<std::mutex> lk(g_mu);
    const std::string dir = parent(gameDir) + "/packs";
    std::string cfg = read_file(dir + "/enabled.txt");
    if (g_done && g_gameDir == gameDir && g_cfg == cfg) return;
    g_done = true;
    g_gameDir = gameDir;
    g_cfg = cfg;
    g_patch = {};
    g_status.clear();
    g_caveEnd = kCodeCave;
    g_contentFiles.clear();
    g_contentDirs.clear();
    g_contentEntries.clear();
    g_customShaders.clear();
    g_textureRules.clear();
    recomp::set_code_patch(nullptr);
    if (cfg.empty()) return;
    std::vector<std::string> folders;
    std::map<std::string, std::map<std::string, std::string>> selection;
    for (auto& line : split(cfg, '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 2 && f[0] == "pack") folders.push_back(f[1]);
        else if (f.size() >= 4 && f[0] == "preset") selection[f[1]][f[2]] = f[3];
    }
    if (folders.empty()) return;
    std::vector<uint8_t> rpx = read_bytes(gameDir + "/code/cking.rpx");
    gpack::Module m = gpack::module_info(rpx);
    std::string keyText;
    uint32_t cave = kCodeCave;
    for (auto& folder : folders) {
        Status st;
        st.folder = folder;
        std::string pdir = dir + "/" + folder;
        gpack::Rules rules;
        std::string err;
        if (!gpack::parse_rules(read_file(pdir + "/rules.txt"), rules, err)) {
            st.notes.push_back(err);
            LOG("[packs] %s: %s", folder.c_str(), err.c_str());
            g_status.push_back(st);
            continue;
        }
        st.name = rules.name;
        PackFiles pf = pack_files(pdir, rules);
        for (auto& u : pf.unsupported) st.notes.push_back("not supported yet: " + u);
        for (auto& e : list_dir(pdir))
            if (e.dir && !strcasecmp(e.name.c_str(), "content")) {
                size_t n = index_content(pdir + "/" + e.name);
                LOG("[packs] %s: %zu game files from content/", folder.c_str(), n);
            }
        if (size_t n = index_shaders(pdir, rules, gpack::resolve(rules, selection[folder]).vars))
            LOG("[packs] %s: %zu shader replacements", folder.c_str(), n);
        {
            auto tr = gpack::texture_rules(read_file(pdir + "/rules.txt"), gpack::resolve(rules, selection[folder]).vars);
            size_t used = 0;
            for (auto& r : tr)
                if (r.anisotropy >= 0 || r.lodBias != -1 || r.relativeLodBias != -1) {
                    g_textureRules.push_back(r);
                    used++;
                }
            if (used) LOG("[packs] %s: %zu texture rules", folder.c_str(), used);
        }
        if (!pf.patches.empty()) {
            gpack::Resolved res = gpack::resolve(rules, selection[folder]);
            gpack::PatchResult pr;
            if (gpack::apply_patches(pf.patches, res.vars, m, cave, pr)) {
                if (pr.caveEnd > kCodeCaveEnd) {
                    st.notes.push_back("not applied: the code caves of all packs exceed 4 MiB");
                } else {
                    st.groups = pr.groups;
                    st.groupsOther = pr.groupsOther;
                    for (auto& w : pr.writes) g_patch.writes.push_back({w.addr, w.bytes});
                    for (uint32_t a : pr.codePointers) g_patch.entries.push_back(a);
                    if (!pr.entryCallbacks.empty()) st.notes.push_back("not supported yet: .callback entry");
                    cave = pr.caveEnd;
                    keyText += folder + "\n";
                    for (auto& [k, v] : res.vars) keyText += k + "=" + std::to_string(v) + "\n";
                }
            } else {
                st.groupsOther = pr.groupsOther;
                for (auto& e : pr.errors) st.notes.push_back(e);
            }
        }
        LOG("[packs] %s (%s): %d patch groups applied, %d for other game versions%s", folder.c_str(), st.name.c_str(), st.groups,
            st.groupsOther, st.notes.empty() ? "" : "; notes:");
        for (auto& n : st.notes) LOG("[packs]   %s", n.c_str());
        g_status.push_back(st);
    }
    if (g_patch.writes.empty()) return;
    g_patch.caveLo = kCodeCave;
    g_patch.caveHi = cave;
    g_caveEnd = (cave + 0xFFFF) & ~0xFFFFu;
    // the key: everything that changes the code (the writes themselves, with the entries)
    std::string blob;
    for (auto& w : g_patch.writes) {
        blob.append((const char*)&w.addr, 4);
        blob.append((const char*)w.bytes.data(), w.bytes.size());
    }
    for (uint32_t a : g_patch.entries) blob.append((const char*)&a, 4);
    uint8_t h[20];
    disc::sha1((const uint8_t*)blob.data(), blob.size(), h);
    char hex[41];
    for (int i = 0; i < 20; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    g_patch.key = std::string("packs ") + hex;
    LOG("[packs] %zu writes, code cave %08X-%08X, %zu extra entries (%s)", g_patch.writes.size(), g_patch.caveLo, g_patch.caveHi,
        g_patch.entries.size(), g_patch.key.c_str());
    recomp::set_code_patch(&g_patch);
}

void apply_memory() {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto& w : g_patch.writes) memcpy(mem::ptr(w.addr), w.bytes.data(), w.bytes.size());
    if (!g_patch.writes.empty()) LOG("[packs] wrote %zu patches into guest memory", g_patch.writes.size());
}

// WWHD_PACK_TRACE=1: how often each block of the packs' code runs (emit.cpp, trace), logged every 5 s
// as "address +runs since the last report" (game thread)
extern "C" void wwhd_pack_trace(Cpu* c, uint32_t addr) {
    static std::vector<uint32_t> runs((kCodeCaveEnd - kCodeCave) / 4), reported((kCodeCaveEnd - kCodeCave) / 4);
    static auto last = std::chrono::steady_clock::now();
    uint32_t i = (addr - kCodeCave) / 4;
    if (i >= runs.size()) return;
    runs[i]++;
    // WWHD_PACK_TRACE_AT=addr,addr...: registers at those blocks (the first 5 runs, then every 150th)
    static const std::vector<uint32_t> at = [] {
        std::vector<uint32_t> v;
        if (const char* e = getenv("WWHD_PACK_TRACE_AT"))
            for (const char* p = e; *p;) {
                char* q;
                v.push_back((uint32_t)strtoul(p, &q, 16));
                p = *q ? q + 1 : q;
            }
        return v;
    }();
    if (std::find(at.begin(), at.end(), addr) != at.end() && (runs[i] <= 5 || runs[i] % 150 == 0)) {
        std::string m;
        char b[24];
        for (int r = 0; r < 32; r++)
            if (r <= 12 || r >= 28) snprintf(b, sizeof b, " r%d=%08X", r, c->r[r]), m += b;
        LOG("[packs] at %08X #%u:%s lr=%08X", addr, runs[i], m.c_str(), c->lr);
    }
    auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(5)) return;
    last = now;
    std::string line;
    for (size_t k = 0; k < runs.size(); k++) {
        if (runs[k] == reported[k]) continue;
        char b[32];
        snprintf(b, sizeof b, " %08X+%u", kCodeCave + 4 * (uint32_t)k, runs[k] - reported[k]);
        line += b;
        reported[k] = runs[k];
        if (line.size() > 900) {
            LOG("[packs] trace%s", line.c_str());
            line.clear();
        }
    }
    if (!line.empty()) LOG("[packs] trace%s", line.c_str());
}

const std::vector<Status>& status() { return g_status; }

std::string content_file(const std::string& rel) {
    if (g_contentFiles.empty()) return "";
    auto it = g_contentFiles.find(lower(rel));
    if (it == g_contentFiles.end()) return "";
    static std::mutex mu;
    static std::set<std::string> logged;
    std::lock_guard<std::mutex> lk(mu);
    if (logged.insert(it->first).second) LOG("[packs] game file %s from %s", rel.c_str(), it->second.c_str());
    return it->second;
}
const TextureOverride* texture_override(uint32_t format, uint32_t tileMode, uint32_t width, uint32_t height, uint32_t depth,
                                        uint32_t addr) {
    if (g_textureRules.empty()) return nullptr;
    const bool mem1 = addr >= 0xF4000000u && addr < 0xF6000000u;
    const uint64_t key = (uint64_t)format | (uint64_t)tileMode << 12 | (uint64_t)width << 17 | (uint64_t)height << 31 |
                         (uint64_t)std::min<uint32_t>(depth, 255) << 45 | (uint64_t)mem1 << 53;
    thread_local std::unordered_map<uint64_t, std::pair<bool, TextureOverride>> cache;
    auto it = cache.find(key);
    if (it != cache.end()) return it->second.first ? &it->second.second : nullptr;
    TextureOverride o;
    bool any = false;
    auto in = [](const std::vector<int>& v, uint32_t x) { return std::find(v.begin(), v.end(), (int)x) != v.end(); };
    for (const gpack::TextureRule& r : g_textureRules) {
        if (!r.formats.empty() && !in(r.formats, format)) continue;
        if (in(r.formatsExcluded, format)) continue;
        if (!r.tileModes.empty() && !in(r.tileModes, tileMode)) continue;
        if (in(r.tileModesExcluded, tileMode)) continue;
        if (r.width != -1 && (uint32_t)r.width != width) continue;
        if (r.height != -1 && (uint32_t)r.height != height) continue;
        if (r.depth != -1 && (uint32_t)r.depth != depth) continue;
        if ((r.inMem1 == 1 && !mem1) || (r.inMem1 == 2 && mem1)) continue;
        if (r.lodBias != -1) o.hasLodBias = true, o.lodBias = r.lodBias, any = true;
        if (r.relativeLodBias != -1) o.hasRelativeLodBias = true, o.relativeLodBias = r.relativeLodBias, any = true;
        if (r.anisotropy != -1) o.anisotropy = r.anisotropy, any = true;
    }
    auto& e = cache[key] = {any, o};
    return any ? &e.second : nullptr;
}

const std::string* custom_shader(uint64_t baseHash, uint64_t auxHash, char type) {
    if (g_customShaders.empty()) return nullptr;
    auto it = g_customShaders.find(ShaderKey{baseHash, auxHash, type});
    if (it == g_customShaders.end()) return nullptr;
    static std::mutex mu;
    static std::set<const std::string*> logged;  // once per replacement (the renderer translates a shader per state variant)
    std::lock_guard<std::mutex> lk(mu);
    if (logged.insert(&it->second).second)
        LOG("[packs] shader %016llx_%016llx_%cs replaced", (unsigned long long)baseHash, (unsigned long long)auxHash, type);
    return &it->second;
}

std::string content_dir(const std::string& rel) {
    if (g_contentDirs.empty()) return "";
    auto it = g_contentDirs.find(lower(rel));
    return it == g_contentDirs.end() ? "" : it->second;
}
std::vector<DirEntry> content_entries(const std::string& relDir) {
    if (g_contentEntries.empty()) return {};
    auto it = g_contentEntries.find(lower(relDir));
    return it == g_contentEntries.end() ? std::vector<DirEntry>{} : it->second;
}

// Compat: packs are made on 1080p consoles / Cemu, where the game's TV layout size (1920x1080, a
// constant it sets at start) is also the size of its TV HUD buffer. In this port the game draws its
// TV HUD into a 1280x720 buffer: scissors a pack computes from the layout size lie outside it (Minimap
// HD's island picture; the color buffer is then dropped). When pack code asks for that size, it gets
// the size of the color buffer the calling thread bound last (target 0: the HUD buffer while the HUD
// draws). The game's own calls are unchanged.
namespace {
thread_local uint32_t t_cbW = 0, t_cbH = 0;
bool from_pack(uint32_t lr) { return lr >= kCodeCave && lr < g_caveEnd; }
}  // namespace
void note_color_buffer(uint32_t w, uint32_t h) { t_cbW = w, t_cbH = h; }

}  // namespace packs

extern "C" {
void f_02739594_orig(Cpu* c);  // TV layout width as an integer (EU 02739E50)
void f_027395E4_orig(Cpu* c);  // ... height (EU 02739EA0)
}
extern "C" void hook_02739594(Cpu* c) {
    uint32_t lr = c->lr;
    f_02739594_orig(c);
    if (packs::from_pack(lr) && packs::t_cbW) c->r[3] = packs::t_cbW;
}
extern "C" void hook_027395E4(Cpu* c) {
    uint32_t lr = c->lr;
    f_027395E4_orig(c);
    if (packs::from_pack(lr) && packs::t_cbH) c->r[3] = packs::t_cbH;
}

namespace packs {
uint32_t cave_end() { return g_caveEnd; }

std::string describe(const std::string& packDir, const std::string& selectionText, const std::string& gameDir) {
    std::string out;
    gpack::Rules rules;
    std::string err;
    if (!gpack::parse_rules(read_file(packDir + "/rules.txt"), rules, err)) return "note\t" + err + "\n";
    auto clean = [](std::string s) {
        for (char& c : s)
            if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return s;
    };
    out += "name\t" + clean(rules.name) + "\npath\t" + clean(rules.path) + "\ndescription\t" + clean(rules.description) + "\nversion\t" +
           std::to_string(rules.version) + "\n";
    std::map<std::string, std::string> sel;
    for (auto& line : split(selectionText, '\n')) {
        auto f = split(line, '\t');
        if (f.size() >= 2) sel[f[0]] = f[1];
    }
    gpack::Resolved res = gpack::resolve(rules, sel);
    for (auto& [cat, names] : res.visible) {
        if (names.empty()) continue;
        out += "category\t" + clean(cat) + "\t" + clean(res.active[cat]);
        for (auto& n : names) out += "\t" + clean(n);
        out += "\n";
    }
    PackFiles pf = pack_files(packDir, rules);
    if (!pf.patches.empty()) {
        std::vector<uint8_t> rpx = read_bytes(gameDir + "/code/cking.rpx");
        gpack::PatchResult pr;
        bool ok = !rpx.empty() && gpack::apply_patches(pf.patches, res.vars, gpack::module_info(rpx), kCodeCave, pr);
        out += "patches\t" + std::to_string(ok ? pr.groups : 0) + "\t" + std::to_string(pr.groupsOther) + "\n";
        for (auto& e : pr.errors) out += "note\t" + clean(e) + "\n";
    }
    if (pf.pixelShaders && rules.version > 3) out += "shaders\t" + std::to_string(pf.pixelShaders) + "\n";
    {
        size_t n = 0;
        for (auto& r : gpack::texture_rules(read_file(packDir + "/rules.txt"), res.vars))
            if (r.anisotropy >= 0 || r.lodBias != -1 || r.relativeLodBias != -1) n++;
        if (n) out += "texrules\t" + std::to_string(n) + "\n";
    }
    if (pf.contentFiles) {
        // files the game doesn't have: added (a file another game version has does nothing here)
        std::vector<std::string> mine;
        std::function<void(const std::string&, const std::string&)> walk = [&](const std::string& d, const std::string& rel) {
            for (auto& e : list_dir(d)) {
                std::string r = rel.empty() ? e.name : rel + "/" + e.name;
                if (e.dir) walk(d + "/" + e.name, r);
                else mine.push_back(r);
            }
        };
        for (auto& e : list_dir(packDir))
            if (e.dir && !strcasecmp(e.name.c_str(), "content")) walk(packDir + "/" + e.name, "");
        std::vector<std::string> added;
        for (auto& r : mine) {
            std::string p = gameDir + "/content/" + r;
            struct stat st;
            bool found = stat(p.c_str(), &st) == 0;
            if (!found) {  // the game's names ignore case
                std::string dir = gameDir + "/content", rest = r;
                found = true;
                while (found && !rest.empty()) {
                    size_t s = rest.find('/');
                    std::string part = rest.substr(0, s);
                    rest = s == std::string::npos ? "" : rest.substr(s + 1);
                    std::string hit;
                    for (auto& e : list_dir(dir))
                        if (!strcasecmp(e.name.c_str(), part.c_str())) hit = e.name;
                    found = !hit.empty();
                    dir += "/" + hit;
                }
            }
            if (!found) added.push_back(r);
        }
        out += "content\t" + std::to_string(pf.contentFiles) + "\t" + std::to_string(added.size()) + "\n";
        for (size_t i = 0; i < added.size() && i < 5; i++) out += "added\t" + clean(added[i]) + "\n";
    }
    // made for another game version (rules.txt titleIds; USA 0005000010143500, EUR ...600, JPN ...400)
    if (!rules.titleIds.empty()) {
        std::vector<uint8_t> rpx = read_bytes(gameDir + "/code/cking.rpx");
        uint32_t crc = gpack::module_crc(rpx);
        uint64_t mine = crc == 0x475BD29F ? 0x0005000010143500ull : crc == 0xB7E748DE ? 0x0005000010143600ull : crc == 0x74BD3F6A ? 0x0005000010143400ull : 0;
        if (mine && std::find(rules.titleIds.begin(), rules.titleIds.end(), mine) == rules.titleIds.end()) {
            std::string v;
            for (uint64_t t : rules.titleIds) {
                const char* n = t == 0x0005000010143500ull ? "USA" : t == 0x0005000010143600ull ? "EUR" : t == 0x0005000010143400ull ? "JPN" : "?";
                if (v.find(n) == std::string::npos) v += (v.empty() ? "" : ", ") + std::string(n);
            }
            out += "versions\t" + v + "\n";
        }
    }
    for (auto& u : pf.unsupported) out += "unsupported\t" + clean(u) + "\n";
    return out;
}

}  // namespace packs
