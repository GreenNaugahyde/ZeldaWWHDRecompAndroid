// Artwork for the touch controls, taken from the player's own game files at run time (never shipped
// with the app; from pull request #8 by SSunnKing): item, equipment and HUD textures from the 2D pack
// (content/Common/Pack/permanent_2d_*.pack: a SARC of Yaz0-compressed layout SARCs whose timg/
// folder holds BFLIM textures). Each texture is decoded to RGBA8 and written as
// "<name>.rgba" (uint32 width, uint32 height, pixels); the app makes the button icons from them
// (GameIcons.java).
#include "ui_icons.h"
#include "game_archive.h"

#include <dirent.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "../runtime.h"
#include "../vk/vk_formats.h"

namespace ui_icons {
using game_archive::Bytes;

bool decode_surface(const uint8_t* b, uint32_t dataSize, uint32_t w, uint32_t h, uint32_t gx2Format, uint32_t tileMode,
                    uint32_t swizzle, Bytes& out, uint32_t& ch) {
    struct F { uint32_t bpb; bool comp; gfx::Convert cv; };
    F f;
    switch (gx2Format & 0x3F) {
        case 0x01: f = {1, false, gfx::Convert::NONE}; break;  // R8
        case 0x07: f = {2, false, gfx::Convert::NONE}; break;  // R8G8
        case 0x1A: f = {4, false, gfx::Convert::NONE}; break;  // RGBA8
        case 0x31: f = {8, true, gfx::Convert::BC1}; break;
        case 0x33: f = {16, true, gfx::Convert::BC3}; break;
        case 0x34: f = {8, true, gfx::Convert::BC4U}; break;
        case 0x35: f = {16, true, gfx::Convert::BC5U}; break;
        default: return false;
    }
    if (!w || !h) return false;
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)gx2Format, w, h, 1, Latte::E_DIM::DIM_2D,
                                          Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)(tileMode & 31)), 0, 0, &info);
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t pipe = (swizzle >> 8) & 1, bank = (swizzle >> 9) & 3;
    uint32_t bw = f.comp ? (w + 3) / 4 : w, bh = f.comp ? (h + 3) / 4 : h, bpp = f.bpb * 8;
    // linear blocks
    Bytes lin((size_t)bw * bh * f.bpb);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t off;
            if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, 0, 0, bpp, info.pitch, info.height, 1);
            else if (!Latte::TM_IsMacroTiled(tm))
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0, bpp, info.pitch, info.height, tm, false);
            else
                off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiled(x, y, 0, 0, bpp, info.pitch, info.height, 1, tm, false, pipe, bank);
            if (off + f.bpb <= dataSize) memcpy(&lin[((size_t)y * bw + x) * f.bpb], &b[off], f.bpb);
        }
    // texels: decoded blocks have 4 rows per block row; channels per texel as convert_row writes them
    uint32_t pw = f.comp ? bw * 4 : w, ph = f.comp ? bh * 4 : h;
    ch = f.cv == gfx::Convert::BC4U ? 1 : f.cv == gfx::Convert::BC5U ? 2 : f.comp ? 4 : f.bpb;
    Bytes tex((size_t)pw * ph * ch);
    if (f.comp)
        for (uint32_t y = 0; y < bh; y++) gfx::convert_row(f.cv, &lin[(size_t)y * bw * f.bpb], &tex[(size_t)y * 4 * pw * ch], bw, pw * ch);
    else
        tex = lin;
    out.resize((size_t)w * h * ch);
    for (uint32_t y = 0; y < h; y++) memcpy(&out[(size_t)y * w * ch], &tex[(size_t)y * pw * ch], (size_t)w * ch);
    return true;
}

