// Picto Box album pages (Game menu): the HD album holds 12 pictos; with this on, any number of pages of 12.
// The game keeps its 12-slot album; the other pages are files of our own next to the save, and the album
// viewer's page switch (the app's overlay) swaps one into the game's album.
//
// The game's album (WWHD, not on the GameCube): in the HD save object (pointer 101F84DC, + 0x12C0) four
// blocks of 0x3C0340 bytes from +0x700: one per save file (0..2, 02720120) and the working album of the
// file in play (02720144). A block: 12 slots of 0x50040 (02726164: a slot's JPEG at +0x40) and at
// +0x3C0300 the order (s8[12], -1 empty) and a byte at +0x30C. A slot's header: +0 checksum of size and
// data (0272581C), +4 changed (written at the next save, 02725FAC), +6 used (02726218 finds the first
// slot without it: none = the album is full), +0xC the JPEG's size.
// Loading a file copies its block into the working album (02720D34); the game's own save copies it back
// (02721B34) and writes the changed slots to cking_pic<file>_<slot>.sav and the order (12 headers + the
// order + the byte) to cking_pic_order<file>.sav. The album viewer keeps a texture per slot (the texture
// cache at [[101F8344] + 0x1D8], 0xA8 per slot; +0x90 0 = made again from the JPEG, as the game's delete
// does, 027172C8).
//
// Switching changes the working album (all its slots marked changed, so the game's next save writes them
// all) and writes the page left at once (save states taken after a switch keep every page); the page in
// play is written when the game saves (hook on 02721B34). When a file is loaded (02720D34) or a save state
// is, the page in play is the one sharing the most pictos with the game's album (by the slots' checksums):
// a save loaded without saving after new pictos still finds its page, and the game's album wins over it,
// as for any unsaved progress. A save that shares none with the pages (a deleted, copied or imported file)
// moves them aside to picto_pages/file<n>-old-<time> and starts over. A full album when a picto is taken
// starts a new page first: the Picto Box's aim (026C1974, each frame) takes the picto on a press of
// [[1018F3F4] + 0x14] == 0 and buttons [[101F5088] + 0x18] & 0x6001 while the count (+0x3C030C) is below 12,
// else it says the album is full. An empty page is dropped when left.
//
// Files: <save>/picto_pages/file<n>/page<k>.bin ("WWPG", version, 12 slot headers, the order block, then
// per slot its JPEG's length and bytes) and index.txt (the page in the game, the count).
// Test switch: WWHD_PICTO_PAGES=1 switches it on.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "mods.h"
#include "../guest_addr.h"
#include "../input.h"
#include "../release.h"
#include "../runtime.h"
#include "../savestate.h"

