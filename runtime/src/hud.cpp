// Hidden HUD elements (see hud.h).
#include "hud.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "runtime.h"

namespace {
constexpr uint32_t kPaneName = 0x80;  // nw::lyt::Pane name (aspect.cpp)
std::mutex g_mu;
std::vector<std::string> g_names;
std::atomic<bool> g_any{false};

void parse(const std::string& s, std::vector<std::string>& out) {
    size_t p = 0;
    while (p < s.size()) {
        size_t e = s.find(',', p);
        if (e == std::string::npos) e = s.size();
        if (e > p) out.push_back(s.substr(p, e - p));
        p = e + 1;
    }
}

std::atomic<int64_t> g_menu_ms{0};  // when the pause menu's layout was last computed
int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
}  // namespace

void hud_root_drawn(uint32_t root) {
    constexpr uint32_t kChildren = 0x14;  // child list sentinel (aspect.cpp)
    uint32_t sentinel = root + kChildren;
    int k = 0;
    for (uint32_t n = ld32(sentinel); n && n != sentinel && k < 16; n = ld32(n), k++)
        if (!strncmp((const char*)mem::ptr(n + kPaneName), "L_MenuArrowL_00", 24)) {
            g_menu_ms = now_ms();
            return;
        }
}

bool hud_menu_open() {
    bool open = now_ms() - g_menu_ms.load() < 250;
    static bool was = false;
    if (open != was) {
        was = open;
        LOG("[hud] pause menu layout %s", open ? "active" : "gone");
    }
    return open;
}

namespace {
struct Fade {
    std::atomic<bool> autoOn{false}, want{true};
    float alpha = 1.0f;
};
Fade g_fade[kFadeCount];
}  // namespace

void hud_set_fade_auto(int which, bool on) { g_fade[which].autoOn = on; }
void hud_set_fade_wanted(int which, bool want) { g_fade[which].want = want; }
float hud_fade_alpha(int which) {
    Fade& f = g_fade[which];
    float target = !f.autoOn.load() || f.want.load() ? 1.0f : 0.0f;
    // in quickly (~0.3 s), out slowly (~1 s), at 30 frames per second
    if (f.alpha < target) f.alpha = std::min(target, f.alpha + 1.0f / 9.0f);
    if (f.alpha > target) f.alpha = std::max(target, f.alpha - 1.0f / 30.0f);
    return f.alpha;
}

bool hud_fade_gone(int which) { return g_fade[which].autoOn.load() && g_fade[which].alpha <= 0.0f; }

void hud_set_hidden(const std::string& names) {
    std::vector<std::string> v;
    parse(names, v);
    if (const char* e = getenv("WWHD_HUD_HIDE")) parse(e, v);
    std::lock_guard<std::mutex> lk(g_mu);
    g_names.swap(v);
    g_any = !g_names.empty();
}

bool hud_hidden(uint32_t pane) {
    // WWHD_HUD_HIDE alone (before the app set a list): applied once
    static bool init = [] {
        if (getenv("WWHD_HUD_HIDE") && !g_any) hud_set_hidden("");
        return true;
    }();
    (void)init;
    if (!g_any.load(std::memory_order_relaxed)) return false;
    const char* n = (const char*)mem::ptr(pane + kPaneName);
    std::lock_guard<std::mutex> lk(g_mu);
    for (const std::string& h : g_names)
        if (!strncmp(n, h.c_str(), 24) && h.size() <= 24) return true;
    return false;
}