namespace {
using game_archive::be16;
using game_archive::be32;
using game_archive::read_file;
using game_archive::sarc_file;
using game_archive::yaz0;

// a GX2 surface (BFLIM format code fmt, st = tile mode | swizzle << 5) of dataSize bytes at b to RGBA8
bool decode_gx2(const uint8_t* b, uint32_t dataSize, uint32_t w, uint32_t h, uint8_t fmt, uint8_t st, Bytes& rgba) {
    uint32_t gx2;
    switch (fmt) {
        case 1: gx2 = 0x01; break;             // A8
        case 3: gx2 = 0x07; break;             // LA8
        case 15: case 16: gx2 = 0x34; break;   // BC4 luminance / alpha
        case 17: gx2 = 0x35; break;            // BC5 (luminance, alpha)
        case 20: gx2 = 0x41A; break;           // RGBA8 sRGB
        case 21: gx2 = 0x431; break;           // BC1 sRGB
        case 23: gx2 = 0x433; break;           // BC3 sRGB
        default: return false;
    }
    Bytes raw;
    uint32_t ch;
    if (!decode_surface(b, dataSize, w, h, gx2, st & 31, ((st >> 5) & 7) << 8, raw, ch)) return false;
    rgba.assign((size_t)w * h * 4, 0);
    for (size_t i = 0; i < (size_t)w * h; i++) {
        const uint8_t* s = &raw[i * ch];
        uint8_t* d = &rgba[i * 4];
        switch (fmt) {
            case 1: d[0] = d[1] = d[2] = 255; d[3] = s[0]; break;
            case 3: case 17: d[0] = d[1] = d[2] = s[0]; d[3] = s[1]; break;
            case 15: d[0] = d[1] = d[2] = s[0]; d[3] = 255; break;
            case 16: d[0] = d[1] = d[2] = 255; d[3] = s[0]; break;
            default: memcpy(d, s, 4); break;
        }
    }
    return true;
}

// BFLIM (Wii U): image data, then the FLIM header and the "imag" block (last 0x28 bytes)
bool decode_bflim(const Bytes& b, uint32_t& w, uint32_t& h, Bytes& rgba) {
    if (b.size() < 0x28 || memcmp(&b[b.size() - 0x28], "FLIM", 4) != 0) return false;
    const uint8_t* im = &b[b.size() - 0x14];
    w = be16(im + 8);
    h = be16(im + 10);
    uint32_t dataSize = be32(im + 16);
    if (dataSize > b.size()) return false;
    return decode_gx2(b.data(), dataSize, w, h, im[14], im[15], rgba);
}

// the 2D pack: the game language's (WWHD_LANGUAGE: its font has that language's letters, e.g. German's Ä),
// else English, else any (the textures used here are the same in all of them)
std::string find_pack(const std::string& gameDir) {
    static const char* const kNames[][2] = {{"en", "English"}, {"fr", "French"}, {"de", "German"},
                                            {"it", "Italian"}, {"es", "Spanish"}, {"ja", "Japanese"}};
    const char* lang = getenv("WWHD_LANGUAGE");
    std::string want = "English";
    for (auto& n : kNames)
        if (lang && !strcmp(lang, n[0])) want = n[1];
    std::string dir = gameDir + "/content/Common/Pack";
    std::string found;
    int rank = 0;  // 3 the language's, 2 English, 1 any
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("permanent_2d_", 0) != 0 || n.size() <= 5 || n.compare(n.size() - 5, 5, ".pack") != 0) continue;
            int r = n.find(want) != std::string::npos ? 3 : n.find("English") != std::string::npos ? 2 : 1;
            if (r > rank) {
                rank = r;
                found = dir + "/" + n;
            }
        }
        closedir(d);
    }
    return found;
}
}  // namespace

// every texture of every layout archive in the pack, as outDir/<layout>__<texture>.rgba (to look at them)
int dump_all(const std::string& gameDir, const std::string& outDir) {
    Bytes pack;
    std::string packPath = find_pack(gameDir);
    if (packPath.empty() || !read_file(packPath, pack)) return 0;
    auto list = [](const Bytes& b, std::vector<std::string>& names) {
        if (b.size() < 0x14 || memcmp(b.data(), "SARC", 4) != 0) return;
        uint32_t hdr = be16(&b[4]);
        size_t p = hdr;
        if (p + 12 > b.size() || memcmp(&b[p], "SFAT", 4) != 0) return;
        uint32_t n = be16(&b[p + 6]);
        size_t nodes = p + 12, nm = nodes + 16 * n + 8;
        for (uint32_t i = 0; i < n; i++) {
            uint32_t attr = be32(&b[nodes + 16 * i + 4]);
            if (!(attr >> 24)) continue;
            size_t o = nm + (attr & 0xFFFFFF) * 4;
            if (o < b.size()) names.push_back((const char*)&b[o]);
        }
    };
    std::vector<std::string> layouts;
    list(pack, layouts);
    int written = 0;
    for (const std::string& l : layouts) {
        if (l.size() < 5 || l.compare(l.size() - 4, 4, ".szs") != 0) continue;
        Bytes packed, layout;
        if (!sarc_file(pack, l.c_str(), packed) || !yaz0(packed, layout)) continue;
        std::vector<std::string> files;
        list(layout, files);
        for (const std::string& f : files) {
            if (f.rfind("timg/", 0) != 0 || f.size() < 12 || f.compare(f.size() - 6, 6, ".bflim") != 0) continue;
            Bytes flim, rgba;
            uint32_t w = 0, h = 0;
            if (!sarc_file(layout, f.c_str(), flim) || !decode_bflim(flim, w, h, rgba)) continue;
            std::string name = l.substr(0, l.size() - 4) + "__" + f.substr(5, f.size() - 11);
            for (char& ch : name)
                if (ch == '^' || ch == '/') ch = '_';
            if (FILE* o = fopen((outDir + "/" + name + ".rgba").c_str(), "wb")) {
                uint32_t hd[2] = {w, h};
                fwrite(hd, 4, 2, o);
                fwrite(rgba.data(), 1, rgba.size(), o);
                fclose(o);
                written++;
            }
        }
    }
    LOG("[icons] dumped %d textures of %zu archives", written, layouts.size());
    return written;
}

