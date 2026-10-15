#include "game_archive.h"

#include <cstdio>
#include <cstring>

namespace game_archive {

bool read_file(const std::string& path, Bytes& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    out.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

bool yaz0(const Bytes& in, Bytes& out) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4) != 0) {
        out = in;
        return true;
    }
    out.assign(be32(&in[4]), 0);
    size_t s = 16, d = 0;
    while (d < out.size() && s < in.size()) {
        uint8_t code = in[s++];
        for (int bit = 0; bit < 8 && d < out.size(); bit++, code <<= 1) {
            if (code & 0x80) {
                if (s >= in.size()) return false;
                out[d++] = in[s++];
                continue;
            }
            if (s + 1 >= in.size()) return false;
            uint32_t b1 = in[s], b2 = in[s + 1];
            s += 2;
            size_t dist = ((b1 & 0x0F) << 8 | b2) + 1, len = b1 >> 4;
            if (len == 0) {
                if (s >= in.size()) return false;
                len = in[s++] + 0x12;
            } else {
                len += 2;
            }
            if (dist > d) return false;
            for (size_t i = 0; i < len && d < out.size(); i++, d++) out[d] = out[d - dist];
        }
    }
    return d == out.size();
}

bool sarc_each(const Bytes& b, const std::function<void(const char*, const uint8_t*, size_t)>& fn) {
    if (b.size() < 0x14 || memcmp(b.data(), "SARC", 4) != 0) return false;
    uint32_t hdr = be16(&b[4]), data = be32(&b[0xC]);
    size_t p = hdr;
    if (p + 12 > b.size() || memcmp(&b[p], "SFAT", 4) != 0) return false;
    uint32_t n = be16(&b[p + 6]);
    size_t nodes = p + 12, names = nodes + 16 * n + 8;
    if (names > b.size() || memcmp(&b[nodes + 16 * n], "SFNT", 4) != 0) return false;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t* e = &b[nodes + 16 * i];
        uint32_t attr = be32(e + 4), start = be32(e + 8), end = be32(e + 12);
        if (!(attr >> 24)) continue;
        size_t o = names + (attr & 0xFFFFFF) * 4;
        if (o >= b.size() || (size_t)data + end > b.size() || start > end) continue;
        fn((const char*)&b[o], &b[data + start], end - start);
    }
    return true;
}

bool sarc_file(const Bytes& b, const char* name, Bytes& out) {
    bool found = false;
    sarc_each(b, [&](const char* n, const uint8_t* d, size_t size) {
        if (found || strcmp(n, name) != 0) return;
        out.assign(d, d + size);
        found = true;
    });
    return found;
}

}  // namespace game_archive
