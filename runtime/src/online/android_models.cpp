// Online co-op: the other players' own player models (the fork's GameCube player models, Mods > Player models).
//
// Upstream draws every other player as copies of this game's Link's models (coop_pose.cpp make_kit), so they share
// this game's Link's model data: with a player model on here, everyone would wear it. Instead:
//   - each game tells its party which model it plays with (kModelMsg 0x60: "WWPM1", the model pack's folder, a hash of its
//     Link.szs; "" = HD Link), again to whoever joins; upstream's games drop the unknown type;
//   - a model this game has too (same folder, same hash) is loaded as an object archive of its own, LkM0..LkM3
//     (/vol/content/Common/Object/LkM<n>.szs: the pack's Link.szs with its Link.bfres renamed, as the game's loader
//     asks for "<archive>.bfres", Yaz0-packed again), and so is HD Link (the played pack's own Link.szs) when this
//     game's Link wears a model;
//   - the game finds an archive's GameCube file list by the CRC-32 of its name in two tables of the executable (USA
//     1009BB1C and 100DE8F4, 629 entries {CRC, list, count}; loader 026045xx, getRes 026069xx); 80 of the entries
//     name archives the game doesn't have: four of them become LkM0..LkM3 with Link's list (set_tables);
//   - a puppet's copies of Link's models are made from the same files of its player's archive (kit_data, called from
//     coop_pose_android.cpp), made again when that changes; a model this game doesn't have: HD Link.
//   - optionally (Game > Online: "Share my player model" / "Accept other players' models", both off at first) a model
//     this game doesn't have comes over the connection: the receiver asks (kModelAsk), the sender sends what its
//     model's Link.szs has other than the game's own Link.szs (per file the 64-byte blocks that differ, zstd-packed;
//     kModelPart, ~1 KB each; through a server what its upload limit leaves beside the poses, ~2.4 KB/s), the receiver
//     makes the model from its own game file, checks it and keeps it (<online>/received/<hash>.sarc).
// What the puppets hold (sword, items) still comes from this game's Link's archive.
#include <algorithm>
#include <cmath>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <tuple>
#include <vector>
#include <zstd.h>

#include "gcmodel/gcm_io.h"
#include "guest_addr.h"
#include "mods/coop_names.h"
#include "mods/coop_net.h"
#include "mods/coop_pose.h"
#include "mods/coop_sync.h"
#include "mods/mods.h"
#include "online/compat/online_compat.h"
#include "online/compat/overlay/hostui.h"
#include "packs.h"
#include "runtime.h"

namespace mods::coop {
void android_trail_received(int from, const uint8_t* d, size_t n);  // (coop_pose.cpp)
}
namespace mods::coop::net::srv {
bool android_sent(uint64_t& bytes, uint64_t& packets);  // (coop_net_server_android.cpp)
double android_room();  // (coop_net_server_android.cpp)
}

namespace android_models {
void kit_again(Cpu* c, int slot);  // (coop_pose_android.cpp)
bool kit_made(int slot);

namespace {
namespace net = mods::coop::net;
using Bytes = std::vector<uint8_t>;
constexpr net::Msg kModelMsg = (net::Msg)0x60;   // (the fork's: which player model a player plays with)
// (below 0x80: a server takes types with bit 7 for state snapshots; upstream uses 1..31 and 0x7E)
constexpr net::Msg kModelAsk = (net::Msg)0x61;   // "WWPR1", the model's hash: send it to me
constexpr net::Msg kModelPart = (net::Msg)0x62;
constexpr net::Msg kTrailMsg = (net::Msg)0x63;   // 'W' 'S', then frames of the sword trail (send_trail)  // 'W' 'P', u64 hash, u32 total, u32 offset, data
constexpr int kArchives = 4;
constexpr int kSlots = mods::coop::kPuppets;
constexpr uint32_t kLinkFiles = 0x73;  // Link's archive's GameCube file list (the tables' count)
const std::string kHd = "\x01HD";      // (the archive of the game's own Link)

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
uint32_t crc32(const char* s) {
    uint32_t c = 0xFFFFFFFFu;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) c = c & 1 ? (c >> 1) ^ 0xEDB88320u : c >> 1;
    }
    return ~c;
}
bool read_file(const std::string& p, Bytes& out) {
    FILE* f = fopen(p.c_str(), "rb");
    if (!f) return false;
    out.clear();
    uint8_t buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return true;
}
std::string hex64(uint64_t v) {
    char b[20];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}
uint64_t fnv(const Bytes& d) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
    return h;
}
std::string label(const std::string& model) {
    return model.empty() ? "this game's Link" : model == "\x01HD" ? "HD Link" : model[0] == '\x03' ? "a received model " + model.substr(1) : model;
}

// ---- the archive tables (see the top)
constexpr uint32_t kLinkCrc = 0x969E36CF;  // crc32("Link")
constexpr uint32_t kSpare[kArchives] = {0x73CF3A7A, 0xE8D2290B, 0x71DB78B1, 0x8CBEEC0F};
constexpr int kEntries = 629;
bool g_tables = false;
bool set_tables() {
    if (g_tables) return true;
    int done = 0;
    for (uint32_t table : {GD(0x1009BB1C), GD(0x100DE8F4)}) {
        uint32_t link = 0;
        for (int i = 0; i < kEntries && !link; i++)
            if (ld32(table + 12 * i) == kLinkCrc) link = table + 12 * i;
        if (!link) continue;
        for (int n = 0; n < kArchives; n++) {
            const char name[5] = {'L', 'k', 'M', (char)('0' + n), 0};
            const uint32_t want = crc32(name);
            for (int i = 0; i < kEntries; i++) {
                const uint32_t e = table + 12 * i;
                if (ld32(e) != kSpare[n] && ld32(e) != want) continue;
                st32(e, want), st32(e + 4, ld32(link + 4)), st32(e + 8, ld32(link + 8));
                done++;
                break;
            }
        }
    }
    g_tables = done == 2 * kArchives;
    LOG("[coopmodel] archive tables: %d of %d entries set", done, 2 * kArchives);
    return g_tables;
}