int extract(const std::string& gameDir, const std::string& outDir, const std::vector<Request>& requests) {
    Bytes pack;
    std::string packPath = find_pack(gameDir);
    if (packPath.empty() || !read_file(packPath, pack)) {
        LOG("[icons] no 2D pack in %s", gameDir.c_str());
        return 0;
    }
    int written = 0;
    std::string lastLayout;
    Bytes layout;
    for (const Request& r : requests) {
        if (r.layout != lastLayout) {
            Bytes packed;
            layout.clear();
            if (sarc_file(pack, (r.layout + ".szs").c_str(), packed)) yaz0(packed, layout);
            lastLayout = r.layout;
        }
        Bytes flim, rgba;
        uint32_t w = 0, h = 0;
        if (layout.empty() || !sarc_file(layout, ("timg/" + r.texture + ".bflim").c_str(), flim) || !decode_bflim(flim, w, h, rgba)) {
            LOG("[icons] %s/%s not found", r.layout.c_str(), r.texture.c_str());
            continue;
        }
        std::string path = outDir + "/" + r.texture + ".rgba";
        if (FILE* f = fopen(path.c_str(), "wb")) {
            uint32_t hdr[2] = {w, h};
            fwrite(hdr, 4, 2, f);
            fwrite(rgba.data(), 1, rgba.size(), f);
            fclose(f);
            written++;
        }
    }
    LOG("[icons] %d of %zu textures from %s", written, requests.size(), packPath.c_str());
    return written;
}

