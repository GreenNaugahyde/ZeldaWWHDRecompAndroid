#pragma once
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
namespace gx2calc {
void CalculateSurfaceInfo(GX2Surface* s, uint32 level, LatteAddrLib::AddrSurfaceInfo_OUT* out);
void CalcSurfaceSizeAndAlignment(GX2Surface* surface);
}
