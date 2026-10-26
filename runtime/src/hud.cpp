// Hidden HUD elements (see hud.h).
#include "hud.h"

#include <cmath>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "runtime.h"

namespace {
constexpr uint32_t kPaneName = 0x80;  // nw::lyt::Pane name (aspect.cpp)
std::mutex g_mu;
// the list as the game thread reads it: replaced whole (the old one is kept: changes come from the
// settings, a handful per session), so the per-pane check takes no lock; looked up by name (the
// names share prefixes: comparing each cost ~0.5% of the game thread with a list set)
struct Names {
    std::vector<std::string> list;
    std::unordered_set<std::string_view> set;  // (views into list) names of up to 24 characters
};
std::atomic<const Names*> g_names{nullptr};
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

namespace {
std::atomic<int64_t> g_rupees_ms{0};
}
void hud_rupees_drawn() { g_rupees_ms = now_ms(); }
float hud_rupee_twinkle_alpha() {
    if (now_ms() - g_rupees_ms.load() > 250) return 0.0f;  // the counter wasn't drawn (faded out, hidden)
    return g_fade[kFadeRupees].autoOn.load() ? g_fade[kFadeRupees].alpha : 1.0f;
}

void hud_set_hidden(const std::string& names) {
    std::vector<std::string> v;
    parse(names, v);
    if (const char* e = getenv("WWHD_HUD_HIDE")) parse(e, v);
    std::lock_guard<std::mutex> lk(g_mu);
    g_any = !v.empty();
    auto* n = new Names{std::move(v), {}};
    for (const std::string& h : n->list)
        if (h.size() <= 24) n->set.insert(std::string_view(h));
    g_names.store(n, std::memory_order_release);
}

namespace {
constexpr uint32_t kPaneFlags = 0x44, kPaneAlpha = 0x45, kChildren = 0x14;  // aspect.cpp
const char* const kClusterNames[kClusterCount] = {"N_X_00", "N_Y_00", "N_R_00", "P_B_00", "P_ZR_00"};
std::atomic<int> g_cluster[kClusterCount] = {-1, -1, -1, -1, -1};
std::atomic<int> g_command[3] = {-1, -1, -1};
const char* const kCommandNames[3] = {"T_CommandX_00", "T_CommandY_00", "T_CommandR_00"};
// Two layouts can hold the button cluster at once (seen after loading a state: a second copy with the item icons at
// alpha 0 drawn in every frame besides the HUD's); following the last drawn copy switched the touch buttons on and off
// each frame. The copies drawn in the last moments count, the most visible one wins.
struct ClusterCopy { uint32_t pane = 0; int alpha = -1, command = -1; int64_t ms = 0; };
std::mutex g_cluster_mu;
ClusterCopy g_copies[kClusterCount][4];
void note_cluster(int which, uint32_t pane, int alpha, int command);

// a pane's own alpha, 0 when invisible
int pane_alpha(uint32_t p) { return (ld8(p + kPaneFlags) & 1) ? ld8(p + kPaneAlpha) : 0; }

void note_cluster(int which, uint32_t pane, int alpha, int command) {
    std::lock_guard<std::mutex> lk(g_cluster_mu);
    int64_t t = now_ms();
    ClusterCopy* copies = g_copies[which];
    ClusterCopy* slot = nullptr;
    for (int k = 0; k < 4 && !slot; k++)
        if (copies[k].pane == pane) slot = &copies[k];
    for (int k = 0; k < 4 && !slot; k++)
        if (!copies[k].pane || t - copies[k].ms > 250) slot = &copies[k];
    if (!slot) slot = &copies[0];
    *slot = {pane, alpha, command, t};
    const ClusterCopy* best = slot;
    for (int k = 0; k < 4; k++)
        if (copies[k].pane && t - copies[k].ms <= 250 && copies[k].alpha > best->alpha) best = &copies[k];
    g_cluster[which].store(best->alpha, std::memory_order_relaxed);
    if (which < 3) g_command[which].store(best->command, std::memory_order_relaxed);
}

// logs a cluster pane and its children (two levels) when any of their alphas changed
void log_cluster(int which, uint32_t pane) {
    static const bool log = getenv("WWHD_TOUCHSTATE_LOG") != nullptr;
    if (!log) return;
    static std::string last[kClusterCount];
    std::string s = std::to_string(pane_alpha(pane));
    std::function<void(uint32_t, int)> walk = [&](uint32_t p, int depth) {
        uint32_t sentinel = p + kChildren;
        int k = 0;
        for (uint32_t n = ld32(sentinel); n && n != sentinel && k < 24; n = ld32(n), k++) {
            uint32_t c = n;  // the list link is at the pane's start (aspect.cpp walks it the same way)
            s += " ";
            s += std::string((const char*)mem::ptr(c + kPaneName), strnlen((const char*)mem::ptr(c + kPaneName), 24));
            s += "=" + std::to_string(pane_alpha(c));
            if (depth < 2) {
                s += "{";
                walk(c, depth + 1);
                s += "}";
            }
        }
    };
    walk(pane, 1);
    if (s == last[which]) return;
    last[which] = s;
    LOG("[hud] %s %08X %s", kClusterNames[which], pane, s.c_str());  // (two copies: two addresses)
}
}  // namespace

int hud_cluster_alpha(int which) { return g_cluster[which].load(std::memory_order_relaxed); }
int hud_cluster_command(int which) { return g_command[which].load(std::memory_order_relaxed); }

namespace {
// a descendant by name (depth first, a few levels), or 0
uint32_t find_pane(uint32_t p, const char* name, int depth) {
    uint32_t sentinel = p + kChildren;
    int k = 0;
    for (uint32_t n = ld32(sentinel); n && n != sentinel && k < 24; n = ld32(n), k++) {
        if (!strncmp((const char*)mem::ptr(n + kPaneName), name, 24)) return n;
        if (depth > 1)
            if (uint32_t f = find_pane(n, name, depth - 1)) return f;
    }
    return 0;
}
}  // namespace

namespace {
// the item buttons' letters (T_X_00 / T_Y_00 / T_R_00 on P_X_00 ... in CommandGuide_00, the HUD, and IconSlideArea_00,
// the GamePad's drop slots): the app's labels (hud_set_slot_labels), else the game's own. nw::lyt::TextBox: the text
// buffer (char16) at +0xA4, its size in characters +0xCC (7 for these), the text's length +0xCE; as SetString (USA
// 02877850) does, bit 0x20 of +0xF4 then has the drawn glyphs rebuilt
constexpr uint32_t kPaneParent = 0x0C, kBoxSize = 0x3C, kFontSize = 0xB8, kTextBuf = 0xA4, kTextBufLen = 0xCC, kTextLen = 0xCE, kTextFlags = 0xF4;
constexpr uint8_t kTextDirty = 0x20;
std::mutex g_label_mu;
std::u16string g_label[3] = {u"X", u"Y", u"R"};
std::atomic<bool> g_labels_set{false};

void slot_label(uint32_t pane, const char* n) {
    if (!g_labels_set.load(std::memory_order_relaxed) || n[4] != '0' || n[5] != '0' || n[6]) return;
    int i = n[2] == 'X' ? 0 : n[2] == 'Y' ? 1 : n[2] == 'R' ? 2 : -1;
    if (i < 0) return;
    uint32_t parent = ld32(pane + kPaneParent);
    const char* pn = parent ? (const char*)mem::ptr(parent + kPaneName) : "";
    if (pn[0] != 'P' || pn[1] != '_' || pn[2] != n[2] || pn[3] != '_') return;  // (only on the button's picture)
    std::u16string want;
    {
        std::lock_guard<std::mutex> lk(g_label_mu);
        want = g_label[i];
    }
    uint32_t buf = ld32(pane + kTextBuf);
    if (!buf || want.size() >= ld16(pane + kTextBufLen)) return;
    bool same = ld16(pane + kTextLen) == want.size();
    for (size_t k = 0; k < want.size() && same; k++) same = ld16(buf + 2 * k) == want[k];
    if (same) return;
    for (size_t k = 0; k < want.size(); k++) st16(buf + 2 * k, want[k]);
    st16(buf + 2 * want.size(), 0);
    st16(pane + kTextLen, (uint16_t)want.size());
    // one letter: the layout's font size (31 x 38) and box (30 wide); more: smaller, in a wider box (it would wrap)
    const float k = want.size() == 1 ? 1.0f : 0.62f;
    stf32(pane + kFontSize, 31.0f * k);
    stf32(pane + kFontSize + 4, 38.0f * k);
    stf32(pane + kBoxSize, std::max(30.0f, 31.0f * k * want.size() + 6.0f));
    st8(pane + kTextFlags, ld8(pane + kTextFlags) | kTextDirty);
}
}  // namespace

void hud_set_slot_labels(const std::string& csv) {
    std::vector<std::string> v;
    parse(csv, v);
    std::lock_guard<std::mutex> lk(g_label_mu);
    static const char16_t* const kGame[3] = {u"X", u"Y", u"R"};
    for (int i = 0; i < 3; i++) {
        g_label[i] = kGame[i];
        if (i < (int)v.size() && !v[i].empty() && v[i].size() < 7) g_label[i] = std::u16string(v[i].begin(), v[i].end());
    }
    g_labels_set = true;  // (from now on: also back to the game's letters)
    LOG("[hud] item button letters %s", csv.empty() ? "X,Y,R" : csv.c_str());
}

namespace {
// the zoom bar: N_Zoom_00 (its first child tells the layout) and the handle N_Scale_00, as last drawn
// (read from the bar: with the large Picto Box view the bar is hidden, its handle not drawn)
constexpr uint32_t kPaneGlobal = 0x48;
std::atomic<int> g_zoom_y{0}, g_zoom_x{0}, g_zoom_kind{0}, g_zoom_handle{0};
std::atomic<int64_t> g_zoom_ms{0};
std::atomic<int64_t> g_finder_ms{0};  // PictographBox_00's N_Finder_00 last drawn
std::atomic<int64_t> g_album_ms{0};   // the picto album's N_PictographAll_00 last drawn
float global_x(uint32_t p) { return ldf32(p + kPaneGlobal + 0xC); }
float global_y(uint32_t p) { return ldf32(p + kPaneGlobal + 0x1C); }
void note_zoom(uint32_t pane, const char* n) {
    if (!strncmp(n, "N_Finder_00", 24)) {
        g_finder_ms = now_ms();
    } else if (!strncmp(n, "N_PictographAll_00", 24)) {
        g_album_ms = now_ms();
    } else if (!strncmp(n, "N_Zoom_00", 24)) {
        uint32_t first = ld32(pane + kChildren);
        if (!first || first == pane + kChildren) return;
        const char* c = (const char*)mem::ptr(first + kPaneName);
        int kind = !strncmp(c, "P_Bar_00", 24) ? 1 : !strncmp(c, "P_TelescopeBar_00", 24) ? 2 : 0;
        if (!kind) return;
        g_zoom_kind = kind;
        g_zoom_x = (int)global_x(pane);
        g_zoom_y = (int)global_y(pane);
        uint32_t handle = find_pane(pane, "N_Scale_00", 1);
        if (!handle || !(ld8(pane + kPaneFlags) & 1) || !(ld8(handle + kPaneFlags) & 1)) return;  // the game hides it
        g_zoom_handle = (int)global_y(handle);
        g_zoom_ms = now_ms();
    }
}
}  // namespace

namespace {
// the message window's choices (W_Choise2_00 / W_Choise3_00 in N_SubWindow_00) and their cursor
// (W_Cursor_00), as last drawn
std::atomic<int> g_choice_n{0}, g_choice_x{0}, g_choice_y{0}, g_cursor_y{0};
std::atomic<int64_t> g_choice_ms{0}, g_cursor_ms{0};
std::atomic<int64_t> g_hide_choice_until{0};  // hud_hide_choice
std::atomic<int64_t> g_key_icon_ms{0};  // the message's "go on" icon (L_KeyIcon_00, under the text)
std::atomic<int64_t> g_number_ms{0};  // P_SelNumber_00 (the message's number entry: the auction's bid) shown
// the message window's box (P_MsgWindow_00 of MsgWindowMain_00: 900 x 280, normally centred 200 below the middle),
// as last drawn: its centre and half size (the global matrix's scale), layout units
std::atomic<int> g_msgwin_x{0}, g_msgwin_y{0}, g_msgwin_hw{0}, g_msgwin_hh{0};
std::atomic<int64_t> g_msgwin_ms{0};
// the wind compass after the Wind's Requiem (ControlWind_00): its centre (P_WindDirectionBase_00), as last drawn
std::atomic<int> g_wind_x{0}, g_wind_y{0};
std::atomic<int64_t> g_wind_ms{0};
constexpr uint32_t kPaneSize = 0x3C;
void note_choice(uint32_t pane, const char* n) {
    if (n[0] == 'P') {
        if (!strncmp(n, "P_WindDirectionBase_00", 24) && pane_alpha(pane)) {
            g_wind_x = (int)global_x(pane);
            g_wind_y = (int)global_y(pane);
            g_wind_ms = now_ms();
        }
        if (!strncmp(n, "P_SelNumber_00", 24) && pane_alpha(pane)) g_number_ms = now_ms();
        if (!strncmp(n, "P_MsgWindow_00", 24) && pane_alpha(pane)) {
            const uint32_t m = pane + kPaneGlobal;
            g_msgwin_x = (int)global_x(pane);
            g_msgwin_y = (int)global_y(pane);
            g_msgwin_hw = (int)(ldf32(pane + kPaneSize) * std::hypot((float)ldf32(m), (float)ldf32(m + 0x10)) / 2);
            g_msgwin_hh = (int)(ldf32(pane + kPaneSize + 4) * std::hypot((float)ldf32(m + 4), (float)ldf32(m + 0x14)) / 2);
            g_msgwin_ms = now_ms();
        }
        return;
    }
    if (!strncmp(n, "L_KeyIcon_00", 24)) {
        if (pane_alpha(pane)) g_key_icon_ms = now_ms();
    } else if (!strncmp(n, "W_Cursor_00", 24)) {
        if (!pane_alpha(pane)) return;
        g_cursor_y = (int)global_y(pane);
        g_cursor_ms = now_ms();
    } else if (!strncmp(n, "W_Choise2_00", 24) || !strncmp(n, "W_Choise3_00", 24)) {
        if (!pane_alpha(pane)) return;
        g_choice_n = n[8] - '0';
        g_choice_x = (int)global_x(pane);
        g_choice_y = (int)global_y(pane);
        g_choice_ms = now_ms();
    }
}
}  // namespace

void hud_hide_choice(int ms) { g_hide_choice_until = ms > 0 ? now_ms() + ms : 0; }

bool hud_msg_key_icon() { return now_ms() - g_key_icon_ms.load() < 250; }
int64_t hud_msg_key_icon_age() { return now_ms() - g_key_icon_ms.load(); }

bool hud_number_entry() { return now_ms() - g_number_ms.load() < 250; }

void hud_msg_window(int32_t out[5]) {
    bool on = now_ms() - g_msgwin_ms.load() < 250;
    out[0] = on;
    out[1] = on ? g_msgwin_x.load() : 0;
    out[2] = on ? g_msgwin_y.load() : 0;
    out[3] = on ? g_msgwin_hw.load() : 0;
    out[4] = on ? g_msgwin_hh.load() : 0;
}

bool hud_wind_compass(int32_t out[2]) {
    if (now_ms() - g_wind_ms.load() >= 250) return false;
    out[0] = g_wind_x.load();
    out[1] = g_wind_y.load();
    return true;
}

void hud_choice(int32_t out[5]) {
    for (int i = 0; i < 5; i++) out[i] = 0;
    int64_t t = now_ms();
    if (t - g_choice_ms.load() > 250 || t - g_cursor_ms.load() > 250) return;
    out[0] = 1;
    out[1] = g_choice_n;
    out[2] = g_choice_x;
    out[3] = g_choice_y;
    out[4] = g_cursor_y;
}

bool hud_picto_screen() { return now_ms() - g_finder_ms.load(std::memory_order_relaxed) < 250; }
bool hud_album_screen() { return now_ms() - g_album_ms.load(std::memory_order_relaxed) < 250; }

// a system message with its one centred button (N_Msg_00 + L_BtnSeqenceC_00 drawn): on the very first start "save data
// created" with OK (the app confirms it there, MainActivity.firstStartMessage)
static std::atomic<int64_t> g_boot_base_ms{0}, g_boot_btn_ms{0};
bool hud_boot_message() {
    int64_t t = now_ms();
    return t - g_boot_base_ms.load() < 250 && t - g_boot_btn_ms.load() < 250;
}

void hud_zoom_bar(int32_t out[6]) {
    for (int i = 0; i < 6; i++) out[i] = 0;
    int kind = g_zoom_kind;
    if (!kind || now_ms() - g_zoom_ms.load() > 250) return;
    // the handle's path relative to N_Zoom_00 (the Scale animations, PictographBox_00 / Telescope_00):
    // Picto Box y -148..140 at x -14..5; telescope y -208..232 at x -120..-34, its number further left
    int x = g_zoom_x, y = g_zoom_y;
    out[0] = 1;
    out[1] = g_zoom_handle;
    out[2] = y + (kind == 1 ? -148 : -208);
    out[3] = y + (kind == 1 ? 140 : 232);
    out[4] = x + (kind == 1 ? -170 : -260);
    out[5] = x + (kind == 1 ? 70 : 20);
}

bool hud_hidden(uint32_t pane) {
    // WWHD_HUD_HIDE alone (before the app set a list): applied once
    static bool init = [] {
        if (getenv("WWHD_HUD_HIDE") && !g_any) hud_set_hidden("");
        if (const char* e = getenv("WWHD_SLOT_LABELS")) hud_set_slot_labels(e);  // debug: "X,Y,R" order
        return true;
    }();
    (void)init;
    const char* n = (const char*)mem::ptr(pane + kPaneName);
    if (n[0] == 'N' && strncmp(n, "N_Msg_00", 9) == 0) g_boot_base_ms = now_ms();
    else if (n[0] == 'L' && strncmp(n, "L_BtnSeqenceC_00", 17) == 0) g_boot_btn_ms = now_ms();
    static const bool pane_log = getenv("WWHD_PANE_LOG") != nullptr;  // debug: each new pane name, where
    if (pane_log) {
        static std::unordered_set<std::string> seen;
        std::string name(n, strnlen(n, 24));
        if (seen.insert(name).second) LOG("[pane] %s at %.0f %.0f", name.c_str(), global_x(pane), global_y(pane));
    }
    if (n[0] == 'T' && n[1] == '_' && n[3] == '_') slot_label(pane, n);
    if (n[0] == 'N') note_zoom(pane, n);
    if (n[0] == 'W' || n[0] == 'P' || n[0] == 'L') note_choice(pane, n);
    // the choice box kept out of sight (hud_hide_choice): noted as drawn (hud_choice) but not drawn; its parts are
    // looked up under N_SubWindow_00 (not drawn either, so not reached by this hook)
    if (now_ms() < g_hide_choice_until.load(std::memory_order_relaxed)) {
        if (n[0] == 'N' && !strncmp(n, "N_SubWindow_00", 24)) {
            for (const char* part : {"W_Choise2_00", "W_Choise3_00", "W_Cursor_00"})
                if (uint32_t f = find_pane(pane, part, 4)) note_choice(f, part);
            return true;
        }
        if (n[0] == 'W' && (!strncmp(n, "W_Cursor_00", 24) || !strncmp(n, "W_Choise", 8))) return true;
    }
    if (n[0] == 'N' || n[0] == 'P')
        for (int i = 0; i < kClusterCount; i++)
            if (!strncmp(n, kClusterNames[i], 24)) {
                uint32_t cmd = i < 3 ? find_pane(pane, kCommandNames[i], 2) : 0;
                note_cluster(i, pane, pane_alpha(pane), cmd ? pane_alpha(cmd) : -1);
                log_cluster(i, pane);
                break;
            }
    if (!g_any.load(std::memory_order_relaxed)) return false;
    const Names* names = g_names.load(std::memory_order_acquire);
    if (!names) return false;
    // (as strncmp over 24 characters: the name up to its end or 24 characters)
    return names->set.count(std::string_view(n, strnlen(n, 24))) != 0;
}
