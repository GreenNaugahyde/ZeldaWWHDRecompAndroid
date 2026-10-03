// Adapted from Cemu VulkanPipelineCompiler.cpp (MPL-2.0).
// Raw UINT fetch; GLSL handles endian, signedness and normalization.
#pragma once
#include <vulkan/vulkan.h>
#include "Cafe/HW/Latte/Core/LatteConst.h"
namespace gfxvk::vk {
inline VkFormat vertex_format(Latte::E_HWFMT format)
{
	switch (format)
	{
	case Latte::E_HWFMT::HWFMT_32_32_32_32_FLOAT:
		return VK_FORMAT_R32G32B32A32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_32_FLOAT:
		return VK_FORMAT_R32G32B32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_FLOAT:
		return VK_FORMAT_R32G32_UINT;
	case Latte::E_HWFMT::HWFMT_32_FLOAT:
		return VK_FORMAT_R32_UINT;
	case Latte::E_HWFMT::HWFMT_8_8_8_8:
		return VK_FORMAT_R8G8B8A8_UINT;
	case Latte::E_HWFMT::HWFMT_8_8_8:
		return VK_FORMAT_R8G8B8_UINT;
	case Latte::E_HWFMT::HWFMT_8_8:
		return VK_FORMAT_R8G8_UINT;
	case Latte::E_HWFMT::HWFMT_8:
		return VK_FORMAT_R8_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_32_32:
		return VK_FORMAT_R32G32B32A32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32_32:
		return VK_FORMAT_R32G32B32_UINT;
	case Latte::E_HWFMT::HWFMT_32_32:
		return VK_FORMAT_R32G32_UINT;
	case Latte::E_HWFMT::HWFMT_32:
		return VK_FORMAT_R32_UINT;
	case Latte::E_HWFMT::HWFMT_16_16_16_16:
		return VK_FORMAT_R16G16B16A16_UINT; // verified to match OpenGL
	case Latte::E_HWFMT::HWFMT_16_16_16:
		return VK_FORMAT_R16G16B16_UINT;
	case Latte::E_HWFMT::HWFMT_16_16:
		return VK_FORMAT_R16G16_UINT;
	case Latte::E_HWFMT::HWFMT_16:
		return VK_FORMAT_R16_UINT;
	case Latte::E_HWFMT::HWFMT_16_16_16_16_FLOAT:
		return VK_FORMAT_R16G16B16A16_UINT; // verified to match OpenGL
	case Latte::E_HWFMT::HWFMT_16_16_16_FLOAT:
		return VK_FORMAT_R16G16B16_UINT;
	case Latte::E_HWFMT::HWFMT_16_16_FLOAT:
		return VK_FORMAT_R16G16_UINT;
	case Latte::E_HWFMT::HWFMT_16_FLOAT:
		return VK_FORMAT_R16_UINT;
	case Latte::E_HWFMT::HWFMT_2_10_10_10:
		return VK_FORMAT_R32_UINT; // verified to match OpenGL
	default:
		cemuLog_log(LogType::Force, "Unsupported vertex format: {:02x}", (uint32)format);
		assert_dbg();
		return VK_FORMAT_UNDEFINED;
	}
}
}
