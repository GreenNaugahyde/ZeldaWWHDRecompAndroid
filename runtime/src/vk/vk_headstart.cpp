// Shader head start from a code-free template (WWHV, written by WWHD_TEMPLATE_EXPORT, see
// template_export in vk_draw.cpp). The app ships the template; the first start with it (and with
// every new template or game) finds the template's programs in the player's own game files and keeps
// them in headstart-programs.bin next to the shader cache. Every start then translates the template's
// states and queues its pipelines like the cache's own records: compiled on first use or a few per
// frame in the background. The template holds no shader code; the programs file is local only.
//
// headstart-programs.bin: "WWHP", u32 version, u64 fingerprint (template + game), u32 programs,
// u32 raw size, zlib( per program of the template (in order of first use): u32 size (0: not found), bytes )
// WWHD_HEADSTART=0 disables it.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "runtime.h"

namespace gfx {

// vk_draw.cpp
uint64_t headstart_hash(const void* data, size_t n);
bool headstart_known(uint64_t key);
uint64_t headstart_replay(uint32_t* regs, bool vertex, const uint8_t* code, uint32_t size, const uint8_t* fsBytes,
                          uint32_t fsSize, bool linked, uint32_t vsMask);
bool headstart_queue_pipeline(const uint8_t* raw, size_t size);
size_t headstart_pipeline_size();
void headstart_progress(int state, uint64_t done, uint64_t total, uint64_t shaders);

namespace {
using Bytes = std::vector<uint8_t>;
constexpr uint32_t kPackVersion = 1;

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}

bool read_file(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize(n > 0 ? (size_t)n : 0);
    bool ok = n > 0 && fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

uint32_t le32(const uint8_t* p) { uint32_t v; memcpy(&v, p, 4); return v; }
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

// ---------------------------------------------------------------- the template
struct State {
    uint64_t key, prog;
    uint32_t size, kind;  // kind: 0 pixel, 1 vertex, 2 pixel linked to a vertex shader's outputs
    int32_t fetch;
    uint32_t mask, nregs;
    const uint8_t* regs;  // (u32 reg, u32 value) pairs
    uint32_t program;     // index into Template::programs
};
struct Template {
    Bytes raw;
    std::vector<std::pair<const uint8_t*, uint32_t>> fetch;
    std::vector<State> states;
    const uint8_t* pipes = nullptr;
    uint32_t npipes = 0;
    size_t pipeSize = 0;
    struct Program { uint64_t hash; uint32_t size; bool vertex; };
    std::vector<Program> programs;  // distinct, in order of first use
};

bool read_template(const Bytes& file, Template& t) {
    if (file.size() < 12 || memcmp(file.data(), "WWHV", 4) != 0 || le32(&file[4]) != 1) return false;
    uLongf rawSize = le32(&file[8]);
    if (rawSize > (1u << 30)) return false;
    t.raw.resize(rawSize);
    if (uncompress(t.raw.data(), &rawSize, &file[12], file.size() - 12) != Z_OK || rawSize != t.raw.size()) return false;
    const uint8_t* p = t.raw.data();
    const uint8_t* end = p + t.raw.size();
    auto u32 = [&](uint32_t& v) { if (p + 4 > end) return false; v = le32(p); p += 4; return true; };
    uint32_t n;
    if (!u32(n) || n > 100000) return false;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t size;
        if (!u32(size) || size > 0x1000 || p + size > end) return false;
        t.fetch.emplace_back(p, size);
        p += size;
    }
    if (!u32(n) || n > 4000000) return false;
    std::unordered_map<uint64_t, uint32_t> programIndex;
    t.states.reserve(n);
    for (uint32_t i = 0; i < n; i++) {
        if (p + 36 > end) return false;
        State s;
        memcpy(&s.key, p, 8);
        memcpy(&s.prog, p + 8, 8);
        s.size = le32(p + 16);
        s.kind = le32(p + 20);
        memcpy(&s.fetch, p + 24, 4);
        s.mask = le32(p + 28);
        s.nregs = le32(p + 32);
        p += 36;
        if (s.kind > 2 || s.size == 0 || s.size > (1u << 20) || s.fetch >= (int32_t)t.fetch.size() ||
            (uint64_t)s.nregs * 8 > (uint64_t)(end - p))
            return false;
        s.regs = p;
        p += s.nregs * 8;
        const bool vertex = s.kind == 1;
        // programs are matched by hash, size and stage
        uint64_t id = s.prog ^ (uint64_t)s.size * 0x9E3779B97F4A7C15ull ^ (vertex ? 0x5653 : 0);
        auto it = programIndex.find(id);
        if (it == programIndex.end()) {
            it = programIndex.emplace(id, (uint32_t)t.programs.size()).first;
            t.programs.push_back({s.prog, s.size, vertex});
        }
        s.program = it->second;
        t.states.push_back(s);
    }
    if (!u32(t.npipes)) return false;
    t.pipes = p;
    t.pipeSize = t.npipes ? (size_t)(end - p) / t.npipes : 0;
    return !t.npipes || t.pipeSize * t.npipes == (size_t)(end - p);
}

// ---------------------------------------------------------------- the game's shader archives
// SHARCFB ("BAHS", AGL binary shader archive, little-endian) inside Yaz0 / SARC nesting; see
// tools/shaderprep.py for the layout. A program's microcode is stored as the game hands it to the GPU.
struct Finder {
    const Template& t;
    std::unordered_map<uint64_t, std::vector<uint32_t>> wanted;  // hash -> program indices
    std::vector<Bytes> found;
    std::vector<std::atomic<bool>> have;
    std::mutex m;
    std::atomic<uint32_t> nfound{0};