// a Link.szs (Yaz0 SARC) as archive `name` (4 letters): its "Link." files renamed (the names' hashes made again, the
// file table sorted by them as SARC wants), packed again
bool sarc_rename(Bytes& d, const char* from, const char* to) {
    const size_t nlen = strlen(from);
    if (strlen(to) != nlen || d.size() < 0x14 || memcmp(d.data(), "SARC", 4)) return false;
    const size_t p = (size_t)(d[4] << 8 | d[5]);
    if (p + 12 > d.size() || memcmp(&d[p], "SFAT", 4)) return false;
    const uint32_t n = (uint32_t)(d[p + 6] << 8 | d[p + 7]), key = be32(&d[p + 8]);
    const size_t nt = p + 12 + (size_t)n * 16 + 8;
    if (nt > d.size()) return false;
    struct E { uint8_t raw[16]; };
    std::vector<E> ents(n);
    int renamed = 0;
    for (uint32_t i = 0; i < n; i++) {
        memcpy(ents[i].raw, &d[p + 12 + i * 16], 16);
        const uint32_t attr = be32(ents[i].raw + 4);
        if (!(attr & 0x01000000)) continue;
        const size_t o = nt + (attr & 0xFFFF) * 4;
        if (o + nlen >= d.size()) continue;
        if (!memcmp(&d[o], from, nlen)) memcpy(&d[o], to, nlen), renamed++;
        uint32_t h = 0;
        for (size_t k = o; k < d.size() && d[k]; k++) h = h * key + (uint32_t)(int32_t)(int8_t)d[k];
        put32(ents[i].raw, h);
    }
    std::sort(ents.begin(), ents.end(), [](const E& a, const E& b) { return be32(a.raw) < be32(b.raw); });
    for (uint32_t i = 0; i < n; i++) memcpy(&d[p + 12 + i * 16], ents[i].raw, 16);
    return renamed > 0;
}
std::string make_archive(const Bytes& szs, const char* name) {
    Bytes d;
    try {
        d = szs.size() >= 4 && !memcmp(szs.data(), "Yaz0", 4) ? gcmodel::yaz0_decode(szs) : szs;
        if (!sarc_rename(d, "Link.", (std::string(name) + ".").c_str())) return "";
        d = gcmodel::yaz0_encode(d, 0x2000);  // (the game's loader takes its object archives Yaz0-packed only)
    } catch (...) {
        return "";
    }
    const std::string dir = online_compat::dir() + "/models";
    mkdir(dir.c_str(), 0700);
    const std::string out = dir + "/" + name + ".szs";
    FILE* f = fopen(out.c_str(), "wb");
    if (!f) return "";
    const bool ok = fwrite(d.data(), 1, d.size(), f) == d.size();
    return fclose(f) == 0 && ok ? out : "";
}
// the Link.szs of the played pack (the game's or a randomized game's permanent_3d.pack)
bool hd_link_szs(Bytes& out) {
    std::string basePack, packsDir;
    mods::player_model_dirs(basePack, packsDir);
    Bytes p;
    if (basePack.empty() || !read_file(basePack, p)) return false;
    try {
        auto files = gcmodel::sarc_read(p);
        auto it = files.find("Link.szs");
        if (it == files.end()) return false;
        out = std::move(it->second);
        return true;
    } catch (...) {
        return false;
    }
}
// a model pack's Link.szs ("" for a name that can't be one)
std::string pack_szs(const std::string& folder) {
    std::string basePack, packsDir;
    mods::player_model_dirs(basePack, packsDir);
    return packsDir.empty() || folder.empty() || folder.find('/') != std::string::npos || folder[0] == '.'
               ? ""
               : packsDir + "/" + folder + "/model/Link.szs";
}
std::mutex g_hash_mu;
std::map<std::string, std::string> g_hashes;  // path, size, time: its hash
std::string hash_of(const std::string& path) {
    struct stat st {};
    if (path.empty() || stat(path.c_str(), &st) != 0) return "";
    const std::string key = path + "\t" + std::to_string((long long)st.st_size) + "\t" + std::to_string((long long)st.st_mtime);
    {
        std::lock_guard<std::mutex> lk(g_hash_mu);
        auto it = g_hashes.find(key);
        if (it != g_hashes.end()) return it->second;
    }
    Bytes d;
    if (!read_file(path, d)) return "";
    const std::string h = hex64(fnv(d));
    std::lock_guard<std::mutex> lk(g_hash_mu);
    g_hashes[key] = h;
    return h;
}

// a model received over the connection, by the hash its player gave
std::string received_path(const std::string& hash) {
    if (hash.size() != 16 || hash.find_first_not_of("0123456789abcdef") != std::string::npos) return "";
    return online_compat::dir() + "/received/" + hash + ".sarc";
}
bool have_received(const std::string& hash) {
    struct stat st {};
    const std::string p = received_path(hash);
    return !p.empty() && stat(p.c_str(), &st) == 0 && st.st_size > 0;
}
bool option(const char* key) {
    std::string v;
    return hostui::get(key, v) && v == "1";
}
constexpr const char* kSendKey = "android.model_send";
constexpr const char* kTakeKey = "android.model_receive";
const std::string kRx = "\x03";  // (a model id: kRx + hash, a received model)

