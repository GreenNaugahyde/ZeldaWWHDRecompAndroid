// The app's converter on the PC: gcmodel_host Link.arc permanent_3d.pack out.pack
//   loot swords: gcmodel_host --sword GAME_DIR SWORD_ID out_Link.bfres (GAME_DIR: the folder with content/)
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>

#include "gcmodel/gcmodel.h"
#include "gcmodel/gcm_io.h"
#include "gcmodel/hd_sword.h"

static std::vector<uint8_t> load(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static int sword(const char* game, const char* id, const char* out) {
    const gcmodel::SwordDef* d = gcmodel::sword_def(id);
    if (!d) return fprintf(stderr, "no sword %s\n", id), 2;
    std::string err, report;
    gcmodel::Bytes src, link, res;
    if (!gcmodel::sword_source(game, *d, src, err)) return fprintf(stderr, "ERROR: %s\n", err.c_str()), 1;
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    auto files = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs")));
    link = files.at("Link.bfres");
    gcmodel::SwordExtra ex;
    if (!gcmodel::make_sword(link, src, *d, res, err, &report, &ex)) return fprintf(stderr, "ERROR: %s\n", err.c_str()), 1;
    std::ofstream(out, std::ios::binary).write((const char*)res.data(), res.size());
    // (the colour tiers: the first rainbow step and one a quarter round on, each as a file of its own)
    for (int st : {4, 4 + gcmodel::kRainbowSteps / 4}) {
        if (st >= (int)ex.tiers.size()) break;
        auto r = res;
        memcpy(&r[ex.image[0]], ex.tiers[st].data(), ex.image[1]);
        if (ex.image[3]) memcpy(&r[ex.image[2]], ex.tiers[st].data() + ex.image[1], ex.image[3]);
        std::ofstream(std::string(out) + ".tier" + std::to_string(st), std::ios::binary).write((const char*)r.data(), r.size());
    }
    printf("%zu tier pictures\n", ex.tiers.size());
    printf("%s\nwritten %zu bytes\n", report.c_str(), res.size());
    return 0;
}

// every sword of the list made: id, ok / why not, its appended part and the bytes it changes (its corner in memory)
static int sword_all(const char* game) {
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    int ok = 0, bad = 0;
    for (const auto& d : gcmodel::sword_list()) {
        std::string err, report;
        gcmodel::Bytes src, res;
        bool good = gcmodel::sword_source(game, d, src, err) && gcmodel::make_sword(link, src, d, res, err, &report);
        size_t changed = 0;
        if (good)
            for (size_t o = 0; o + 4 <= link.size(); o += 4)
                if (memcmp(&res[o], &link[o], 4)) changed += 4;
        const size_t tail = good ? res.size() - link.size() : 0, corner = 0x100 + tail + 2 * changed + 0x20000;
        if (good && corner > 0x100000) good = false, err = "too big for the sword's corner";
        printf("%s\t%s\t%zu\t%zu\t%s\n", d.id, good ? "ok" : "FAIL", tail, changed, good ? report.c_str() : err.c_str());
        (good ? ok : bad)++;
    }
    fprintf(stderr, "%d ok, %d failed\n", ok, bad);
    return 0;
}

static int shield(const char* game, const char* id, const char* out) {
    const gcmodel::SwordDef* d = gcmodel::shield_def(id);
    if (!d) return fprintf(stderr, "no shield %s\n", id), 2;
    std::string err, report;
    gcmodel::Bytes src, res;
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    if (!gcmodel::sword_source(game, *d, src, err) || !gcmodel::make_shield(link, src, *d, res, err, &report))
        return fprintf(stderr, "ERROR: %s\n", err.c_str()), 1;
    std::ofstream(out, std::ios::binary).write((const char*)res.data(), res.size());
    printf("%s\n", report.c_str());
    return 0;
}
static int shield_all(const char* game) {
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    int ok = 0, bad = 0;
    for (const auto& d : gcmodel::shield_list()) {
        std::string err, report;
        gcmodel::Bytes src, res;
        const bool good = gcmodel::sword_source(game, d, src, err) && gcmodel::make_shield(link, src, d, res, err, &report);
        printf("%s\t%s\t%s\n", d.id, good ? "ok" : "FAIL", good ? report.c_str() : err.c_str());
        (good ? ok : bad)++;
    }
    fprintf(stderr, "%d ok, %d failed\n", ok, bad);
    return 0;
}

// every piece of armour made: slot, id, ok / why not, its appended part and its picture
static int armor_all(const char* game) {
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    int bad = 0;
    for (int slot = 0; slot < 2; slot++)
        for (const auto& d : gcmodel::armor_list(slot)) {
            gcmodel::Bytes src, res;
            std::string err, report;
            gcmodel::SwordExtra ex;
            const bool good = gcmodel::armor_source(game, d, src, err) && gcmodel::make_armor(link, src, d, slot, res, err, &report, &ex);
            if (good) printf("%d\t%s\tok\t%zu\t%zu\t%s\n", slot, d.id, res.size() - link.size(), ex.own.size(), report.c_str());
            else printf("%d\t%s\tERROR %s\n", slot, d.id, err.c_str()), bad++;
        }
    return bad != 0;
}

// an outfit on Link: its Link.bfres (for a look at it)
static int outfit(const char* game, const char* id, const char* out) {
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    const gcmodel::ArmorDef* d = gcmodel::armor_def(2, id);
    if (!d) return fprintf(stderr, "no outfit %s\n", id), 2;
    gcmodel::Bytes src, res;
    std::string err, report;
    gcmodel::SwordExtra ex;
    if (!gcmodel::armor_source(game, *d, src, err) || !gcmodel::make_outfit(link, src, *d, res, err, &report, &ex)) return fprintf(stderr, "ERROR: %s\n", err.c_str()), 1;
    printf("%s; added %zu, picture %zu\n", report.c_str(), res.size() - link.size(), ex.own.size());
    if (FILE* pv = fopen((std::string(out) + ".rgba").c_str(), "wb")) {
        fwrite(ex.preview.data(), 1, ex.preview.size(), pv);
        fclose(pv);
    }
    FILE* fp = fopen(out, "wb");
    fwrite(res.data(), 1, res.size(), fp);
    fclose(fp);
    return 0;
}

// every outfit made: id, name, ok / why not, size added (its corner: 1 MiB)
static int outfit_all(const char* game) {
    const auto pack = load((std::string(game) + "/content/Common/Pack/permanent_3d.pack").c_str());
    const auto link = gcmodel::sarc_read(gcmodel::yaz0_decode(gcmodel::sarc_read(pack).at("Link.szs"))).at("Link.bfres");
    int bad = 0;
    for (const auto& d : gcmodel::armor_list(2)) {
        gcmodel::Bytes src, res;
        std::string err, report;
        gcmodel::SwordExtra ex;
        const bool good = gcmodel::armor_source(game, d, src, err) && gcmodel::make_outfit(link, src, d, res, err, &report, &ex);
        if (good) {
            printf("%s\t%s\tok\t%zu\t%s\n", d.id, d.name, res.size() - link.size() + ex.own.size(), report.c_str());
        } else printf("%s\t%s\tERROR %s\n", d.id, d.name, err.c_str()), bad++;
    }
    fprintf(stderr, "%d failed\n", bad);
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string(argv[1]) == "--outfit-all") return outfit_all(argv[2]);
    if (argc == 5 && std::string(argv[1]) == "--outfit") return outfit(argv[2], argv[3], argv[4]);
    if (argc == 3 && std::string(argv[1]) == "--armor-all") return armor_all(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--sword-all") return sword_all(argv[2]);
    if (argc == 3 && std::string(argv[1]) == "--shield-all") return shield_all(argv[2]);
    if (argc == 5 && std::string(argv[1]) == "--shield") return shield(argv[2], argv[3], argv[4]);
    if (argc == 5 && std::string(argv[1]) == "--sword") return sword(argv[2], argv[3], argv[4]);
    if (argc != 4) {
        fprintf(stderr, "usage: gcmodel_host Link.arc permanent_3d.pack out.pack\n");
        return 2;
    }
    auto t0 = std::chrono::steady_clock::now();
    std::vector<uint8_t> out;
    auto r = gcmodel::convert(load(argv[1]), load(argv[2]), out, [](int pc, const std::string& s) { fprintf(stderr, "\r%3d%% %-40s", pc, s.c_str()); });
    fprintf(stderr, "\n");
    printf("%s", r.report.c_str());
    if (!r.error.empty()) {
        fprintf(stderr, "ERROR: %s\n", r.error.c_str());
        return 1;
    }
    std::string err;
    const auto pack = gcmodel::assemble(load(argv[2]), out, err);
    if (pack.empty()) {
        fprintf(stderr, "ERROR: %s\n", err.c_str());
        return 1;
    }
    out = pack;
    std::ofstream(argv[3], std::ios::binary).write((const char*)out.data(), out.size());
    printf("written %zu bytes in %.1f s\n", out.size(), std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}
