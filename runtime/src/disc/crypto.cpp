// AES-128 decryption (FIPS-197, table-driven equivalent inverse cipher) and SHA-1 (FIPS 180-4).
#include "crypto.h"

#include <cstring>

#if defined(__aarch64__) && defined(__linux__)  // Android: hardware AES, detected at run time
#define WWHD_AES_HW 1
#include <arm_neon.h>
#include <sys/auxv.h>
#ifndef HWCAP_AES
#define HWCAP_AES (1 << 3)
#endif
#endif

namespace disc {
namespace {

uint8_t g_sbox[256], g_inv[256];
uint32_t g_td[4][256];  // inverse round tables: InvSubBytes + InvMixColumns
uint32_t g_tdk[4][256];  // InvMixColumns alone (round key conversion)
bool g_init = false;

uint8_t xtime(uint8_t x) { return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1B : 0)); }
uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    while (b) {
        if (b & 1) r ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return r;
}

void init_tables() {
    if (g_init) return;
    // S-box from the multiplicative inverse in GF(2^8) and the affine map
    uint8_t p = 1, q = 1;
    do {
        p = p ^ (uint8_t)(p << 1) ^ ((p & 0x80) ? 0x1B : 0);  // p *= 3
        q ^= q << 1;
        q ^= q << 2;
        q ^= q << 4;
        if (q & 0x80) q ^= 0x09;  // q /= 3
        uint8_t x = q ^ (uint8_t)((q << 1) | (q >> 7)) ^ (uint8_t)((q << 2) | (q >> 6)) ^ (uint8_t)((q << 3) | (q >> 5)) ^
                    (uint8_t)((q << 4) | (q >> 4));
        g_sbox[p] = x ^ 0x63;
    } while (p != 1);
    g_sbox[0] = 0x63;
    for (int i = 0; i < 256; i++) g_inv[g_sbox[i]] = (uint8_t)i;
    for (int i = 0; i < 256; i++) {
        uint8_t s = g_inv[i];
        uint32_t t = (uint32_t)mul(s, 14) << 24 | (uint32_t)mul(s, 9) << 16 | (uint32_t)mul(s, 13) << 8 | mul(s, 11);
        uint32_t k = (uint32_t)mul((uint8_t)i, 14) << 24 | (uint32_t)mul((uint8_t)i, 9) << 16 | (uint32_t)mul((uint8_t)i, 13) << 8 |
                     mul((uint8_t)i, 11);
        for (int r = 0; r < 4; r++) {
            g_td[r][i] = (t >> (8 * r)) | (t << (32 - 8 * r));
            g_tdk[r][i] = (k >> (8 * r)) | (k << (32 - 8 * r));
        }
    }
    g_init = true;
}

inline uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
inline void put_be32(uint8_t* p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

}  // namespace

Aes128Dec::Aes128Dec(const uint8_t key[16]) {
    init_tables();
    // encryption key schedule
    uint32_t ek[44];
    for (int i = 0; i < 4; i++) ek[i] = be32(key + 4 * i);
    uint8_t rcon = 1;
    for (int i = 4; i < 44; i++) {
        uint32_t t = ek[i - 1];
        if (i % 4 == 0) {
            t = (uint32_t)g_sbox[(t >> 16) & 0xFF] << 24 | (uint32_t)g_sbox[(t >> 8) & 0xFF] << 16 | (uint32_t)g_sbox[t & 0xFF] << 8 |
                g_sbox[t >> 24];
            t ^= (uint32_t)rcon << 24;
            rcon = xtime(rcon);
        }
        ek[i] = ek[i - 4] ^ t;
    }
    // decryption keys: reversed round order, InvMixColumns on the middle rounds
    for (int r = 0; r <= 10; r++)
        for (int c = 0; c < 4; c++) {
            uint32_t w = ek[(10 - r) * 4 + c];
            if (r > 0 && r < 10)
                w = g_tdk[0][w >> 24] ^ g_tdk[1][(w >> 16) & 0xFF] ^ g_tdk[2][(w >> 8) & 0xFF] ^ g_tdk[3][w & 0xFF];
            rk_[r * 4 + c] = w;
        }
    for (int i = 0; i < 44; i++) put_be32(ekBytes_ + 4 * i, ek[i]);
#ifdef WWHD_AES_HW
    hw_ = (getauxval(AT_HWCAP) & HWCAP_AES) != 0;
#endif
}

