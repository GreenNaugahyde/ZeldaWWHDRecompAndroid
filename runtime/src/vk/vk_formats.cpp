// GX2 surface format mapping. Texel layouts follow Cemu's LatteTextureLoader decoders; the BC
// decoders follow the published BC1-BC5 (S3TC/RGTC) block layouts.
#include "vk_formats.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "runtime.h"

namespace gfx {

static VkPhysicalDevice g_pd = VK_NULL_HANDLE;

void formats_init(VkPhysicalDevice pd) { g_pd = pd; }

static VkFormatFeatureFlags features(VkFormat f) {
    static std::unordered_map<int, VkFormatFeatureFlags> cache;
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    auto it = cache.find((int)f);
    if (it != cache.end()) return it->second;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(g_pd, f, &p);
    cache[(int)f] = p.optimalTilingFeatures;
    return p.optimalTilingFeatures;
}
static bool sampleable(VkFormat f) { return (features(f) & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0; }
static bool color_renderable(VkFormat f) { return (features(f) & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) != 0; }
static bool depth_renderable(VkFormat f) { return (features(f) & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0; }

static FormatInfo make(VkFormat p, uint32_t bpb, FormatInfo::Kind kind = FormatInfo::FLOAT, Convert cv = Convert::NONE,
                       uint32_t hostBpb = 0) {
    FormatInfo f;
    f.format = p;
    f.bytesPerBlock = bpb;
    f.hostBytesPerBlock = hostBpb ? hostBpb : bpb;
    f.convert = cv;
    f.kind = kind;
    return f;
}

static FormatInfo bc(VkFormat native, uint32_t bpb, Convert decode, VkFormat decoded, uint32_t decodedBpp) {
    FormatInfo f = make(native, bpb);
    f.compressed = true;
    // debug: WWHD_NO_BC=1 decodes BC textures on the CPU even where the GPU supports them
    static const bool forceDecode = getenv("WWHD_NO_BC") != nullptr;
    if (sampleable(native) && !forceDecode) {
        f.hostCompressed = true;
        return f;
    }
    f.format = decoded;
    f.convert = decode;
    f.hostBytesPerBlock = decodedBpp;  // per texel
    return f;
}

// 16-bit normalized formats are optional on mobile GPUs: fall back to half floats
static FormatInfo norm16(VkFormat unorm, VkFormat snorm, VkFormat uint, VkFormat sint, VkFormat half, uint32_t bpb, bool isInt,
                         bool isSigned) {
    FormatInfo::Kind kind = isInt ? (isSigned ? FormatInfo::SINT : FormatInfo::UINT) : FormatInfo::FLOAT;
    if (isInt) return make(isSigned ? sint : uint, bpb, kind);
    VkFormat want = isSigned ? snorm : unorm;
    if (sampleable(want)) return make(want, bpb);
    return make(half, bpb, FormatInfo::FLOAT, isSigned ? Convert::SNORM16_F16 : Convert::UNORM16_F16);
}

static FormatInfo compute(uint32_t fmt, bool isDepth) {
    const uint32_t hw = fmt & 0x3F;
    const bool isInt = fmt & 0x100, isSigned = fmt & 0x200, isSrgb = fmt & 0x400;
    const auto kind = isInt ? (isSigned ? FormatInfo::SINT : FormatInfo::UINT) : FormatInfo::FLOAT;
    auto pick = [&](VkFormat unorm, VkFormat snorm, VkFormat uint, VkFormat sint) {
        if (isInt) return isSigned ? sint : uint;
        return isSigned ? snorm : unorm;
    };
    if (isDepth) {
        FormatInfo f;
        f.depth = true;
        switch (hw) {
        case 0x05: f.format = VK_FORMAT_D16_UNORM; f.bytesPerBlock = f.hostBytesPerBlock = 2; return f;
        case 0x0E:
            f.format = depth_renderable(VK_FORMAT_D32_SFLOAT) ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_X8_D24_UNORM_PACK32;
            f.bytesPerBlock = f.hostBytesPerBlock = 4;
            return f;
        case 0x11: case 0x12: case 0x13: case 0x14: case 0x1C:
            // 32-bit float depth keeps the precision the game expects; D24S8 where D32S8 is missing
            f.format = depth_renderable(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_D24_UNORM_S8_UINT;
            f.stencil = true;
            f.bytesPerBlock = hw == 0x1C ? 8 : 4;
            f.hostBytesPerBlock = 8;
            return f;
        default: break;
        }
    }
    FormatInfo f;
    switch (hw) {
    case 0x01: f = make(pick(VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT), 1, kind); break;
    case 0x02: f = make(VK_FORMAT_R8G8_UNORM, 1, kind, Convert::RG4, 2); break;
    case 0x05:
        f = norm16(VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16_SFLOAT, 2, isInt,
                   isSigned);
        break;
    case 0x06: f = make(VK_FORMAT_R16_SFLOAT, 2); break;
    case 0x07: f = make(pick(VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT), 2, kind); break;
    case 0x08: f = make(VK_FORMAT_R8G8B8A8_UNORM, 2, kind, Convert::RGB565, 4); break;
    case 0x0A: f = make(VK_FORMAT_R8G8B8A8_UNORM, 2, kind, Convert::RGBA5551, 4); break;
    case 0x0B: f = make(VK_FORMAT_R8G8B8A8_UNORM, 2, kind, Convert::RGBA4, 4); break;
    case 0x0C: f = make(VK_FORMAT_R8G8B8A8_UNORM, 2, kind, Convert::ABGR1555, 4); break;
    case 0x0D: f = make(isSigned ? VK_FORMAT_R32_SINT : VK_FORMAT_R32_UINT, 4, isSigned ? FormatInfo::SINT : FormatInfo::UINT); break;
    case 0x0E: f = make(VK_FORMAT_R32_SFLOAT, 4); break;
    case 0x0F:
        f = norm16(VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
                   VK_FORMAT_R16G16_SFLOAT, 4, isInt, isSigned);
        break;
    case 0x10: f = make(VK_FORMAT_R16G16_SFLOAT, 4); break;
    case 0x16: case 0x15: f = make(VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4); break;
    case 0x19: f = make(isInt ? VK_FORMAT_A2B10G10R10_UINT_PACK32 : VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, kind); break;
    case 0x1A:
        f = make(isSrgb ? VK_FORMAT_R8G8B8A8_SRGB
                        : pick(VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SNORM, VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT),
                 4, kind);
        break;
    case 0x1B: f = make(VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, kind); break;
    case 0x1D: f = make(isSigned ? VK_FORMAT_R32G32_SINT : VK_FORMAT_R32G32_UINT, 8, isSigned ? FormatInfo::SINT : FormatInfo::UINT); break;
    case 0x1E: f = make(VK_FORMAT_R32G32_SFLOAT, 8); break;
    case 0x1F:
        f = norm16(VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
                   VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SFLOAT, 8, isInt, isSigned);
        break;
    case 0x20: f = make(VK_FORMAT_R16G16B16A16_SFLOAT, 8); break;
    case 0x22:
        f = make(isSigned ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT, 16, isSigned ? FormatInfo::SINT : FormatInfo::UINT);
        break;
    case 0x23: f = make(VK_FORMAT_R32G32B32A32_SFLOAT, 16); break;
    case 0x31:
        f = bc(isSrgb ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, Convert::BC1,
               isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4);
        break;
    case 0x32:
        f = bc(isSrgb ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK, 16, Convert::BC2,
               isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4);
        break;
    case 0x33:
        f = bc(isSrgb ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK, 16, Convert::BC3,
               isSrgb ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM, 4);
        break;
    case 0x34:
        f = isSigned ? bc(VK_FORMAT_BC4_SNORM_BLOCK, 8, Convert::BC4S, VK_FORMAT_R8_SNORM, 1)
                     : bc(VK_FORMAT_BC4_UNORM_BLOCK, 8, Convert::BC4U, VK_FORMAT_R8_UNORM, 1);
        break;
    case 0x35:
        f = isSigned ? bc(VK_FORMAT_BC5_SNORM_BLOCK, 16, Convert::BC5S, VK_FORMAT_R8G8_SNORM, 2)
                     : bc(VK_FORMAT_BC5_UNORM_BLOCK, 16, Convert::BC5U, VK_FORMAT_R8G8_UNORM, 2);
        break;
    // depth formats sampled as color textures
    case 0x11: case 0x12: case 0x13: case 0x14: f = make(VK_FORMAT_R32_SFLOAT, 4, FormatInfo::FLOAT, Convert::D24_R32F, 4); break;
    default: f = make(VK_FORMAT_R8G8B8A8_UNORM, 4); break;
    }
    return f;
}

FormatInfo format_info(uint32_t fmt, bool isDepth) {
    static std::unordered_map<uint32_t, FormatInfo> cache;
    static std::mutex m;
    uint32_t key = (fmt & 0xFFFF) | (isDepth ? 0x80000000u : 0);
    {
        std::lock_guard<std::mutex> lk(m);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    FormatInfo f = compute(fmt, isDepth);
    f.renderable = f.depth ? depth_renderable(f.format) : (!f.hostCompressed && color_renderable(f.format));
    if (!sampleable(f.format) && !f.depth)
        LOG("[vk] format %X maps to VkFormat %d, which this device cannot sample", fmt, (int)f.format);
    std::lock_guard<std::mutex> lk(m);
    cache[key] = f;
    return f;
}

static VkFormat vertex_format_raw(uint32_t hw) {
    // Latte E_HWFMT values (see Cafe/HW/Latte/ISA/LatteReg.h); data stays big-endian, the shader swaps
    switch (hw) {
    case 0x01: return VK_FORMAT_R8_UINT;                   // 8
    case 0x02: return VK_FORMAT_R8G8_UINT;                 // 4_4 (unused)
    case 0x05: case 0x06: return VK_FORMAT_R16_UINT;       // 16, 16_FLOAT
    case 0x07: return VK_FORMAT_R8G8_UINT;                 // 8_8
    case 0x0D: case 0x0E: return VK_FORMAT_R32_UINT;       // 32, 32_FLOAT
    case 0x0F: case 0x10: return VK_FORMAT_R16G16_UINT;    // 16_16, 16_16_FLOAT
    case 0x19: case 0x1B: return VK_FORMAT_R32_UINT;       // 10_10_10_2, 2_10_10_10
    case 0x1A: return VK_FORMAT_R8G8B8A8_UINT;             // 8_8_8_8
    case 0x1D: case 0x1E: return VK_FORMAT_R32G32_UINT;    // 32_32, 32_32_FLOAT
    case 0x1F: case 0x20: return VK_FORMAT_R16G16B16A16_UINT;  // 16_16_16_16(_FLOAT)
    case 0x22: case 0x23: return VK_FORMAT_R32G32B32A32_UINT;  // 32_32_32_32(_FLOAT)
    case 0x2C: return VK_FORMAT_R8G8B8_UINT;               // 8_8_8 (rare)
    case 0x2D: case 0x2E: return VK_FORMAT_R16G16B16_UINT; // 16_16_16(_FLOAT)
    case 0x2F: case 0x30: return VK_FORMAT_R32G32B32_UINT; // 32_32_32(_FLOAT)
    default: return VK_FORMAT_UNDEFINED;
    }
}

VkFormat vertex_format(uint32_t hw) {
    VkFormat f = vertex_format_raw(hw);
    if (f == VK_FORMAT_UNDEFINED) return f;
    VkFormatProperties p{};
    vkGetPhysicalDeviceFormatProperties(g_pd, f, &p);
    if (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) return f;
    // 3-component 8/16-bit vertex formats are optional: fetch a 4th component the shader ignores
    if (f == VK_FORMAT_R8G8B8_UINT) return VK_FORMAT_R8G8B8A8_UINT;
    if (f == VK_FORMAT_R16G16B16_UINT) return VK_FORMAT_R16G16B16A16_UINT;
    return f;
}

// ---------------------------------------------------------------- texel conversion
static inline uint8_t ex5(uint32_t v) { return (uint8_t)((v << 3) | (v >> 2)); }
static inline uint8_t ex6(uint32_t v) { return (uint8_t)((v << 2) | (v >> 4)); }
static inline uint8_t ex4(uint32_t v) { return (uint8_t)((v << 4) | v); }

static uint16_t f32_to_f16(float f) {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int32_t e = (int32_t)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFF;
    if (e <= 0) {  // subnormal or zero
        if (e < -10) return (uint16_t)sign;
        m = (m | 0x800000) >> (1 - e);
        return (uint16_t)(sign | ((m + 0x1000) >> 13));
    }
    if (e >= 31) return (uint16_t)(sign | 0x7C00);
    uint32_t h = sign | ((uint32_t)e << 10) | (m >> 13);
    if (m & 0x1000) h++;  // round to nearest
    return (uint16_t)h;
}

// BC1 color block -> 16 RGBA texels (row-major 4x4). `alpha` = BC1 punch-through alpha allowed.
static void decode_bc1_color(const uint8_t* b, uint8_t out[16][4], bool bc1Alpha) {
    uint16_t c0 = (uint16_t)(b[0] | b[1] << 8), c1 = (uint16_t)(b[2] | b[3] << 8);
    uint8_t pal[4][4];
    auto rgb = [](uint16_t c, uint8_t* o) {
        o[0] = ex5((c >> 11) & 0x1F);
        o[1] = ex6((c >> 5) & 0x3F);
        o[2] = ex5(c & 0x1F);
        o[3] = 255;
    };
    rgb(c0, pal[0]);
    rgb(c1, pal[1]);
    if (c0 > c1 || !bc1Alpha) {
        for (int i = 0; i < 3; i++) {
            pal[2][i] = (uint8_t)((2 * pal[0][i] + pal[1][i] + 1) / 3);
            pal[3][i] = (uint8_t)((pal[0][i] + 2 * pal[1][i] + 1) / 3);
        }
        pal[2][3] = pal[3][3] = 255;
    } else {
        for (int i = 0; i < 3; i++) pal[2][i] = (uint8_t)((pal[0][i] + pal[1][i]) / 2);
        pal[2][3] = 255;
        pal[3][0] = pal[3][1] = pal[3][2] = pal[3][3] = 0;
    }
    uint32_t idx = (uint32_t)b[4] | (uint32_t)b[5] << 8 | (uint32_t)b[6] << 16 | (uint32_t)b[7] << 24;
    for (int i = 0; i < 16; i++) memcpy(out[i], pal[(idx >> (2 * i)) & 3], 4);
}

// BC4 channel block -> 16 values (signed: values are int8 stored as uint8)
static void decode_bc4(const uint8_t* b, uint8_t out[16], bool isSigned) {
    int v[8];
    int a0 = isSigned ? (int8_t)b[0] : b[0], a1 = isSigned ? (int8_t)b[1] : b[1];
    if (isSigned) { if (a0 == -128) a0 = -127; if (a1 == -128) a1 = -127; }
    v[0] = a0;
    v[1] = a1;
    if (a0 > a1) {
        for (int i = 1; i < 7; i++) v[i + 1] = ((7 - i) * a0 + i * a1 + 3) / 7;
    } else {
        for (int i = 1; i < 5; i++) v[i + 1] = ((5 - i) * a0 + i * a1 + 2) / 5;
        v[6] = isSigned ? -127 : 0;
        v[7] = isSigned ? 127 : 255;
    }
    uint64_t idx = 0;
    for (int i = 0; i < 6; i++) idx |= (uint64_t)b[2 + i] << (8 * i);
    for (int i = 0; i < 16; i++) out[i] = (uint8_t)v[(idx >> (3 * i)) & 7];
}

void convert_row(Convert c, const uint8_t* src, uint8_t* dst, uint32_t n, uint32_t dstPitch) {
    switch (c) {
    case Convert::NONE: break;
    case Convert::RGB565:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ((const uint16_t*)src)[i];
            dst[4 * i + 0] = ex5(v & 0x1F);
            dst[4 * i + 1] = ex6((v >> 5) & 0x3F);
            dst[4 * i + 2] = ex5((v >> 11) & 0x1F);
            dst[4 * i + 3] = 255;
        }
        break;
    case Convert::RGBA5551:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ((const uint16_t*)src)[i];
            dst[4 * i + 0] = ex5(v & 0x1F);
            dst[4 * i + 1] = ex5((v >> 5) & 0x1F);
            dst[4 * i + 2] = ex5((v >> 10) & 0x1F);
            dst[4 * i + 3] = (v >> 15) ? 255 : 0;
        }
        break;
    case Convert::ABGR1555:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ((const uint16_t*)src)[i];
            dst[4 * i + 0] = ex5((v >> 11) & 0x1F);
            dst[4 * i + 1] = ex5((v >> 6) & 0x1F);
            dst[4 * i + 2] = ex5((v >> 1) & 0x1F);
            dst[4 * i + 3] = (v & 1) ? 255 : 0;
        }
        break;
    case Convert::RGBA4:
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ((const uint16_t*)src)[i];
            dst[4 * i + 0] = ex4(v & 0xF);
            dst[4 * i + 1] = ex4((v >> 4) & 0xF);
            dst[4 * i + 2] = ex4((v >> 8) & 0xF);
            dst[4 * i + 3] = ex4((v >> 12) & 0xF);
        }
        break;
    case Convert::RG4:
        for (uint32_t i = 0; i < n; i++) {
            uint8_t v = src[i];
            dst[2 * i + 0] = ex4(v >> 4);
            dst[2 * i + 1] = ex4(v & 0xF);
        }
        break;
    case Convert::D24_R32F:
        for (uint32_t i = 0; i < n; i++) {
            uint32_t v = ((const uint32_t*)src)[i];
            float d = (float)(v & 0xFFFFFF) / 16777215.0f;
            memcpy(dst + 4 * i, &d, 4);
        }
        break;
    case Convert::UNORM16_F16:
        // n counts 16-bit components here (the caller passes texels * components)
        for (uint32_t i = 0; i < n; i++) {
            uint16_t v = ((const uint16_t*)src)[i];
            ((uint16_t*)dst)[i] = f32_to_f16(v / 65535.0f);
        }
        break;
    case Convert::SNORM16_F16:
        for (uint32_t i = 0; i < n; i++) {
            int16_t v = ((const int16_t*)src)[i];
            ((uint16_t*)dst)[i] = f32_to_f16(std::max(v / 32767.0f, -1.0f));
        }
        break;
    case Convert::BC1: case Convert::BC2: case Convert::BC3: {
        const uint32_t bs = c == Convert::BC1 ? 8 : 16;
        for (uint32_t bi = 0; bi < n; bi++) {
            const uint8_t* b = src + bi * bs;
            uint8_t px[16][4];
            decode_bc1_color(c == Convert::BC1 ? b : b + 8, px, c == Convert::BC1);
            if (c == Convert::BC2) {
                for (int i = 0; i < 16; i++) px[i][3] = ex4((b[i / 2] >> ((i & 1) * 4)) & 0xF);
            } else if (c == Convert::BC3) {
                uint8_t a[16];
                decode_bc4(b, a, false);
                for (int i = 0; i < 16; i++) px[i][3] = a[i];
            }
            for (int y = 0; y < 4; y++) memcpy(dst + y * dstPitch + bi * 16, px[y * 4], 16);
        }
        break;
    }
    case Convert::BC4U: case Convert::BC4S:
        for (uint32_t bi = 0; bi < n; bi++) {
            uint8_t v[16];
            decode_bc4(src + bi * 8, v, c == Convert::BC4S);
            for (int y = 0; y < 4; y++) memcpy(dst + y * dstPitch + bi * 4, v + y * 4, 4);
        }
        break;
    case Convert::BC5U: case Convert::BC5S:
        for (uint32_t bi = 0; bi < n; bi++) {
            uint8_t r[16], g[16];
            decode_bc4(src + bi * 16, r, c == Convert::BC5S);
            decode_bc4(src + bi * 16 + 8, g, c == Convert::BC5S);
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++) {
                    dst[y * dstPitch + (bi * 4 + x) * 2] = r[y * 4 + x];
                    dst[y * dstPitch + (bi * 4 + x) * 2 + 1] = g[y * 4 + x];
                }
        }
        break;
    }
}

}  // namespace gfx
