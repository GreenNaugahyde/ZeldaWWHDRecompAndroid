// GX2 texture and sampler register initialization.
// Adapted from Cemu src/Cafe/OS/libs/gx2/GX2_Texture.cpp
// Copyright (c) Cemu contributors. Licensed under the Mozilla Public License 2.0 (see LICENSE.txt).
#include "gx2_texture_regs.h"

using namespace Latte;
#define HAS_FLAG(v, f) ((((uint32)(v)) & ((uint32)(f))) != 0)

namespace GX2 {

void GX2InitTextureRegs(GX2Texture* texture)
	{
		uint32 _regs[5] = { 0 };

		// some values may not be zero
		if (texture->viewNumMips == 0)
			texture->viewNumMips = 1;
		if (texture->viewNumSlices == 0)
			texture->viewNumSlices = 1;

		if (texture->surface.height == 0)
			texture->surface.height = 1;
		if (texture->surface.depth == 0)
			texture->surface.depth = 1;
		if (texture->surface.numLevels == 0)
			texture->surface.numLevels = 1;

		// texture parameters
		uint32 viewNumMips = texture->viewNumMips;
		uint32 viewNumSlices = texture->viewNumSlices;
		uint32 viewFirstMip = texture->viewFirstMip;
		uint32 viewFirstSlice = texture->viewFirstSlice;
		uint32 compSel = texture->compSel;

		// surface parameters
		uint32 width = texture->surface.width;
		uint32 height = texture->surface.height;
		uint32 depth = texture->surface.depth;
		uint32 pitch = texture->surface.pitch;
		uint32 numMips = texture->surface.numLevels;
		Latte::E_GX2SURFFMT format = texture->surface.format;
		Latte::E_DIM dim = texture->surface.dim;
		uint32 tileMode = (uint32)texture->surface.tileMode.value();
		uint32 surfaceFlags = texture->surface.resFlag;
		uint32 surfaceAA = texture->surface.aa;

		// calculate register word 0
		Latte::E_HWFMT formatHw = Latte::GetHWFormat(format);

		Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N newRegWord0;
		newRegWord0.set_DIM(dim);
		newRegWord0.set_TILE_MODE(Latte::MakeHWTileMode(texture->surface.tileMode));
		newRegWord0.set_TILE_TYPE((surfaceFlags&4) != 0);

		uint32 pixelPitch = pitch;
		if (Latte::IsCompressedFormat(formatHw))
			pixelPitch *= 4;

		if(pixelPitch == 0)
			newRegWord0.set_PITCH(0x7FF);
		else
			newRegWord0.set_PITCH((pixelPitch >> 3) - 1);

		if (width == 0)
			newRegWord0.set_WIDTH(0x1FFF);
		else
			newRegWord0.set_WIDTH(width - 1);

		texture->regTexWord0 = newRegWord0;

		// calculate register word 1
		Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N newRegWord1;
		newRegWord1.set_HEIGHT(height - 1);

		if (dim == Latte::E_DIM::DIM_CUBEMAP)
		{
			newRegWord1.set_DEPTH((depth / 6) - 1);
		}
		else if (dim == E_DIM::DIM_3D ||
			dim == E_DIM::DIM_2D_ARRAY_MSAA ||
			dim == E_DIM::DIM_2D_ARRAY ||
			dim == E_DIM::DIM_1D_ARRAY)
		{
			newRegWord1.set_DEPTH(depth - 1);
		}
		else
		{
			newRegWord1.set_DEPTH(0);
		}
		newRegWord1.set_DATA_FORMAT(formatHw);
		texture->regTexWord1 = newRegWord1;

		// calculate register word 2
		LATTE_SQ_TEX_RESOURCE_WORD4_N newRegWord4;

		LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP formatComp;
		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SIGNED))
			formatComp = LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_SIGNED;
		else
			formatComp = LATTE_SQ_TEX_RESOURCE_WORD4_N::E_FORMAT_COMP::COMP_UNSIGNED;
		newRegWord4.set_FORMAT_COMP_X(formatComp);
		newRegWord4.set_FORMAT_COMP_Y(formatComp);
		newRegWord4.set_FORMAT_COMP_Z(formatComp);
		newRegWord4.set_FORMAT_COMP_W(formatComp);

		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_FLOAT))
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_SCALED);
		else if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_INT))
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_INT);
		else
			newRegWord4.set_NUM_FORM_ALL(LATTE_SQ_TEX_RESOURCE_WORD4_N::E_NUM_FORMAT_ALL::NUM_FORMAT_NORM);

		if (HAS_FLAG(format, Latte::E_GX2SURFFMT::FMT_BIT_SRGB))
			newRegWord4.set_FORCE_DEGAMMA(true);

		newRegWord4.set_ENDIAN_SWAP(Latte::E_ENDIAN_SWAP::SWAP_NONE);

		newRegWord4.set_REQUEST_SIZE(2);

		newRegWord4.set_DST_SEL_X((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 24) & 0x7));
		newRegWord4.set_DST_SEL_Y((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 16) & 0x7));
		newRegWord4.set_DST_SEL_Z((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 8) & 0x7));
		newRegWord4.set_DST_SEL_W((Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N::E_SEL)((compSel >> 0) & 0x7));

		newRegWord4.set_BASE_LEVEL(viewFirstMip);
		texture->regTexWord4 = newRegWord4;

		// calculate register word 3
		LATTE_SQ_TEX_RESOURCE_WORD5_N newRegWord5;
		newRegWord5.set_LAST_LEVEL(viewFirstMip + viewNumMips - 1);
		newRegWord5.set_BASE_ARRAY(viewFirstSlice);
		newRegWord5.set_LAST_ARRAY(viewFirstSlice + viewNumSlices - 1);
		if (dim == Latte::E_DIM::DIM_CUBEMAP && ((depth / 6) - 1) != 0)
			newRegWord5.set_UKN_BIT_30(true);
		if(surfaceAA >= 1 && surfaceAA <= 3)
			newRegWord5.set_LAST_LEVEL(surfaceAA);
		texture->regTexWord5 = newRegWord5;
		// calculate register word 4
		LATTE_SQ_TEX_RESOURCE_WORD6_N newRegWord6;
		newRegWord6.set_MAX_ANISO(4);
		newRegWord6.set_PERF_MODULATION(7);
		newRegWord6.set_TYPE(Latte::LATTE_SQ_TEX_RESOURCE_WORD6_N::E_TYPE::VTX_VALID_TEXTURE);
		texture->regTexWord6 = newRegWord6;
	}


