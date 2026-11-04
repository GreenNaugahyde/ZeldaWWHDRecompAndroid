// Area-sampled taps for the game's separable Gaussian blur on upscaled render targets (original
// project ea45a3d, issue #66). The game blurs its light buffer (ambient occlusion in R, the shadow
// mask in G) and its bloom chain with PS 44B3AB00 / 44B39A00: three bilinear taps 1.36 guest texels
// apart. At the console's size they cover five texels and the occlusion pass's 4x4 noise averages
// out; on a target scaled by the internal resolution the pixels between the taps get no weight and
// a fine grid shows around everything standing on the ground. Each tap of these two shaders averages
// bilinear samples spread over one guest texel instead, each sample covering about 2x2 image texels:
// n = max(2, ceil(scale / 2)) per axis above 1x (4 reads at 2x-4x; the original project's ceil(scale)^2
// was 9 at 3x and cost a lot of GPU time). Up to a scale of 1.47 it is the game's one sample: taps
// 1.36 guest texels apart are then at most 2 image texels apart, which bilinear taps cover without a
// gap (at 1x on a 20:9 screen the widened buffers have a scale of 1.249, and the 2x2 reads per tap
// cost ~1 ms of an Adreno 650's frame for nothing).
//
// Grid of the half-size light buffer (the original project's 15035dcc). Scenes with point lights
// (torches) are lit into a 640x360 buffer next to a 640x360 copy of the depth, and every lit material
// reads the light back with a depth-aware upsample: from a uniform (2w, 2h, 1/2w, 1/2h), the screen's
// size for a w x h buffer, it puts the pixel on the buffer's grid, gathers the four depths around it
// and nudges one bilinear read of the light towards the texels whose depth matches its own. On upscaled
// buffers the gather returns texels a fraction of a grid cell apart while the grid stays the console's,
// so the light along silhouettes (a torch against the wall it lights) came out in console-sized blocks.
// For the units a shader gathers from, the renderer scales that uniform to the buffer's real size, so
// the grid is the upscaled buffer's texels (scale_gather_grid).
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace vk_area_sample {

// Cemu's program hash: the program's words, wherever it was loaded
inline uint64_t program_hash(const void* bytes, uint32_t size) {
    uint64_t a = 0, b = 0;
    for (uint32_t i = 0; i < size / 4; i++) {
        uint32_t word;
        std::memcpy(&word, static_cast<const uint8_t*>(bytes) + i * 4, 4);
        a = std::rotl(a + word, 3);
        b = std::rotr(b ^ word, 7);
    }
    return a + b;
}

// The bloom extract (PS 44F91200, the original project's 2e38bea9 / d3fade20, issue #307) reads the
// scene with four bilinear taps on guest texel corners, each the average of a 2x2 block, so together
// they cover the 4x4 block under an output texel. On the upscaled scene those taps read single pixels:
// small bright things (rupees, eyes) glowed only where a tap landed. Its taps go through the
// console-grid helper (wwhdGuestSample) instead, which reads exactly what the console's bilinear tap
// reads, from the pixels under those guest texels (at a whole scale; else the area helper). The rest of
// the chain keeps the console's size (size_request, vk_surfaces.cpp color_target): its blur's taps are
// sparse lumps that the final bilinear upscale blends into a soft glow, while on an upscaled chain every
// lump got its own pixels and lights glowed as blocks.
inline bool bloom_extract(const void* bytes, uint32_t size) {
    return bytes && size == 656 && program_hash(bytes, size) == 0x3a8d0f3808f1501bull;
}

// texture units of this pixel shader that are read area-sampled (bit n = unit n); 0 for all others
inline uint32_t units_for_pixel_shader(const void* bytes, uint32_t size) {
    if (bloom_extract(bytes, size)) return 3u;
    if (!bytes || size != 448) return 0;
    switch (program_hash(bytes, size)) {
    case 0x51a7ccdf69184627ull:  // PS 44B3AB00, horizontal Gaussian
    case 0x16285301a96cbf8dull:  // PS 44B39A00, vertical Gaussian
        return 1u;
    default:
        return 0;
    }
}

// What a pixel shader's draws ask of their render targets' size (vk_surfaces.cpp color_target): the
// bloom chain's extract and blur (PS 44F91200, 44FD6E00, 44FD7600) claim the console's size; the copy
// passes that also draw the chain's levels (PS 44FD7E00, and 44FD8500, which draws the screen buffers
// too) keep the claim but never make one.
enum SizeRequest : uint8_t { kSizeInternal = 0, kSizeClaimConsole = 1, kSizeRenewConsole = 2 };
inline uint8_t size_request(const void* bytes, uint32_t size) {
    if (!bytes) return kSizeInternal;
    if (bloom_extract(bytes, size)) return kSizeClaimConsole;
    if (size == 768) {
        const uint64_t hash = program_hash(bytes, size);
        if (hash == 0x4657565560d951b7ull || hash == 0x4102408f288aaa94ull) return kSizeClaimConsole;
    }
    if (size == 416) {
        const uint64_t hash = program_hash(bytes, size);
        if (hash == 0x686be3682830bd88ull || hash == 0x960d3ef6661fc3c7ull) return kSizeRenewConsole;
    }
    return kSizeInternal;
}

// Rewrites the sample calls of `units` in a decompiled Vulkan GLSL pixel shader to the helper and
// inserts the helper. Returns the number of calls rewritten (0: source left unchanged).
// guest: the taps go through the console-grid helper (the bloom extract) instead of the area helper.
inline int rewrite(std::string& src, uint32_t units, bool guest = false) {
    const size_t entry = src.find("void main(");
    if (!units || entry == std::string::npos) return 0;
    int count = 0;
    for (uint32_t unit = 0; unit < 32; unit++) {
        if (!(units >> unit & 1)) continue;
        const std::string n = std::to_string(unit);
        const std::string from = "texture(textureUnitPS" + n + ", ";
        const std::string to = (guest ? "wwhdGuestSample(textureUnitPS" : "wwhdAreaSample(textureUnitPS") + n + ", uf_tex" + n + "Scale, ";
        for (size_t at = src.find(from); at != std::string::npos; at = src.find(from, at + to.size())) {
            src.replace(at, from.size(), to);
            count++;
        }
    }
    if (!count) return 0;
    static const char* const kGlsl =
        "// area-sampled tap for upscaled render targets (runtime/src/vk/area_sample.h)\n"
        "vec4 wwhdAreaSample(sampler2D t, vec2 scale, vec2 uv) {\n"
        "    if (scale.x <= 1.47 && scale.y <= 1.47) return texture(t, uv);\n"
        "    vec2 k = max(ceil(scale * 0.5 - 0.001), vec2(2.0));\n"
        "    vec2 step = scale / (vec2(textureSize(t, 0)) * k);\n"
        "    vec4 sum = vec4(0.0);\n"
        "    for (float j = 0.5; j < k.y; j += 1.0)\n"
        "        for (float i = 0.5; i < k.x; i += 1.0) sum += texture(t, uv + (vec2(i, j) - 0.5 * k) * step);\n"
        "    return sum / (k.x * k.y);\n"
        "}\n";
    // a bilinear tap on the console's texel grid, read from a texture k (whole) times larger per axis:
    // per axis the two console texels around uv cover 2k host texels weighted (1-f)/k and f/k, fetched in
    // pairs with one bilinear sample each (the original project's gfx/area_sample.h)
    static const char* const kGuestGlsl =
        "vec4 wwhdGuestSample(sampler2D t, vec2 scale, vec2 uv) {\n"
        "    vec2 k = floor(scale + 0.5);\n"
        "    if (k.x <= 1.0 && k.y <= 1.0) return texture(t, uv);\n"
        "    if (any(greaterThan(abs(scale - k), vec2(0.01)))) return wwhdAreaSample(t, scale, uv);\n"
        "    vec2 dx = dFdx(uv), dy = dFdy(uv);  // the tap's own footprint: the fetch positions jump between console texels\n"
        "    vec2 host = vec2(textureSize(t, 0));\n"
        "    vec2 g = uv * host / k - 0.5;\n"
        "    vec2 i0 = floor(g);\n"
        "    vec2 f = g - i0;\n"
        "    vec4 sum = vec4(0.0);\n"
        "    for (float m = 0.0; m < k.y; m += 1.0) {\n"
        "        vec2 wy = vec2(2.0 * m < k.y ? 1.0 - f.y : f.y, 2.0 * m + 1.0 < k.y ? 1.0 - f.y : f.y);\n"
        "        float y = i0.y * k.y + 2.0 * m + 0.5 + wy.y / max(wy.x + wy.y, 1e-6);\n"
        "        for (float n = 0.0; n < k.x; n += 1.0) {\n"
        "            vec2 wx = vec2(2.0 * n < k.x ? 1.0 - f.x : f.x, 2.0 * n + 1.0 < k.x ? 1.0 - f.x : f.x);\n"
        "            float x = i0.x * k.x + 2.0 * n + 0.5 + wx.y / max(wx.x + wx.y, 1e-6);\n"
        "            sum += (wx.x + wx.y) * (wy.x + wy.y) * textureGrad(t, vec2(x, y) / host, dx, dy);\n"
        "        }\n"
        "    }\n"
        "    return sum / (k.x * k.y);\n"
        "}\n";
    src.insert(src.find("void main("), guest ? std::string(kGlsl) + kGuestGlsl : std::string(kGlsl));
    return count;
}

// The texture units a decompiled pixel shader (Vulkan GLSL) gathers from (bit n = unit n).
inline uint32_t gathered_units(const char* src) {
    if (!src || !strstr(src, "textureGather(")) return 0;
    uint32_t units = 0;
    for (uint32_t unit = 0; unit < 32; unit++) {
        const std::string n = "textureGather(textureUnitPS" + std::to_string(unit) + ",";
        if (strstr(src, n.c_str())) units |= 1u << unit;
    }
    return units;
}

// The half-size light buffer's grid (see above): scales each vec4 of `uniforms` (count vec4s) that is
// (2w, 2h, 1/2w, 1/2h) for a gathered texture of guest size w x h to the texture's sx/sy texels per
// guest texel. Returns the number of vec4s scaled.
inline int scale_gather_grid(float* uniforms, size_t count, uint32_t w, uint32_t h, float sx, float sy) {
    if (!uniforms || !w || !h || (sx == 1.0f && sy == 1.0f)) return 0;
    const float gw = 2.0f * float(w), gh = 2.0f * float(h);
    int scaled = 0;
    for (size_t i = 0; i < count; i++) {
        float* v = uniforms + i * 4;
        if (v[0] != gw || v[1] != gh || v[2] * gw < 0.9999f || v[2] * gw > 1.0001f || v[3] * gh < 0.9999f ||
            v[3] * gh > 1.0001f)
            continue;
        v[0] *= sx; v[1] *= sy; v[2] /= sx; v[3] /= sy;
        scaled++;
    }
    return scaled;
}

}  // namespace vk_area_sample
