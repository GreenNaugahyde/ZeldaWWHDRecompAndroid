// Wii U disc image reader and extractor (see wud.h).
#include "wud.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

namespace disc {
namespace {

constexpr uint64_t kSector = 0x8000;
constexpr uint32_t kBlockSize = 0x10000, kBlockHashSize = 0x400, kBlockFileSize = 0xFC00;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
uint32_t le32(const uint8_t* p) { return (uint32_t)p[3] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[1] << 8 | p[0]; }

bool mkdirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); i++)
        if (i == path.size() || path[i] == '/') {
            std::string d = path.substr(0, i);
            if (mkdir(d.c_str(), 0755) != 0 && errno != EEXIST) return false;
        }
    return true;
}

}  // namespace

bool parse_key(const std::string& raw, uint8_t out[16]) {
    if (raw.size() == 16) {
        memcpy(out, raw.data(), 16);
        return true;
    }
    std::string s = raw;
    while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
    size_t b = 0;
    while (b < s.size() && isspace((unsigned char)s[b])) b++;
    s = s.substr(b);
    if (s.size() != 32) return false;
    for (int i = 0; i < 16; i++) {
        auto nib = [](char c) { return isdigit((unsigned char)c) ? c - '0' : (tolower(c) >= 'a' && tolower(c) <= 'f') ? tolower(c) - 'a' + 10 : -1; };
        int hi = nib(s[2 * i]), lo = nib(s[2 * i + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return true;
}

Image::~Image() = default;

bool Image::read(uint64_t offset, void* out, size_t n) {
    auto* o = (uint8_t*)out;
    while (n > 0) {
        uint64_t real;
        size_t len;
        if (!wux_) {
            real = offset;
            len = n;
        } else {
            uint64_t sec = offset / sectorSize_, within = offset % sectorSize_;
            if (sec >= index_.size()) return false;
            real = sectorBase_ + (uint64_t)index_[sec] * sectorSize_ + within;
            len = (size_t)std::min<uint64_t>(n, sectorSize_ - within);
        }
        ssize_t r = pread(fd_, o, len, (off_t)real);
        if (r <= 0) return false;
        o += r;
        offset += r;
        n -= r;
    }
    return true;
}

bool Image::read_fst(Fst& f, uint64_t base, const uint8_t key[16], std::string& err) {
    uint8_t ph[0x60];
    if (!read(base, ph, sizeof ph)) return err = "cannot read a partition header", false;
    uint32_t fstSize = be32(ph + 0x14), fstSector = be32(ph + 0x18);
    std::vector<uint8_t> data((fstSize + 15) & ~15u);
    if (!read(base + fstSector * kSector, data.data(), data.size())) return err = "cannot read a file table", false;
    uint8_t iv[16] = {};
    f.aes = std::make_unique<Aes128Dec>(key);
    f.aes->cbc_decrypt(data.data(), data.size(), iv);
    if (data.size() < 0x20 || be32(data.data()) != 0x46535400) return err = "file table can't be decrypted (wrong key?)", false;
    f.base = base;
    f.offsetFactor = be32(data.data() + 4);
    uint32_t ncluster = be32(data.data() + 8);
    size_t ft = 0x20 + (size_t)ncluster * 0x20;
    if (ft + 12 > data.size()) return err = "corrupt file table", false;
    for (uint32_t i = 0; i < ncluster; i++) {
        const uint8_t* c = data.data() + 0x20 + i * 0x20;
        f.clusters.push_back({be32(c), c[0x14]});
    }
    uint32_t n = be32(data.data() + ft + 8);
    size_t names = ft + (size_t)n * 0x10;
    if (names > data.size()) return err = "corrupt file table", false;
    std::vector<std::pair<std::string, uint32_t>> stack = {{"", n}};  // (directory path, end index)
    for (uint32_t i = 0; i < n; i++) {
        while (stack.size() > 1 && i >= stack.back().second) stack.pop_back();
        const uint8_t* p = data.data() + ft + (size_t)i * 0x10;
        uint32_t tno = be32(p);
        Entry e;
        e.flags = (uint8_t)(tno >> 24);
        e.offset = be32(p + 4);
        e.size = be32(p + 8);
        e.cluster = be16(p + 14);
        e.dir = e.flags & 1;
        std::string name;
        if (i) {
            size_t o = names + (tno & 0xFFFFFF);
            while (o < data.size() && data[o]) name += (char)data[o++];
        }
        std::string parent = stack.back().first;
        e.path = parent.empty() ? name : parent + "/" + name;
        if (e.cluster >= f.clusters.size() && !e.dir) return err = "corrupt file table", false;
        f.entries.push_back(e);
        if (e.dir && i) stack.push_back({e.path, e.size});
    }
    return true;
}

bool Image::read_file(const Fst& f, const Entry& e, const std::function<bool(const uint8_t*, size_t)>& sink, std::string& err) {
    uint8_t mode = f.clusters[e.cluster].second;
    uint64_t pos = (uint64_t)e.offset * f.offsetFactor, remaining = e.size;
    uint64_t base = f.base + (uint64_t)f.clusters[e.cluster].first * kSector;
    if (mode == 2) {  // hashed: 64 KiB blocks of 1 KiB hashes + 63 KiB data
        std::vector<uint8_t> raw(kBlockSize);
        uint64_t blk = pos / kBlockFileSize, within = pos % kBlockFileSize;
        while (remaining > 0) {
            if (!read(base + blk * kBlockSize, raw.data(), kBlockSize)) return err = "read error in " + e.path, false;
            uint8_t iv[16] = {};
            f.aes->cbc_decrypt(raw.data(), kBlockHashSize, iv);  // the hashes
            const uint8_t* h0 = raw.data() + (blk % 16) * 20;
            memcpy(iv, h0, 16);
            f.aes->cbc_decrypt(raw.data() + kBlockHashSize, kBlockFileSize, iv);
            uint8_t sum[20];
            sha1(raw.data() + kBlockHashSize, kBlockFileSize, sum);
            if (memcmp(sum, h0, 20) != 0) return err = "hash mismatch in " + e.path + " (damaged image?)", false;
            uint64_t take = std::min<uint64_t>(remaining, kBlockFileSize - within);
            if (!sink(raw.data() + kBlockHashSize + within, (size_t)take)) return false;
            remaining -= take;
            within = 0;
            blk++;
        }
    } else {  // plain: CBC over the whole cluster, the IV of sector 0 is the cluster index
        uint64_t blk = pos / kSector, within = pos % kSector;
        uint8_t iv[16] = {};
        if (blk == 0) {
            iv[0] = (uint8_t)(e.cluster >> 8);
            iv[1] = (uint8_t)e.cluster;
        } else if (!read(base + blk * kSector - 16, iv, 16)) {
            return err = "read error in " + e.path, false;
        }
        constexpr uint64_t kChunk = 64 * kSector;
        std::vector<uint8_t> buf(kChunk);
        while (remaining > 0) {
            uint64_t want = std::min<uint64_t>(kChunk, (within + remaining + kSector - 1) / kSector * kSector);
            if (!read(base + blk * kSector, buf.data(), (size_t)want)) return err = "read error in " + e.path, false;
            f.aes->cbc_decrypt(buf.data(), (size_t)want, iv);
            uint64_t take = std::min<uint64_t>(remaining, want - within);
            if (!sink(buf.data() + within, (size_t)take)) return false;
            remaining -= take;
            within = 0;
            blk += want / kSector;
        }
    }
    return true;
}

bool Image::open(int fd, const uint8_t discKey[16], const uint8_t commonKey[16], std::string& err) {
    fd_ = fd;
    uint8_t hdr[32];
    if (pread(fd, hdr, 32, 0) != 32) return err = "cannot read the image", false;
    if (le32(hdr) == 0x30585557 && le32(hdr + 4) == 0x1099D02E) {  // "WUX0"
        wux_ = true;
        sectorSize_ = le32(hdr + 8);
        memcpy(&size_, hdr + 16, 8);
        if (!sectorSize_ || sectorSize_ > (1u << 24)) return err = "corrupt WUX header", false;
        uint64_t n = (size_ + sectorSize_ - 1) / sectorSize_;
        index_.resize(n);
        if (pread(fd, index_.data(), n * 4, 32) != (ssize_t)(n * 4)) return err = "cannot read the WUX index", false;
        for (auto& v : index_) v = le32((const uint8_t*)&v);
        uint64_t off = 32 + 4 * n;
        sectorBase_ = (off + sectorSize_ - 1) / sectorSize_ * sectorSize_;
    } else {
        struct stat st;
        if (fstat(fd, &st) != 0) return err = "cannot read the image", false;
        size_ = st.st_size;
    }
    uint8_t magic[4];
    if (!read(kSector * 2, magic, 4) || be32(magic) != 0xCC549EB9) return err = "not a Wii U disc image", false;
    std::vector<uint8_t> pt(kSector);
    if (!read(kSector * 3, pt.data(), kSector)) return err = "cannot read the partition table", false;
    uint8_t iv[16] = {};
    Aes128Dec(discKey).cbc_decrypt(pt.data(), pt.size(), iv);
    if (be32(pt.data()) != 0xCCA6E67B) return err = "the disc key doesn't fit this image", false;
    uint32_t nparts = be32(pt.data() + 0x1C);
    std::vector<std::pair<std::string, uint64_t>> parts;
    for (uint32_t i = 0; i < nparts && 0x800 + (i + 1) * 0x80 <= pt.size(); i++) {
        const uint8_t* ent = pt.data() + 0x800 + i * 0x80;
        std::string name((const char*)ent, strnlen((const char*)ent, 31));
        parts.push_back({name, (uint64_t)be32(ent + 0x20) * kSector});
    }
    int si = -1, gm = -1;
    for (int i = 0; i < (int)parts.size(); i++) {
        if (si < 0 && parts[i].first.rfind("SI", 0) == 0) si = i;
        if (gm < 0 && parts[i].first.rfind("GM", 0) == 0) gm = i;
    }
    if (si < 0 || gm < 0) return err = "no game partition on this disc", false;
    Fst siFst;
    if (!read_fst(siFst, parts[si].second, discKey, err)) return false;
    char tikName[32];
    snprintf(tikName, sizeof tikName, "%02x/title.tik", gm);
    const Entry* tikEntry = nullptr;
    for (auto& e : siFst.entries)
        if (e.path == tikName) tikEntry = &e;
    if (!tikEntry) return err = "no ticket for the game partition", false;
    std::vector<uint8_t> tik;
    if (!read_file(siFst, *tikEntry, [&](const uint8_t* p, size_t n) { tik.insert(tik.end(), p, p + n); return true; }, err))
        return false;
    if (tik.size() < 0x1E4) return err = "corrupt ticket", false;
    uint8_t titleKey[16], tiv[16] = {};
    memcpy(titleKey, tik.data() + 0x1BF, 16);
    memcpy(tiv, tik.data() + 0x1DC, 8);  // title id, zero padded
    char tid[17];
    for (int i = 0; i < 8; i++) snprintf(tid + 2 * i, 3, "%02x", tik[0x1DC + i]);
    titleId_ = tid;
    Aes128Dec(commonKey).cbc_decrypt(titleKey, 16, tiv);
    if (!read_fst(gm_, parts[gm].second, titleKey, err)) {
        err = "the game's files can't be decrypted (wrong common key?)";
        return false;
    }
    files_.clear();
    for (auto& e : gm_.entries)
        if (!e.dir && !(e.flags & 0x80)) files_.push_back({e.path, e.size});
    return true;
}

bool Image::extract(const std::string& outDir, const std::function<bool(uint64_t, uint64_t, const std::string&)>& progress,
                    std::string& err) {
    // files are extracted in parallel (reads are positioned, the cipher is read only); progress
    // is reported from one thread at a time
    std::vector<const Entry*> todo;
    uint64_t total = 0;
    for (auto& e : gm_.entries)
        if (!e.dir && !(e.flags & 0x80)) {
            todo.push_back(&e);
            total += e.size;
        }
    std::sort(todo.begin(), todo.end(), [](const Entry* a, const Entry* b) { return a->size > b->size; });  // big ones first
    std::atomic<size_t> next{0};
    std::atomic<uint64_t> done{0};
    std::atomic<bool> stop{false};
    std::mutex mu;  // progress callback and the first error
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
        for (size_t i; !stop && (i = next++) < todo.size();) {
            const Entry& e = *todo[i];
            std::string dst = outDir + "/" + e.path;
            if (!mkdirs(dst.substr(0, dst.find_last_of('/')))) return fail("cannot create a folder for " + e.path);
            std::string tmp = dst + ".part";
            FILE* out = fopen(tmp.c_str(), "wb");
            if (!out) return fail("cannot write " + e.path);
            std::string ferr;
            uint64_t sinceReport = 0;
            bool ok = read_file(gm_, e, [&](const uint8_t* p, size_t n) {
                if (fwrite(p, 1, n, out) != n) {
                    ferr = "cannot write " + e.path + " (storage full?)";
                    return false;
                }
                done += n;
                if ((sinceReport += n) >= (4u << 20)) {
                    sinceReport = 0;
                    report(e.path);
                }
                return !stop.load();
            }, ferr);
            ok = (fclose(out) == 0) && ok;
            if (!ok) {
                remove(tmp.c_str());
                if (!stop || !ferr.empty()) fail(ferr.empty() ? "cancelled" : ferr);
                return;
            }
            if (rename(tmp.c_str(), dst.c_str()) != 0) return fail("cannot write " + e.path);
            report(e.path);
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
