// Game > Instant text: message boxes show their whole text at once, as the TWWHD Randomizer's
// "Instant text boxes" (tweaks.cpp make_all_text_instant + asm/patches/b_button_skips_text.asm), but
// switchable at run time and without changing any game file. Off: every site below leaves the game as it is.
//
// Sites (tools/recomp/hooks_mods.txt, USA addresses):
// - @026014F4 `lbz r9, 0x696(r5)` in the message window's typing step (026014C8): byte +0x696 of the window
//   state (r5) is the draw type (0 types letter by letter, 1 instant). The game sets it from the message's
//   attribute (02601848, attribute byte 2 == 1) or the text's own instant on / off codes (025FB148 / 025FB15C);
//   the randomizer sets that attribute in every message. Here the byte is set before the step reads it, so
//   the other readers (025FAA80, 026017A4) see it as well.
// - the instruction after each of the randomizer's nine `lwz rD, 0x54(rA)` -> `lwz rD, 0x50(rA)` patches
//   (B skips text): rD is loaded again from +0x50.
// - @026F7EA0 / @026F802C, two `beq` the randomizer turns into nop: the box closes without waiting for the
//   continue arrow to finish appearing, and the next box starts without waiting for it to fade (cr0.eq cleared).
// Pauses (Game > Instant text > No pauses in texts): the texts' wait code 0E 0001 0006 0002 <frames> (UTF-16BE,
// group 1 tag 6, the randomizer deletes it from the message files) gets 0 frames in every message file (MSBT)
// in memory: a background scan of 10000000-50000000 for "MsgStdBn", parsing TXT2 strings, when a box types and
// the last scan is a minute ago; the old values are kept and written back when it goes off (where still a 0 in
// a wait code).
#include <atomic>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "mods.h"
#include "ppc.h"
#include "runtime.h"