// a font of the 2D pack (<font>_bffnt.szs holding <font>.bffnt, Wii U FFNT version 3): its glyph sheets as
// outDir/<font>_<n>.rgba (white, the glyph in alpha) and outDir/<font>.txt:
//   "cell W H baseline cols rows sheetW sheetH sheets", "default left glyphW charW",
//   then a line "code index left glyphW charW" per character (codes in UTF-16)
bool extract_font(const std::string& gameDir, const std::string& outDir, const std::string& font) {
    Bytes pack, packed, arc, f;
    std::string packPath = find_pack(gameDir);
    if (packPath.empty() || !read_file(packPath, pack) || !sarc_file(pack, (font + "_bffnt.szs").c_str(), packed) ||
        !yaz0(packed, arc) || !sarc_file(arc, (font + ".bffnt").c_str(), f) || f.size() < 0x40 || memcmp(f.data(), "FFNT", 4) != 0) {
        LOG("[icons] font %s not found", font.c_str());
        return false;
    }
    uint32_t hdr = be16(&f[6]);
    // sections in order: FINF, TGLP, CWDH..., CMAP...
    const uint8_t *finf = nullptr, *tglp = nullptr;
    std::vector<const uint8_t*> cwdh, cmap;
    for (size_t p = hdr; p + 8 <= f.size();) {
        uint32_t sz = be32(&f[p + 4]);
        if (!memcmp(&f[p], "FINF", 4)) finf = &f[p];
        else if (!memcmp(&f[p], "TGLP", 4)) tglp = &f[p];
        else if (!memcmp(&f[p], "CWDH", 4)) cwdh.push_back(&f[p]);
        else if (!memcmp(&f[p], "CMAP", 4)) cmap.push_back(&f[p]);
        if (sz < 8) break;
        p += sz;
    }
    if (!finf || !tglp) return false;
    uint32_t cw = tglp[8], chh = tglp[9], sheets = tglp[10], sheetSize = be32(tglp + 12), baseline = be16(tglp + 16);
    uint32_t format = be16(tglp + 18) & 0x7FFF, cols = be16(tglp + 20), rows = be16(tglp + 22), sw = be16(tglp + 24), sh = be16(tglp + 26);
    uint32_t data = be32(tglp + 28);
    // TGLP sheet formats -> BFLIM format codes (the alpha ones; the font's glyphs are coverage)
    uint8_t fmt;
    switch (format) {
        case 8: fmt = 1; break;    // A8
        case 5: fmt = 3; break;    // LA8
        case 12: fmt = 16; break;  // BC4 (alpha)
        case 13: fmt = 17; break;  // BC5
        default: LOG("[icons] font %s: sheet format %u", font.c_str(), format); return false;
    }
    static const int tileMode = getenv("WWHD_FONT_TILE") ? atoi(getenv("WWHD_FONT_TILE")) : 4;  // (2D tiled, as GX2 lays out sheets)
    for (uint32_t i = 0; i < sheets; i++) {
        if (data + (size_t)(i + 1) * sheetSize > f.size()) return false;
        Bytes rgba;
        if (!decode_gx2(&f[data + (size_t)i * sheetSize], sheetSize, sw, sh, fmt, (uint8_t)tileMode, rgba)) return false;
        FILE* o = fopen((outDir + "/" + font + "_" + std::to_string(i) + ".rgba").c_str(), "wb");
        if (!o) return false;
        uint32_t hd[2] = {sw, sh};
        fwrite(hd, 4, 2, o);
        fwrite(rgba.data(), 1, rgba.size(), o);
        fclose(o);
    }
    // widths by glyph index; then the character map
    std::vector<std::array<int, 3>> widths;
    for (const uint8_t* c : cwdh) {
        uint32_t first = be16(c + 8), last = be16(c + 10);
        if (widths.size() <= last) widths.resize(last + 1, {(int8_t)finf[16], finf[17], (int8_t)finf[18]});
        for (uint32_t g = first; g <= last; g++) {
            const uint8_t* e = c + 16 + (g - first) * 3;
            widths[g] = {(int8_t)e[0], e[1], (int8_t)e[2]};
        }
    }
    std::string out;
    char line[96];
    snprintf(line, sizeof line, "cell %u %u %u %u %u %u %u %u\n", cw, chh, baseline, cols, rows, sw, sh, sheets);
    out += line;
    snprintf(line, sizeof line, "default %d %u %d\n", (int8_t)finf[16], finf[17], (int8_t)finf[18]);
    out += line;
    auto put = [&](uint32_t code, uint32_t index) {
        if (index == 0xFFFF) return;
        std::array<int, 3> w = index < widths.size() ? widths[index] : std::array<int, 3>{(int8_t)finf[16], finf[17], (int8_t)finf[18]};
        snprintf(line, sizeof line, "%u %u %d %d %d\n", code, index, w[0], w[1], w[2]);
        out += line;
    };
    for (const uint8_t* m : cmap) {
        uint32_t first = be16(m + 8), last = be16(m + 10), method = be16(m + 12);
        const uint8_t* d = m + 20;
        if (method == 0) {  // direct: an offset
            for (uint32_t c = first; c <= last; c++) put(c, be16(d) + (c - first));
        } else if (method == 1) {  // a table
            for (uint32_t c = first; c <= last; c++) put(c, be16(d + (c - first) * 2));
        } else {  // scan: pairs
            uint32_t n = be16(d);
            for (uint32_t k = 0; k < n; k++) put(be16(d + 2 + k * 4), be16(d + 4 + k * 4));
        }
    }
    FILE* o = fopen((outDir + "/" + font + ".txt").c_str(), "wb");
    if (!o) return false;
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    LOG("[icons] font %s: %u sheets %ux%u (format %u), %zu widths", font.c_str(), sheets, sw, sh, format, widths.size());
    return true;
}
// the texts of a layout's message file in the 2D pack (<layout>_msbt.szs holding <layout>.msbt, MsgStdBn: LBL1
// labels, TXT2 texts in UTF-16) as outFile, a line "label<TAB>text" each in UTF-8 (a line break as \n); the game's control codes
// (0x0E, e.g. an icon) are left out, but a colour change is a '*' (names in messages are coloured)
bool extract_texts(const std::string& gameDir, const std::string& outFile, const std::string& layout) {
    Bytes pack, packed, arc, m;
    std::string packPath = find_pack(gameDir);
    if (packPath.empty() || !read_file(packPath, pack) || !sarc_file(pack, (layout + "_msbt.szs").c_str(), packed) ||
        !yaz0(packed, arc) || !sarc_file(arc, (layout + ".msbt").c_str(), m) || m.size() < 0x20 || memcmp(m.data(), "MsgStdBn", 8) != 0 ||
        m[8] != 0xFE || m[9] != 0xFF || m[0x0C] != 1) {  // big-endian UTF-16 only
        LOG("[icons] texts %s not found", layout.c_str());
        return false;
    }
    const uint8_t *lbl = nullptr, *txt = nullptr;
    size_t lblSize = 0, txtSize = 0;
    uint32_t sections = be16(&m[0x0E]);
    for (size_t p = 0x20; sections-- && p + 16 <= m.size();) {
        uint32_t sz = be32(&m[p + 4]);
        if (p + 16 + sz > m.size()) break;
        if (!memcmp(&m[p], "LBL1", 4)) lbl = &m[p + 16], lblSize = sz;
        else if (!memcmp(&m[p], "TXT2", 4)) txt = &m[p + 16], txtSize = sz;
        p = (p + 16 + sz + 15) & ~(size_t)15;
    }
    if (!lbl || !txt || txtSize < 4 || lblSize < 4) return false;
    uint32_t count = be32(txt);
    if (4 + (size_t)count * 4 > txtSize) return false;
    std::vector<std::string> labels(count);
    uint32_t buckets = be32(lbl);
    for (uint32_t b = 0; b < buckets && 12 + (size_t)b * 8 <= lblSize; b++) {
        uint32_t n = be32(lbl + 4 + b * 8);
        size_t q = be32(lbl + 8 + b * 8);
        for (uint32_t k = 0; k < n && q < lblSize; k++) {
            uint32_t len = lbl[q];
            if (q + 1 + len + 4 > lblSize) break;
            uint32_t index = be32(lbl + q + 1 + len);
            if (index < count) labels[index].assign((const char*)lbl + q + 1, len);
            q += 1 + len + 4;
        }
    }
    std::string out;
    for (uint32_t i = 0; i < count; i++) {
        size_t a = be32(txt + 4 + i * 4), e = i + 1 < count ? be32(txt + 8 + i * 4) : txtSize;
        if (labels[i].empty() || a > e || e > txtSize) continue;
        std::string t;
        for (size_t q = a; q + 1 < e;) {
            uint32_t ch = be16(txt + q);
            q += 2;
            if (ch == 0) break;
            if (ch == 0x0E) {  // group, type, parameter size, parameters
                if (q + 6 > e) break;
                if (be16(txt + q) == 0 && be16(txt + q + 2) == 3) t += '*';  // a colour (a name's start or end)
                q += 6 + be16(txt + q + 4);
                continue;
            }
            if (ch == '\n') {  // kept as the two characters \n (the figurines' names are their texts' first lines)
                t += "\\n";
                continue;
            }
            if (ch == '\t' || ch == '\r') ch = ' ';
            if (ch >= 0xD800 && ch < 0xDC00 && q + 1 < e) {  // a surrogate pair
                ch = 0x10000 + ((ch - 0xD800) << 10) + (be16(txt + q) - 0xDC00);
                q += 2;
            }
            if (ch < 0x80) t += (char)ch;
            else if (ch < 0x800) t += (char)(0xC0 | ch >> 6), t += (char)(0x80 | (ch & 0x3F));
            else if (ch < 0x10000) t += (char)(0xE0 | ch >> 12), t += (char)(0x80 | (ch >> 6 & 0x3F)), t += (char)(0x80 | (ch & 0x3F));
            else t += (char)(0xF0 | ch >> 18), t += (char)(0x80 | (ch >> 12 & 0x3F)), t += (char)(0x80 | (ch >> 6 & 0x3F)), t += (char)(0x80 | (ch & 0x3F));
        }
        out += labels[i] + "\t" + t + "\n";
    }
    FILE* o = fopen(outFile.c_str(), "wb");
    if (!o) return false;
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    LOG("[icons] texts %s: %u from %s", layout.c_str(), count, packPath.c_str());
    return true;
}
}  // namespace ui_icons
