// GX2 surface size/alignment calculation.
// Adapted from Cemu src/Cafe/OS/libs/gx2/GX2_Surface.cpp
// Copyright (c) Cemu contributors. Licensed under the Mozilla Public License 2.0 (see LICENSE.txt).
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include "gx2_surface_calc.h"

#define GX2_RESFLAG_USAGE_COLOR_BUFFER (1 << 1)
#define GX2_RESFLAG_USAGE_DEPTH_BUFFER (1 << 2)
#define GX2_RESFLAG_USAGE_SCAN_BUFFER (1 << 3)

namespace gx2calc {

void CalculateSurfaceInfo(GX2Surface* s, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* out) {
    bool depth = (s->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0;
    bool scan = (s->resFlag & GX2_RESFLAG_USAGE_SCAN_BUFFER) != 0;
    LatteAddrLib::GX2CalculateSurfaceInfo(s->format, s->width, s->height, s->depth, s->dim, s->tileMode, s->aa, level, out,
                                          depth, scan);
}

static uint32 calc_levels(uint32 resolution) {
    uint32 x = 0x80000000, n = 0;
    while (!(resolution & x)) {
        n++;
        if (n == 32) break;
        x >>= 1;
    }
    return 32 - n;
}

static uint32 adjust_level_count(GX2Surface* s) {
    if (s->numLevels <= 1) return 1;
    uint32 levels = std::max(calc_levels(s->width), calc_levels(s->height));
    if (s->dim == Latte::E_DIM::DIM_3D) levels = std::max(levels, calc_levels(s->depth));
    return levels;
}

void CalcSurfaceSizeAndAlignment(GX2Surface* surface) {
    LatteAddrLib::AddrSurfaceInfo_OUT surfOut = {0};
    uint32 firstMipOffset = 0;
    bool changeTilemode = false;
    Latte::E_GX2TILEMODE lastTilemode = surface->tileMode;
    bool hasTileMode32 = surface->tileMode == Latte::E_GX2TILEMODE::TM_32_SPECIAL;
    if (surface->tileMode == Latte::E_GX2TILEMODE::TM_LINEAR_GENERAL || hasTileMode32) {
        if (surface->dim != Latte::E_DIM::DIM_1D || (surface->resFlag & GX2_RESFLAG_USAGE_DEPTH_BUFFER) != 0 || surface->aa) {
            if (surface->dim != Latte::E_DIM::DIM_3D || (surface->resFlag & GX2_RESFLAG_USAGE_COLOR_BUFFER) != 0)
                surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THIN1;
            else
                surface->tileMode = Latte::E_GX2TILEMODE::TM_2D_TILED_THICK;
            changeTilemode = true;
        } else {
            surface->tileMode = Latte::E_GX2TILEMODE::TM_LINEAR_ALIGNED;
        }
        lastTilemode = surface->tileMode;
    }
    if (surface->numLevels == 0) surface->numLevels = 1;
    surface->numLevels = std::min<uint32>(surface->numLevels, adjust_level_count(surface));
    surface->mipOffset[0] = 0;
    if (Latte::TM_IsMacroTiled(surface->tileMode))
        surface->swizzle = (surface->swizzle & 0xFF00FFFF) | 0xD0000;
    else
        surface->swizzle = surface->swizzle & 0xFF00FFFF;
    uint32 fix32Mode = (hasTileMode32 && Latte::IsCompressedFormat(surface->format)) ? 2 : 0;
    uint32 prevSize = 0;
    for (uint32 level = 0; level < surface->numLevels; ++level) {
        CalculateSurfaceInfo(surface, level, &surfOut);
        if (level) {
            uint32 pad = 0;
            if (Latte::TM_IsMacroTiled(lastTilemode) && !Latte::TM_IsMacroTiled(surfOut.hwTileMode)) {
                surface->swizzle = (surface->swizzle & 0xFF00FFFF) | (level << 16);
                lastTilemode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
                if (level > 1) pad = surface->swizzle & 0xFFFF;
            }
            pad += (surfOut.baseAlign - prevSize % surfOut.baseAlign) % surfOut.baseAlign;
            if (level == 1)
                firstMipOffset = pad + prevSize;
            else if (level > 1)
                surface->mipOffset[level - 1] = pad + prevSize + surface->mipOffset[level - 2];
        } else {
            if (changeTilemode) {
                if (surface->tileMode != (Latte::E_GX2TILEMODE)surfOut.hwTileMode) {
                    surface->tileMode = (Latte::E_GX2TILEMODE)surfOut.hwTileMode;
                    CalculateSurfaceInfo(surface, 0, &surfOut);
                    if (!Latte::TM_IsMacroTiled(surface->tileMode)) surface->swizzle = surface->swizzle & 0xFF00FFFF;
                    lastTilemode = surface->tileMode;
                }
                if (surface->width < (surfOut.pitchAlign << fix32Mode) && surface->height < (surfOut.heightAlign << fix32Mode)) {
                    if (surface->tileMode == Latte::E_GX2TILEMODE::TM_2D_TILED_THICK)
                        surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THICK;
                    else
                        surface->tileMode = Latte::E_GX2TILEMODE::TM_1D_TILED_THIN1;
                    CalculateSurfaceInfo(surface, 0, &surfOut);
                    surface->swizzle = surface->swizzle & 0xFF00FFFF;
                    lastTilemode = surface->tileMode;
                }
            }
            surface->imageSize = (uint32)(surfOut.surfSize);
            surface->alignment = surfOut.baseAlign;
            surface->pitch = surfOut.pitch;
        }
        prevSize = (uint32)(surfOut.surfSize);
    }
    surface->mipSize = surface->numLevels > 1 ? prevSize + surface->mipOffset[surface->numLevels - 2] : 0;
    surface->mipOffset[0] = firstMipOffset;
    if (surface->format == Latte::E_GX2SURFFMT::NV12_UNORM) {
        uint32 padding = (surface->alignment - surface->imageSize % surface->alignment) % surface->alignment;
        surface->mipOffset[0] = padding + surface->imageSize;
        surface->imageSize = surface->mipOffset[0] + ((uint32)surface->imageSize >> 1);
    }
}

}  // namespace gx2calc
