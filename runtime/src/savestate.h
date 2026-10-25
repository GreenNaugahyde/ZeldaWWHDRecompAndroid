// Save states: snapshot the whole running game (guest memory, guest threads, HLE state) into one
// of 5 slots and restore it later, also in a fresh process. See savestate.cpp for the design.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

#include "ppc.h"

namespace ss {

// little serializer for host-side state (each module writes its own section)
struct Writer {
    std::vector<uint8_t> b;
    void bytes(const void* p, size_t n) { b.insert(b.end(), (const uint8_t*)p, (const uint8_t*)p + n); }
    template <class T> void pod(const T& v) {
        static_assert(std::is_trivially_copyable<T>::value, "pod");
        bytes(&v, sizeof v);
    }
    void u8(uint8_t v) { pod(v); }
    void u32(uint32_t v) { pod(v); }
    void u64(uint64_t v) { pod(v); }
    void str(const std::string& s) { u32((uint32_t)s.size()); bytes(s.data(), s.size()); }
};

struct Reader {
    const uint8_t* p = nullptr;
    const uint8_t* e = nullptr;
    bool ok = true;
    Reader() = default;
    Reader(const void* d, size_t n) : p((const uint8_t*)d), e((const uint8_t*)d + n) {}
    bool bytes(void* out, size_t n) {
        if (!ok || (size_t)(e - p) < n) { ok = false; if (out) memset(out, 0, n); return false; }
        if (out) memcpy(out, p, n);
        p += n;
        return true;
    }
    template <class T> T pod() {
        T v{};
        bytes(&v, sizeof v);
        return v;
    }
    uint8_t u8() { return pod<uint8_t>(); }
    uint32_t u32() { return pod<uint32_t>(); }
    uint64_t u64() { return pod<uint64_t>(); }
    std::string str() {
        uint32_t n = u32();
        if (!ok || (size_t)(e - p) < n) { ok = false; return {}; }
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    bool at_end() const { return p == e; }
};

// ---- UI / test API (any thread) ----
constexpr int kSlots = 5;
constexpr int kAutoSlots = 2;  // slots kSlots+1.. : written by the auto save only, taking turns
constexpr int kLastSlot = kSlots + kAutoSlots + 1;  // written when the app goes to the background
constexpr int kAllSlots = kLastSlot;
struct SlotInfo {
    bool used = false;
    bool compatible = true;
    std::string when;  // local time of the save
    int64_t created = 0;  // ... as seconds since 1970 (the app shows it in the device's language)
    std::string area;  // stage name, if known
    int controller = 0;  // controls when saved: 0 unknown (older states), 1 GamePad, 2 Pro Controller
};
SlotInfo slot_info(int slot);           // 1..kAllSlots
void request_save(int slot);            // 1..kSlots
void request_load(int slot);            // 1..kAllSlots
// the app goes to the background (UI thread): saves into kLastSlot; returns once the game has been
// stopped and its state taken (the rest is written in the background), at most ~1 s
void save_on_leave();
int writes_pending();                   // saves taken but not yet on disk (quitting waits for them)
void set_auto_interval(int minutes);    // 0: no auto saves
int auto_interval();
std::string last_message();             // short status for the title bar ("" when stale)
// the app's save state picker at start: armed before the game starts, the game thread then waits at
// the first frame boundary after the shader cache replay (before the intro) until a state loaded
// (or failed to) or the hold is released; start_hold_state: 0 off, 1 armed, 2 the game is waiting
void set_start_hold(bool on);
int start_hold_state();
// the stage loaded: 1 the title screen (sea_T), 2 the file select (Name), 3 on from the file select
// (a new game's opening, or loading), 4 none yet (booting), 0 another
int front_screen();
// how many states have been loaded (game thread; mods that keep host-side copies read theirs again)
unsigned load_count();

// ---- game thread: call at the frame boundary (top of the per-frame function) ----
void service(Cpu* c);

// host code about to write guest memory from the kernel (read() into it): a save's memory capture
// that is still running keeps the old content first (a protected page would make read() fail)
void prepare_host_write(uint32_t addr, uint32_t size);

// crash log: what the copy-on-write fault handler saw (async-signal-safe enough: snprintf)
int fault_note(char* buf, size_t n);

// guest memory reader used while validating a snapshot (reads the snapshot's memory, not the live one)
uint32_t snap_ld32(uint32_t ea);

}  // namespace ss