namespace mods {
namespace {
const release::Data kSaveInfo{0x101F84DC}, kTextures{0x101F8344}, kPlay{0x1046F0B0};
const release::Data kAimBusy{0x1018F3F4}, kAimPad{0x101F5088};  // the Picto Box's aim: see the top
// R's action (touch_state.cpp: the forced one if set, else the normal one): 0x48 in the album (back to the
// Picto Box)
uint8_t r_action() {
    uint32_t normal = kPlay + (release::id() == release::Id::JPN ? 0x5BB1 : 0x5BB5);
    uint8_t forced = ld8(normal + 3);
    return forced ? forced : ld8(normal);
}
constexpr uint32_t kAlbums = 0x12C0 + 0x700, kBlock = 0x3C0340, kSlot = 0x50040, kSlots = 12;
constexpr uint32_t kOrder = 0x3C0300, kOrderLen = 0x40, kHeader = 0x40, kJpegMax = 0x50000, kCount = 0x3C030C;
constexpr uint32_t kSlotChecksum = 0x0, kSlotUsed = 0x6, kSlotSize = 0xC;
constexpr uint32_t kFileIndex = 0x12B0;  // the save file in play (u8; 02720170)
constexpr uint32_t kMagic = 0x57575047, kVersion = 1;  // "WWPG"

std::atomic<bool> g_on{false};
std::atomic<int> g_request{0};             // page switch asked by the app: -1 / +1
std::atomic<int> g_cur_shown{-1}, g_count_shown{0}, g_used_shown{0};
// the album viewer keeps its count and picture after a switch: ZR to the Picto Box and back makes it read the
// album again (picto_pad presses it; 1 to the Picto Box, 2 let go, 3 back to the album, 4 let go)
std::atomic<int> g_reopen{0};
// the press that took a picto into a full album only made the new page: A pressed again (picto_pad)
std::atomic<bool> g_press_a{false};

struct Page {
    bool loaded = false;  // data holds the page (else only on disk)
    bool dirty = false;   // to be written at the next game save
    std::vector<uint8_t> data;
    std::vector<uint32_t> sig;  // its used slots' checksums, sorted
    int used = 0;
    bool missing = false;  // in index.txt, but never written (a page started and the game not saved since)
};
struct Album {
    int file = -1;
    int cur = 0;
    std::vector<Page> pages;
    unsigned loads = 0;  // ss::load_count() when read
    int index_cur = -1, index_count = -1;  // what index.txt says
} g;

uint32_t save_object() { return ld32(kSaveInfo); }
uint32_t working() { return save_object() + kAlbums + 3 * kBlock; }

std::string dir_of(int file) { return config::save_dir + "/picto_pages/file" + std::to_string(file); }
std::string page_path(int file, int k) { return dir_of(file) + "/page" + std::to_string(k) + ".bin"; }

void make_dirs(const std::string& d) {
    for (size_t i = 1; i <= d.size(); i++)
        if (i == d.size() || d[i] == '/') mkdir(d.substr(0, i).c_str(), 0755);
}

// big-endian fields of a page's bytes (the guest's own order)
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
void put32(std::vector<uint8_t>& v, uint32_t x) {
    uint8_t b[4] = {(uint8_t)(x >> 24), (uint8_t)(x >> 16), (uint8_t)(x >> 8), (uint8_t)x};
    v.insert(v.end(), b, b + 4);
}

// the checksums of the used slots from 12 headers (kHeader bytes each)
void sign(Page& p, const uint8_t* headers) {
    p.sig.clear();
    for (uint32_t i = 0; i < kSlots; i++) {
        const uint8_t* h = headers + i * kHeader;
        if (h[kSlotUsed]) p.sig.push_back(be32(h + kSlotChecksum));
    }
    std::sort(p.sig.begin(), p.sig.end());
    p.used = (int)p.sig.size();
}

Page capture(uint32_t block) {
    Page p;
    p.loaded = p.dirty = true;
    std::vector<uint8_t>& v = p.data;
    put32(v, kMagic);
    put32(v, kVersion);
    for (uint32_t i = 0; i < kSlots; i++) v.insert(v.end(), mem::ptr(block + i * kSlot), mem::ptr(block + i * kSlot) + kHeader);
    v.insert(v.end(), mem::ptr(block + kOrder), mem::ptr(block + kOrder) + kOrderLen);
    for (uint32_t i = 0; i < kSlots; i++) {
        uint32_t s = block + i * kSlot;
        uint32_t n = ld8(s + kSlotUsed) ? std::min(ld32(s + kSlotSize), kJpegMax) : 0;
        put32(v, n);
        v.insert(v.end(), mem::ptr(s + kHeader), mem::ptr(s + kHeader) + n);
    }
    sign(p, v.data() + 8);
    return p;
}

bool valid(const std::vector<uint8_t>& v) {
    return v.size() >= 8 + kSlots * kHeader + kOrderLen && be32(v.data()) == kMagic && be32(v.data() + 4) == kVersion;
}

// writes the page into the album block (the slots' data beyond each JPEG zeroed, as the game keeps it)
bool apply(const Page& p, uint32_t block) {
    const std::vector<uint8_t>& v = p.data;
    if (!valid(v)) return false;
    const uint8_t* h = v.data() + 8;
    const uint8_t* q = h + kSlots * kHeader + kOrderLen;
    const uint8_t* end = v.data() + v.size();
    for (uint32_t i = 0; i < kSlots; i++) {
        if (end - q < 4) return false;
        uint32_t n = std::min(be32(q), kJpegMax);
        q += 4;
        if ((uint32_t)(end - q) < n) return false;
        uint32_t s = block + i * kSlot;
        memcpy(mem::ptr(s), h + i * kHeader, kHeader);
        memcpy(mem::ptr(s + kHeader), q, n);
        memset(mem::ptr(s + kHeader + n), 0, kJpegMax - n);
        q += n;
    }
    memcpy(mem::ptr(block + kOrder), h + kSlots * kHeader, kOrderLen);
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t limit = SIZE_MAX) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while (out.size() < limit && (n = fread(buf, 1, std::min(sizeof buf, limit - out.size()), f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}

bool write_file(const std::string& path, const void* d, size_t n) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(d, 1, n, f) == n;
    ok = fclose(f) == 0 && ok;
    return ok && rename(tmp.c_str(), path.c_str()) == 0;
}

bool load_page(int k) {
    Page& p = g.pages[k];
    if (p.loaded) return true;
    if (!read_file(page_path(g.file, k), p.data) || !valid(p.data)) {
        LOG("[picto] page %d of file %d unreadable", k + 1, g.file);
        return false;
    }
    p.loaded = true;
    return true;
}

void publish() {
    g_count_shown = g.file >= 0 ? (int)g.pages.size() : 0;
    g_cur_shown = g.file >= 0 ? g.cur : -1;
    g_used_shown = g.file >= 0 ? g.pages[g.cur].used : 0;
}

// the album viewer makes each slot's texture again from its JPEG
void refresh_textures() {
    uint32_t mgr = ld32(kTextures);
    uint32_t tex = mgr ? ld32(mgr + 0x1D8) : 0;
    if (!tex) return;
    for (uint32_t i = 0; i < kSlots; i++) st32(tex + i * 0xA8 + 0x90, 0);
}

void write_pages(bool with_cur);

// takes page k out; the pages after it move down (kept in memory to be written under their new number)
void erase_page(int k) {
    for (size_t j = k + 1; j < g.pages.size(); j++)
        if (!g.pages[j].missing && load_page((int)j)) g.pages[j].dirty = true;
    g.pages.erase(g.pages.begin() + k);
    if (g.cur > k) g.cur--;
}

// reads the file's pages (their headers) and finds the one in the game's album
void attach(int file) {
    uint32_t w = working();
    Page now = capture(w);
    g = Album{};
    g.file = file;
    g.loads = ss::load_count();
    std::string d = dir_of(file);
    int cur = -1, count = 0;
    if (FILE* f = fopen((d + "/index.txt").c_str(), "r")) {
        if (fscanf(f, "cur %d count %d", &cur, &count) != 2) cur = -1, count = 0;
        fclose(f);
    }
    for (int k = 0; k < count; k++) {
        Page p;
        std::vector<uint8_t> head;
        if (read_file(page_path(file, k), head, 8 + kSlots * kHeader) && head.size() == 8 + kSlots * kHeader &&
            be32(head.data()) == kMagic)
            sign(p, head.data() + 8);
        else
            p.missing = true;
        g.pages.push_back(std::move(p));
    }
    g.index_cur = cur;
    g.index_count = count;
    // the page sharing the most pictos (an empty album: an empty page); the last one in play first
    int match = -1, best = 0;
    for (int k = 0; k < count; k++) {
        const std::vector<uint32_t>& a = g.pages[k].sig;
        int common = 0;
        for (size_t i = 0, j = 0; i < a.size() && j < now.sig.size();)
            if (a[i] < now.sig[j]) i++;
            else if (a[i] > now.sig[j]) j++;
            else common++, i++, j++;
        int score = now.sig.empty() ? (a.empty() ? 1 : 0) : common;
        if (score > best || (score == best && score > 0 && k == cur)) best = score, match = k;
    }
    // no picto in common: the page started last and never written is the one in play (a save state's)
    if (match < 0 && cur >= 0 && cur < count && g.pages[cur].missing) match = cur;
    if (count && match < 0) {  // not this save's pages
        char stamp[32];
        time_t t = time(nullptr);
        strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&t));
        std::string old = d + "-old-" + stamp;
        LOG("[picto] file %d: the album (%d pictos) matches none of its %d pages: moved to %s", file, now.used, count, old.c_str());
        rename(d.c_str(), old.c_str());
        g.pages.clear();
        count = 0;
    }
    if (!count) {
        g.pages.push_back(std::move(now));
        g.cur = 0;
    } else {
        g.cur = match;
        bool same = g.pages[match].sig == now.sig && !g.pages[match].missing;
        g.pages[match] = std::move(now);
        g.pages[match].dirty = !same;
        size_t before = g.pages.size();
        for (int k = (int)g.pages.size() - 1; k >= 0; k--)  // pages never written hold nothing
            if (k != g.cur && g.pages[k].missing) erase_page(k);
        if (g.pages.size() != before || g.cur != cur) write_pages(false);
    }
    LOG("[picto] file %d: page %d of %zu (%d pictos)", file, g.cur + 1, g.pages.size(), g.pages[g.cur].used);
    publish();
}

// the file in play (only in play: the title screen's placeholder save would not match the pages)
void ensure_attached() {
    uint32_t so = save_object();
    if (!so || ss::front_screen() != 0) return;
    int file = ld8(so + kFileIndex);
    if (file > 2) return;
    if (g.file != file) attach(file);
}

void detach() {
    g.file = -1;
    g.pages.clear();
    publish();
}

void drop_empty(int k) {
    if (k != g.cur && g.pages.size() > 1 && !g.pages[k].used) erase_page(k);
}

// puts page `to` into the game's album (to == count: a new, empty page)
void switch_to(Cpu* c, int to) {
    uint32_t w = working();
    int from = g.cur;
    if (to < 0 || to > (int)g.pages.size() || to == from) return;
    g.pages[from] = capture(w);
    if (to == (int)g.pages.size()) {
        if (!g.pages[from].used) return;  // already on an empty page
        guest_call(c, GC(0x02725DA8), {w});  // every slot empty, the order cleared
        g.pages.push_back(capture(w));
    } else {
        if (!load_page(to) || !apply(g.pages[to], w)) return;
    }
    guest_call(c, GC(0x02725FD0), {w});  // every slot changed: the next save writes them all
    if (picto_album_open()) g_reopen = 1;
    g.cur = to;
    drop_empty(from);
    write_pages(false);
    refresh_textures();
    LOG("[picto] page %d -> %d of %zu (%d pictos)", from + 1, g.cur + 1, g.pages.size(), g.pages[g.cur].used);
    publish();
}

// writes the changed pages (but the one in play: the game's save writes it, commit) and index.txt
void write_pages(bool with_cur) {
    std::string d = dir_of(g.file);
    make_dirs(d);
    int written = 0;
    for (size_t k = 0; k < g.pages.size(); k++) {
        Page& p = g.pages[k];
        bool cur = k == (size_t)g.cur;
        if (cur && !with_cur) continue;
        if (p.dirty && p.loaded) {
            if (write_file(page_path(g.file, (int)k), p.data.data(), p.data.size())) {
                p.dirty = false;
                written++;
            } else {
                LOG("[picto] writing page %zu failed (%s)", k + 1, strerror(errno));
            }
        }
        if (!cur && !p.dirty) p.data = {}, p.loaded = false;  // only the page in play stays in memory
    }
    if ((int)g.pages.size() < g.index_count)
        for (int k = (int)g.pages.size(); k < g.index_count; k++) unlink(page_path(g.file, k).c_str());
    if (g.cur != g.index_cur || (int)g.pages.size() != g.index_count) {
        char idx[64];
        int n = snprintf(idx, sizeof idx, "cur %d count %zu\n", g.cur, g.pages.size());
        if (write_file(d + "/index.txt", idx, n)) g.index_cur = g.cur, g.index_count = (int)g.pages.size();
    }
    if (written) LOG("[picto] file %d: %d page(s) written", g.file, written);
}

// the game saved file `file`: the page in play to disk too
void commit(int file) {
    if (g.file != file) return;
    Page now = capture(working());
    Page& old = g.pages[g.cur];
    bool dirty = old.dirty || !old.loaded || now.data != old.data;
    old = std::move(now);
    old.dirty = dirty;
    write_pages(true);
}

// the aim takes a picto this frame (its own test) but the album is full
bool shot_into_full_album() {
    uint32_t busy = ld32(kAimBusy), pad = ld32(kAimPad);
    return busy && pad && !ld32(busy + 0x14) && (ld32(pad + 0x18) & 0x6001) && ld8(working() + kCount) >= kSlots;
}
}  // namespace

