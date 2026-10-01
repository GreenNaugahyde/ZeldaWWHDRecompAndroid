// shim: GX2Surface layout (see Cemu src/Cafe/OS/libs/gx2/GX2_Surface.h)
#pragma once
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"

struct GX2Surface
{
	/* +0x000 */ betype<Latte::E_DIM> dim;
	/* +0x004 */ uint32be width;
	/* +0x008 */ uint32be height;
	/* +0x00C */ uint32be depth;
	/* +0x010 */ uint32be numLevels;
	/* +0x014 */ betype<Latte::E_GX2SURFFMT> format;
	/* +0x018 */ uint32be aa;
	/* +0x01C */ uint32be resFlag;
	/* +0x020 */ uint32be imageSize;
	/* +0x024 */ uint32be imagePtr;
	/* +0x028 */ uint32be mipSize;
	/* +0x02C */ uint32be mipPtr;
	/* +0x030 */ betype<Latte::E_GX2TILEMODE> tileMode;
	/* +0x034 */ uint32be swizzle;
	/* +0x038 */ uint32be alignment;
	/* +0x03C */ uint32be pitch;
	/* +0x040 */ uint32be mipOffset[13];
};
static_assert(sizeof(GX2Surface) == 0x74);
