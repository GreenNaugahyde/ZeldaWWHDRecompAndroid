// Online co-op compat (README.md): what upstream's co-op code asks of upstream's runtime, on this fork's.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "imgui.h"
#include "item_icons.h"
#include "link_poses.h"
#include "lyt_build.h"
#include "../hle/save_root.h"
#include "../overlay/extensions.h"
#include "../overlay/hostui.h"
#include "../platform/host.h"
#include "../randomizer_identity.h"
#include "online_compat.h"
#include "portable_restart.h"
#include "portable_state.h"

#include "gcmodel/gcm_io.h"
#include "guest_addr.h"
#include "mods/mods.h"
#include "packs.h"
#include "runtime.h"
#include "savestate.h"

// ---------------------------------------------------------------- the folders
namespace online_compat {
namespace {
std::mutex g_dir_mu;
std::string g_dir;  // the app's internal folder for online (Native.coopInit)
}  // namespace
void set_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lk(g_dir_mu);
    g_dir = dir;
    mkdir(dir.c_str(), 0700);
}
std::string dir() {
    std::lock_guard<std::mutex> lk(g_dir_mu);
    return g_dir;
}
}  // namespace online_compat

namespace host {
std::string config_dir() {
    std::lock_guard<std::mutex> lk(online_compat::g_dir_mu);
    if (!online_compat::g_dir.empty()) return online_compat::g_dir;
    // (not set: beside the caches, as the app sets it)
    const std::string& c = config::cache_dir;
    const std::string d = (c.empty() ? std::string("/data/local/tmp") : c.substr(0, c.find_last_of('/'))) + "/online";
    mkdir(d.c_str(), 0700);
    return d;
}
}  // namespace host

namespace hle_fs {
std::string save_root() { return config::save_dir; }
}  // namespace hle_fs

// ---------------------------------------------------------------- the settings (hostui)
// (fork: Android) the settings come in profiles: each game, or each save file of a game, that plays online has its own
// (online_compat::set_profile; the Online page's "Plays online"): profiles/<name>.ini holds every key but the global
// ones (the privacy notice, the saved servers), which stay in settings.ini. A new profile starts from settings.ini's
// values (the settings as they were before profiles).
namespace hostui {
namespace {
std::mutex g_mu;
bool g_loaded = false;
std::map<std::string, std::string> g_kv;   // settings.ini
std::string g_profile;                     // "" none: every key in settings.ini
std::map<std::string, std::string> g_pkv;  // the profile's keys
std::string file() { return host::config_dir() + "/settings.ini"; }
std::string profile_file() { return host::config_dir() + "/profiles/" + g_profile + ".ini"; }
bool global(const std::string& key) { return key == "online.notice_accepted" || key == "online.servers"; }
void read(const std::string& path, std::map<std::string, std::string>& kv) {
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const size_t eq = line.find('=');
        if (eq != std::string::npos) kv[line.substr(0, eq)] = line.substr(eq + 1);
    }
}
void write(const std::string& path, const std::map<std::string, std::string>& kv) {
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        for (const auto& [k, v] : kv) {
            std::string clean = v;
            std::replace(clean.begin(), clean.end(), '\n', ' ');
            f << k << '=' << clean << '\n';
        }
        if (!f) return;
    }
    rename(tmp.c_str(), path.c_str());
}
void load_locked() {
    if (g_loaded) return;
    g_loaded = true;
    read(file(), g_kv);
}
std::map<std::string, std::string>& store(const std::string& key) { return g_profile.empty() || global(key) ? g_kv : g_pkv; }
void save_locked(const std::string& key) {
    if (&store(key) == &g_kv) write(file(), g_kv);
    else write(profile_file(), g_pkv);
}
}  // namespace
void post(std::function<void()> fn) { if (fn) fn(); }
bool get(const char* key, std::string& value) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    const auto& kv = store(key);
    const auto it = kv.find(key);
    if (it == kv.end()) return false;
    value = it->second;
    return true;
}
void set(const char* key, const std::string& value) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    auto& kv = store(key);
    auto it = kv.find(key);
    if (it != kv.end() && it->second == value) return;
    kv[key] = value;
    save_locked(key);
}
// the profile the settings are read from and written to; true when it changed
bool set_profile(const std::string& name) {
    std::lock_guard<std::mutex> lk(g_mu);
    load_locked();
    if (name == g_profile) return false;
    g_profile = name;
    g_pkv.clear();
    if (name.empty()) return true;
    mkdir((host::config_dir() + "/profiles").c_str(), 0700);
    std::ifstream probe(profile_file());
    if (probe.good()) read(profile_file(), g_pkv);
    else {  // (new: from the settings as they were)
        for (const auto& [k, v] : g_kv) if (!global(k)) g_pkv[k] = v;
        write(profile_file(), g_pkv);
    }
    return true;
}
std::string profile() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_profile;
}
void set_clipboard(const std::string&) {}
}  // namespace hostui