void GX2InitSampler(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampXYZ, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filterMinMag)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0{};
		word0.set_CLAMP_X(clampXYZ).set_CLAMP_Y(clampXYZ).set_CLAMP_Z(clampXYZ);
		word0.set_XY_MAG_FILTER(filterMinMag).set_XY_MIN_FILTER(filterMinMag);
		word0.set_Z_FILTER(LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT);
		word0.set_MIP_FILTER(LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER::POINT);
		word0.set_TEX_ARRAY_OVERRIDE(true);

		LATTE_SQ_TEX_SAMPLER_WORD1_0 word1{};
		word1.set_MAX_LOD(0x3FF);

		LATTE_SQ_TEX_SAMPLER_WORD2_0 word2{};
		word2.set_TYPE(LATTE_SQ_TEX_SAMPLER_WORD2_0::E_SAMPLER_TYPE::UKN1);

		sampler->word0 = word0;
		sampler->word1 = word1;
		sampler->word2 = word2;
	}


void GX2InitSamplerXYFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER magFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER minFilter, uint32 maxAnisoRatio)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		if (maxAnisoRatio == 0)
		{
			word0.set_XY_MAG_FILTER(magFilter);
			word0.set_XY_MIN_FILTER(minFilter);
			word0.set_MAX_ANISO_RATIO(0);
		}
		else
		{
			auto getAnisoFilter = [](LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER filter) -> LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER
			{
				if (filter == LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT)
					return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_POINT;
				else if (filter == LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::BILINEAR)
					return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::ANISO_BILINEAR;
				else
					cemu_assert_debug(false);
				return LATTE_SQ_TEX_SAMPLER_WORD0_0::E_XY_FILTER::POINT;
			};
			word0.set_XY_MAG_FILTER(getAnisoFilter(magFilter));
			word0.set_XY_MIN_FILTER(getAnisoFilter(minFilter));
			word0.set_MAX_ANISO_RATIO(maxAnisoRatio);
		}
		sampler->word0 = word0;
	}