namespace mods {
namespace {
std::atomic<bool> g_on{false};

std::atomic<bool> g_no_pauses{false};
std::atomic<bool> g_scanning{false};
std::atomic<int64_t> g_last_scan{-1000000};  // steady seconds
std::mutex g_mu;
std::unordered_map<uint32_t, uint16_t> g_patched;  // address of a wait's frames -> the game's value

inline bool on() { return g_on.load(std::memory_order_relaxed); }
int64_t now_s() {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// one MSBT at a (its header checked): the waits in its TXT2 strings set to 0 frames
int patch_msbt(uint32_t a, uint32_t end) {
    if (ld16(a + 8) != 0xFEFF) return 0;  // big endian only
    const uint32_t size = ld32(a + 0x12);
    const int sections = ld16(a + 0xE);
    if (size < 0x30 || size > 0x2000000 || a + size > end || sections <= 0 || sections > 16) return 0;
    int n = 0;
    uint32_t sec = a + 0x20;
    for (int k = 0; k < sections && sec + 16 <= a + size; k++) {
        const uint32_t ssize = ld32(sec + 4), data = sec + 16;
        if (data + ssize > a + size) break;
        if (!memcmp(ppc_ptr(sec), "TXT2", 4) && ssize >= 4) {
            const uint32_t count = ld32(data);
            for (uint32_t i = 0; i < count && 4 + 4 * i + 4 <= ssize; i++) {
                uint32_t p = data + ld32(data + 4 + 4 * i);
                const uint32_t stop = data + ssize;
                while (p + 2 <= stop) {
                    const uint16_t ch = ld16(p);
                    if (ch == 0) break;
                    if (ch == 0x0E) {
                        if (p + 8 > stop) break;
                        const uint16_t grp = ld16(p + 2), tag = ld16(p + 4), psz = ld16(p + 6);
                        if (grp == 1 && tag == 6 && psz == 2 && p + 10 <= stop) {
                            const uint32_t f = p + 8;
                            if (!g_patched.count(f) && ld16(f) != 0) {
                                g_patched[f] = ld16(f);
                                st16(f, 0);
                                n++;
                            }
                        }
                        p += 8 + psz;
                    } else if (ch == 0x0F) {
                        p += 6;
                    } else {
                        p += 2;
                    }
                }
            }
        }
        sec = data + ((ssize + 15) & ~15u);
    }
    return n;
}

void scan() {
    constexpr uint32_t lo = 0x10000000, hi = 0x50000000;
    const auto t0 = std::chrono::steady_clock::now();
    int files = 0, waits = 0;
    {
        std::lock_guard<std::mutex> l(g_mu);
        const uint8_t* base = ppc_ptr(lo);
        const size_t len = hi - lo;
        const uint8_t* p = base;
        while (true) {
            p = (const uint8_t*)memmem(p, len - (p - base), "MsgStdBn", 8);
            if (!p) break;
            const uint32_t a = lo + (uint32_t)(p - base);
            if ((a & 3) == 0) {
                int w = patch_msbt(a, hi);
                files++;
                waits += w;
            }
            p += 8;
        }
    }
    LOG("[mods] text pauses: %d message files, %d waits set to 0 (%lld ms)", files, waits,
        (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count());
}

void restore() {
    std::lock_guard<std::mutex> l(g_mu);
    int n = 0;
    for (auto& [f, v] : g_patched)
        if (ld16(f) == 0 && ld16(f - 8) == 0x0E && ld16(f - 6) == 1 && ld16(f - 4) == 6 && ld16(f - 2) == 2) {
            st16(f, v);
            n++;
        }
    g_patched.clear();
    if (n) LOG("[mods] text pauses: %d waits back", n);
}

// a box types: scan for message files now and then while the pauses are to go
void maybe_scan() {
    if (!g_no_pauses.load(std::memory_order_relaxed) || g_scanning.load(std::memory_order_relaxed)) return;
    const int64_t t = now_s();
    if (t - g_last_scan.load(std::memory_order_relaxed) < 60) return;
    if (g_scanning.exchange(true)) return;
    g_last_scan = t;
    std::thread([] {
        if (on() && g_no_pauses.load()) scan();
        g_scanning = false;
    }).detach();
}
void update_pauses() {
    if (on() && g_no_pauses.load()) {
        g_last_scan = -1000000;  // at the next box
    } else {
        std::thread([] {
            while (g_scanning.load()) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            restore();
        }).detach();
    }
}
inline void reload50(Cpu* c, int rd, int ra) {
    if (on()) c->r[rd] = ld32(c->r[ra] + 0x50);
}
inline void no_wait(Cpu* c) {
    if (on()) c->cr[2] = 0;  // cr0.eq: the beq isn't taken
}
}  // namespace

bool instant_text() { return on(); }
void set_instant_text(bool v) {
    g_on = v;
    LOG("[mods] instant text %s", v ? "on" : "off");
    update_pauses();
}
bool text_pauses_off() { return g_no_pauses.load(std::memory_order_relaxed); }
void set_text_pauses_off(bool v) {
    g_no_pauses = v;
    LOG("[mods] no pauses in texts %s", v ? "on" : "off");
    update_pauses();
}

}  // namespace mods

extern "C" void site_026014F4(Cpu* c) {
    if (!mods::instant_text()) return;
    st8(c->r[5] + 0x696, 1);
    mods::maybe_scan();
}
extern "C" void site_026BA648(Cpu* c) { mods::reload50(c, 0, 3); }
extern "C" void site_026FCF1C(Cpu* c) { mods::reload50(c, 8, 30); }
extern "C" void site_026FD294(Cpu* c) { mods::reload50(c, 0, 30); }
extern "C" void site_026FD3E0(Cpu* c) { mods::reload50(c, 12, 11); }
extern "C" void site_026FD9BC(Cpu* c) { mods::reload50(c, 9, 30); }
extern "C" void site_026FDB44(Cpu* c) { mods::reload50(c, 9, 30); }
extern "C" void site_026FE62C(Cpu* c) { mods::reload50(c, 10, 8); }
extern "C" void site_026FE81C(Cpu* c) { mods::reload50(c, 8, 30); }
extern "C" void site_026FEB20(Cpu* c) { mods::reload50(c, 0, 30); }
extern "C" void site_026F7EA0(Cpu* c) { mods::no_wait(c); }
extern "C" void site_026F802C(Cpu* c) { mods::no_wait(c); }