// ---------------------------------------------------------------- the overlay's shapes (ImGui stand-in)
namespace {
std::mutex g_layers_mu;
std::vector<overlay::ext::FrameLayer> g_layers;
// the shapes being recorded: kShapeFloats each
std::vector<float>* g_rec = nullptr;
std::vector<std::string>* g_texts = nullptr;
float g_text_h = 20;  // text height in pixels for this view
ImGuiIO g_io;
ImDrawList g_list;
enum ShapeKind { kLine = 1, kRect, kRectFilled, kQuad, kQuadFilled, kTriFilled, kCircle, kCircleFilled, kText, kClipPush, kClipPop };
void put(int kind, ImU32 col, std::initializer_list<ImVec2> pts, float size = 0, int text = -1) {
    if (!g_rec) return;
    float f[online_compat::kShapeFloats] = {};
    f[0] = (float)kind;
    f[1] = float(col & 0xFF), f[2] = float((col >> 8) & 0xFF), f[3] = float((col >> 16) & 0xFF), f[4] = float(col >> 24);
    int i = 5;
    for (const ImVec2& p : pts) if (i < 13) f[i++] = p.x, f[i++] = p.y;
    f[13] = size, f[14] = (float)text;
    g_rec->insert(g_rec->end(), f, f + online_compat::kShapeFloats);
}
}  // namespace

void ImDrawList::AddLine(const ImVec2& a, const ImVec2& b, ImU32 col, float t) { put(kLine, col, {a, b}, t); }
void ImDrawList::AddRect(const ImVec2& lo, const ImVec2& hi, ImU32 col, float rounding, int, float t) {
    put(kRect, col, {lo, hi, ImVec2(rounding, 0)}, t);
}
void ImDrawList::AddRectFilled(const ImVec2& lo, const ImVec2& hi, ImU32 col, float rounding, int) {
    put(kRectFilled, col, {lo, hi, ImVec2(rounding, 0)});
}
void ImDrawList::AddQuad(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d, ImU32 col, float t) {
    put(kQuad, col, {a, b, c, d}, t);
}
void ImDrawList::AddQuadFilled(const ImVec2& a, const ImVec2& b, const ImVec2& c, const ImVec2& d, ImU32 col) {
    put(kQuadFilled, col, {a, b, c, d});
}
void ImDrawList::AddTriangleFilled(const ImVec2& a, const ImVec2& b, const ImVec2& c, ImU32 col) { put(kTriFilled, col, {a, b, c}); }
void ImDrawList::AddCircle(const ImVec2& c, float r, ImU32 col, int, float t) { put(kCircle, col, {c, ImVec2(r, 0)}, t); }
void ImDrawList::AddCircleFilled(const ImVec2& c, float r, ImU32 col, int) { put(kCircleFilled, col, {c, ImVec2(r, 0)}); }
void ImDrawList::AddText(const ImVec2& at, ImU32 col, const char* text, const char* end) {
    if (!g_texts || !text) return;
    g_texts->emplace_back(text, end ? end : text + strlen(text));
    put(kText, col, {at}, g_text_h, (int)g_texts->size() - 1);
}
void ImDrawList::PushClipRect(const ImVec2& lo, const ImVec2& hi, bool) { put(kClipPush, 0, {lo, hi}); }
void ImDrawList::PopClipRect() { put(kClipPop, 0, {}); }
namespace ImGui {
ImDrawList* GetForegroundDrawList() { return &g_list; }
ImVec2 CalcTextSize(const char* text, const char* end) {
    const size_t n = text ? (end ? size_t(end - text) : strlen(text)) : 0;
    return ImVec2(0.56f * g_text_h * (float)n, g_text_h);  // (the app draws the text to fit this width)
}
ImGuiIO& GetIO() { return g_io; }
}  // namespace ImGui

namespace overlay::ext {
void add_frame_layer(FrameLayer layer) {
    std::lock_guard<std::mutex> lk(g_layers_mu);
    g_layers.push_back(std::move(layer));
}
void draw_layers(float w, float h, std::vector<float>& shapes, std::vector<std::string>& texts) {
    std::lock_guard<std::mutex> lk(g_layers_mu);
    shapes.clear(), texts.clear();
    g_rec = &shapes, g_texts = &texts;
    g_text_h = std::max(14.0f, h / 720.0f * 22.0f);
    g_io.DisplaySize = ImVec2(w, h);
    for (auto& l : g_layers) {
        std::string toast;
        if (l.wanted && !l.wanted(toast)) continue;
        if (l.draw) l.draw(w, h);
    }
    g_rec = nullptr, g_texts = nullptr;
}
}  // namespace overlay::ext