    explicit Finder(const Template& tmpl) : t(tmpl), found(tmpl.programs.size()), have(tmpl.programs.size()) {
        for (uint32_t i = 0; i < t.programs.size(); i++) wanted[t.programs[i].hash].push_back(i);
    }

    void program(bool vertex, const uint8_t* code, uint32_t size) {
        auto it = wanted.find(headstart_hash(code, size));
        if (it == wanted.end()) return;
        for (uint32_t i : it->second) {
            const auto& pr = t.programs[i];
            if (pr.size != size || pr.vertex != vertex || have[i].exchange(true)) continue;
            std::lock_guard<std::mutex> lk(m);
            found[i].assign(code, code + size);
            nfound++;
        }
    }

    void sharcfb(const uint8_t* b, size_t n) {
        if (n < 0x18) return;
        size_t o = 0x18 + (size_t)le32(b + 0x14);
        if (o + 8 > n) return;
        uint32_t count = le32(b + o + 4);
        size_t p = o + 8;
        for (uint32_t i = 0; i < count && p + 16 <= n; i++) {
            uint32_t entry = le32(b + p), type = le32(b + p + 4), dsz = le32(b + p + 12);
            if (entry < 16 || p + 16 + (size_t)dsz > n) return;
            const uint8_t* d = b + p + 16;
            p += entry;
            uint32_t szOff, ptrOff;
            if (type == 0 && dsz >= 0xDC) szOff = 0xD0, ptrOff = 0xD4;       // vertex shader
            else if (type == 1 && dsz >= 0xAC) szOff = 0xA4, ptrOff = 0xA8;  // pixel shader
            else continue;
            uint32_t size = le32(d + szOff), ptr = le32(d + ptrOff);
            if (size && (uint64_t)ptr + size <= dsz) program(type == 0, d + ptr, size);
        }
    }

