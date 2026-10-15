// RPX loading and function discovery (see program.h).
#include "program.h"

#include <zlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace recomp {
namespace {

constexpr uint32_t SHT_SYMTAB = 2, SHT_RELA = 4, SHT_NOBITS = 8, SHT_RPL_IMPORTS = 0x80000002u;
constexpr uint32_t SHF_RPL_ZLIB = 0x08000000u;
constexpr uint32_t R_PPC_ADDR32 = 1, R_PPC_ADDR16_LO = 4, R_PPC_ADDR16_HI = 5, R_PPC_ADDR16_HA = 6, R_PPC_REL24 = 10;
// imported data objects get runtime-owned storage at fixed addresses (as tools/recomp/recomp.py)
constexpr uint32_t kDataImportBase = 0xC1000000u, kDataImportStride = 0x1000;

uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
int32_t sext(uint32_t v, int bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

struct Section {
    uint32_t nameOff, type, flags, addr, offset, fileSize, link, info;
    std::string name;
    std::vector<uint8_t> data;
};
struct Symbol {
    std::string name;
    uint32_t value;
    uint16_t shndx;
    uint8_t type;
    const Section* section = nullptr;
    std::string importLib;
    char importKind = 0;  // 'f' or 'd'
};

std::string cstr(const std::vector<uint8_t>& b, uint32_t off) {
    std::string s;
    while (off < b.size() && b[off]) s += (char)b[off++];
    return s;
}

// static target of a non-linking b/bc (0xFFFFFFFF if none)
uint32_t branch_target(uint32_t addr, uint32_t w) {
    uint32_t op = w >> 26;
    if (op == 18 && !(w & 1)) return (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)addr));
    if (op == 16 && !(w & 1)) return (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)addr));
    return 0xFFFFFFFFu;
}

const CodePatch* g_patch = nullptr;

}  // namespace

void set_code_patch(const CodePatch* p) { g_patch = p; }
const CodePatch* code_patch() { return g_patch; }

bool Program::load(const std::string& path, std::string& err) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return err = "cannot open " + path, false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d(n > 0 ? n : 0);
    bool ok = n > 0 && fread(d.data(), 1, n, f) == (size_t)n;
    fclose(f);
    if (!ok) return err = "cannot read " + path, false;
    return load(d, err);
}

bool Program::load(const std::vector<uint8_t>& file, std::string& err) {
    if (!parse(file, err)) return false;
    discover();
    return true;
}