// ---------------------------------------------------------------- Link's files, archives, surfaces
namespace link_poses {
namespace {
std::mutex g_mu;
std::string g_pack3d;
}  // namespace
void note_open(const std::string& guest_path, const std::string& host_path) {
    const size_t slash = guest_path.find_last_of('/');
    std::string name = slash == std::string::npos ? guest_path : guest_path.substr(slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return (char)tolower(ch); });
    if (name != "permanent_3d.pack") return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_pack3d = host_path;
}
bool game_files(Bytes& link_bfres, Bytes& anim_bfres, std::string& why) {
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        path = g_pack3d;
    }
    if (path.empty()) path = config::game_dir + "/content/Common/Pack/permanent_3d.pack";
    std::ifstream f(path, std::ios::binary);
    if (!f) return why = path + " can't be read", false;
    const Bytes pack((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try {
        const auto files = item_icons::read_sarc(pack);
        auto inner = [&](const char* arc, const char* file, Bytes& out) {
            const auto it = files.find(arc);
            if (it == files.end()) throw std::runtime_error(std::string(arc) + " not in the pack");
            const auto sarc = item_icons::read_sarc(item_icons::yaz0_decode(it->second));
            const auto fi = sarc.find(file);
            if (fi == sarc.end()) throw std::runtime_error(std::string(file) + " not in " + arc);
            out = fi->second;
        };
        inner("Link.szs", "Link.bfres", link_bfres);
        inner("LkAnm.szs", "LkAnm.bfres", anim_bfres);
        return true;
    } catch (const std::exception& e) {
        why = e.what();
        return false;
    }
}
}  // namespace link_poses

namespace item_icons {
std::map<std::string, Bytes> read_sarc(const Bytes& sarc) { return gcmodel::sarc_read(sarc); }
Bytes yaz0_decode(const Bytes& in) { return gcmodel::yaz0_decode(in); }
}  // namespace item_icons

namespace lyt_build {
namespace {
// (upstream's lyt_build.cpp, bflim.py / gx2tile.py)
struct Surface { uint32_t pitch, height, size, align; };
Surface surface_size(uint32_t w, uint32_t h, uint32_t bpp, int mode) {
    if (mode == 1) {
        const uint32_t a = std::max(64u, 256u * 8 / bpp), pitch = (w + a - 1) / a * a;
        return {pitch, h, pitch * h * bpp / 8, 0x100};
    }
    if (mode == 2) {
        const uint32_t pitch = (w + 7) / 8 * 8, height = (h + 7) / 8 * 8;
        return {pitch, height, pitch * height * bpp / 8, 256};
    }
    const uint32_t pitch = (w + 31) / 32 * 32, height = (h + 15) / 16 * 16;
    return {pitch, height, pitch * height * bpp / 8, 32 * 16 * bpp / 8};
}
uint32_t address(uint32_t x, uint32_t y, uint32_t pitch, int mode, uint32_t swizzle, uint32_t bpp = 32) {
    const uint32_t bytes = bpp / 8;
    if (mode == 1) return (x + pitch * y) * bytes;
    auto bit = [](uint32_t v, int i) { return (v >> i) & 1; };
    uint32_t idx;
    if (bpp == 64) idx = bit(x, 0) | bit(y, 0) << 1 | bit(x, 1) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
    else if (bpp == 128) idx = bit(y, 0) | bit(x, 0) << 1 | bit(x, 1) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
    else idx = bit(x, 0) | bit(x, 1) << 1 | bit(y, 0) << 2 | bit(x, 2) << 3 | bit(y, 1) << 4 | bit(y, 2) << 5;
    const uint32_t pix = idx * bytes;
    if (mode == 2) return pix + 64 * bytes * ((x >> 3) + (pitch >> 3) * (y >> 3));
    uint32_t pipe = ((y >> 3) ^ (x >> 3)) & 1, bank = (y >> 4) & 3;
    bank = ((bank >> 1) | (bank << 1)) & 3;
    bank = (bank ^ (x >> 3)) & 3;
    uint32_t bank_pipe = ((pipe + 2 * bank) ^ (((swizzle >> 8) & 1) + 2 * ((swizzle >> 9) & 3))) % 8;
    pipe = bank_pipe % 2, bank = bank_pipe / 2;
    const uint32_t macro_bytes = 32 * 16 * bytes, macro_off = ((x / 32) + (pitch / 32) * (y / 16)) * macro_bytes;
    uint32_t off = (macro_off >> 3) + pix;
    off = ((off & ~0xFFu) << 3) | (off & 0xFF);
    return off | (pipe << 8) | (bank << 9);
}
}  // namespace
Bytes untile(const Bytes& surface, uint32_t ew, uint32_t eh, uint32_t bpp, int mode, uint32_t swizzle) {
    if ((mode != 1 && mode != 2 && mode != 4) || (bpp != 32 && bpp != 64 && bpp != 128) || !ew || !eh || ew > 8192 || eh > 8192)
        return {};
    const Surface s = surface_size(ew, eh, bpp, mode);
    if (surface.size() < s.size) return {};
    const uint32_t eb = bpp / 8;
    Bytes out(size_t(ew) * eh * eb);
    for (uint32_t y = 0; y < eh; y++)
        for (uint32_t x = 0; x < ew; x++) memcpy(&out[(size_t(y) * ew + x) * eb], &surface[address(x, y, s.pitch, mode, swizzle << 8, bpp)], eb);
    return out;
}
}  // namespace lyt_build