    void walk(const uint8_t* b, size_t n, int depth) {
        if (depth > 6 || n < 4) return;
        if (!memcmp(b, "Yaz0", 4)) {
            if (n < 16) return;
            Bytes out(be32(b + 4));
            if (out.size() > (256u << 20)) return;
            size_t s = 16, d = 0;
            while (d < out.size() && s < n) {
                uint8_t code = b[s++];
                for (int bit = 0; bit < 8 && d < out.size(); bit++, code <<= 1) {
                    if (code & 0x80) {
                        if (s >= n) return;
                        out[d++] = b[s++];
                        continue;
                    }
                    if (s + 1 >= n) return;
                    uint32_t b1 = b[s], b2 = b[s + 1];
                    s += 2;
                    size_t dist = ((b1 & 0x0F) << 8 | b2) + 1, len = b1 >> 4;
                    if (!len) {
                        if (s >= n) return;
                        len = b[s++] + 0x12;
                    } else {
                        len += 2;
                    }
                    if (dist > d) return;
                    for (size_t i = 0; i < len && d < out.size(); i++, d++) out[d] = out[d - dist];
                }
            }
            if (d == out.size()) walk(out.data(), out.size(), depth + 1);
        } else if (!memcmp(b, "SARC", 4)) {
            if (n < 0x14) return;
            const bool big = b[6] == 0xFE && b[7] == 0xFF;
            auto r16 = [&](size_t o) -> uint32_t { return big ? be16(b + o) : (uint32_t)(b[o] | b[o + 1] << 8); };
            auto r32 = [&](size_t o) -> uint32_t { return big ? be32(b + o) : le32(b + o); };
            size_t hl = r16(4), data = r32(0xC);
            if (hl + 12 > n) return;
            uint32_t count = r16(hl + 6);
            if (hl + 12 + 16ull * count > n) return;
            for (uint32_t i = 0; i < count; i++) {
                size_t e = hl + 12 + 16 * i;
                size_t s = data + r32(e + 8), t2 = data + r32(e + 12);
                if (s <= t2 && t2 <= n) walk(b + s, t2 - s, depth + 1);
            }
        } else if (!memcmp(b, "BAHS", 4)) {
            sharcfb(b, n);
        }
    }
};

void list_archives(const std::string& dir, std::vector<std::pair<std::string, uint64_t>>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string path = dir + "/" + e->d_name;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            list_archives(path, out);
            continue;
        }
        size_t len = strlen(e->d_name);
        auto ends = [&](const char* x) { size_t k = strlen(x); return len >= k && !strcmp(e->d_name + len - k, x); };
        if (ends(".szs") || ends(".pack") || ends(".sarc") || ends(".sharcfb")) out.emplace_back(path, (uint64_t)st.st_size);
    }
    closedir(d);
}

// scans the game's archives for the template's programs; false if none of them were found
bool build_pack(const Template& t, uint64_t fingerprint, const std::string& path) {
    auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<std::string, uint64_t>> files;
    list_archives(config::game_dir + "/content", files);
    std::sort(files.begin(), files.end(), [](auto& a, auto& b) { return a.second > b.second; });  // big ones first
    uint64_t total = 0;
    for (auto& f : files) total += f.second;
    Finder finder(t);
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    headstart_progress(3, 0, total, 0);
    unsigned threads = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 4u);
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; i++)
        pool.emplace_back([&] {
            Bytes b;
            for (size_t k; (k = next++) < files.size();) {
                if (read_file(files[k].first, b)) finder.walk(b.data(), b.size(), 0);
                done += files[k].second;
                headstart_progress(3, done, total, finder.nfound);
            }
        });
    for (auto& th : pool) th.join();
    Bytes raw;
    for (auto& code : finder.found) {
        uint32_t size = (uint32_t)code.size();
        raw.insert(raw.end(), (uint8_t*)&size, (uint8_t*)&size + 4);
        raw.insert(raw.end(), code.begin(), code.end());
    }
    uLongf csize = compressBound(raw.size());
    Bytes comp(csize);
    if (compress2(comp.data(), &csize, raw.data(), raw.size(), 6) != Z_OK) return false;
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    uint32_t hdr[4] = {kPackVersion, 0, (uint32_t)finder.found.size(), (uint32_t)raw.size()};
    fwrite("WWHP", 4, 1, f);
    fwrite(&hdr[0], 4, 1, f);
    fwrite(&fingerprint, 8, 1, f);
    fwrite(&hdr[2], 4, 2, f);
    bool ok = fwrite(comp.data(), 1, csize, f) == csize;
    ok = fclose(f) == 0 && ok && rename(tmp.c_str(), path.c_str()) == 0;
    LOG("[gfx] shader head start: %u of %zu programs found in %zu game archives (%.0f MB) with %u threads, %.1f s",
        finder.nfound.load(), t.programs.size(), files.size(), total / 1e6, threads, ms_since(t0) / 1000.0);
    return ok && finder.nfound > 0;
}

bool read_pack(const std::string& path, uint64_t fingerprint, size_t programs, Bytes& raw,
               std::vector<std::pair<const uint8_t*, uint32_t>>& code) {
    Bytes file;
    if (!read_file(path, file) || file.size() < 24 || memcmp(file.data(), "WWHP", 4) != 0) return false;
    uint64_t fp;
    memcpy(&fp, &file[8], 8);
    if (le32(&file[4]) != kPackVersion || fp != fingerprint || le32(&file[16]) != programs) return false;
    uLongf rawSize = le32(&file[20]);
    raw.resize(rawSize);
    if (uncompress(raw.data(), &rawSize, &file[24], file.size() - 24) != Z_OK || rawSize != raw.size()) return false;
    code.clear();
    size_t p = 0;
    for (size_t i = 0; i < programs; i++) {
        if (p + 4 > raw.size()) return false;
        uint32_t size = le32(&raw[p]);
        p += 4;
        if (p + size > raw.size()) return false;
        code.emplace_back(&raw[p], size);
        p += size;
    }
    return true;
}