bool picto_pages() { return g_on; }
void set_picto_pages(bool on) {
    g_on = on;
    LOG("[picto] album pages %s", on ? "on" : "off");
}
void picto_page_request(int dir) { g_request = dir; }
bool picto_album_open() { return g_on && r_action() == 0x48; }
int picto_count() { return save_object() ? ld8(working() + kCount) : 0; }
bool picto_album_shown() { return save_object() && r_action() == 0x48; }

// The picto the album shows and what it caught (the gold medal, "BestShotMark_00"): the album viewer (the texture
// cache object above) keeps its counter's number at +0x880 and the count at +0x881; the slot is the order's entry
// before the number (026C5B34). A slot's header (GameCube dSnap, d_snap.cpp, kept per picto): +8 the subject (DSNAP_TYPE_*; figurines
// from 0x49: gallery number = subject - 0x49, dSnap_PhotoIndex2TableIndex), +5 the shot's flaws (mResultDetail:
// 1 turned away / out of view, 2 partly hidden), +7 a colour picto. The album shows the medal for a gallery
// subject in colour without flaws (026C5D84). Returns subject | flaws << 8 | colour << 16, or -1 (no album / picto).
int picto_subject() {
    if (r_action() != 0x48 || !save_object()) return -1;
    uint32_t mgr = ld32(kTextures);
    uint32_t ui = mgr ? ld32(mgr + 0x1D8) : 0;
    if (ui < 0x10000000 || ui >= 0x50000000) return -1;
    // +0x880 is the counter's number (1 .. count): the picto shown is the one before it (026C5ED8 lays the five
    // buttons out around it, 026C5B34 wraps below 0)
    int count = ld8(ui + 0x881), pos = (int)ld8(ui + 0x880) - 1;
    if (count <= 0 || count > (int)kSlots) return -1;
    if (pos < 0) pos += count;
    if (pos >= count) return -1;
    int slot = (int8_t)ld8(working() + kOrder + pos);
    if (slot < 0 || slot >= (int)kSlots) return -1;
    uint32_t s = working() + slot * kSlot;
    if (!ld8(s + kSlotUsed)) return -1;
    return (int)(ld32(s + 8) & 0xFF) | ld8(s + 5) << 8 | (ld8(s + 7) == 1 ? 1 << 16 : 0);
}