// ---------------------------------------------------------------- the game's identity
namespace randomizer {
std::string game_rpx(const std::string& game_dir) { return packs::executable(game_dir); }
bool compiled_seed() { return packs::executable(config::game_dir) != config::game_dir + "/code/cking.rpx"; }
}  // namespace randomizer

// ---------------------------------------------------------------- the save states' part (savestate.h's online block)
namespace ss {
namespace {
std::mutex g_note_mu;
std::deque<std::string> g_notes;
// the game's globals (USA ids: GD() makes them this release's; upstream's savestate.cpp)
const uint32_t kSaveInfoPtr = GD(0x101F84DC);
const uint32_t kPlay = GD(0x1046F0B0);
const uint32_t kStartStage = kPlay + 0x5134;
const uint32_t kNextStage = kPlay + 0x5140;
const uint32_t kLinkPtr = kPlay + 0x5B34;
const uint32_t kEventRun = kPlay + 0x5292;
const uint32_t kMesgStatus = kPlay + 0x5BB2, kScopeMesgStatus = kPlay + 0x5BB3;
const uint32_t kMenuFlag = GD(0x101EA069);
const uint32_t kPlayerPtr = kPlay + 0x5B2C;
bool in_mem2(uint32_t a) { return a >= 0x10000000u && a < 0x50000000u; }
std::string stage_now() {
    const char* p = (const char*)mem::ptr(kStartStage);
    const size_t n = strnlen(p, 8);
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7E) return "";
    return std::string(p, n);
}
// a warp of the co-op code, waiting for the fork's warp to land (online_compat::tick)
std::mutex g_warp_mu;
std::function<void(bool, const std::string&)> g_warp_done;
}  // namespace

void notice(const std::string& text) {
    LOG("[online] %s", text.c_str());
    std::lock_guard<std::mutex> lk(g_note_mu);
    g_notes.push_back(text);
    while (g_notes.size() > 16) g_notes.pop_front();
}

