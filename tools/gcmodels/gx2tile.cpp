// Wii U textures tiled / untiled with the runtime's own address library (Cemu's LatteAddrLib), for gcmodels:
//   gx2tile tile   FORMAT WIDTH HEIGHT MIPS TILEMODE SWIZZLE linear.bin image.bin mips.bin
//   gx2tile untile FORMAT WIDTH HEIGHT MIPS TILEMODE SWIZZLE image.bin mips.bin linear.bin
// linear.bin: the levels one after the other, rows of blocks (BC formats: 4x4 texel blocks) without padding.
// image.bin / mips.bin: a BFRES texture's data and mip data (FTEX sizData / sizMipData bytes).
// Numbers in hex or decimal. Only block-compressed formats (BC1-BC5) and 8/16/32-bit plain ones.
//   c++ -std=c++20 -O2 -I runtime/third_party/cemu -I runtime/third_party/cemu/Cafe -I runtime/src/vk
//       -include runtime/third_party/cemu/cemu_shim.h tools/gcmodels/gx2tile.cpp
//       runtime/third_party/cemu/Cafe/HW/Latte/LatteAddrLib/LatteAddrLib*.cpp -o gx2tile
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "texture_levels.h"

static std::vector<uint8_t> load(const char* p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

static void save(const char* p, const std::vector<uint8_t>& d) {
    std::ofstream(p, std::ios::binary).write((const char*)d.data(), d.size());
}

int main(int argc, char** argv) {
    if (argc != 11) {
        fprintf(stderr, "usage: gx2tile tile|untile FORMAT WIDTH HEIGHT MIPS TILEMODE SWIZZLE a b c\n");
        return 2;
    }
    const bool tile = std::string(argv[1]) == "tile";
    auto num = [](const char* s) { return (uint32_t)strtoul(s, nullptr, 0); };
    const uint32_t format = num(argv[2]), width = num(argv[3]), height = num(argv[4]), mips = num(argv[5]);
    const uint32_t tileMode = num(argv[6]), swizzle = num(argv[7]);
    const uint32_t f = format & 0x3F;
    const bool bc = f >= 0x31 && f <= 0x35;
    const uint32_t blockBytes = bc ? (f == 0x31 || f == 0x34 ? 8 : 16)
                                   : (f == 0x01 ? 1 : f == 0x05 || f == 0x07 || f == 0x08 ? 2 : 4);
    const uint32_t bpp = blockBytes * 8;

    // where each level lives: level 0 at the image, the others in the mip data (fake aligned addresses)
    const uint32_t imageBase = 0x10000000, mipBase = 0x20000000;
    const uint32_t swz = swizzle & 0x700;
    LatteAddrLib::AddrSurfaceInfo_OUT info0{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)format, width, height, 1, Latte::E_DIM::DIM_2D,
                                          (Latte::E_GX2TILEMODE)tileMode, 0, 0, &info0);
    const auto hwTile = (uint32_t)info0.hwTileMode;

    std::vector<uint8_t> linear, image, mipData;
    if (tile) {
        linear = load(argv[8]);
    } else {
        image = load(argv[8]);
        mipData = load(argv[9]);
    }
    size_t lin = 0;
    uint32_t imageSize = (uint32_t)info0.surfSize, mipEnd = 0;
    for (uint32_t level = 0; level < mips; level++) {
        LatteAddrLib::AddrSurfaceInfo_OUT info{};
        LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)format, width, height, 1, Latte::E_DIM::DIM_2D,
                                              (Latte::E_GX2TILEMODE)tileMode, 0, level, &info);
        const texture_levels::Level at = texture_levels::level(imageBase, mipBase | swz, format, width, height, 1,
                                                               (uint32_t)Latte::E_DIM::DIM_2D, hwTile, swz, level);
        const uint32_t lw = std::max(width >> level, 1u), lh = std::max(height >> level, 1u);
        const uint32_t bw = bc ? (lw + 3) / 4 : lw, bh = bc ? (lh + 3) / 4 : lh;
        const auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
        const bool macro = Latte::TM_IsMacroTiled(tm);
        const uint32_t pipeSwizzle = (at.swizzle >> 8) & 1, bankSwizzle = (at.swizzle >> 9) & 3;
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        if (macro)
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, 0, 0, bpp, info.pitch, info.height, 1, 1, tm, false, pipeSwizzle,
                                                     bankSwizzle);
        std::vector<uint8_t>& block = level == 0 ? image : mipData;
        const uint32_t base = level == 0 ? 0 : at.address - mipBase;
        if (tile) {
            size_t need = base + (size_t)info.surfSize;
            if (level == 0) need = imageSize;
            if (block.size() < need) block.resize(need, 0);
        }
        for (uint32_t y = 0; y < bh; y++)
            for (uint32_t x = 0; x < bw; x++) {
                uint32_t off;
                if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, 0, 0, bpp, info.pitch, info.height, 1);
                else if (!macro)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, 0, bpp, info.pitch, info.height, tm, false);
                else
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                const size_t at2 = base + off;
                if (tile) {
                    if (at2 + blockBytes > block.size()) block.resize(at2 + blockBytes, 0);
                    if (lin + blockBytes <= linear.size()) memcpy(&block[at2], &linear[lin], blockBytes);
                } else {
                    linear.resize(lin + blockBytes, 0);
                    if (at2 + blockBytes <= block.size()) memcpy(&linear[lin], &block[at2], blockBytes);
                }
                lin += blockBytes;
            }
        if (level > 0) mipEnd = std::max<uint32_t>(mipEnd, base + (uint32_t)info.surfSize);
        printf("level %u %ux%u blocks %ux%u pitch %u tile %u offset %u size %llu\n", level, lw, lh, bw, bh, info.pitch,
               (uint32_t)tm, base, (unsigned long long)info.surfSize);
    }
    if (tile) {
        image.resize(imageSize, 0);
        save(argv[9], image);
        save(argv[10], mipData);
    } else {
        save(argv[10], linear);
    }
    printf("image %u mips %zu\n", imageSize, tile ? mipData.size() : (size_t)mipEnd);
    return 0;
}
