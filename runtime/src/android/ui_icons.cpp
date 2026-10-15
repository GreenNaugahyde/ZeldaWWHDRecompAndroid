// Artwork for the touch controls, taken from the player's own game files at run time (never shipped
// with the app; from pull request #8 by SSunnKing): item, equipment and HUD textures from the 2D pack
// (content/Common/Pack/permanent_2d_*.pack: a SARC of Yaz0-compressed layout SARCs whose timg/
// folder holds BFLIM textures). Each texture is decoded to RGBA8 and written as
// "<name>.rgba" (uint32 width, uint32 height, pixels); the app makes the button icons from them
// (GameIcons.java).
#include "ui_icons.h"
#include "game_archive.h"

#include <dirent.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "../runtime.h"
#include "../vk/vk_formats.h"

namespace ui_icons {
namespace {
using game_archive::be16;
using game_archive::be32;
using game_archive::Bytes;
using game_archive::read_file;
using game_archive::sarc_file;
using game_archive::yaz0;

// BFLIM (Wii U): image data, then the FLIM header and the "imag" block (last 0x28 bytes)
bool decode_bflim(const Bytes& b, uint32_t& w, uint32_t& h, Bytes& rgba) {
    if (b.size() < 0x28 || memcmp(&b[b.size() - 0x28], "FLIM", 4) != 0) return false;
    const uint8_t* im = &b[b.size() - 0x14];
    w = be16(im + 8);
    h = be16(im + 10);
    uint8_t fmt = im[14], st = im[15];
    uint32_t dataSize = be32(im + 16);
    struct F { uint32_t gx2, bpb; bool comp; gfx::Convert cv; };
    F f;
    switch (fmt) {
        case 1: f = {0x01, 1, false, gfx::Convert::NONE}; break;    // A8
        case 3: f = {0x07, 2, false, gfx::Convert::NONE}; break;    // LA8
        case 15: case 16: f = {0x34, 8, true, gfx::Convert::BC4U}; break;  // BC4 luminance / alpha
        case 17: f = {0x35, 16, true, gfx::Convert::BC5U}; break;   // BC5 (luminance, alpha)
        case 20: f = {0x41A, 4, false, gfx::Convert::NONE}; break;  // RGBA8 sRGB
        case 21: f = {0x431, 8, true, gfx::Convert::BC1}; break;    // BC1 sRGB
        case 23: f = {0x433, 16, true, gfx::Convert::BC3}; break;   // BC3 sRGB
        default: return false;
    }
    if (dataSize > b.size() || !w || !h) return false;
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)f.gx2, w, h, 1, Latte::E_DIM::DIM_2D,
                                          Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)(st & 31)), 0, 0, &info);
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t swizzle = ((st >> 5) & 7) << 8, pipe = (swizzle >> 8) & 1, bank = (swizzle >> 9) & 3;
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
    uint32_t ch = f.cv == gfx::Convert::BC4U ? 1 : f.cv == gfx::Convert::BC5U ? 2 : f.comp ? 4 : f.bpb;
    Bytes tex((size_t)pw * ph * ch);
    if (f.comp)
        for (uint32_t y = 0; y < bh; y++) gfx::convert_row(f.cv, &lin[(size_t)y * bw * f.bpb], &tex[(size_t)y * 4 * pw * ch], bw, pw * ch);
    else
        tex = lin;
    rgba.assign((size_t)w * h * 4, 0);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* s = &tex[((size_t)y * pw + x) * ch];
            uint8_t* d = &rgba[((size_t)y * w + x) * 4];
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

// the 2D pack: any language (the textures used here are the same in all of them)
std::string find_pack(const std::string& gameDir) {
    std::string dir = gameDir + "/content/Common/Pack";
    std::string found;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.rfind("permanent_2d_", 0) == 0 && n.size() > 5 && n.compare(n.size() - 5, 5, ".pack") == 0)
                if (found.empty() || n.find("UsEnglish") != std::string::npos) found = dir + "/" + n;
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
}  // namespace ui_icons
