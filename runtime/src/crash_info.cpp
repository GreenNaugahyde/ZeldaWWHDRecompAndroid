// Prepared crash log context (see crash_info.h).
#include "crash_info.h"

#include <atomic>
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

const char* text() {
    int c = g_cur.load();
    return c < 0 ? "" : g_buf[c];
}

}  // namespace crash_info
