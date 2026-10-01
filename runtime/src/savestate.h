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
struct SlotInfo {
    bool used = false;
    bool compatible = true;
    std::string when;  // local time of the save
    std::string area;  // stage name, if known
};
SlotInfo slot_info(int slot);           // 1..5
void request_save(int slot);
void request_load(int slot);
std::string last_message();             // short status for the title bar ("" when stale)

// ---- game thread: call at the frame boundary (top of the per-frame function) ----
void service(Cpu* c);

// guest memory reader used while validating a snapshot (reads the snapshot's memory, not the live one)
uint32_t snap_ld32(uint32_t ea);

}  // namespace ss
