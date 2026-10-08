// Cemu .wua archive reader and extractor (see wua.h).
#include "wua.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "zstd.h"

namespace disc {
namespace {

constexpr uint64_t kBlock = 0x10000;  // uncompressed bytes per data block
constexpr uint32_t kBlocksPerRecord = 16, kRecordSize = 8 + 2 * kBlocksPerRecord;
constexpr uint32_t kFooterSize = 0x90, kMagic = 0x169F52D6, kVersion = 0x61BF3A01;

uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | p[i];
    return v;
}
uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

bool mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') {
            std::string d = path.substr(0, i);
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

}  // namespace

bool Archive::read(uint64_t offset, void* out, size_t n) const {
    auto* o = (uint8_t*)out;
    while (n > 0) {
        ssize_t r = pread(fd_, o, n, (off_t)offset);
        if (r <= 0) return false;
        o += r;
        offset += (uint64_t)r;
        n -= (size_t)r;
    }
    return true;
}

bool Archive::block_at(uint64_t i, uint64_t& pos, uint32_t& size) const {
    uint64_t rec = i / kBlocksPerRecord;
    if ((rec + 1) * kRecordSize > records_.size()) return false;
    const uint8_t* r = records_.data() + rec * kRecordSize;
    pos = be64(r);
    for (uint32_t j = 0; j < i % kBlocksPerRecord; j++) pos += be16(r + 8 + 2 * j) + 1u;
    size = be16(r + 8 + 2 * (i % kBlocksPerRecord)) + 1u;
    return pos + size <= dataSize_;
}

void Archive::collect(const std::vector<Node>& tree, uint32_t dir, const std::string& prefix, int depth) {
    if (depth > 32) return;
    const Node& d = tree[dir];
    for (uint64_t c = d.a; c < d.a + d.b && c < tree.size(); c++) {
        const Node& n = tree[c];
        std::string path = prefix + n.name;
        if (n.file) files_.push_back({path, n.a, n.b});
        else collect(tree, (uint32_t)c, path + "/", depth + 1);
    }
}

bool Archive::open(int fd, std::string& err) {
    fd_ = fd;
    struct stat st{};
    if (fstat(fd, &st) != 0 || (uint64_t)st.st_size < kFooterSize) return err = "the archive can't be read", false;
    uint64_t fileSize = (uint64_t)st.st_size;
    uint8_t f[kFooterSize];
    if (!read(fileSize - kFooterSize, f, sizeof f)) return err = "the archive can't be read", false;
    if (be32(f + 0x8C) != kMagic || be32(f + 0x88) != kVersion) return err = "not a Cemu .wua archive (or an unknown version)", false;
    if (be64(f + 0x80) != fileSize) return err = "the archive is incomplete", false;
    uint64_t sec[6][2];
    for (int i = 0; i < 6; i++) {
        sec[i][0] = be64(f + i * 16);
        sec[i][1] = be64(f + i * 16 + 8);
        if (sec[i][0] > fileSize || sec[i][1] > fileSize - sec[i][0]) return err = "the archive is damaged", false;
    }
    dataOffset_ = sec[0][0];
    dataSize_ = sec[0][1];
    // offset records, names and file tree: small (tens of KiB)
    if (sec[1][1] > (64u << 20) || sec[2][1] > (64u << 20) || sec[3][1] > (64u << 20)) return err = "the archive is damaged", false;
    records_.resize(sec[1][1]);
    std::vector<uint8_t> names(sec[2][1]), raw(sec[3][1]);
    if (!read(sec[1][0], records_.data(), records_.size()) || !read(sec[2][0], names.data(), names.size()) ||
        !read(sec[3][0], raw.data(), raw.size()))
        return err = "the archive can't be read", false;

    auto name_at = [&](uint32_t o, std::string& out) {
        if (o == 0x7FFFFFFF) return out.clear(), true;  // the root
        if (o >= names.size()) return false;
        uint32_t len = names[o++];
        if (len & 0x80) {
            if (o >= names.size()) return false;
            len = (len & 0x7F) | (uint32_t)names[o++] << 7;
        }
        if (len > names.size() - o) return false;
        out.assign((const char*)names.data() + o, len);
        // only plain names: the extracted paths must stay inside the output folder
        return !out.empty() && out != "." && out != ".." && out.find('/') == std::string::npos && out.find('\\') == std::string::npos &&
               out.find('\0') == std::string::npos;
    };
    std::vector<Node> tree(raw.size() / 16);
    for (size_t i = 0; i < tree.size(); i++) {
        const uint8_t* e = raw.data() + i * 16;
        uint32_t tn = be32(e);
        Node& n = tree[i];
        n.file = tn >> 31;
        if (!name_at(tn & 0x7FFFFFFF, n.name) && i != 0) return err = "the archive's file names are damaged", false;
        if (n.file) {
            n.a = be32(e + 4) | (uint64_t)be16(e + 14) << 32;  // offset
            n.b = be32(e + 8) | (uint64_t)be16(e + 12) << 32;  // size
            uint64_t cap = (uint64_t)records_.size() / kRecordSize * kBlocksPerRecord * kBlock;  // uncompressed bytes
            if (n.a > cap || n.b > cap - n.a) return err = "the archive is damaged", false;
        } else {
            n.a = be32(e + 4);
            n.b = be32(e + 8);
        }
    }
    if (tree.empty() || tree[0].file) return err = "the archive is empty", false;

    // the game title: a top-level folder 00050000xxxxxxxx_vN with code/cking.rpx (updates are
    // 0005000E, DLC 0005000C: not used)
    const Node& root = tree[0];
    for (uint64_t c = root.a; c < root.a + root.b && c < tree.size(); c++) {
        const Node& t = tree[c];
        if (t.file || t.name.compare(0, 8, "00050000") != 0 || t.name.size() < 16) continue;
        files_.clear();
        collect(tree, (uint32_t)c, "", 0);
        if (std::any_of(files_.begin(), files_.end(), [](const File& x) { return x.path == "code/cking.rpx"; })) {
            titleId_ = t.name.substr(0, 16);
            return true;
        }
    }
    files_.clear();
    err = "the archive holds no Wind Waker HD game (its code/cking.rpx)";
    return false;
}