// filter_pad (camera.cpp): in the album ZL / L turn the pages (the album has no use for them); after a switch
// ZR is pressed to the Picto Box and back (g_reopen)
void picto_pad(input::PadState& s) {
    using clk = std::chrono::steady_clock;
    static clk::time_point since;
    static int step = 0;
    static clk::time_point a_since;
    static bool a_on = false;
    if (g_press_a) {  // let go for a moment, then pressed for ~100 ms
        if (!a_on) a_on = true, a_since = clk::now();
        float ms = std::chrono::duration<float, std::milli>(clk::now() - a_since).count();
        if (ms < 60) s.buttons &= ~input::kA;
        else if (ms < 160) s.buttons |= input::kA;
        else g_press_a = false, a_on = false;
    }
    int r = g_reopen.load();
    if (r) {
        if (r != step) step = r, since = clk::now();
        float ms = std::chrono::duration<float, std::milli>(clk::now() - since).count();
        uint8_t a = r_action();
        s.buttons &= ~(input::kZR | input::kZL | input::kL);
        if (ms > 1500) g_reopen = 0;                                // (something else came up)
        else if (r == 1 || r == 3) {
            if (a == (r == 1 ? 0x49 : 0x48)) g_reopen = r + 1;     // there: let go
            else s.buttons |= input::kZR;
        } else if (ms > 60) g_reopen = r == 2 ? 3 : 0;
        return;
    }
    step = 0;
    static uint32_t held = 0;
    const uint32_t keys = input::kZL | input::kL;
    if (!picto_album_open()) {
        held = 0;
        return;
    }
    uint32_t down = s.buttons & keys & ~held;
    held = s.buttons & keys;
    if (down & input::kZL) g_request = -1;
    if (down & input::kL) g_request = 1;
    s.buttons &= ~keys;
}
int picto_page_current() { return g_on ? g_cur_shown.load() : -1; }
int picto_page_count() { return g_on ? g_count_shown.load() : 0; }
int picto_page_used() { return g_on ? g_used_shown.load() : 0; }

