#pragma once
// A BFRES (version 3, big-endian) read and grown in place: groups, relative offsets, appended data. Shared by the
// player model converter (gcmodel.cpp) and the loot swords (hd_sword.cpp).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "gcm_io.h"
namespace gcmodel {
struct Bfres {
    Bytes d;
    uint8_t u8(size_t o) const { return d.at(o); }
    uint16_t u16(size_t o) const { return be16(&d.at(o)); }
    int16_t s16(size_t o) const { return bes16(&d.at(o)); }
    uint32_t u32(size_t o) const { return be32(&d.at(o)); }
    float f32(size_t o) const { return bef32(&d.at(o)); }
    size_t rel(size_t o) const {
        const int32_t v = (int32_t)u32(o);
        return v ? (size_t)((int64_t)o + v) : 0;
    }
    void set_rel(size_t o, size_t target) { put32(&d[o], (uint32_t)(int32_t)((int64_t)target - (int64_t)o)); }
    std::string str(size_t o) const {
        std::string s;
        if (!o) return s;
        while (o < d.size() && d[o]) s += (char)d[o++];
        return s;
    }
    std::vector<std::pair<std::string, size_t>> group(size_t o) const {
        std::vector<std::pair<std::string, size_t>> r;
        if (!o) return r;
        const uint32_t n = u32(o + 4);
        for (uint32_t i = 1; i <= n; i++) r.push_back({str(rel(o + 8 + i * 16 + 8)), rel(o + 8 + i * 16 + 12)});
        return r;
    }
    size_t section(int i) const { return rel(0x20 + i * 4); }
    size_t append(const uint8_t* data, size_t n, size_t align) {
        d.resize((d.size() + align - 1) / align * align, 0);
        const size_t at = d.size();
        d.insert(d.end(), data, data + n);
        put32(&d[0xC], (uint32_t)d.size());
        return at;
    }
    void putf(size_t o, float v) { putf32(&d[o], v); }
};
}  // namespace gcmodel
