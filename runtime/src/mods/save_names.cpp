// Mods > Save file names (the app's SaveNames.java): a save file's player name changed in play, and names longer than the
// game's 8 characters (up to 16).
//
// The names in memory (HD only; the GameCube's dSv_player_info_c name at save + 0x15C stays empty): after the save data,
// at save + 0x1340, one entry of 0x40 per file (0..2): a UTF-16 string object {u32 data pointer, vtable, u32 capacity in
// characters with the end, its own 9 characters} and at +0x20 a byte string object the same way (capacity 18). The
// game's text reads the name through the UTF-16 one's data pointer, as far as its capacity allows; its Save writes what
// that gives into cking.sav's 18-byte name (the file select shows 8 of them). Tried on the Mi 9 (EUR, 2026-11-04):
//   - the pointer aimed at 16 characters elsewhere with capacity 17: the text shows all 16 (16 x W stay in the box, the
//     line wraps), the Save keeps the first 9 (no end, the file select shows 8);
//   - capacity 9: the text shows 8, the Save keeps 8 and the end.
// So a name goes in as: its first 8 characters in the file's own buffer (pointer on it, capacity 9: what the game keeps
// when it saves), and a longer one in a place of its own (kArea, the end of the player model area) with the pointer
// aimed there and capacity 17. The long name holds while the file's own name starts with its first 8 characters (a new
// game in that file, or a save state from before the change, has another name: then the file's own name shows). The
// game writing a name of its own through the pointer (a new game's name entry) shows as the place's text changed: that
// name then becomes the file's own.
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

#include "mods.h"
#include "../release.h"
#include "../runtime.h"

namespace mods {
namespace {
const release::Data kInfoPointer{0x10114B90};
constexpr uint32_t kSaveOffset = 0xC92C;  // touch_state.cpp
constexpr uint32_t kTable = 0x1340, kEntry = 0x40;
constexpr uint32_t kWideVtable = 0x10140C80, kByteVtable = 0x10140CB0;  // USA addresses
constexpr uint32_t kOwn = 9, kLongCap = kSaveNameMax + 1;
constexpr int kFiles = 3;

std::mutex g_mu;
std::array<std::u16string, kFiles> g_long;   // the app's names over 8 characters ("" none)
std::array<std::u16string, kFiles> g_rename;  // renames asked for, not done yet
std::array<bool, kFiles> g_renamePending{};
std::atomic<uint32_t> g_gen{1};
std::array<std::u16string, kFiles> g_written;  // what this run put into each long name's place

uint32_t table() {
    uint32_t base = ld32(kInfoPointer);
    if (base < 0x10000000 || base > 0x50000000) return 0;
    uint32_t t = base + kSaveOffset + kTable;
    for (int f = 0; f < kFiles; f++) {
        uint32_t e = t + f * kEntry;
        if (ld32(e + 4) != release::data(kWideVtable) || ld32(e + 0x24) != release::data(kByteVtable)) return 0;
    }
    return t;
}

uint32_t place(int file) { return kSaveNameArea + (uint32_t)file * 0x40; }

std::u16string read16(uint32_t a, uint32_t max) {
    std::u16string s;
    for (uint32_t i = 0; i < max; i++) {
        char16_t ch = (char16_t)ld16(a + 2 * i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

void write16(uint32_t a, const std::u16string& s, uint32_t cap) {
    for (uint32_t i = 0; i < cap; i++) st16(a + 2 * i, i < s.size() ? (uint16_t)s[i] : 0);
}

// the file's own buffers: its first 8 characters (the byte one as Latin-1 when every character fits, outside Japan)
void write_own(uint32_t e, const std::u16string& name) {
    write16(e + 0xC, name.substr(0, kOwn - 1), kOwn);
    st32(e, e + 0xC);
    st32(e + 8, kOwn);
    bool latin = release::id() != release::Id::JPN;
    for (char16_t ch : name) latin = latin && ch < 0x100;
    if (!latin) return;
    const uint32_t cap = ld32(e + 0x28);
    if (cap < 2 || cap > 0x40) return;
    for (uint32_t i = 0; i < cap; i++) st8(e + 0x2C + i, i < name.size() && i + 1 < cap ? (uint8_t)name[i] : 0);
    st32(e + 0x20, e + 0x2C);
}
}  // namespace

void set_long_save_names(const std::array<std::u16string, 3>& names) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (int f = 0; f < kFiles; f++) g_long[f] = names[f].size() > kOwn - 1 ? names[f].substr(0, kSaveNameMax) : u"";
    g_gen++;
}

void rename_save_file(int file, const std::u16string& name) {
    if (file < 0 || file >= kFiles || name.empty()) return;
    std::lock_guard<std::mutex> lk(g_mu);
    g_rename[file] = name.substr(0, kSaveNameMax);
    g_renamePending[file] = true;
    g_gen++;
}

bool save_names(std::array<std::u16string, 3>& out) {
    uint32_t t = table();
    if (!t) return false;
    for (int f = 0; f < kFiles; f++) {
        uint32_t e = t + f * kEntry, p = ld32(e);
        out[f] = p == place(f) ? read16(p, kSaveNameMax) : read16(e + 0xC, kOwn);
    }
    return true;
}

// game main thread, frame start (interp.cpp)
void save_names_service() {
    uint32_t t = table();
    if (!t) return;
    std::array<std::u16string, kFiles> longs, renames;
    std::array<bool, kFiles> pending{};
    {
        std::lock_guard<std::mutex> lk(g_mu);
        longs = g_long;
        renames = g_rename;
        pending = g_renamePending;
        g_renamePending = {};
    }
    for (int f = 0; f < kFiles; f++) {
        const uint32_t e = t + f * kEntry, at = place(f);
        if (pending[f]) {
            write_own(e, renames[f]);
            LOG("[savenames] file %d renamed (%zu characters)", f + 1, renames[f].size());
        }
        // the game wrote a name of its own through the pointer (a new game's name entry): it becomes the file's own
        if (ld32(e) == at) {
            std::u16string there = read16(at, kSaveNameMax);
            if (!there.empty() && there != g_written[f]) {
                LOG("[savenames] file %d: the game gave it a name of its own", f + 1);
                write_own(e, there);
                g_written[f].clear();
            }
        }
        const std::u16string& l = longs[f];
        const std::u16string own = read16(e + 0xC, kOwn);
        const bool holds = !l.empty() && own.size() >= kOwn - 1 && own.compare(0, kOwn - 1, l, 0, kOwn - 1) == 0;
        if (holds) {
            if (g_written[f] != l || read16(at, kSaveNameMax) != l) {
                write16(at, l, kLongCap);
                g_written[f] = l;
            }
            if (ld32(e) != at || ld32(e + 8) != kLongCap) {
                st32(e, at);
                st32(e + 8, kLongCap);
                LOG("[savenames] file %d: long name in", f + 1);
            }
        } else if (ld32(e) != e + 0xC || ld32(e + 8) != kOwn) {
            // (a save state from another name, or the long name taken away: the file's own)
            if (ld32(e) == at) write16(e + 0xC, read16(e + 0xC, kOwn).substr(0, kOwn - 1), kOwn);
            st32(e, e + 0xC);
            st32(e + 8, kOwn);
            LOG("[savenames] file %d: own name", f + 1);
        }
    }
}

}  // namespace mods
