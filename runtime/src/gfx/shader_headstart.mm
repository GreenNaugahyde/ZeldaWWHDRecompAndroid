// Shader head start: shaders pre-translated from the game's own shader archives (tools/shaderprep.py
// builds game/shadercache/headstart.bin from the game files and a template of recorded render
// states). Records use the runtime cache's recipe format:
//   type 1 ("known"): programs and states seen in play; translated at startup, compiled on first use or
//                     gradually in the background (like the user cache)
//   type 2 (pipeline): pipeline recipes seen in play, referencing type 1 shaders by key; built in the
//                     background once their shaders are compiled (like the user cache's)
//   type 3 (speculative): other archive programs with states of the same shader family; only
//                     `wwhd --warm-shaders` compiles them, to fill the macOS Metal shader cache
// `wwhd --warm-shaders` compiles all three kinds.
// WWHD_HEADSTART=<file> overrides the location, WWHD_HEADSTART=0 disables it.
#include <chrono>
#include <thread>
#include <unordered_map>
#include <vector>

#include <zlib.h>

#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "metal.h"
#include "runtime.h"

namespace gfx {

bool headstart_translate(const uint32_t* regs, bool vertex, bool compileNow);  // metal_draw.mm
size_t headstart_compiling();                                                   // metal_draw.mm
bool headstart_queue_pipeline(const uint8_t* raw, size_t size);                 // metal_draw.mm
size_t headstart_build_pipelines(int maxInFlight, size_t& built, size_t& dropped);  // metal_draw.mm

namespace {
constexpr uint32_t kRecShader = 1, kRecPipeline = 2, kRecSpeculative = 3;

std::string headstart_path() {
    if (const char* e = getenv("WWHD_HEADSTART")) return e;
    return config::game_dir + "/shadercache/headstart.bin";
}

// guest copies of programs and fetch shaders, shared by all records that use them
uint32_t guest_copy(std::unordered_map<std::string, uint32_t>& pool, const uint8_t* p, uint32_t size) {
    std::string key((const char*)p, size);
    auto it = pool.find(key);
    if (it != pool.end()) return it->second;
    uint32_t addr = mem::host_alloc(size, 0x100);
    memcpy(mem::ptr(addr), p, size);
    pool.emplace(std::move(key), addr);
    return addr;
}

struct Stats {
    size_t records = 0, translated = 0, failed = 0, pipelines = 0;
};

// replays the records of the requested types; returns false if there is no head start
bool replay(bool speculative, bool compileNow, Stats& st) {
    std::string path = headstart_path();
    if (path == "0") return false;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    static std::unordered_map<std::string, uint32_t> programs, fetch;
    static std::vector<uint32_t> regs(0x10000);
    uint32_t hdr[3];
    std::vector<uint8_t> comp, raw;
    while (fread(hdr, sizeof hdr, 1, f) == 1) {
        bool want = hdr[0] == kRecShader || hdr[0] == kRecPipeline || (speculative && hdr[0] == kRecSpeculative);
        if (!want) {
            fseek(f, hdr[2], SEEK_CUR);
            continue;
        }
        comp.resize(hdr[2]);
        raw.resize(hdr[1]);
        if (fread(comp.data(), 1, hdr[2], f) != hdr[2]) break;
        uLongf rsize = hdr[1];
        if (uncompress(raw.data(), &rsize, comp.data(), hdr[2]) != Z_OK || rsize != hdr[1] || rsize < 16) break;
        if (hdr[0] == kRecPipeline) {
            st.pipelines += headstart_queue_pipeline(raw.data(), raw.size());
            continue;
        }
        uint32_t vertex, size, fsSize, n;
        memcpy(&vertex, &raw[0], 4);
        memcpy(&size, &raw[4], 4);
        memcpy(&fsSize, &raw[8], 4);
        memcpy(&n, &raw[12], 4);
        if (16 + size + fsSize + (size_t)n * 8 > raw.size()) break;
        const uint8_t* p = raw.data() + 16;
        std::fill(regs.begin(), regs.end(), 0);
        const uint8_t* rp = p + size + fsSize;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t r, v;
            memcpy(&r, rp + i * 8, 4);
            memcpy(&v, rp + i * 8 + 4, 4);
            if (r < regs.size()) regs[r] = v;
        }
        regs[vertex ? mmSQ_PGM_START_VS : mmSQ_PGM_START_PS] = guest_copy(programs, p, size) >> 8;
        if (fsSize) regs[mmSQ_PGM_START_FS] = guest_copy(fetch, p + size, fsSize) >> 8;
        st.records++;
        if (headstart_translate(regs.data(), vertex != 0, compileNow)) st.translated++;
        else st.failed++;
        // warming: keep the number of Metal compiles in flight bounded
        if (compileNow && st.records % 64 == 0)
            while (headstart_compiling() > 128) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    fclose(f);
    return true;
}

double ms_since(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
}  // namespace

// called from cache_load (render thread, before the first draw): known shaders only
void headstart_load() {
    auto t0 = std::chrono::steady_clock::now();
    Stats st;
    if (replay(false, false, st))
        LOG("[gfx] shader head start: %zu records, %zu translated, %zu failed, %zu pipelines queued (%.0f ms) from %s",
            st.records, st.translated, st.failed, st.pipelines, ms_since(t0), headstart_path().c_str());
}

// `wwhd --warm-shaders`: translate and compile every head-start shader and pipeline once, so the macOS
// Metal shader cache holds the compiled code before the game first asks for it. Returns when done.
int headstart_warm() {
    auto t0 = std::chrono::steady_clock::now();
    Stats st;
    if (!replay(true, true, st)) {
        LOG("[warm] no shader head start at %s (build it with tools/shaderprep.py build)", headstart_path().c_str());
        return 1;
    }
    LOG("[warm] %zu records translated (%zu failed) in %.0f ms, compiling...", st.translated, st.failed, ms_since(t0));
    for (size_t left; (left = headstart_compiling()) > 0;) {
        static auto last = std::chrono::steady_clock::now();
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
            last = std::chrono::steady_clock::now();
            LOG("[warm] %zu compiles pending", left);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    double tShaders = ms_since(t0);
    // pipelines: their shaders are all compiled (or failed) now
    size_t built = 0, dropped = 0;
    auto last = std::chrono::steady_clock::now();
    while (headstart_build_pipelines(128, built, dropped) > 0 || headstart_compiling() > 0) {
        if (std::chrono::steady_clock::now() - last > std::chrono::seconds(5)) {
            last = std::chrono::steady_clock::now();
            LOG("[warm] %zu of %zu pipelines started, %zu compiles pending", built, st.pipelines, headstart_compiling());
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    LOG("[warm] %zu pipelines built (%zu unresolved) in %.1f s", built, dropped, (ms_since(t0) - tShaders) / 1000.0);
    LOG("[warm] done in %.1f s", ms_since(t0) / 1000.0);
    return 0;
}

}  // namespace gfx

int gfx_headstart_warm() { return gfx::headstart_warm(); }  // main.cpp: --warm-shaders
