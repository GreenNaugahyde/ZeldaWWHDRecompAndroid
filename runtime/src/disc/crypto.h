// AES-128-CBC decryption and SHA-1, as the Wii U disc format needs them (disc/wud.cpp). Small
// portable implementations: Android offers no crypto library to native code.
#pragma once
#include <cstddef>
#include <cstdint>

namespace disc {

class Aes128Dec {
public:
    explicit Aes128Dec(const uint8_t key[16]);
    // CBC: decrypts `n` bytes (a multiple of 16) in place; `iv` is updated to continue the chain
    void cbc_decrypt(uint8_t* data, size_t n, uint8_t iv[16]) const;

private:
    void decrypt_block(const uint8_t in[16], uint8_t out[16]) const;
    uint32_t rk_[44];       // decryption round keys (equivalent inverse cipher)
    uint8_t ekBytes_[176];  // the encryption key schedule as bytes (the ARMv8 AES instructions derive their own)
    bool hw_ = false;       // the CPU has AES instructions
};

void sha1(const uint8_t* data, size_t n, uint8_t out[20]);

}  // namespace disc