// ---- the archives
struct Archive {
    std::string model;          // a pack's folder, kHd; "" unused
    std::atomic<int> state{0};  // 0 unused, 1 making its file, 2 file made, 3 loading, 4 ready, 5 failed
    std::string file;           // (from state 2: its file)
    uint32_t phase = 0, name = 0;
    std::array<uint32_t, kLinkFiles> data{};
};
Archive g_arc[kArchives];
uint32_t g_step = 0;

uint32_t guest_text(const char* s) {
    const uint32_t n = (uint32_t)strlen(s), b = mem::host_alloc(n + 1);
    for (uint32_t i = 0; i <= n; i++) st8(b + i, (uint8_t)s[i]);
    return b;
}
bool ready(const std::string& model) {
    for (int n = 0; n < kArchives; n++)
        if (g_arc[n].model == model && g_arc[n].state.load() == 4) return true;
    return false;
}
// a model's archive asked for (made on a thread, loaded by the game; archives stay while the game runs: 4 at most)
void ask(const std::string& model) {
    int pick = -1;
    for (int n = 0; n < kArchives; n++) {
        if (g_arc[n].model == model) return;
        if (pick < 0 && g_arc[n].model.empty()) pick = n;
    }
    if (pick < 0) return;  // (more models than archives: the others as HD Link... or the model in this game's Link)
    Archive& a = g_arc[pick];
    a.model = model, a.state = 1;
    LOG("[coopmodel] %s: made into archive LkM%d", label(model).c_str(), pick);
    std::thread([pick, model] {
        Bytes szs;
        const bool have = model == kHd                  ? hd_link_szs(szs)
                          : model.compare(0, 1, kRx) == 0 ? read_file(received_path(model.substr(1)), szs)
                                                          : read_file(pack_szs(model), szs);
        const char nm[5] = {'L', 'k', 'M', (char)('0' + pick), 0};
        const std::string f = have ? make_archive(szs, nm) : "";
        g_arc[pick].file = f;
        if (f.empty()) LOG("[coopmodel] archive %s: could not be made", nm);
        g_arc[pick].state = f.empty() ? 5 : 2;
    }).detach();
}
// one step of each archive being loaded (game thread)
void load_step(Cpu* c) {
    for (int n = 0; n < kArchives; n++) {
        Archive& a = g_arc[n];
        const int s = a.state.load();
        if (s != 2 && s != 3) continue;
        const char nm[5] = {'L', 'k', 'M', (char)('0' + n), 0};
        if (s == 2) {
            packs::add_virtual_file(std::string("Common/Object/") + nm + ".szs", a.file);
            if (!a.phase) a.phase = mem::host_alloc(16), a.name = guest_text(nm);
            a.state = 3;
        }
        const uint32_t r = guest_call(c, GC(0x02520460), {a.phase, a.name});  // dComIfG_resLoad
        if (r < 4) continue;
        if (r >= 5) {
            a.state = 5;
            LOG("[coopmodel] archive %s did not load (%u)", nm, r);
            continue;
        }
        const uint32_t s8 = mem::host_alloc(8);  // the archive's name as a string object (text, vtable)
        st32(s8, a.name), st32(s8 + 4, GD(0x10023B10));
        int files = 0;
        for (uint32_t i = 0; i < kLinkFiles; i++)
            if ((a.data[i] = guest_call(c, GC(0x026066C4), {ld32(GD(0x101F4F28)), s8, i}))) files++;  // dComIfG_getObjectRes
        a.state = files ? 4 : 5;
        LOG("[coopmodel] archive %s (%s): loaded, %d files", nm, label(a.model).c_str(), files);
    }
}

// ---- who plays with what
std::mutex g_mu;
std::map<int, std::string> g_wants;  // player id: their model when this game has it ("" HD Link, or one it doesn't have)
std::map<int, std::string> g_said;   // player id: what they said (its folder; for the log)
std::map<int, std::string> g_hash;   // player id: its model's hash
std::string g_sent = "\x02";         // what this game said last
std::atomic<bool> g_resend{false};
std::string g_kit_with[kSlots];     // what each puppet's kit was made from ("" this game's Link's model data)
std::array<uint32_t, kLinkFiles> g_link{};  // Link's own model data by file
uint32_t g_link_got = ~0u;