void GX2InitSamplerZMFilter(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER zFilter, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_Z_FILTER mipFilter)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_Z_FILTER(zFilter);
		word0.set_MIP_FILTER(mipFilter);
		sampler->word0 = word0;
	}


void GX2InitSamplerLOD(GX2Sampler* sampler, float minLod, float maxLod, float lodBias)
	{
		// known special cases: Mario & Sonic Rio passes minimum and maximum float values for minLod/maxLod
		if (minLod < 0.0)
			minLod = 0.0;
		if (maxLod > 16.0)
			maxLod = 16.0;

		uint32 iMinLod = ((uint32)floorf(minLod * 64.0f));
		uint32 iMaxLod = ((uint32)floorf(maxLod * 64.0f));
		sint32 iLodBias = (sint32)((sint32)floorf(lodBias * 64.0f)); // input range: -32.0 to 32.0
		iMinLod = std::clamp(iMinLod, 0u, 1023u);
		iMaxLod = std::clamp(iMaxLod, 0u, 1023u);
		iLodBias = std::clamp(iLodBias, -2048, 2047);

		LATTE_SQ_TEX_SAMPLER_WORD1_0 word1 = sampler->word1;
		word1.set_MIN_LOD(iMinLod);
		word1.set_MAX_LOD(iMaxLod);
		word1.set_LOD_BIAS(iLodBias);

		sampler->word1 = word1;
	}


void GX2InitSamplerClamping(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampX, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampY, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_CLAMP clampZ)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_CLAMP_X(clampX);
		word0.set_CLAMP_Y(clampY);
		word0.set_CLAMP_Z(clampZ);
		sampler->word0 = word0;
	}


void GX2InitSamplerBorderType(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_BORDER_COLOR_TYPE borderColorType)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_BORDER_COLOR_TYPE(borderColorType);
		sampler->word0 = word0;
	}


void GX2InitSamplerDepthCompare(GX2Sampler* sampler, LATTE_SQ_TEX_SAMPLER_WORD0_0::E_DEPTH_COMPARE depthCompareFunction)
	{
		LATTE_SQ_TEX_SAMPLER_WORD0_0 word0 = sampler->word0;
		word0.set_DEPTH_COMPARE_FUNCTION(depthCompareFunction);
		sampler->word0 = word0;
	}


}  // namespace GX2

