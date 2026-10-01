#pragma once
#include "Cafe/OS/libs/gx2/GX2_Surface.h"
#include "Cafe/HW/Latte/ISA/LatteReg.h"

namespace GX2 {
struct GX2Texture {
    /* +0x00 */ GX2Surface surface;
    /* +0x74 */ uint32be viewFirstMip;
    /* +0x78 */ uint32be viewNumMips;
    /* +0x7C */ uint32be viewFirstSlice;
    /* +0x80 */ uint32be viewNumSlices;
    /* +0x84 */ uint32be compSel;
    /* +0x88 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N> regTexWord0;
    /* +0x8C */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N> regTexWord1;
    /* +0x90 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N> regTexWord4;
    /* +0x94 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N> regTexWord5;
    /* +0x98 */ betype<Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N> regTexWord6;
};
static_assert(sizeof(GX2Texture) == 0x9C);

struct GX2Sampler {
    betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0> word0;
    betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD1_0> word1;
    betype<Latte::LATTE_SQ_TEX_SAMPLER_WORD2_0> word2;
};
static_assert(sizeof(GX2Sampler) == 12);

using Latte::LATTE_SQ_TEX_SAMPLER_WORD0_0;
void GX2InitTextureRegs(GX2Texture* texture);
void GX2InitSampler(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampXYZ, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filterMinMag);
void GX2InitSamplerXYFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER magFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER minFilter, uint32 maxAnisoRatio);
void GX2InitSamplerZMFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER zFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER mipFilter);
void GX2InitSamplerLOD(GX2Sampler* sampler, float minLod, float maxLod, float lodBias);
void GX2InitSamplerClamping(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampX, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampY, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampZ);
void GX2InitSamplerBorderType(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE borderColorType);
void GX2InitSamplerDepthCompare(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_DEPTH_COMPARE depthCompareFunction);
}  // namespace GX2

namespace GX2 {
struct GX2ColorBuffer {
    /* +0x00 */ GX2Surface surface;
    /* +0x74 */ uint32be viewMip;
    /* +0x78 */ uint32be viewFirstSlice;
    /* +0x7C */ uint32be viewNumSlices;
    /* +0x80 */ uint32be auxData;
    /* +0x84 */ uint32be auxSize2;
    /* +0x88 */ uint32be reg_size;
    /* +0x8C */ uint32be reg_info;
    /* +0x90 */ uint32be reg_view;
    /* +0x94 */ uint32be reg_mask;
    /* +0x98 */ uint32be reg4;
};
static_assert(sizeof(GX2ColorBuffer) == 0x9C);

struct GX2DepthBuffer {
    /* +0x00 */ GX2Surface surface;
    /* +0x74 */ uint32be viewMip;
    /* +0x78 */ uint32be viewFirstSlice;
    /* +0x7C */ uint32be viewNumSlices;
    /* +0x80 */ uint32be hiZPtr;
    /* +0x84 */ uint32be hiZSize;
    /* +0x88 */ float32be clearDepth;
    /* +0x8C */ uint32be clearStencil;
    /* +0x90 */ uint32be reg_size;
    /* +0x94 */ uint32be reg_view;
    /* +0x98 */ uint32be reg_base;
    /* +0x9C */ uint32be reg_htile_surface;
    /* +0xA0 */ uint32be reg_prefetch_limit;
    /* +0xA4 */ uint32be reg_preload_control;
    /* +0xA8 */ uint32be reg_poly_offset_db_fmt_cntl;
};
static_assert(sizeof(GX2DepthBuffer) == 0xAC);

void GX2InitColorBufferRegs(GX2ColorBuffer* colorBuffer);
void GX2InitDepthBufferRegs(GX2DepthBuffer* depthBuffer);
}  // namespace GX2