#ifdef WWHD_AES_HW
// ARMv8 AES instructions: AESD = AddRoundKey + InvShiftRows + InvSubBytes, AESIMC = InvMixColumns.
// Round keys of the equivalent inverse cipher: the encryption keys in reverse, InvMixColumns
// applied to the middle ones (with AESIMC itself)
__attribute__((target("aes"))) static void cbc_decrypt_hw(const uint8_t* ek, uint8_t* data, size_t n, uint8_t iv[16]) {
    uint8x16_t k[11];
    for (int r = 0; r <= 10; r++) {
        uint8x16_t w = vld1q_u8(ek + 16 * (10 - r));
        k[r] = (r > 0 && r < 10) ? vaesimcq_u8(w) : w;
    }
    uint8x16_t prev = vld1q_u8(iv);
    for (size_t o = 0; o + 16 <= n; o += 16) {
        uint8x16_t c = vld1q_u8(data + o), s = c;
        for (int r = 0; r < 9; r++) s = vaesimcq_u8(vaesdq_u8(s, k[r]));
        s = veorq_u8(vaesdq_u8(s, k[9]), k[10]);
        vst1q_u8(data + o, veorq_u8(s, prev));
        prev = c;
    }
    vst1q_u8(iv, prev);
}
#endif

void Aes128Dec::decrypt_block(const uint8_t in[16], uint8_t out[16]) const {
    const uint32_t* rk = rk_;
    uint32_t s0 = be32(in) ^ rk[0], s1 = be32(in + 4) ^ rk[1], s2 = be32(in + 8) ^ rk[2], s3 = be32(in + 12) ^ rk[3];
    for (int r = 1; r < 10; r++) {
        rk += 4;
        uint32_t t0 = g_td[0][s0 >> 24] ^ g_td[1][(s3 >> 16) & 0xFF] ^ g_td[2][(s2 >> 8) & 0xFF] ^ g_td[3][s1 & 0xFF] ^ rk[0];
        uint32_t t1 = g_td[0][s1 >> 24] ^ g_td[1][(s0 >> 16) & 0xFF] ^ g_td[2][(s3 >> 8) & 0xFF] ^ g_td[3][s2 & 0xFF] ^ rk[1];
        uint32_t t2 = g_td[0][s2 >> 24] ^ g_td[1][(s1 >> 16) & 0xFF] ^ g_td[2][(s0 >> 8) & 0xFF] ^ g_td[3][s3 & 0xFF] ^ rk[2];
        uint32_t t3 = g_td[0][s3 >> 24] ^ g_td[1][(s2 >> 16) & 0xFF] ^ g_td[2][(s1 >> 8) & 0xFF] ^ g_td[3][s0 & 0xFF] ^ rk[3];
        s0 = t0, s1 = t1, s2 = t2, s3 = t3;
    }
    rk += 4;
    auto last = [&](uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t k) {
        return ((uint32_t)g_inv[a >> 24] << 24 | (uint32_t)g_inv[(b >> 16) & 0xFF] << 16 | (uint32_t)g_inv[(c >> 8) & 0xFF] << 8 |
                g_inv[d & 0xFF]) ^
               k;
    };
    put_be32(out, last(s0, s3, s2, s1, rk[0]));
    put_be32(out + 4, last(s1, s0, s3, s2, rk[1]));
    put_be32(out + 8, last(s2, s1, s0, s3, rk[2]));
    put_be32(out + 12, last(s3, s2, s1, s0, rk[3]));
}

void Aes128Dec::cbc_decrypt(uint8_t* data, size_t n, uint8_t iv[16]) const {
#ifdef WWHD_AES_HW
    if (hw_) {
        cbc_decrypt_hw(ekBytes_, data, n, iv);
        return;
    }
#endif
    uint8_t prev[16], cur[16];
    memcpy(prev, iv, 16);
    for (size_t o = 0; o + 16 <= n; o += 16) {
        memcpy(cur, data + o, 16);
        decrypt_block(cur, data + o);
        for (int i = 0; i < 16; i++) data[o + i] ^= prev[i];
        memcpy(prev, cur, 16);
    }
    memcpy(iv, prev, 16);
}

void sha1(const uint8_t* data, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    auto rol = [](uint32_t x, int s) { return (x << s) | (x >> (32 - s)); };
    auto block = [&](const uint8_t* b) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++) w[i] = be32(b + 4 * i);
        for (int i = 16; i < 80; i++) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) f = (bb & c) | (~bb & d), k = 0x5A827999;
            else if (i < 40) f = bb ^ c ^ d, k = 0x6ED9EBA1;
            else if (i < 60) f = (bb & c) | (bb & d) | (c & d), k = 0x8F1BBCDC;
            else f = bb ^ c ^ d, k = 0xCA62C1D6;
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d, d = c, c = rol(bb, 30), bb = a, a = t;
        }
        h[0] += a, h[1] += bb, h[2] += c, h[3] += d, h[4] += e;
    };
    size_t full = n / 64 * 64;
    for (size_t o = 0; o < full; o += 64) block(data + o);
    uint8_t tail[128] = {};
    size_t rest = n - full;
    memcpy(tail, data + full, rest);
    tail[rest] = 0x80;
    size_t tl = rest + 9 <= 64 ? 64 : 128;
    uint64_t bits = (uint64_t)n * 8;
    for (int i = 0; i < 8; i++) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    block(tail);
    if (tl == 128) block(tail + 64);
    for (int i = 0; i < 5; i++) put_be32(out + 4 * i, h[i]);
}

}  // namespace disc