std::string my_body() {
    const std::string mine = mods::player_model_in_memory();
    return "WWPM1\t" + mine + "\t" + (mine.empty() ? "" : hash_of(pack_szs(mine)));
}
void received(int from, const uint8_t* d, size_t n) {
    const std::string b((const char*)d, std::min<size_t>(n, 512));
    if (b.compare(0, 6, "WWPM1\t") != 0) return;
    const size_t t = b.find('\t', 6);
    const std::string folder = b.substr(6, t == std::string::npos ? std::string::npos : t - 6);
    const std::string hash = t == std::string::npos ? "" : b.substr(t + 1);
    std::string have;
    if (!folder.empty() && !hash.empty() && hash_of(pack_szs(folder)) == hash) have = folder;
    else if (!folder.empty() && have_received(hash)) have = kRx + hash;
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_said[from] != folder || g_wants[from] != have)
        LOG("[coopmodel] player %d plays with %s%s", from, folder.empty() ? "HD Link" : folder.c_str(),
            folder.empty() || !have.empty() ? "" : " (not here, or another version of it: shown as HD Link)");
    g_said[from] = folder, g_wants[from] = have, g_hash[from] = hash;
}
// the model a puppet slot's player should be shown with ("" this game's Link's model data), its archive asked for
std::string wanted(int slot) {
    const int me = net::my_id(), id = slot < me ? slot : slot + 1;
    std::string want;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        auto it = g_wants.find(id);
        if (it != g_wants.end()) want = it->second;
    }
    // (test aid: WWHD_COOP_MODEL_ALL=<pack folder> shows everyone with that model)
    if (static const char* all = getenv("WWHD_COOP_MODEL_ALL"); all && *all) want = all;
    const std::string mine = mods::player_model_in_memory();
    const std::string model = want.empty() ? (mine.empty() ? "" : kHd) : want == mine ? "" : want;
    if (!model.empty()) ask(model);
    return model;
}
// ---- models over the connection
struct Blob {  // a model's difference from the game's Link.szs, zstd-packed
    std::string hash;
    std::atomic<int> state{0};  // 0 none, 1 making, 2 ready, 3 can't
    Bytes data;
};
Blob g_out;  // this game's model, made once a model is asked for (again when the model changes)
struct Send { int to; std::string hash; uint32_t at = 0; };
std::vector<Send> g_sends;
std::mutex g_send_mu;  // (g_sends: the game thread, and the Online page reading)
struct Take {  // the one model being received
    int from = -1;
    std::string hash, folder;
    uint64_t want = 0;
    uint32_t total = 0, got = 0, asked = 0;
    Bytes data;
    std::atomic<int> state{0};  // 0 none, 1 receiving, 2 making, 3 done, 4 failed
} g_take;
std::mutex g_take_mu;
std::set<std::string> g_failed;  // hashes that couldn't be had (not asked again this session)

