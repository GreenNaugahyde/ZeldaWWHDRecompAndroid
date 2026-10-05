// GX2 surface formats -> Vulkan formats, and texel conversion for formats the device can't use
// directly (mobile GPUs typically lack BC compression and some 16-bit normalized formats).
#pragma once
#include <volk.h>  // Vulkan through function pointers (no prototypes; see create_device)

#include <cstdint>

namespace gfx {

enum class Convert : uint8_t {
    NONE,        // copy texels as stored
    RGB565,      // Latte 5_6_5 -> RGBA8
    RGBA5551,    // Latte 1_5_5_5 (R in low bits) -> RGBA8
    ABGR1555,    // Latte 5_5_5_1 -> RGBA8
    RGBA4,       // Latte 4_4_4_4 -> RGBA8
    RG4,         // Latte 4_4 -> RG8
    D24_R32F,    // 24-bit depth sampled as a color texture -> R32 float
    UNORM16_F16, // 16-bit normalized -> half float (device lacks the 16-bit normalized format)
    SNORM16_F16,
    BC1, BC2, BC3, BC4U, BC4S, BC5U, BC5S,  // block compressed -> decoded texels (device lacks BC)
};

struct FormatInfo {
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t bytesPerBlock = 0;      // guest (source) bytes per texel or per 4x4 block
    uint32_t hostBytesPerBlock = 0;  // host bytes per texel (or per 4x4 block if hostCompressed)
    bool compressed = false;         // guest data is in 4x4 blocks
    bool hostCompressed = false;     // host image is block compressed too
    bool depth = false;
    bool stencil = false;
    bool renderable = false;         // usable as a color/depth attachment on this device
    Convert convert = Convert::NONE;
    enum Kind : uint8_t { FLOAT, UINT, SINT } kind = FLOAT;  // shader-visible data type
};

// probe the device's format support once, before the first format_info call
void formats_init(VkPhysicalDevice pd);
// isDepth: the surface is used as a depth buffer (selects depth formats)
FormatInfo format_info(uint32_t gx2Format, bool isDepth);
// uint vertex format for a Latte vertex fetch format (the shader byte-swaps and decodes)
VkFormat vertex_format(uint32_t hwFormat);

// convert one row of `count` texels (or blocks, for compressed guest data) from guest layout to
// host layout. For decoded BC formats `dst` receives 4 texel rows: `dstPitch` bytes apart.
void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t count, uint32_t dstPitch = 0);
inline bool is_bc_decode(Convert c) { return c >= Convert::BC1; }

}  // namespace gfx