bool Archive::extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                      std::string& err) {
    // as disc::Image::extract: files in parallel, biggest first, progress from one thread at a time
    std::vector<const File*> todo;
    uint64_t total = 0;
    for (auto& f : files_) {
        todo.push_back(&f);
        total += f.size;
    }
    std::sort(todo.begin(), todo.end(), [](const File* a, const File* b) { return a->size > b->size; });
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    std::atomic<bool> stop{false};
    std::mutex mu;
    std::string firstErr;
    auto report = [&](const std::string& name) {
        std::lock_guard<std::mutex> lk(mu);
        if (progress && !progress(done.load(), total, name)) stop = true;
    };
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lk(mu);
        if (firstErr.empty()) firstErr = why;
        stop = true;
    };
    auto worker = [&] {
        std::unique_ptr<ZSTD_DCtx, size_t (*)(ZSTD_DCtx*)> dctx(ZSTD_createDCtx(), ZSTD_freeDCtx);
        std::vector<uint8_t> packed(kBlock), block(kBlock);
        for (size_t i; !stop && (i = next++) < todo.size();) {
            const File& f = *todo[i];
            std::string dst = outDir + "/" + f.path;
            if (!mkdirs(dst.substr(0, dst.find_last_of('/')))) return fail("cannot create a folder for " + f.path);
            std::string tmp = dst + ".part";
            FILE* out = fopen(tmp.c_str(), "wb");
            if (!out) return fail("cannot write " + f.path);
            std::string ferr;
            uint64_t pos = f.offset, end = f.offset + f.size, sinceReport = 0;
            while (pos < end && !stop) {
                uint64_t at;
                uint32_t stored;
                if (!block_at(pos / kBlock, at, stored)) {
                    ferr = "the archive is damaged (" + f.path + ")";
                    break;
                }
                if (!read(dataOffset_ + at, packed.data(), stored)) {
                    ferr = "the archive can't be read";
                    break;
                }
                size_t have = kBlock;
                const uint8_t* src = packed.data();
                if (stored < kBlock) {  // a full-size block is stored as is
                    have = ZSTD_decompressDCtx(dctx.get(), block.data(), kBlock, packed.data(), stored);
                    if (ZSTD_isError(have)) {
                        ferr = "the archive is damaged (" + f.path + ")";
                        break;
                    }
                    src = block.data();
                }
                size_t within = (size_t)(pos % kBlock), n = (size_t)std::min<uint64_t>(kBlock - within, end - pos);
                if (within + n > have) {
                    ferr = "the archive is damaged (" + f.path + ")";
                    break;
                }
                if (fwrite(src + within, 1, n, out) != n) {
                    ferr = "cannot write " + f.path + " (storage full?)";
                    break;
                }
                pos += n;
                done += n;
                if ((sinceReport += n) >= (4u << 20)) {
                    sinceReport = 0;
                    report(f.path);
                }
            }
            bool ok = (fclose(out) == 0) && pos == end && ferr.empty();
            if (!ok) {
                remove(tmp.c_str());
                if (!stop || !ferr.empty()) fail(ferr.empty() ? "cancelled" : ferr);
                return;
            }
            if (rename(tmp.c_str(), dst.c_str()) != 0) return fail("cannot write " + f.path);
            report(f.path);
        }
    };
    unsigned n = std::max(1u, std::min(6u, std::thread::hardware_concurrency()));
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < n; t++) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    if (stop) {
        err = firstErr.empty() ? "cancelled" : firstErr;
        return false;
    }
    if (progress) progress(total, total, "");
    return true;
}

}  // namespace disc