// ---- adapted from Cemu src/Cafe/OS/libs/gx2/GX2_RenderTarget.cpp and GX2_Surface.cpp
namespace GX2 {

static uint32 GetSurfaceColorBufferExportFormat(Latte::E_GX2SURFFMT fmt)
{
	const uint8 table[0x40] = {
		0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
		0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };
	return table[(uint32)fmt & 0x3F];
}

void GX2InitColorBufferRegs(GX2ColorBuffer* colorBuffer)
{

	LatteAddrLib::AddrSurfaceInfo_OUT surfaceInfo;
	LatteAddrLib::GX2CalculateSurfaceInfo(colorBuffer->surface.format, colorBuffer->surface.width, colorBuffer->surface.height, colorBuffer->surface.depth, colorBuffer->surface.dim, colorBuffer->surface.tileMode, colorBuffer->surface.aa, colorBuffer->viewMip, &surfaceInfo);

	uint32 pitchHeight = (surfaceInfo.height * surfaceInfo.pitch) >> 6;

	uint32 cSize = ((surfaceInfo.pitch >> 3) - 1) & 0x3FF;
	cSize |= (((pitchHeight - 1) & 0xFFFFF) << 10);
	colorBuffer->reg_size = cSize;
	colorBuffer->reg_mask = 0;
	// reg color_info
	Latte::E_GX2SURFFMT format = colorBuffer->surface.format;
	Latte::E_HWFMT hwFormat = Latte::GetHWFormat(format);
	uint32 formatHighBits = (uint32)format & 0xF00;
	uint32 regInfo = 0;
	regInfo = (uint32)Latte::E_ENDIAN_SWAP::SWAP_NONE;
	regInfo |= ((uint32)hwFormat<<2);
	cemu_assert_debug(LatteAddrLib::IsValidHWTileMode(surfaceInfo.hwTileMode));
	regInfo |= ((uint32)surfaceInfo.hwTileMode << 8);
	bool clampBlend = false;
	if (formatHighBits == 0x000)
	{
		regInfo |= (0 << 12);
		clampBlend = true;
	}
	else if (formatHighBits == 0x100) // integer
	{
		regInfo |= (4 << 12);
	}
	else if (formatHighBits == 0x200) // signed
	{
		regInfo |= (1 << 12);
		clampBlend = true;
	}
	else if (formatHighBits == 0x300) // integer + signed
	{
		regInfo |= (5 << 12);
	}
	else if (formatHighBits == 0x400) // srgb
	{
		clampBlend = true;
		regInfo |= (6 << 12);
	}
	else if (formatHighBits == 0x800) // float
	{
		regInfo |= (7 << 12);
	}
	else
		cemu_assert_debug(false);
	if (hwFormat == Latte::E_HWFMT::HWFMT_5_5_5_1 || hwFormat == Latte::E_HWFMT::HWFMT_10_10_10_2 )
		regInfo |= (2 << 16);
	else
		regInfo &= ~(3 << 16); // COMP_SWAP_mask
	if(colorBuffer->surface.aa != 0)
		regInfo |= (2 << 18); // TILE_MODE
	bool isIntegerFormat = (uint32)(format & Latte::E_GX2SURFFMT::FMT_BIT_INT) != 0;
	if (isIntegerFormat == false)
		regInfo |= (GetSurfaceColorBufferExportFormat(colorBuffer->surface.format) << 27); // 0 -> full, 1 -> normalized
	if (isIntegerFormat
		|| format ==Latte::E_GX2SURFFMT::R24_X8_UNORM
		|| format ==Latte::E_GX2SURFFMT::R24_X8_FLOAT
		|| format ==Latte::E_GX2SURFFMT::R32_X8_FLOAT)
	{
		// set the blend bypass bit for formats which dont support blending
		regInfo |= (1<<22);
		clampBlend = false;
	}
	if (clampBlend)
		regInfo |= (1<<20); // BLEND_CLAMP_bit
	if ((uint32)(format & Latte::E_GX2SURFFMT::FMT_BIT_FLOAT) != 0)
		regInfo |= (1<<25); // ROUND_MODE_bit
	colorBuffer->reg_info = regInfo;
	// reg color_view
	uint32 regView = 0;
	if (colorBuffer->surface.tileMode != Latte::E_GX2TILEMODE::TM_LINEAR_SPECIAL)
	{
		regView |= ((uint32)colorBuffer->viewFirstSlice & 0x7FF);
		regView |= ((((uint32)colorBuffer->viewNumSlices + (uint32)colorBuffer->viewFirstSlice - 1) & 0x7FF) << 13);
	}
	colorBuffer->reg_view = regView;
	colorBuffer->reg_mask = 0;

	// todo - aa stuff

}

void GX2InitDepthBufferRegs(GX2DepthBuffer* depthBuffer)
{

	LatteAddrLib::AddrSurfaceInfo_OUT surfaceInfo;
	LatteAddrLib::GX2CalculateSurfaceInfo(depthBuffer->surface.format, depthBuffer->surface.width, depthBuffer->surface.height, depthBuffer->surface.depth, depthBuffer->surface.dim, depthBuffer->surface.tileMode, depthBuffer->surface.aa, depthBuffer->viewMip, &surfaceInfo);

	cemu_assert_debug(depthBuffer->viewNumSlices != 0);

	uint32 cSize = ((surfaceInfo.pitch >> 3) - 1) & 0x3FF;
	cSize |= ((((surfaceInfo.height * surfaceInfo.pitch >> 6) - 1) & 0xFFFFF) << 10);

	depthBuffer->reg_size = cSize;
	// todo - other regs

}



}  // namespace GX2