// the template and the game the programs file was made from: a new template, or another game in
// the game folder (region switch, new import), makes it again
uint64_t fingerprint(const Bytes& templateFile) {
    uint64_t h = headstart_hash(templateFile.data(), templateFile.size());
    struct stat st;
    std::string rpx = config::game_dir + "/code/cking.rpx";
    if (stat(rpx.c_str(), &st) == 0) {
        uint64_t id[2] = {(uint64_t)st.st_size, (uint64_t)st.st_mtime};
        h ^= headstart_hash(id, sizeof id) * 31;
    }
    return h ^ headstart_hash(config::game_dir.data(), config::game_dir.size()) * 37;
}
}  // namespace

// cache_load (render thread, before the first draw), after the cache's own records
void headstart_vk_load(const std::string& dir) {
    if (const char* e = getenv("WWHD_HEADSTART"); e && !strcmp(e, "0")) return;
    auto t0 = std::chrono::steady_clock::now();
    Bytes file;
    std::string packPath = dir + "/headstart-programs.bin";
    if (!read_file(dir + "/headstart-template.bin", file)) {
        remove(packPath.c_str());
        return;
    }
    Template t;
    if (!read_template(file, t)) {
        LOG("[gfx] shader head start: template unreadable");
        return;
    }
    const uint64_t fp = fingerprint(file);
    Bytes packRaw;
    std::vector<std::pair<const uint8_t*, uint32_t>> code;
    if (!read_pack(packPath, fp, t.programs.size(), packRaw, code)) {
        if (!build_pack(t, fp, packPath) || !read_pack(packPath, fp, t.programs.size(), packRaw, code)) {
            LOG("[gfx] shader head start: no programs file");
            return;
        }
    }
    const double tRead = ms_since(t0);
    // the states, like the cache's records
    static std::vector<uint32_t> regs(0x10000);
    std::vector<uint32_t> set;  // registers the previous state set (cleared for the next one)
    size_t known = 0, translated = 0, failed = 0, missing = 0, keyMismatch = 0;
    headstart_progress(1, 0, t.states.size(), 0);
    for (size_t i = 0; i < t.states.size(); i++) {
        const State& s = t.states[i];
        if ((i & 255) == 0) headstart_progress(1, i, t.states.size(), translated);
        if (headstart_known(s.key)) { known++; continue; }
        const auto& c = code[s.program];
        if (!c.second) { missing++; continue; }
        for (uint32_t r : set) regs[r] = 0;
        set.clear();
        for (uint32_t k = 0; k < s.nregs; k++) {
            uint32_t r = le32(s.regs + k * 8) & 0xFFFF;
            regs[r] = le32(s.regs + k * 8 + 4);
            set.push_back(r);
        }
        for (uint32_t r : {mmSQ_PGM_START_VS, mmSQ_PGM_START_PS, mmSQ_PGM_START_FS}) set.push_back(r);
        const uint8_t* fs = s.fetch >= 0 ? t.fetch[s.fetch].first : nullptr;
        uint32_t fsSize = s.fetch >= 0 ? t.fetch[s.fetch].second : 0;
        uint64_t key = headstart_replay(regs.data(), s.kind == 1, c.first, c.second, fs, fsSize, s.kind == 2, s.mask);
        if (!key) failed++;
        else if (key != s.key) keyMismatch++;
        else translated++;
    }
    size_t queued = 0;
    for (uint32_t i = 0; i < t.npipes; i++) queued += headstart_queue_pipeline(t.pipes + i * t.pipeSize, t.pipeSize);
    LOG("[gfx] shader head start: %zu states (%zu already cached, %zu translated, %zu failed, %zu programs missing, "
        "%zu with another key), %zu of %u pipelines queued (%.0f ms, %.0f reading)",
        t.states.size(), known, translated, failed, missing, keyMismatch, queued, t.npipes, ms_since(t0), tRead);
    if (keyMismatch > t.states.size() / 100 || (t.npipes && t.pipeSize != headstart_pipeline_size()))
        LOG("[gfx] shader head start: the template doesn't fit this build (shader keys or pipeline state changed); "
            "export a new one (WWHD_TEMPLATE_EXPORT=1)");
}

}  // namespace gfx
