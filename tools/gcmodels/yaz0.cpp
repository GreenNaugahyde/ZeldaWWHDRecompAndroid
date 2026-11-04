// Yaz0 compression for gcmodels (Link.szs): yaz0 IN OUT [ALIGNMENT]
//   c++ -std=c++17 -O2 tools/gcmodels/yaz0.cpp -o yaz0
// Greedy matches from hash chains over the 4 KB window (lengths 3..273), as Nintendo's own files.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: yaz0 IN OUT [ALIGNMENT]\n");
        return 2;
    }
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> in{std::istreambuf_iterator<char>(f), {}};
    const uint32_t align = argc > 3 ? (uint32_t)strtoul(argv[3], nullptr, 0) : 0;
    const size_t n = in.size();
    std::vector<uint8_t> out = {'Y', 'a', 'z', '0', (uint8_t)(n >> 24), (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n,
                                (uint8_t)(align >> 24), (uint8_t)(align >> 16), (uint8_t)(align >> 8), (uint8_t)align, 0, 0, 0, 0};
    const int HASH = 1 << 16;
    std::vector<int32_t> head(HASH, -1), prev(n, -1);
    auto hash = [&](size_t p) { return ((in[p] << 8) ^ (in[p + 1] << 4) ^ in[p + 2]) & (HASH - 1); };
    auto insert = [&](size_t p) {
        if (p + 2 >= n) return;
        int h = hash(p);
        prev[p] = head[h];
        head[h] = (int32_t)p;
    };
    size_t p = 0;
    while (p < n) {
        size_t codePos = out.size();
        out.push_back(0);
        uint8_t code = 0;
        for (int bit = 0; bit < 8 && p < n; bit++) {
            size_t bestLen = 0, bestDist = 0;
            if (p + 2 < n) {
                int chain = 0;
                for (int32_t q = head[hash(p)]; q >= 0 && p - q <= 0x1000 && chain < 256; q = prev[q], chain++) {
                    size_t l = 0, maxl = std::min<size_t>(0x111, n - p);
                    while (l < maxl && in[q + l] == in[p + l]) l++;
                    if (l > bestLen) {
                        bestLen = l;
                        bestDist = p - q;
                        if (l == maxl) break;
                    }
                }
            }
            if (bestLen >= 3) {
                size_t d = bestDist - 1;
                if (bestLen >= 0x12) {
                    out.push_back((uint8_t)(d >> 8));
                    out.push_back((uint8_t)d);
                    out.push_back((uint8_t)(bestLen - 0x12));
                } else {
                    out.push_back((uint8_t)(((bestLen - 2) << 4) | (d >> 8)));
                    out.push_back((uint8_t)d);
                }
                for (size_t k = 0; k < bestLen; k++) insert(p + k);
                p += bestLen;
            } else {
                code |= 0x80 >> bit;
                out.push_back(in[p]);
                insert(p);
                p++;
            }
        }
        out[codePos] = code;
    }
    std::ofstream(argv[2], std::ios::binary).write((const char*)out.data(), out.size());
    printf("%zu -> %zu\n", n, out.size());
    return 0;
}