// game main thread, frame start (interp.cpp): the app's page switch
void picto_pages_service(Cpu* c) {
    static bool env = getenv("WWHD_PICTO_PAGES") && atoi(getenv("WWHD_PICTO_PAGES"));
    if (env && !g_on) set_picto_pages(true);
    int dir = g_request.exchange(0);
    if (!g_on || !save_object()) return;
    if (g.file >= 0 && g.loads != ss::load_count()) detach();  // a save state loaded: read again when needed
    if (g.file < 0 && (dir || picto_album_open())) ensure_attached();
    if (dir && g.file >= 0) switch_to(c, g.cur + dir);
}

}  // namespace mods

// the Picto Box's aim, each frame: a picto taken into a full album gets a new page first
extern "C" void f_026C1974_orig(Cpu* c);
extern "C" void hook_026C1974(Cpu* c) {
    using namespace mods;
    if (g_on && save_object() && shot_into_full_album()) {
        uint32_t r3 = c->r[3];
        LOG("[picto] picto taken into a full album (buttons %08X): a new page", ld32(ld32(kAimPad) + 0x18));
        ensure_attached();
        if (g.file >= 0) switch_to(c, (int)g.pages.size());
        if (g.file >= 0 && ld8(working() + kCount) < kSlots) g_press_a = true;
        c->r[3] = r3;
    }
    f_026C1974_orig(c);
}

// a file loaded: its album copied into the working album (r3 the save object's album part, r4 the file)
extern "C" void f_02720D34_orig(Cpu* c);
extern "C" void hook_02720D34(Cpu* c) {
    int file = (int)c->r[4];
    f_02720D34_orig(c);
    LOG("[picto] file %d loaded (in play: %u)", file, mods::save_object() ? ld8(mods::save_object() + mods::kFileIndex) : 99u);
    mods::detach();  // read again in play, when needed
}

// the game saves: the working album copied into the file's block (then written to its files)
extern "C" void f_02721B34_orig(Cpu* c);
extern "C" void hook_02721B34(Cpu* c) {
    int file = (int)c->r[4];
    f_02721B34_orig(c);
    LOG("[picto] file %d saved by the game", file);
    if (mods::g_on && file >= 0 && file <= 2) mods::commit(file);
}