bool Program::parse(const std::vector<uint8_t>& d, std::string& err) {
    if (d.size() < 0x34 || memcmp(d.data(), "\x7F" "ELF", 4) != 0) return err = "not an RPX executable", false;
    entry = be32(&d[0x18]);
    uint32_t shoff = be32(&d[0x20]);
    uint16_t shentsize = be16(&d[0x2E]), shnum = be16(&d[0x30]), shstrndx = be16(&d[0x32]);
    std::vector<Section> secs(shnum);
    for (uint32_t i = 0; i < shnum; i++) {
        size_t o = shoff + (size_t)i * shentsize;
        if (o + 40 > d.size()) return err = "corrupt section table", false;
        Section& s = secs[i];
        s.nameOff = be32(&d[o]);
        s.type = be32(&d[o + 4]);
        s.flags = be32(&d[o + 8]);
        s.addr = be32(&d[o + 12]);
        s.offset = be32(&d[o + 16]);
        s.fileSize = be32(&d[o + 20]);
        s.link = be32(&d[o + 24]);
        s.info = be32(&d[o + 28]);
        if (s.type == SHT_NOBITS || s.fileSize == 0) continue;
        if ((size_t)s.offset + s.fileSize > d.size()) return err = "corrupt section", false;
        if (s.flags & SHF_RPL_ZLIB) {
            uLongf size = be32(&d[s.offset]);
            s.data.resize(size);
            if (uncompress(s.data.data(), &size, &d[s.offset + 4], s.fileSize - 4) != Z_OK) return err = "corrupt compressed section", false;
            s.data.resize(size);
        } else {
            s.data.assign(d.begin() + s.offset, d.begin() + s.offset + s.fileSize);
        }
    }
    if (shstrndx >= shnum) return err = "corrupt section names", false;
    for (auto& s : secs) s.name = cstr(secs[shstrndx].data, s.nameOff);
    const Section* text = nullptr;
    for (auto& s : secs)
        if (s.name == ".text") text = &s;
    if (!text) return err = "no .text section", false;
    textLo = text->addr;
    textHi = text->addr + (uint32_t)text->data.size();
    std::vector<uint8_t> textBytes = text->data, cave;
    const CodePatch* pt = patch;  // (nullptr for the unpatched layout, compile.cpp)
    if (pt && pt->caveHi > pt->caveLo) {
        caveLo = pt->caveLo & ~3u;
        caveHi = (pt->caveHi + 3) & ~3u;
        if (caveHi > textLo) return err = "code cave overlaps .text", false;
        cave.assign(caveHi - caveLo, 0);
    }
    if (pt)
        for (auto& w : pt->writes)
            for (size_t k = 0; k < w.bytes.size(); k++) {
                uint32_t a = w.addr + (uint32_t)k;
                if (a >= textLo && a < textHi) textBytes[a - textLo] = w.bytes[k];
                else if (a >= caveLo && a < caveHi) cave[a - caveLo] = w.bytes[k];
            }
    words.resize(textBytes.size() / 4);
    for (size_t i = 0; i < words.size(); i++) words[i] = be32(&textBytes[4 * i]);
    caveWords.resize(cave.size() / 4);
    for (size_t i = 0; i < caveWords.size(); i++) caveWords[i] = be32(&cave[4 * i]);
    // code in the cave: what control flow reaches from the branches into it and the address-taken
    // entries (packs also write instructions as data, e.g. `.int 0x7CCC42E6` for mftb; data that is
    // never reached stays data and ends the functions before it)
    caveIsCode.assign(caveWords.size(), false);
    if (!caveWords.empty()) {
        std::vector<uint32_t> work;
        auto inCave = [&](uint32_t a) { return a >= caveLo && a < caveHi; };
        if (pt)
            for (uint32_t a : pt->entries)
                if (inCave(a)) work.push_back(a);
        for (size_t i = 0; i < words.size(); i++) {
            uint32_t a = textLo + 4 * (uint32_t)i, w = words[i], op = w >> 26, t = 0xFFFFFFFFu;
            if (op == 18) t = (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a));
            else if (op == 16) t = (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)a));
            if (inCave(t)) work.push_back(t);
        }
        while (!work.empty()) {
            uint32_t a = work.back() & ~3u;
            work.pop_back();
            for (; inCave(a) && !caveIsCode[(a - caveLo) >> 2]; a += 4) {
                caveIsCode[(a - caveLo) >> 2] = true;
                uint32_t w = caveWords[(a - caveLo) >> 2], op = w >> 26;
                if (op == 18 || op == 16) {
                    uint32_t t = op == 18 ? (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a))
                                          : (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)a));
                    if (inCave(t)) work.push_back(t);
                    if (op == 18 && !(w & 1)) break;                    // b: no fall-through
                    if (op == 16 && !(w & 1) && (w & 0x02800000) == 0x02800000) break;  // bc always (BO 1z1zz)
                } else if (op == 19 && ((w >> 1) & 0x3FF) != 0 && (((w >> 1) & 0x3FF) == 16 || ((w >> 1) & 0x3FF) == 528)) {
                    if (!(w & 1) && (w & 0x02800000) == 0x02800000) break;  // blr / bctr always
                }
            }
        }
    }

    // symbols
    std::vector<Symbol> syms;
    for (auto& s : secs) {
        if (s.type != SHT_SYMTAB) continue;
        const auto& strtab = secs[s.link].data;
        for (size_t j = 0; j + 16 <= s.data.size(); j += 16) {
            Symbol y;
            y.name = cstr(strtab, be32(&s.data[j]));
            y.value = be32(&s.data[j + 4]);
            y.type = s.data[j + 12] & 0xF;
            y.shndx = be16(&s.data[j + 14]);
            if (y.shndx > 0 && y.shndx < secs.size()) y.section = &secs[y.shndx];
            if (y.section && y.section->type == SHT_RPL_IMPORTS) {
                size_t us = y.section->name.find('_');
                y.importLib = us == std::string::npos ? "" : y.section->name.substr(us + 1);
                y.importKind = y.section->name.size() > 1 ? y.section->name[1] : 0;
            }
            syms.push_back(y);
        }
    }
    for (auto& y : syms)
        if (!y.importLib.empty() && y.type != 3)  // not section symbols
            imports[y.value] = Import{y.importLib, y.name, y.importKind == 'f', 0};
    uint32_t n = 0;
    for (auto& [slot, imp] : imports)
        if (!imp.function) imp.dataAddr = kDataImportBase + n++ * kDataImportStride;

    // relocations
    std::map<uint32_t, std::vector<std::pair<uint32_t, bool>>> codeRefs;  // target -> (site, site in .text)
    for (auto& s : secs) {
        if (s.type != SHT_RELA || s.info >= secs.size()) continue;
        const Section& target = secs[s.info];
        for (size_t k = 0; k + 12 <= s.data.size(); k += 12) {
            uint32_t off = be32(&s.data[k]), info = be32(&s.data[k + 4]);
            int32_t addend = (int32_t)be32(&s.data[k + 8]);
            uint32_t typ = info & 0xFF, si = info >> 8;
            if (si >= syms.size()) continue;
            const Symbol& y = syms[si];
            if (!y.importLib.empty()) {
                if (typ == R_PPC_REL24) importCalls[off] = y.value;
                else if (target.name == ".text") {
                    auto it = imports.find(y.value);
                    uint32_t v = (it != imports.end() && !it->second.function ? it->second.dataAddr : y.value) + addend;
                    if (typ == R_PPC_ADDR16_HA) immOverride[off & ~3u] = (uint16_t)((v + 0x8000) >> 16);
                    else if (typ == R_PPC_ADDR16_LO) immOverride[off & ~3u] = (uint16_t)v;
                    else if (typ == R_PPC_ADDR16_HI) immOverride[off & ~3u] = (uint16_t)(v >> 16);
                }
                continue;
            }
            if (typ == R_PPC_REL24) {
                if (y.name == "$UNDEF") undefCalls.insert(off);
                continue;
            }
            uint32_t tgt = y.value + addend;
            if (inText(tgt) && (typ == R_PPC_ADDR32 || typ == R_PPC_ADDR16_LO)) codeRefs[tgt].push_back({off, target.name == ".text"});
        }
    }

    // jump tables: a table of `b` right after the dispatching bctr, its address formed just before
    for (auto& [t, refs] : codeRefs) {
        bool near = false;
        for (auto& [site, inTextSec] : refs) near |= inTextSec && site >= t - 40 && site < t;
        if (!near || t - 4 < textLo || word(t - 4) != 0x4E800420u) continue;
        int count = -1;
        for (int k = 2; k < 12; k++) {
            uint32_t w = word(t - 4 * k);
            if ((w >> 26) == 10) { count = (int)(w & 0xFFFF) + 1; break; }
        }
        if (count < 0) {
            count = 0;
            while (inText(t + 4 * count) && (word(t + 4 * count) >> 26) == 18 && !(word(t + 4 * count) & 3)) count++;
        }
        jumpTables[t - 4] = {t, (uint32_t)count};
    }
    // function entries: the entry point, call targets, address-taken code (minus jump tables)
    std::set<uint32_t> e = {entry};
    if (patch)
        for (uint32_t a : patch->entries)
            if (inText(a)) e.insert(a);
    for_each_word([&](uint32_t a, uint32_t w) {
        uint32_t op = w >> 26;
        if (op == 18 && (w & 1)) {
            if (importCalls.count(a) || undefCalls.count(a)) return;
            uint32_t t = (uint32_t)(sext(w & 0x03FFFFFC, 26) + (w & 2 ? 0 : (int32_t)a));
            if (inText(t)) e.insert(t);
        } else if (op == 16 && (w & 1)) {
            uint32_t t = (uint32_t)(sext(w & 0xFFFC, 16) + (w & 2 ? 0 : (int32_t)a));
            if (inText(t)) e.insert(t);
        }
    });
    std::set<uint32_t> tables;
    for (auto& [b, jt] : jumpTables) tables.insert(jt.first);
    for (auto& [t, refs] : codeRefs)
        if (!tables.count(t)) e.insert(t);
    entries.assign(e.begin(), e.end());
    return true;
}

