// Wii U disc images (.wud, and .wux: deduplicated sectors): partitions, the game partition's file
// table and its files. Same logic as tools/wudextract.py (itself a port of Cemu's WUD/FST code,
// MPL-2.0). Used by the Android app to extract the user's own image on the device.
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "crypto.h"

namespace disc {

// "16 raw bytes or 32 hex digits" (surrounding whitespace ignored); false if neither
bool parse_key(const std::string& data, uint8_t out[16]);

class Image {
public:
    // `fd` stays owned by the caller and must stay open; false and `err` on failure
    bool open(int fd, const uint8_t discKey[16], const uint8_t commonKey[16], std::string& err);
    std::string title_id() const { return titleId_; }

    struct File {
        std::string path;  // e.g. "code/cking.rpx"
        uint64_t size;
    };
    const std::vector<File>& files() const { return files_; }

    // writes the game partition's files under `outDir` (code/, content/, meta/). `progress` gets
    // the bytes written so far, the total and the current file; returning false cancels.
    bool extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                 std::string& err);

    ~Image();

private:
    struct Entry {
        std::string path;
        bool dir;
        uint8_t flags;
        uint32_t offset, size;
        uint16_t cluster;
    };
    struct Fst {
        uint64_t base = 0;
        uint32_t offsetFactor = 1;
        std::vector<std::pair<uint32_t, uint8_t>> clusters;  // (offset in sectors, hash mode)
        std::vector<Entry> entries;
        std::unique_ptr<Aes128Dec> aes;
    };
    bool read(uint64_t offset, void* out, size_t n);
    bool read_fst(Fst& f, uint64_t base, const uint8_t key[16], std::string& err);
    // `sink` gets the file's bytes in order; false from it stops
    bool read_file(const Fst& f, const Entry& e, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err);

    int fd_ = -1;
    bool wux_ = false;
    uint32_t sectorSize_ = 0;
    uint64_t sectorBase_ = 0, size_ = 0;
    std::vector<uint32_t> index_;
    std::string titleId_;
    Fst gm_;
    std::vector<File> files_;
};

}  // namespace disc