void put_u32(Bytes& b, uint32_t v) { b.push_back(v >> 24), b.push_back(v >> 16), b.push_back(v >> 8), b.push_back(v); }
void put_u64(Bytes& b, uint64_t v) { put_u32(b, (uint32_t)(v >> 32)), put_u32(b, (uint32_t)v); }
uint64_t be64(const uint8_t* p) { return (uint64_t)be32(p) << 32 | be32(p + 4); }
bool sarc_files(const Bytes& szs, Bytes& raw, std::map<std::string, Bytes>& files) {
    try {
        raw = szs.size() >= 4 && !memcmp(szs.data(), "Yaz0", 4) ? gcmodel::yaz0_decode(szs) : szs;
        files = gcmodel::sarc_read(raw);
        return !files.empty();
    } catch (...) {
        return false;
    }
}
uint64_t fnv_files(const std::map<std::string, Bytes>& files) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (auto& [name, d] : files) {
        for (char ch : name) h = (h ^ (uint8_t)ch) * 0x100000001b3ull;
        for (uint8_t b : d) h = (h ^ b) * 0x100000001b3ull;
    }
    return h;
}
// "WWMD1", the game file's hash, the model's hash, the file count; per file (name order): u16 name length, name, u8 kind
// (0 the game's, 1 whole, 2 blocks), u32 size, then the data or u32 block runs of {u32 offset, u32 length, data}
constexpr uint32_t kBlock = 64;
bool make_blob(const std::string& folder, Bytes& out) {
    Bytes gsz, msz, graw, mraw;
    std::map<std::string, Bytes> game, model;
    if (!hd_link_szs(gsz) || !read_file(pack_szs(folder), msz) || !sarc_files(gsz, graw, game) || !sarc_files(msz, mraw, model)) return false;
    if (game.size() != model.size()) return false;
    for (auto& [n, d] : model)
        if (!game.count(n)) return false;  // (the same files as the game's: made again from its SARC)
    Bytes b{'W', 'W', 'M', 'D', '1'};
    put_u64(b, fnv_files(game)), put_u64(b, fnv_files(model)), put_u32(b, (uint32_t)model.size());
    for (auto& [name, d] : model) {
        const Bytes& g = game[name];
        b.push_back((uint8_t)(name.size() >> 8)), b.push_back((uint8_t)name.size());
        b.insert(b.end(), name.begin(), name.end());
        if (d == g) { b.push_back(0), put_u32(b, (uint32_t)d.size()); continue; }
        std::vector<std::pair<uint32_t, uint32_t>> runs;
        for (uint32_t o = 0; o < d.size(); o += kBlock) {
            const uint32_t n = std::min<uint32_t>(kBlock, (uint32_t)d.size() - o);
            const bool same = o + n <= g.size() && !memcmp(&d[o], &g[o], n);
            if (same) continue;
            if (!runs.empty() && runs.back().first + runs.back().second == o) runs.back().second += n;
            else runs.push_back({o, n});
        }
        uint64_t diff = 0;
        for (auto& r : runs) diff += r.second;
        if (diff * 10 > d.size() * 9) {  // (most of it differs: whole)
            b.push_back(1), put_u32(b, (uint32_t)d.size()), b.insert(b.end(), d.begin(), d.end());
            continue;
        }
        b.push_back(2), put_u32(b, (uint32_t)d.size()), put_u32(b, (uint32_t)runs.size());
        for (auto& r : runs) put_u32(b, r.first), put_u32(b, r.second), b.insert(b.end(), d.begin() + r.first, d.begin() + r.first + r.second);
    }
    out.resize(ZSTD_compressBound(b.size()));
    const size_t n = ZSTD_compress(out.data(), out.size(), b.data(), b.size(), 19);
    if (ZSTD_isError(n)) return false;
    out.resize(n);
    LOG("[coopmodel] %s: %zu KB to send (%zu KB before packing)", folder.c_str(), out.size() >> 10, b.size() >> 10);
    return true;
}
// the model from a blob and this game's Link.szs, as a SARC file (received_path)
bool take_blob(const Bytes& packed, const std::string& hash) {
    const unsigned long long size = ZSTD_getFrameContentSize(packed.data(), packed.size());
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64u << 20)) return false;
    Bytes b(size);
    if (ZSTD_isError(ZSTD_decompress(b.data(), b.size(), packed.data(), packed.size()))) return false;
    Bytes gsz, graw;
    std::map<std::string, Bytes> game;
    if (!hd_link_szs(gsz) || !sarc_files(gsz, graw, game)) return false;
    size_t p = 0;
    auto need = [&](size_t n) { return p + n <= b.size(); };
    if (!need(5 + 20) || memcmp(b.data(), "WWMD1", 5)) return false;
    p = 5;
    const uint64_t game_h = be64(&b[p]), model_h = be64(&b[p + 8]);
    const uint32_t files = be32(&b[p + 16]);
    p += 20;
    if (game_h != fnv_files(game)) return LOG("[coopmodel] a received model: made from another game's Link (a randomized game?)"), false;
    std::map<std::string, Bytes> model, repl;
    for (uint32_t i = 0; i < files; i++) {
        if (!need(2)) return false;
        const size_t nl = (size_t)(b[p] << 8 | b[p + 1]);
        p += 2;
        if (!need(nl + 5)) return false;
        const std::string name((const char*)&b[p], nl);
        p += nl;
        const uint8_t kind = b[p];
        const uint32_t sz = be32(&b[p + 1]);
        p += 5;
        auto g = game.find(name);
        if (g == game.end()) return false;
        Bytes d;
        if (kind == 0) d = g->second;
        else if (kind == 1) {
            if (!need(sz)) return false;
            d.assign(b.begin() + p, b.begin() + p + sz), p += sz;
        } else if (kind == 2) {
            if (!need(4)) return false;
            d = g->second;
            d.resize(sz);
            const uint32_t runs = be32(&b[p]);
            p += 4;
            for (uint32_t r = 0; r < runs; r++) {
                if (!need(8)) return false;
                const uint32_t o = be32(&b[p]), n = be32(&b[p + 4]);
                p += 8;
                if (!need(n) || (uint64_t)o + n > sz) return false;
                memcpy(&d[o], &b[p], n), p += n;
            }
        } else return false;
        if (d != g->second) repl[name] = d;
        model[name] = std::move(d);
    }
    if (fnv_files(model) != model_h) return LOG("[coopmodel] a received model: not what was sent (check failed)"), false;
    Bytes sarc;
    try {
        sarc = gcmodel::sarc_replace(graw, repl);
    } catch (...) {
        return false;
    }
    const std::string out = received_path(hash), tmp = out + ".tmp";
    mkdir((online_compat::dir() + "/received").c_str(), 0700);
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = fwrite(sarc.data(), 1, sarc.size(), f) == sarc.size();
    if (fclose(f) != 0 || !ok || rename(tmp.c_str(), out.c_str()) != 0) return remove(tmp.c_str()), false;
    return true;
}
void ask_for(int from, const std::string& folder, const std::string& hash) {
    std::lock_guard<std::mutex> lk(g_take_mu);
    if (g_take.state.load() == 1 || g_take.state.load() == 2 || g_failed.count(hash)) return;
    g_take.from = from, g_take.hash = hash, g_take.folder = folder, g_take.total = g_take.got = 0, g_take.data.clear();
    g_take.asked = g_step, g_take.state = 1;
    LOG("[coopmodel] asking player %d for %s", from, folder.c_str());
    net::send(from, kModelAsk, "WWPR1\t" + hash);
}
void asked(int from, const uint8_t* d, size_t n) {
    const std::string b((const char*)d, std::min<size_t>(n, 64));
    if (b.compare(0, 6, "WWPR1\t") != 0 || !option(kSendKey)) return;
    const std::string hash = b.substr(6), mine = mods::player_model_in_memory();
    if (mine.empty() || hash_of(pack_szs(mine)) != hash) return;
    {
        std::lock_guard<std::mutex> lk(g_send_mu);
        for (Send& s : g_sends)
            if (s.to == from && s.hash == hash) return;
        g_sends.push_back({from, hash, 0});
    }
    LOG("[coopmodel] player %d asks for %s: sending", from, mine.c_str());
    if (g_out.hash != hash || g_out.state.load() == 0) {
        g_out.hash = hash, g_out.state = 1;
        std::thread([mine] {
            Bytes out;
            const bool ok = make_blob(mine, out);
            g_out.data = std::move(out);
            g_out.state = ok ? 2 : 3;
            if (!ok) LOG("[coopmodel] %s can't be sent (not made from this game's Link.szs)", mine.c_str());
        }).detach();
    }
}
void part(int from, const uint8_t* d, size_t n) {
    std::lock_guard<std::mutex> lk(g_take_mu);
    if (n < 18 || d[0] != 'W' || d[1] != 'P' || g_take.state.load() != 1 || from != g_take.from) return;
    char h[20];
    snprintf(h, sizeof h, "%016llx", (unsigned long long)be64(d + 2));
    const uint32_t total = be32(d + 10), at = be32(d + 14);
    if (h != g_take.hash || total > (16u << 20) || at != g_take.got) return;
    if (!g_take.total) g_take.total = total, g_take.data.reserve(total);
    if (total != g_take.total || at + (n - 18) > total) return;
    g_take.data.insert(g_take.data.end(), d + 18, d + n);
    g_take.got += (uint32_t)(n - 18), g_take.asked = g_step;
    if (g_take.got < g_take.total) return;
    g_take.state = 2;
    LOG("[coopmodel] %s received (%u KB): making it", g_take.folder.c_str(), g_take.total >> 10);
    std::thread([] {
        Bytes data;
        std::string hash;
        {
            std::lock_guard<std::mutex> lk2(g_take_mu);
            data = std::move(g_take.data), hash = g_take.hash;
        }
        const bool ok = take_blob(data, hash);
        LOG("[coopmodel] received model %s: %s", hash.c_str(), ok ? "ready" : "could not be made");
        std::lock_guard<std::mutex> lk2(g_take_mu);
        if (!ok) g_failed.insert(hash);
        g_take.state = ok ? 3 : 4;
    }).detach();
}
// what goes to the server, measured: this game's bytes and packets sent over the last second (a step's samples)
constexpr int kWindow = 30;
uint64_t g_up_b[kWindow], g_up_p[kWindow];
bool g_up_set = false;
// each step: the parts due to the players asking. Through a server they fill what its upload limit leaves beside the
// poses and states: its default is 8 KB/s and 60 packets/s a player (packets over it are dropped and counted toward a
// kick), so the parts go while this game's measured sending of the last second stays under 6.8 KB/s and 45 packets;
// the poses (~5 KB/s) leave ~2.4 KB/s. All receivers share it, one after the other. Direct: 4 parts a step each.
void send_step() {
    std::lock_guard<std::mutex> lk(g_send_mu);
    const bool server = net::via_server();
    uint64_t bytes = 0, packets = 0;
    const bool measured = server && net::srv::android_sent(bytes, packets);
    const int at = (int)(g_step % kWindow);
    if (measured && !g_up_set) std::fill(g_up_b, g_up_b + kWindow, bytes), std::fill(g_up_p, g_up_p + kWindow, packets), g_up_set = true;
    if (!measured) g_up_set = false;
    // (the oldest sample, a second ago, makes way for this step's)
    const uint64_t used_b = measured ? bytes - g_up_b[at] : 0, used_p = measured ? packets - g_up_p[at] : 0;
    if (measured) g_up_b[at] = bytes, g_up_p[at] = packets;
    if (g_sends.empty() || g_out.state.load() != 2) return;
    if (server && !measured) return;
    constexpr uint32_t kPart = 1000, kWire = kPart + 80;  // (a part with its headers, sealing and the channel's own)
    constexpr uint64_t kBytes = 6800, kPackets = 45;  // (under the poses' 7.2 KB/s, coop_net_server.cpp: they go first)
    int parts = server ? 0 : 4 * (int)g_sends.size();
    if (server) {  // (at most 2 a step: a burst stays well inside the server's allowance)
        uint64_t b = used_b, n = used_p;
        // (and only from an allowance more than half full: the poses go first, a swing's take what it saved)
        while (parts < 2 && b + kWire <= kBytes && n + 1 <= kPackets && net::srv::android_room() > 9000.0 + parts * kWire) b += kWire, n++, parts++;
    }
    static size_t next = 0;  // (whose part is next)
    const Bytes& o = g_out.data;
    while (parts > 0 && !g_sends.empty()) {
        if (next >= g_sends.size()) next = 0;
        Send& s = g_sends[next];
        if (!net::connected(s.to) || s.hash != g_out.hash || s.at >= o.size()) {
            if (s.at >= o.size()) LOG("[coopmodel] model sent to player %d", s.to);
            g_sends.erase(g_sends.begin() + next);
            continue;
        }
        const uint32_t n = std::min<uint32_t>(kPart, (uint32_t)o.size() - s.at);
        Bytes b{'W', 'P'};
        put_u64(b, std::stoull(s.hash, nullptr, 16)), put_u32(b, (uint32_t)o.size()), put_u32(b, s.at);
        b.insert(b.end(), o.begin() + s.at, o.begin() + s.at + n);
        net::send(s.to, kModelPart, std::string((const char*)b.data(), b.size()));
        s.at += n, next++, parts--;
    }
}
}  // namespace