uint32_t Program::funcOf(uint32_t a) const {
    auto it = std::upper_bound(entries.begin(), entries.end(), a);
    return it == entries.begin() ? 0 : *(it - 1);
}

uint32_t Program::funcEnd(uint32_t start) const {
    uint32_t regionEnd = textHi;
    if (start >= caveLo && start < caveHi) {  // a cave function ends where its instructions do
        regionEnd = start + 4;
        while (regionEnd < caveHi && caveIsCode[(regionEnd - caveLo) >> 2]) regionEnd += 4;
    }
    auto it = std::upper_bound(entries.begin(), entries.end(), start);
    return it == entries.end() || *it > regionEnd ? regionEnd : *it;
}

// branch targets that land inside another function become entries (tail calls)
void Program::discover() {
    fixpointRounds = 0;
    for (;;) {
        std::set<uint32_t> add;
        for_each_word([&](uint32_t a, uint32_t w) {
            uint32_t t = branch_target(a, w);
            if (t == 0xFFFFFFFFu || !inText(t) || std::binary_search(entries.begin(), entries.end(), t)) return;
            if (funcOf(t) != funcOf(a) || (t >= caveLo && t < caveHi) != (a >= caveLo && a < caveHi)) add.insert(t);
        });
        fixpointRounds++;
        if (add.empty()) break;
        std::set<uint32_t> all(entries.begin(), entries.end());
        all.insert(add.begin(), add.end());
        entries.assign(all.begin(), all.end());
    }
}

}  // namespace recomp
