// Prepared crash log context (see crash_info.h).
#include "crash_info.h"

#include <algorithm>
#include <atomic>
#include <utility>
#include <vector>

#include "recomp_table.h"
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

extern char** environ;

namespace crash_info {
namespace {

std::mutex g_mu;
std::map<std::string, std::string> g_sections;
std::map<std::string, int> g_options;
// two buffers: the crash handler reads the current one while the other is rewritten
char g_buf[2][16384];
std::atomic<int> g_cur{-1};

// "/a/b/c/" -> "c": paths say where the user keeps things; their last part is what helps
std::string last_part(std::string v) {
    while (v.size() > 1 && v.back() == '/') v.pop_back();
    size_t s = v.find_last_of('/');
    return s == std::string::npos ? v : v.substr(s + 1);
}

void rebuild() {  // with g_mu held
    std::string all;
    for (auto& [name, text] : g_sections) {
        all += "  [" + name + "]\n";
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            if (e > p) all += "    " + text.substr(p, e - p) + "\n";
            p = e + 1;
        }
    }
    if (!g_options.empty()) {
        all += "  [options set while running]\n    ";
        for (auto& [n, v] : g_options) all += n + "=" + std::to_string(v) + " ";
        all += "\n";
    }
    int next = g_cur.load() == 0 ? 1 : 0;
    size_t n = std::min(all.size(), sizeof g_buf[next] - 1);
    memcpy(g_buf[next], all.data(), n);
    g_buf[next][n] = 0;
    g_cur.store(next);
}

}  // namespace

void set(const std::string& section, const std::string& text) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_sections[section] = text;
    rebuild();
}

void option(const std::string& name, int value) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_options[name] = value;
    rebuild();
}

void capture_env() {
    std::map<std::string, std::string> vars;
    for (char** e = environ; e && *e; e++) {
        const char* s = *e;
        if (strncmp(s, "WWHD_", 5) && strncmp(s, "TU_", 3) && strncmp(s, "MESA_", 5) && strncmp(s, "FD_", 3)) continue;
        const char* eq = strchr(s, '=');
        if (!eq) continue;
        std::string v(eq + 1);
        vars[std::string(s, eq)] = v.find('/') != std::string::npos ? last_part(v) : v;
    }
    std::string text;
    for (auto& [k, v] : vars) text += k + "=" + v + "\n";
    set("env", text);
}

namespace {
std::vector<std::pair<uintptr_t, uint32_t>> g_funcs;  // (host address, guest address), sorted
std::atomic<bool> g_funcs_ready{false};
}  // namespace

void index_functions() {
    std::vector<std::pair<uintptr_t, uint32_t>> v;
    v.reserve(g_recomp_func_count);
    for (unsigned i = 0; i < g_recomp_func_count; i++)
        if (g_recomp_funcs[i].fn) v.push_back({(uintptr_t)g_recomp_funcs[i].fn, g_recomp_funcs[i].addr});
    std::sort(v.begin(), v.end());
    g_funcs = std::move(v);
    g_funcs_ready = true;
}

bool guest_function(uintptr_t pc, uint32_t* addr, uint32_t* offset) {
    if (!g_funcs_ready.load() || g_funcs.empty() || pc < g_funcs.front().first) return false;
    auto it = std::upper_bound(g_funcs.begin(), g_funcs.end(), std::make_pair(pc, UINT32_MAX));
    --it;
    // past the last function or far from its start: not game code (functions are rarely > 256 KiB)
    if (pc - it->first > 0x40000) return false;
    *addr = it->second;
    *offset = (uint32_t)(pc - it->first);
    return true;
}

const char* text() {
    int c = g_cur.load();
    return c < 0 ? "" : g_buf[c];
}

}  // namespace crash_info