// a puppet's copy of one of Link's model data: the same file of its player's model archive (0: Link's own)
uint32_t kit_data(Cpu* c, int slot, uint32_t link_data) {
    if (slot < 0 || slot >= kSlots) return 0;
    const std::string model = wanted(slot);
    const bool have = !model.empty() && ready(model);
    g_kit_with[slot] = have ? model : "";
    if (!have) return 0;
    if (g_link_got != g_step) {  // (Link's files, once a step at most)
        g_link_got = g_step;
        static const uint32_t link = guest_text("Link");
        const uint32_t s8 = mem::host_alloc(8);  // (a few bytes a kit made: kits are made rarely)
        st32(s8, link), st32(s8 + 4, GD(0x10023B10));
        const Cpu saved = *c;
        for (uint32_t i = 0; i < kLinkFiles; i++) g_link[i] = guest_call(c, GC(0x026066C4), {ld32(GD(0x101F4F28)), s8, i});
        *c = saved;
    }
    for (int n = 0; n < kArchives; n++)
        if (g_arc[n].model == model && g_arc[n].state.load() == 4)
            for (uint32_t i = 0; i < kLinkFiles; i++)
                if (g_link[i] && g_link[i] == link_data) return g_arc[n].data[i];
    return 0;
}

// the Online page: what is being sent or received, one line each ("" none)
std::vector<std::string> transfers() {
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> lk(g_take_mu);
        const int st = g_take.state.load();
        if (st == 1) out.push_back("in\t" + g_take.folder + "\t" + net::name(g_take.from) + "\t" +
                                   std::to_string(g_take.total ? (int)(100.0 * g_take.got / g_take.total) : 0));
        if (st == 2) out.push_back("in\t" + g_take.folder + "\t" + net::name(g_take.from) + "\t100");
    }
    std::lock_guard<std::mutex> lk(g_send_mu);
    for (const Send& s : g_sends)
        out.push_back("out\t" + mods::player_model_in_memory() + "\t" + net::name(s.to) + "\t" +
                      std::to_string(g_out.data.empty() ? 0 : (int)(100.0 * s.at / g_out.data.size())));
    return out;
}

