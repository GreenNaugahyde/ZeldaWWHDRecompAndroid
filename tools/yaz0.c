/* Yaz0 decompressor used by tools/shaderprep.py (loaded with ctypes; built on demand). */
#include <stdint.h>
#include <stddef.h>

/* returns bytes written, or -1 on malformed input */
long yaz0_decompress(const uint8_t* src, size_t srclen, uint8_t* dst, size_t dstlen) {
    if (srclen < 16 || src[0] != 'Y' || src[1] != 'a' || src[2] != 'z' || src[3] != '0') return -1;
    size_t s = 16, d = 0;
    while (d < dstlen && s < srclen) {
        uint8_t code = src[s++];
        for (int bit = 0; bit < 8 && d < dstlen; bit++, code <<= 1) {
            if (code & 0x80) {
                if (s >= srclen) return -1;
                dst[d++] = src[s++];
            } else {
                if (s + 1 >= srclen) return -1;
                uint32_t b1 = src[s], b2 = src[s + 1];
                s += 2;
                size_t dist = ((b1 & 0x0F) << 8 | b2) + 1;
                size_t len = b1 >> 4;
                if (len == 0) {
                    if (s >= srclen) return -1;
                    len = src[s++] + 0x12;
                } else {
                    len += 2;
                }
                if (dist > d) return -1;
                for (size_t i = 0; i < len && d < dstlen; i++, d++) dst[d] = dst[d - dist];
            }
        }
    }
    return (long)d;
}