uint64_t game_data_id() {
    static const uint64_t id = [] {
        uint64_t h = 0xcbf29ce484222325ull;  // FNV-1a 64 over the whole file, as upstream's ss::game_id
        FILE* f = fopen(randomizer::game_rpx(config::game_dir).c_str(), "rb");
        if (!f) return h;
        std::vector<uint8_t> buf(1 << 20);
        size_t n;
        while ((n = fread(buf.data(), 1, buf.size(), f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
        fclose(f);
        return h;
    }();
    return id;
}

std::string area_name(const char* stage) {
    static const std::map<std::string, std::string> names = {
        {"sea", "Great Sea"}, {"LinkRM", "Link's House"}, {"MajyuE", "Forsaken Fortress"}, {"M_NewD2", "Dragon Roost Cavern"},
        {"kindan", "Forbidden Woods"}, {"Siren", "Tower of the Gods"}, {"Asoko", "Tetra's Ship"},
    };
    const auto it = names.find(stage ? std::string(stage, strnlen(stage, 8)) : "");
    return it != names.end() ? it->second : std::string(stage ? stage : "", stage ? strnlen(stage, 8) : 0);
}

std::string control_problem() {
    const uint32_t sv = ld32(kSaveInfoPtr);
    if (!in_mem2(sv)) return "no game in progress";
    const std::string st = stage_now();
    if (st.empty() || st == "sea_T" || st == "Name" || st == "ENDumi") return "no game in progress";
    if (!in_mem2(ld32(kLinkPtr))) return "Link is not in the scene";
    if (ld8(kEventRun)) return "a cutscene or event is running";
    if (ld8(kMesgStatus) || ld8(kScopeMesgStatus)) return "a message or dialogue is open";
    if (ld8(kMenuFlag)) return "a game menu is open";
    if (ld8(kNextStage + 12)) return "a stage change is in progress";
    if (ld32(kPlayerPtr) != ld32(kLinkPtr)) return "Link is not the controlled character";
    return "";
}

void request_warp(const Warp& w, std::function<void(bool ok, const std::string& why)> done) {
    if (w.at_position) mods::set_warp_place(w.pos[0], w.pos[1], w.pos[2], w.angle);
    if (!mods::request_warp(w.stage.c_str(), w.room, w.point, -1)) {
        if (done) done(false, "no Quest Log is loaded");
        return;
    }
    std::function<void(bool, const std::string&)> old;
    {
        std::lock_guard<std::mutex> lk(g_warp_mu);
        old = std::move(g_warp_done);
        g_warp_done = std::move(done);
    }
    if (old) old(false, "another warp came first");
}

// ---- portable states, for Stay together (a guest plays in the host's world): upstream's savestate.cpp capture_portable,
// apply_portable and watch_arrival, on the fork's runtime. A portable state is the Quest Log's save block as the
// game's own save writes it, the HD per-file sections, the stage / room / layer and Link's (and the boat's) place
// (portable_state.h, upstream's file unchanged); loading puts the save data into the running game and enters the
// stage the way the game restarts a room after a void-out.
namespace {
constexpr uint32_t kActorPos = 0x314, kActorRoom = 0x326, kShapeAngleY = 0x32A;  // fopAc_ac_c
constexpr uint32_t kLinkProc = 0x65F0;                                           // daPy_lk_c::mCurProc
constexpr uint32_t kInfo = 0x20;                                                 // dSv_info_c in the save area
constexpr uint32_t kDataNum = kInfo + 0x1290, kReturnPlace = 0x50, kMemoryTable = kInfo + 0x380;
constexpr uint32_t kTurnRestart = kInfo + 0x1258, kRestart = kInfo + 0x1128, kHdArea = 0x12C0, kCardStatusB = 0x18;
const uint32_t kStageData = kPlay + 0x5150, kShipPtr = kPlay + 0x5B3C;
// (USA ids of the game's functions, GC() at the call)
constexpr uint32_t fn_memory_to_card = 0x025BA9FC, fn_card_to_memory = 0x025BA7B0, fn_putSave = 0x025B9D24,
                   fn_getSave = 0x025B9C9C, fn_setGameStartStage = 0x025217F8, fn_danInit = 0x025B9174,
                   fn_eventInit = 0x025B8B10, fn_refreshGame = 0x02721880, fn_turnRestartSet = 0x025B998C,
                   fn_name_obj = 0x02720154;
struct HdSection { uint32_t slot_get, live_get, copy; size_t size; std::vector<uint8_t> pstate::State::*field; };
const HdSection kHdSections[] = {
    {0x027200A0, 0x027200D0, 0x0271FCB4, pstate::kHdPlayerSize, &pstate::State::hd_player},
    {0x027200D8, 0x027200F4, 0x0271FAC0, pstate::kHdStatusSize, &pstate::State::hd_status},
    {0x027200FC, 0x02720118, 0x0271F914, pstate::kHdEventSize, &pstate::State::hd_event},
    {0x02720180, 0x0272019C, 0x027208F4, pstate::kHdMapSize, &pstate::State::hd_map},
};
uint32_t scratch() {
    static const uint32_t s = mem::host_alloc(0x2000, 64);
    return s;
}
struct CallScope {  // guest calls between the game's steps: every register put back afterwards
    Cpu* c;
    Cpu saved;
    explicit CallScope(Cpu* cpu) : c(cpu), saved(*cpu) {}
    ~CallScope() { *c = saved; }
};
uint32_t call(Cpu* c, uint32_t usa, std::initializer_list<uint32_t> args = {}) { return guest_call(c, GC(usa), args); }
int current_stage_no(Cpu* c) {  // dStage_stagInfo_GetSaveTbl, as dStage_Delete computes it
    const uint32_t vt = ld32(kStageData);
    if (!in_mem2(vt)) return -1;
    const uint32_t stag = guest_call(c, ld32(vt + 0x15C), {kStageData});
    if (!in_mem2(stag)) return -1;
    return (ld8(stag + 9) >> 1) & 0x7F;
}
std::string meta_value(const char* key) {
    std::string meta;
    if (FILE* f = fopen((config::game_dir + "/meta/meta.xml").c_str(), "rb")) {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0 && meta.size() < (1u << 20)) meta.append(buf, n);
        fclose(f);
    }
    size_t p = meta.find(std::string("<") + key);
    if (p == std::string::npos || (p = meta.find('>', p)) == std::string::npos) return "";
    const size_t e = meta.find('<', p);
    return e == std::string::npos ? "" : meta.substr(p + 1, e - p - 1);
}
std::string utf8_from_utf16be(uint32_t p, int max) {
    std::string s;
    for (int i = 0; i < max && in_mem2(p); i++, p += 2) {
        const uint16_t ch = ld16(p);
        if (!ch) break;
        if (ch < 0x80) s += (char)ch;
        else if (ch < 0x800) s += (char)(0xC0 | ch >> 6), s += (char)(0x80 | (ch & 0x3F));
        else s += (char)(0xE0 | ch >> 12), s += (char)(0x80 | (ch >> 6 & 0x3F)), s += (char)(0x80 | (ch & 0x3F));
    }
    return s;
}
bool capture(Cpu* c, pstate::State& s, std::string& why) {
    CallScope scope(c);
    const uint32_t sv = ld32(kSaveInfoPtr), info = sv + kInfo, buf = scratch();
    const std::string stage = stage_now();
    const int stage_no = current_stage_no(c);
    // what the in-game save does first (SaveMgr 02721C44), on a copy: the live game stays unchanged
    uint8_t rp[0xC], entry[0x24];
    memcpy(rp, mem::ptr(sv + kReturnPlace), sizeof rp);
    if (stage_no >= 0 && stage_no < 16) {
        memcpy(entry, mem::ptr(sv + kMemoryTable + stage_no * 0x24), sizeof entry);
        call(c, fn_putSave, {info, (uint32_t)stage_no});
    }
    if (stage != "PShip") call(c, fn_setGameStartStage);
    memset(mem::ptr(buf), 0, pstate::kSaveDataSize);
    const int32_t r = (int32_t)call(c, fn_memory_to_card, {info, buf, 0});
    memcpy(mem::ptr(sv + kReturnPlace), rp, sizeof rp);
    if (stage_no >= 0 && stage_no < 16) memcpy(mem::ptr(sv + kMemoryTable + stage_no * 0x24), entry, sizeof entry);
    if (r == -1) { why = "the game's save function failed"; return false; }
    s.savedata.assign(mem::ptr(buf), mem::ptr(buf) + pstate::kSaveDataSize);
    pstate::seal_savedata(s.savedata);
    const uint32_t hd = sv + kHdArea;
    const int slot = ld8(sv + kDataNum);
    for (const HdSection& h : kHdSections) {  // this file's stored copy with the live data over it (SaveMgr 02721B34)
        const uint32_t stored = call(c, h.slot_get, {hd, (uint32_t)slot}), live = call(c, h.live_get, {hd});
        memcpy(mem::ptr(buf), mem::ptr(stored), h.size);
        call(c, h.copy, {buf, live});
        (s.*h.field).assign(mem::ptr(buf), mem::ptr(buf) + h.size);
    }
    s.player_name = utf8_from_utf16be(ld32(call(c, fn_name_obj, {hd, (uint32_t)slot})), 8);
    s.title_id = meta_value("title_id");
    if (s.title_id.empty()) s.title_id = "0005000010143500";
    s.title_version = (uint32_t)strtoul(meta_value("title_version").c_str(), nullptr, 10);
    char h[20];
    snprintf(h, sizeof h, "%016llx", (unsigned long long)game_data_id());
    s.game_hash = h;
    s.runtime = "Android";
    {
        const time_t t = time(nullptr);
        struct tm tmv;
        localtime_r(&t, &tmv);
        char b[32];
        strftime(b, sizeof b, "%Y-%m-%d %H:%M:%S", &tmv);
        s.created = b;
    }
    s.file_slot = slot;
    s.stage = stage;
    s.start_point = (int16_t)ld16(kStartStage + 8);
    s.start_room = (int8_t)ld8(kStartStage + 10);
    s.layer = (int8_t)ld8(kStartStage + 11);
    const uint32_t link = ld32(kLinkPtr);
    for (int i = 0; i < 3; i++) s.pos[i] = (float)ldf32(link + kActorPos + 4 * i);
    s.room = (int8_t)ld8(link + kActorRoom);
    s.angle_y = (int16_t)ld16(link + kShapeAngleY);
    s.link_proc = (int)ld32(link + kLinkProc);
    const uint32_t ship = ld32(kShipPtr);
    s.has_ship = in_mem2(ship);
    if (s.has_ship) {
        for (int i = 0; i < 3; i++) s.ship_pos[i] = (float)ldf32(ship + kActorPos + 4 * i);
        s.ship_angle_y = (int16_t)ld16(ship + kShapeAngleY);
    }
    s.on_ship = s.has_ship && s.link_proc >= 0x86 && s.link_proc <= 0x91 && s.link_proc != 0x90;  // daPyProc_SHIP_*
    auto be32 = [&](size_t o) { return (uint32_t)s.savedata[o] << 24 | (uint32_t)s.savedata[o + 1] << 16 | (uint32_t)s.savedata[o + 2] << 8 | s.savedata[o + 3]; };
    const uint32_t tb = be32(kCardStatusB + 0xC);
    float t;
    memcpy(&t, &tb, 4);
    s.time_of_day = t;
    s.date = s.savedata[kCardStatusB + 0x10] << 8 | s.savedata[kCardStatusB + 0x11];
    return true;
}
void apply_hd_sections(Cpu* c, const pstate::State& s) {
    const uint32_t hd = ld32(kSaveInfoPtr) + kHdArea, buf = scratch();
    for (const HdSection& h : kHdSections) {
        memcpy(mem::ptr(buf), (s.*h.field).data(), h.size);
        call(c, h.copy, {call(c, h.live_get, {hd}), buf});
    }
}
bool apply(Cpu* c, const pstate::State& s, std::string& why) {
    CallScope scope(c);
    const uint32_t sv = ld32(kSaveInfoPtr), info = sv + kInfo, buf = scratch();
    memcpy(mem::ptr(buf), s.savedata.data(), pstate::kSaveDataSize);
    if ((int32_t)call(c, fn_card_to_memory, {info, buf, 0}) == -1) { why = "the game refused the save data"; return false; }
    const int stage_no = current_stage_no(c);
    if (stage_no >= 0 && stage_no < 16) call(c, fn_getSave, {info, (uint32_t)stage_no});
    call(c, fn_danInit, {info + 0x79C, 0xFFFFFFFFu});  // dSv_danBit_c::init(-1): the next stage starts fresh
    call(c, fn_eventInit, {info + 0x1158});            // dSv_info_c::mTmp (temporary flags)
    apply_hd_sections(c, s);
    call(c, fn_refreshGame, {0});  // SaveMgr: item buttons and equipment from the loaded data
    const uint32_t rs = sv + kRestart;
    st8(rs + 0, (uint8_t)s.room);
    st16(rs + 0x16, (uint16_t)s.angle_y);
    for (int i = 0; i < 3; i++) stf32(rs + 0x18 + 4 * i, s.pos[i]);
    st32(rs + 0x24, (uint32_t)(s.room & 0x3F) | (pstate::restore_crawl(s) ? 5u << 12 : 0u) | 0xFF000000u);
    stf32(rs + 0x28, 0.0);
    st32(rs + 0x2C, pstate::restart_scene_mode(s));
    char name[8] = {};
    memcpy(name, s.stage.data(), std::min<size_t>(s.stage.size(), 7));
    for (int i = 0; i < 8; i++) st8(kNextStage + i, (uint8_t)name[i]);
    {
        const uint32_t param = pstate::restart_parameters(s);
        for (int i = 0; i < 3; i++) stf32(buf + 4 * i, s.pos[i]), stf32(buf + 0x10 + 4 * i, s.ship_pos[i]);
        call(c, fn_turnRestartSet, {sv + kTurnRestart, buf, (uint32_t)(uint16_t)s.angle_y, (uint32_t)(uint8_t)s.room, param, buf + 0x10,
                                    (uint32_t)(uint16_t)s.ship_angle_y, (uint32_t)s.has_ship});
    }
    st16(kNextStage + 8, 0xFFFD);  // dStage_playerInit reads the turn-restart record
    st8(kNextStage + 10, (uint8_t)s.room);
    st8(kNextStage + 11, (uint8_t)s.layer);
    st8(kNextStage + 13, 0);  // wipe: fade
    st8(kNextStage + 12, 1);  // enabled
    return true;
}
// a load waiting for the game step (online_compat::tick), then its arrival
std::mutex g_pload_mu;
std::shared_ptr<pstate::State> g_pload;
std::function<void(bool, const std::string&)> g_pload_done;
struct Arrival {
    std::shared_ptr<pstate::State> s;
    uint32_t link_id = 0;
    int waited = 0, frames = 0;
    std::function<void(bool, const std::string&)> done;
    bool crawl_stopped = false;  // (upstream 224c7c4b)
} g_arrival;
}  // namespace

bool capture_portable_text(Cpu* c, std::string& out, std::string& why) {
    out.clear();
    why = control_problem();
    if (!why.empty()) return false;
    pstate::State s;
    if (!capture(c, s, why)) return false;
    out = pstate::write(s, why);
    return !out.empty();
}
void request_load_portable_state(std::shared_ptr<pstate::State> s, std::function<void(bool, const std::string&)> done) {
    std::function<void(bool, const std::string&)> old;
    {
        std::lock_guard<std::mutex> lk(g_pload_mu);
        old = std::move(g_pload_done);
        g_pload = std::move(s), g_pload_done = std::move(done);
    }
    if (old) old(false, "superseded by a newer load request");
}
}  // namespace ss

namespace android_models { void step(Cpu* c); }
namespace android_raid { void step(Cpu* c); }
namespace android_fight { void step(Cpu* c); }
namespace android_quests { void step(Cpu* c); }
namespace android_down { void step(Cpu* c); }
namespace android_siege { void step(Cpu* c); }
namespace android_shipraid { void step(Cpu* c); }
namespace android_say { void step(Cpu* c); }
namespace online_compat {
namespace {
void portable_step(Cpu* c) {  // (upstream's service_portable_load + watch_arrival)
    using namespace ss;
    std::shared_ptr<pstate::State> s;
    std::function<void(bool, const std::string&)> done;
    {
        std::lock_guard<std::mutex> lk(g_pload_mu);
        if (g_pload && control_problem().empty()) s = std::move(g_pload), done = std::move(g_pload_done), g_pload = nullptr;
    }
    if (s) {
        std::string why;
        if (!apply(c, *s, why)) {
            LOG("[online] portable state: cannot load (%s)", why.c_str());
            if (done) done(false, why);
        } else {
            LOG("[online] portable state applied: entering %s room %d at %.1f %.1f %.1f", s->stage.c_str(), s->room, s->pos[0], s->pos[1], s->pos[2]);
            if (g_arrival.done) g_arrival.done(false, "superseded by a newer portable load");
            g_arrival = Arrival{s, in_mem2(ld32(kLinkPtr)) ? ld32(ld32(kLinkPtr) + 4) : 0, 0, 0, std::move(done)};
        }
        return;
    }
    if (!g_arrival.s) return;
    const uint32_t link = ld32(kLinkPtr);
    if (stage_now() != g_arrival.s->stage || !in_mem2(link) || ld32(link + 4) == g_arrival.link_id) {  // fpcBs mBsPcId: the new Link
        if (++g_arrival.waited > 30 * 60) {
            LOG("[online] portable state: did not arrive in %s", g_arrival.s->stage.c_str());
            auto d = std::move(g_arrival.done);
            g_arrival = Arrival{};
            if (d) d(false, "did not arrive in the host's stage");
        }
        return;
    }
    if (g_arrival.frames++ == 0) {
        CallScope scope(c);
        apply_hd_sections(c, *g_arrival.s);  // (leaving the old stage updated the live sea chart position)
    }
    // A crawling state (upstream 224c7c4b): once the new Link is in the crawl entry (procedure 0x10, its moving
    // animation), that animation is stopped once; with no input he would coast ~30 units on every load. Crawl input
    // starts it again as usual.
    if (!g_arrival.crawl_stopped && pstate::restore_crawl(*g_arrival.s) && ld32(link + kLinkProc) == 0x10 &&
        ld16(link + 0x420) == 4 && ld32(link + 0x430) == 0x0E) {
        stf32(link + 0x5898, 0.0f);  // mFrameCtrlUnder[0].mRate
        g_arrival.crawl_stopped = true;
        LOG("[online] portable state: crawl entry animation stopped at arrival frame %d", g_arrival.frames);
    }
    if (g_arrival.frames < 30) return;  // Link settles (start animation, ground check)
    LOG("[online] portable state: arrived in %s room %d", stage_now().c_str(), (int8_t)ld8(link + kActorRoom));
    if (pstate::restore_crawl(*g_arrival.s)) {
        const int proc = (int)ld32(link + kLinkProc);
        LOG("[online] portable state: crawl entry %s (procedure 0x%X)", pstate::crawl_procedure(proc) ? "restored" : "has ended", proc);
    }
    auto d = std::move(g_arrival.done);
    g_arrival = Arrival{};
    if (d) d(true, "");
}
}  // namespace
void tick(Cpu* c) {
    portable_step(c);
    android_models::step(c);
    android_raid::step(c);  // (the fork's raid: more monsters, online/android_raid.cpp)
    android_fight::step(c);  // (the same monster animations on every game, online/android_fight.cpp)
    android_quests::step(c);  // (daily quests from the fork's server, online/android_quests.cpp)
    android_down::step(c);  // (down, not dead, while a party member is near: online/android_down.cpp)
    android_siege::step(c);  // (the event "Outset under siege": online/android_siege.cpp)
    android_shipraid::step(c);  // (the event "Raiders from the sea": online/android_shipraid.cpp)
    android_say::step(c);  // (quest texts in the game's own message window: online/android_say.cpp)
    std::function<void(bool, const std::string&)> done;
    int64_t st[3];
    mods::warp_status(st);
    {
        std::lock_guard<std::mutex> lk(ss::g_warp_mu);
        if (!ss::g_warp_done) return;
        if (st[0] != 3 && st[0] != 4) return;  // (requested or changing scene)
        done = std::move(ss::g_warp_done);
    }
    done(st[0] == 3, st[0] == 3 ? "" : "a scene change was under way");
}
std::vector<std::string> take_notices() {
    std::lock_guard<std::mutex> lk(ss::g_note_mu);
    std::vector<std::string> out(ss::g_notes.begin(), ss::g_notes.end());
    ss::g_notes.clear();
    return out;
}
}  // namespace online_compat