// game thread, each full step (online_compat::tick)
// This game's Link's sword trail as the game makes it, to the party (fork games; upstream's drop the type): a frame
// a step while it shows (and the one it ends with), sent ~10 times a second: u32 this game's time (the poses' clock),
// its newest top and bottom points relative to Link's place and heading (x4, i16), its point count (i16, the fade in
// it), the points added this step (u8), colour (u8), top rate (u8, x20). The other games play it as his puppet is
// shown at that time (coop_pose.cpp, android_trail_received): the trail as here, not one made from a pose's 60 points
// (which don't fit a pose beside a swing's body) nor guessed from the sword.
void send_trail() {
    static Bytes out;
    static int last_n = 0, last_idx = 0;
    static uint32_t frames = 0;
    if (!net::connected(net::my_id())) return;
    const uint32_t link = ld32(GD(0x1046F0B0) + mods::coop::names::inf::kLink);
    const uint32_t blur = link >= 0x10000000u ? ld32(link + mods::coop::names::py::mpSwBlur) : 0;
    namespace sb = mods::coop::names::swblur;
    const int n = blur ? (int32_t)ld32(blur + sb::mPointCount) : 0, idx = blur ? (int32_t)ld32(blur + sb::mSampleIndex) : 0;
    if (n > 0 || last_n > 0) {
        int added = 0;
        if (n > last_n) added = n - last_n;
        else if (n == last_n && n >= 58) added = idx != last_idx ? std::min(std::abs(idx - last_idx), 60) : 10;
        const float px = ldf32(link + mods::coop::names::ac::kPos), py = ldf32(link + mods::coop::names::ac::kPos + 4),
                    pz = ldf32(link + mods::coop::names::ac::kPos + 8);
        const float a = (int16_t)ld16(link + mods::coop::names::ac::kAngleY) * 3.14159265f / 32768, sn = std::sin(a), co = std::cos(a);
        if (out.empty()) out = {'W', 'S'};
        put_u32(out, net::now_ms());
        for (const uint32_t at : {sb::mTopPos, sb::mBottomPos}) {
            const float d[3] = {blur ? (float)ldf32(blur + at) - px : 0.f, blur ? (float)ldf32(blur + at + 4) - py : 0.f, blur ? (float)ldf32(blur + at + 8) - pz : 0.f};
            const float l[3] = {co * d[0] - sn * d[2], d[1], sn * d[0] + co * d[2]};
            for (const float v : l) {
                const int16_t q = (int16_t)std::clamp<long>(std::lround(v * 4), -32768, 32767);
                out.push_back((uint8_t)(q >> 8)), out.push_back((uint8_t)q);
            }
        }
        const int16_t nn = (int16_t)std::max(0, n);
        out.push_back((uint8_t)(nn >> 8)), out.push_back((uint8_t)nn);
        out.push_back((uint8_t)std::clamp(added, 0, 60));
        out.push_back((uint8_t)(blur ? ld32(blur + sb::mBlurColorType) : 0));
        out.push_back((uint8_t)std::clamp<long>(std::lround((blur ? ldf32(blur + sb::mBlurTopRate) : 0) * 20), 0, 255));
        frames++;
    }
    last_n = std::max(0, n), last_idx = idx;
    if (!out.empty() && (frames >= 3 || n <= 0)) net::send(-1, kTrailMsg, std::string((const char*)out.data(), out.size())), out.clear(), frames = 0;
}
void step(Cpu* c) {
    g_step++;
    const char* stage = (const char*)mem::ptr(GD(0x1046F0B0) + 0x5134);
    if (!stage[0] || !set_tables()) return;
    const Cpu saved = *c;
    load_step(c);
    *c = saved;
    if (net::role() == net::Role::kOff) {
        g_sent = "\x02";
        return;
    }
    // (test aids: WWHD_COOP_MODEL_INVITE=<friend code>: invited once, 10 s online through a server; WWHD_COOP_MODEL_ACCEPT=1:
    // the first invitation taken)
    if (net::via_server()) {
        static uint32_t online_at = 0;
        if (!online_at) online_at = g_step;
        static bool invited = false;
        if (const char* code = getenv("WWHD_COOP_MODEL_INVITE"); code && !invited && g_step - online_at > 300) {
            invited = true;
            std::string why;
            LOG("[coopmodel] test: inviting %s: %s", code, net::invite(code, why) ? "sent" : why.c_str());
        }
        static bool accepted = false;
        if (getenv("WWHD_COOP_MODEL_ACCEPT") && !accepted) {
            const auto inv = net::invites();
            if (!inv.empty()) accepted = true, LOG("[coopmodel] test: joining %s's party", inv[0].second.c_str()), net::answer(inv[0].first, true);
        }
    }
    // (test aid: WWHD_COOP_MODEL_GOTO=1: once, 10 s after another player is first there, Go to them)
    if (static const bool go = getenv("WWHD_COOP_MODEL_GOTO") != nullptr; go) {
        static uint32_t seen = 0;
        static bool done = false;
        for (int id = 0; id < net::kMaxPlayers && !done; id++)
            if (id != net::my_id() && net::member(id)) {
                if (!seen) seen = g_step;
                if (g_step - seen == 900 || g_step - seen == 1800) {
                    const uint32_t sv = mods::save_info();
                    LOG("[coopmodel] test: going to player %d (save %08X loaded %08X: max %u life %u rupees %u wallet %u return %.8s)", id, sv,
                        mods::loaded_save(), ld16(sv), ld16(sv + 2), ld16(sv + 4), ld8(sv + 0x12), (const char*)mem::ptr(sv + 0x30));
                    mods::coop::sync::go_to(id);
                }
                done = g_step - seen > 1800;
                break;
            }
    }
    // what this game plays with, to the party: again to whoever joined, when this game's own place in the session
    // changes (the others then see it leave and join, this game sees no one join: its model was forgotten there), and
    // every 10 s (a few dozen bytes; a lost or missed one is mended)
    const std::string body = my_body();
    static int last_id = -1;
    static uint32_t sent_at = 0;
    const int id = net::my_id();
    if (body != g_sent || g_resend.exchange(false) || id != last_id || g_step - sent_at >= 300) {
        g_sent = body, last_id = id, sent_at = g_step;
        net::send(-1, kModelMsg, body);
    }
    send_step();
    send_trail();
    {  // models to ask for (one at a time), a stalled one let go, one just made taken into use
        std::unique_lock<std::mutex> lk(g_take_mu);
        const int st = g_take.state.load();
        if (st == 1 && g_step - g_take.asked > 30 * 60) {  // (a minute without a part)
            LOG("[coopmodel] %s: nothing more came", g_take.folder.c_str());
            g_failed.insert(g_take.hash), g_take.state = 4;
        }
        if (st == 3 || st == 4) {
            const std::string hash = g_take.hash;
            const bool ok = st == 3;
            g_take.state = 0;
            lk.unlock();
            std::lock_guard<std::mutex> lk2(g_mu);
            if (ok)
                for (auto& [id, h] : g_hash)
                    if (h == hash) g_wants[id] = kRx + hash;
        }
    }
    if (option(kTakeKey)) {
        std::vector<std::tuple<int, std::string, std::string>> need;
        {
            std::lock_guard<std::mutex> lk(g_mu);
            for (auto& [id, folder] : g_said)
                if (!folder.empty() && g_wants[id].empty() && net::member(id)) need.emplace_back(id, folder, g_hash[id]);
        }
        for (auto& [id, folder, hash] : need) ask_for(id, folder, hash);
    }
    // a puppet whose kit is of another model than it should be now: made again (a model still loading: once ready)
    for (int slot = 0; slot < kSlots; slot++) {
        if (!kit_made(slot)) continue;
        std::string now = wanted(slot);
        if (!now.empty() && !ready(now)) continue;
        if (now == g_kit_with[slot]) continue;
        LOG("[coopmodel] puppet %d: %s instead of %s", slot, label(now).c_str(), label(g_kit_with[slot]).c_str());
        g_kit_with[slot] = now;
        kit_again(c, slot);
        *c = saved;
    }
}
}  // namespace android_models

