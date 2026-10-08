// Cemu's Wii U archives (.wua): decrypted titles in a ZArchive file. The footer at the end locates
// the sections: the data as 64 KiB blocks, each zstd-compressed or stored as is; offset records
// (the file position of every 16th block plus the 16 blocks' sizes); the names; and the file tree
// (16-byte entries, a directory listing a run of child entries). Every title in the archive is a
// folder at the top ("0005000010143500_v0" holding code/, content/, meta/). Used by the Android
// app to extract the game from the user's own archive, as disc::Image does for disc images.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace disc {

class Archive {
public:
    // `fd` stays owned by the caller and must stay open; false and `err` on failure (also when
    // the archive holds no game title with code/cking.rpx)
    bool open(int fd, std::string& err);
    std::string title_id() const { return titleId_; }

    struct File {
        std::string path;  // inside the title, e.g. "code/cking.rpx"
        uint64_t offset, size;
    };
    const std::vector<File>& files() const { return files_; }

    // writes the title's files under `outDir` (code/, content/, meta/). `progress` gets the bytes
    // written so far, the total and the current file; returning false cancels.
    bool extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                 std::string& err);

private:
    struct Node {
        bool file;
        std::string name;
        uint64_t a, b;  // file: offset, size; directory: first child, count
    };
    bool read(uint64_t offset, void* out, size_t n) const;
    // the file position and stored size of data block `i`
    bool block_at(uint64_t i, uint64_t& pos, uint32_t& size) const;
    void collect(const std::vector<Node>& tree, uint32_t dir, const std::string& prefix, int depth);

    int fd_ = -1;
    uint64_t dataOffset_ = 0, dataSize_ = 0;
    std::vector<uint8_t> records_;
    std::string titleId_;
    std::vector<File> files_;
};

}  // namespace disc