// the fork's messages out of the session's poll (coop_sync_android.cpp)
namespace android_raid {
void received(int from, const uint8_t* d, size_t n);
void joined();
void wave_received(int from, const uint8_t* d, size_t n);
void gone_received(int from, const uint8_t* d, size_t n);
}
namespace android_fight {
void received(int from, const uint8_t* d, size_t n);
}
namespace android_down {
void received(int from, const uint8_t* d, size_t n);
}
namespace android_siege {
void received(int from, const uint8_t* d, size_t n);
}
namespace android_shipraid {
void received(int from, const uint8_t* d, size_t n);
}
namespace mods::coop::net {
namespace {
struct Tap : Handler {
    Handler* in = nullptr;
    void message(int from, Msg type, const uint8_t* d, size_t n) override {
        if (type == android_models::kModelMsg) return android_models::received(from, d, n);
        if (type == android_models::kModelAsk) return android_models::asked(from, d, n);
        if (type == android_models::kModelPart) return android_models::part(from, d, n);
        if (type == android_models::kTrailMsg) return mods::coop::android_trail_received(from, d, n);
        if (type == (Msg)0x64) return android_raid::received(from, d, n);  // (online/android_raid.cpp)
        if (type == (Msg)0x65) return android_fight::received(from, d, n);  // (online/android_fight.cpp)
        if (type == (Msg)0x66) return android_raid::wave_received(from, d, n);  // (online/android_raid.cpp)
        if (type == (Msg)0x69) return android_raid::gone_received(from, d, n);  // (online/android_raid.cpp)
        if (type == (Msg)0x67) return android_down::received(from, d, n);  // (online/android_down.cpp)
        if (type == (Msg)0x68) return android_siege::received(from, d, n);  // (online/android_siege.cpp)
        if (type == (Msg)0x6A) return android_shipraid::received(from, d, n);  // (online/android_shipraid.cpp)
        in->message(from, type, d, n);
    }
    void state(int from, uint8_t kind, uint32_t ms, const uint8_t* d, size_t n) override { in->state(from, kind, ms, d, n); }
    void joined(int id) override {
        android_models::g_resend = true;
        android_raid::joined();
        in->joined(id);
    }
    void left(int id, const char* why) override {
        {
            std::lock_guard<std::mutex> lk(android_models::g_mu);
            android_models::g_wants.erase(id), android_models::g_said.erase(id);
        }
        in->left(id, why);
    }
};
}  // namespace
void android_poll(Handler& h) {
    Tap t;
    t.in = &h;
    poll(t);
}
}  // namespace mods::coop::net
