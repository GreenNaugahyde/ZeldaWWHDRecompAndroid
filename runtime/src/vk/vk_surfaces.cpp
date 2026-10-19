#include <atomic>
#include <cmath>
#include <tuple>
// Guest surfaces <-> Vulkan images: render targets, depth buffers, sampled textures. Same rules as
// the Metal renderer (gfx/metal_surfaces.mm); tiled layouts are decoded with the vendored LatteAddrLib.
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "mem_writes.h"
#include "runtime.h"
#include "vk.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);

namespace gfx {

// A surface's content, to notice CPU updates: every 8-byte word of small surfaces, one per cache line of
// large ones (the lines a byte every 61 read before). Four independent lanes: one serial multiply per
// byte made this ~3% of the render thread at Windfall on a Helio G85. Only compared with its own earlier
// values (Surface::contentHash).
static uint64_t content_hash(const uint8_t* p, size_t n) {
    const size_t step = n > (1 << 16) ? 64 : 8;
    uint64_t a = 0x243F6A8885A308D3ull, b = 0x13198A2E03707344ull, c = 0xA4093822299F31D0ull, d = 0x082EFA98EC4E6C89ull;
    auto lane = [](uint64_t& h, const uint8_t* q) {
        uint64_t w;
        memcpy(&w, q, 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    };
    size_t i = 0;
    for (; i + 3 * step + 8 <= n; i += 4 * step) {
        lane(a, p + i);
        lane(b, p + i + step);
        lane(c, p + i + 2 * step);
        lane(d, p + i + 3 * step);
    }
    for (; i + 8 <= n; i += step) lane(a, p + i);
    if (step == 8)
        for (; i < n; i++) a = (a ^ p[i]) * 0x100000001B3ull;
    return (a ^ (b << 17 | b >> 47)) * 0xFF51AFD7ED558CCDull ^ (c ^ (d << 41 | d >> 23)) * 0xC4CEB9FE1A85EC53ull ^ n;
}

static uint64_t g_write_seq = 0;
static std::vector<Surface*> g_linear_targets;  // linear-aligned colour render targets (surfaces are never freed)
static Surface* adopt_newer_alias(Surface* s);
uint64_t next_write_seq() { return ++g_write_seq; }
uint64_t write_seq() { return g_write_seq; }

// image shape for a surface: Vulkan image type, natural view type, layers, depth
static void image_shape(uint32_t dim, uint32_t slices, bool forRendering, VkImageType& type, VkImageViewType& view, uint32_t& layers,
                        uint32_t& depth, bool& cube) {
    type = VK_IMAGE_TYPE_2D;
    view = VK_IMAGE_VIEW_TYPE_2D;
    layers = 1;
    depth = 1;
    cube = false;
    switch ((Latte::E_DIM)dim) {
    case Latte::E_DIM::DIM_1D: type = VK_IMAGE_TYPE_1D; view = VK_IMAGE_VIEW_TYPE_1D; break;
    case Latte::E_DIM::DIM_1D_ARRAY: type = VK_IMAGE_TYPE_1D; view = VK_IMAGE_VIEW_TYPE_1D_ARRAY; layers = slices; break;
    case Latte::E_DIM::DIM_3D: type = VK_IMAGE_TYPE_3D; view = VK_IMAGE_VIEW_TYPE_3D; depth = slices; break;
    case Latte::E_DIM::DIM_CUBEMAP:
        layers = std::max<uint32_t>(slices / 6, 1) * 6;
        cube = true;
        view = layers > 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        break;
    case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA: view = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = slices; break;
    default:
        if (slices > 1) { view = VK_IMAGE_VIEW_TYPE_2D_ARRAY; layers = slices; }
        break;
    }
    if (forRendering && type != VK_IMAGE_TYPE_2D) {  // render targets are 2D (arrays)
        type = VK_IMAGE_TYPE_2D;
        view = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        layers = std::max(layers, depth);
        depth = 1;
        cube = false;
    }
}

// Rendering at a higher (or lower) resolution than the console: render targets with the screen's
// 16:9 shape (the TV image and its post-processing chain, the GamePad image) are created this much
// larger. Shadow maps and other square or array targets keep their size. Shaders still see guest
// units: viewports and scissors are scaled, and uf_fragCoordScale / uf_texNScale undo the scale.
// The factor can change while the game runs (app setting): it is requested from any thread and
// latched at the frame boundary; render targets made at another factor are reallocated when they
// are next rendered to (rescaled()), keeping their contents.
static float clamp_scale(float v) { return (v >= 0.25f && v <= 4.0f) ? v : 1.0f; }
static float g_res = [] {
    const char* e = getenv("WWHD_RES_SCALE");
    float v = clamp_scale(e ? (float)atof(e) : 1.0f);
    // the game renders its 3D view at 1280x720 (and copies it to a 1080p scan buffer)
    if (v != 1.0f) LOG("[gfx] resolution scale %.2fx (3D view %.0fx%.0f)", v, 1280 * v, 720 * v);
    return v;
}();
static std::atomic<float> g_res_requested{0};
static float g_res_setting = g_res;  // the setting; dynamic resolution may go below it
// for the performance overlay (read from the app's thread)
static std::atomic<float> g_res_shown{g_res}, g_tv_kx_shown{1}, g_tv_ky_shown{1};

float resolution_scale() { return g_res; }

void set_resolution_scale(float v) { g_res_requested = clamp_scale(v); }

void latch_resolution_scale() {
    float v = g_res_requested.exchange(0);
    if (v > 0 && v != g_res_setting) {
        g_res_setting = v;
        LOG("[gfx] resolution scale setting %.2fx (3D view %.0fx%.0f)", v, 1280 * v, 720 * v);
    }
    v = dynres_scale(g_res_setting);
    if (v == g_res) return;
    g_res = v;
    g_res_shown = v;
    if (!dynamic_resolution_on()) LOG("[gfx] resolution scale %.2fx (3D view %.0fx%.0f)", v, 1280 * v, 720 * v);
}

void resolution_shown(float out[3]) {
    float s = g_res_shown;
    out[0] = s;
    out[1] = (float)std::max(1L, lroundf(1280 * s * g_tv_kx_shown));
    out[2] = (float)std::max(1L, lroundf(720 * s * g_tv_ky_shown));
}

static bool screen_shaped(const Surface* s) {
    if (s->slices != 1 || s->width < 16 || s->height < 9) return false;
    float aspect = (float)s->width / (float)s->height;
    return aspect > 1.70f && aspect < 1.84f;
}

// Aspect ratio of the TV picture (aspect.cpp): the game's TV-shaped buffers (1280x720 ... and their
// reductions, not the GamePad's 854x480 family) are created kx times wider (ky taller) on top of the
// resolution scale. Draws keep their guest viewports, which then cover the wider image, and the
// game's projections are widened to match (Hor+), so every full-screen pass lines up. The factor
// travels with the swap command and changes between frames only (render thread).
// The GamePad's 854x480 family gets its own factors: the GamePad picture on a second display of
// another shape (aspect.cpp "GamePad picture"; only the layouts adapt to it).
static float g_aspect_kx = 1.0f, g_aspect_ky = 1.0f, g_drc_kx = 1.0f, g_drc_ky = 1.0f;
static void aspect_factors(float a, float& kx, float& ky) {
    const float base = 16.0f / 9.0f;
    if (!(a > 0.49f && a < 8.0f)) a = base;
    kx = a >= base ? a / base : 1.0f;
    ky = a >= base ? 1.0f : base / a;
}
void set_frame_aspect(float a, float drc) {
    float kx, ky;
    aspect_factors(a, kx, ky);
    if (kx != g_aspect_kx || ky != g_aspect_ky) LOG("[gfx] aspect %.4f: TV targets x%.4f wide, x%.4f tall", a, kx, ky);
    g_aspect_kx = kx;
    g_aspect_ky = ky;
    g_tv_kx_shown = kx;
    g_tv_ky_shown = ky;
    aspect_factors(drc, kx, ky);
    if (kx != g_drc_kx || ky != g_drc_ky) LOG("[gfx] GamePad aspect %.4f: GamePad targets x%.4f wide, x%.4f tall", drc, kx, ky);
    g_drc_kx = kx;
    g_drc_ky = ky;
}
static bool drc_shaped(uint32_t width, uint32_t height) {
    for (uint32_t w = 854, h = 480; w >= 32; w >>= 1, h >>= 1)
        if ((width == w || width == w + 1) && height == h) return true;
    return false;
}
static bool tv_shaped(uint32_t width, uint32_t height) {
    if (width < 32 || height < 18 || drc_shaped(width, height)) return false;
    float r = (float)width * 9.0f / ((float)height * 16.0f);
    return r > 0.97f && r < 1.03f;
}
bool target_is_tv(uint32_t w, uint32_t h) { return tv_shaped(w, h); }
bool target_is_drc(uint32_t w, uint32_t h) { return drc_shaped(w, h); }
bool target_aspect_factors(uint32_t w, uint32_t h, float& kx, float& ky) {
    kx = ky = 1.0f;
    if (tv_shaped(w, h)) {
        kx = g_aspect_kx;
        ky = g_aspect_ky;
    } else if (drc_shaped(w, h)) {
        kx = g_drc_kx;
        ky = g_drc_ky;
    }
    return kx != 1.0f || ky != 1.0f;
}
static void target_aspect(const Surface* s, float& ax, float& ay) {
    if (s->slices != 1 || s->mips > 1 || s->fmt.compressed) { ax = ay = 1.0f; return; }
    target_aspect_factors(s->width, s->height, ax, ay);
}

Surface* rescaled(Surface* s) {
    if (!s || !s->img.image || s->img.type != VK_IMAGE_TYPE_2D || s->mips > 1 || !screen_shaped(s)) return s;
    float want = resolution_scale(), ax, ay;
    target_aspect(s, ax, ay);
    if (std::fabs(s->rscale - want) < 1e-3f && s->ax == ax && s->ay == ay) return s;
    Image old = s->img;
    float oldScale = s->rscale, oldAx = s->ax, oldAy = s->ay;
    s->img = Image{};
    if (!create_surface_image(s, true)) {
        s->img = old;
        s->rscale = oldScale;
        s->ax = oldAx;
        s->ay = oldAy;
        return s;
    }
    // keep the contents: a clear still waiting for the next pass (lazy clears) moves to the new image;
    // else a filtered copy where the format allows, else a nearest one; formats the GPU can't
    // blit (depth + stencil on Adreno) start cleared (depth 1, stencil 0, colour 0). Before, the new
    // image started with whatever its memory held and the game's clear, waiting on the old image, was
    // dropped: a frame of block noise over the 3D picture at each change (dynamic resolution)
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R.pd, s->img.format, &fp);
    VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    const bool blit = (f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (f & VK_FORMAT_FEATURE_BLIT_DST_BIT);
    const Image::PendingClear& pc = old.pendingClear;
    if (!blit || (pc.on && pc.layer == 0 && pc.aspects == s->img.aspect)) {
        Image::PendingClear& c = s->img.pendingClear;
        c.on = true;
        c.layer = 0;
        c.aspects = s->img.aspect;
        if (s->img.aspect & VK_IMAGE_ASPECT_COLOR_BIT) {
            if (pc.on && pc.layer == 0) c.value = pc.value;
        } else {
            c.value.depthStencil.depth = pc.on && pc.layer == 0 && (pc.aspects & VK_IMAGE_ASPECT_DEPTH_BIT) ? pc.value.depthStencil.depth : 1.0f;
            c.value.depthStencil.stencil = pc.on && pc.layer == 0 && (pc.aspects & VK_IMAGE_ASPECT_STENCIL_BIT) ? pc.value.depthStencil.stencil : 0;
        }
    } else {
        bool linear = !s->fmt.depth && (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT);
        prepare(old, Use::COPY_SRC);
        prepare(s->img, Use::COPY_DST);
        VkImageBlit b{};
        b.srcSubresource = {old.aspect, 0, 0, 1};
        b.dstSubresource = {s->img.aspect, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)old.width, (int32_t)old.height, 1};
        b.dstOffsets[1] = {(int32_t)s->img.width, (int32_t)s->img.height, 1};
        vkCmdBlitImage(command_buffer(), old.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    retire_image(old);
    if (Surface* c = s->feedbackCopy) {  // recreated at the new size when next needed
        retire_image(c->img);
        delete c;
        s->feedbackCopy = nullptr;
    }
    if (Surface* c = s->mipChain) {
        retire_image(c->img);
        delete c;
        s->mipChain = nullptr;
    }
    if (Surface* c = s->volume) {
        retire_image(c->img);
        delete c;
        s->volume = nullptr;
    }
    static int logged = 0;
    if (getenv("WWHD_LOG_RESCALE") || logged++ < 3)
        LOG("[gfx] rescaled %08X %ux%u to %ux%u (%s)", s->addr, s->width, s->height, s->img.width, s->img.height,
            s->img.pendingClear.on ? "cleared" : "copied");
    return s;
}

bool create_surface_image(Surface* s, bool forRendering) {
    VkImageType type;
    VkImageViewType view;
    uint32_t layers, depth;
    bool cube;
    image_shape(s->dim, s->slices, forRendering, type, view, layers, depth, cube);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (s->fmt.renderable && type == VK_IMAGE_TYPE_2D)
        usage |= s->fmt.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    uint32_t w = s->width, h = type == VK_IMAGE_TYPE_1D ? 1 : s->height;
    s->rscale = 1.0f;
    s->ax = s->ay = 1.0f;
    if (forRendering && type == VK_IMAGE_TYPE_2D && screen_shaped(s)) target_aspect(s, s->ax, s->ay);
    if (forRendering && type == VK_IMAGE_TYPE_2D && (resolution_scale() != 1.0f || s->ax != 1.0f || s->ay != 1.0f) && screen_shaped(s)) {
        s->rscale = resolution_scale();
        w = std::max<uint32_t>(1, (uint32_t)lroundf(s->width * s->rscale * s->ax));
        h = std::max<uint32_t>(1, (uint32_t)lroundf(s->height * s->rscale * s->ay));
    }
    // a mip chain can't be longer than the size allows
    uint32_t maxMips = 1;
    for (uint32_t d = std::max({w, h, depth}); d > 1; d >>= 1) maxMips++;
    s->mips = std::min(s->mips, maxMips);
    if (cube && !R.features.imageCubeArray && layers > 6) layers = 6;
    return create_image(s->img, type, view, s->fmt.format, w, h, depth, layers, s->mips, usage, cube, s->fmt.depth, s->fmt.stencil);
}

Surface* find_or_create_surface(const SurfaceDesc& d, bool forRendering) {
    auto range = R.surfaces.equal_range(d.addr);
    Surface* exact = nullptr;
    // sampling a GPU-written surface: several can alias one address (mip chains rendered into the same
    // memory, a render target recreated as a texture array...). Prefer the same size, then the same
    // array size, then the most recent write.
    Surface* rendered = nullptr;
    auto score = [&](Surface* s) {
        return std::make_tuple(s->width == d.width && s->height == d.height, s->slices == d.slices, s->writeSeq);
    };
    auto consider = [&](Surface* s) { if (!rendered || score(s) > score(rendered)) rendered = s; };
    for (auto it = range.first; it != range.second; ++it) {
        Surface* s = it->second.get();
        // a rendered depth buffer sampled as a texture (fog, depth of field, shadow maps...)
        if (!forRendering && s->isDepth && !d.isDepth && s->gpuWritten && s->width == d.width && s->height == d.height)
            consider(s);
        if (s->isDepth != d.isDepth) continue;
        // a render target is one level: don't adopt a texture made first by sampling the address with
        // a mip chain (rendering would define level 0 only and mip-filtered reads would see the
        // never-rendered levels: black shadows for the session; original project efc9f66)
        if (forRendering && s->mips > 1) continue;
        if (s->width == d.width && s->height == d.height && s->format == d.format && s->slices == d.slices &&
            (forRendering || s->mips >= d.mips || s->gpuWritten)) {
            if (forRendering) return s;
            if (!exact || s->writeSeq > exact->writeSeq) exact = s;
            continue;
        }
        // render target being sampled with a compatible format but different view parameters
        if (!forRendering && s->gpuWritten && (s->format & 0x3F) == (d.format & 0x3F)) consider(s);
    }
    if (exact && (exact->gpuWritten || !rendered || exact->writeSeq > rendered->writeSeq))
        return exact->gpuWritten ? adopt_newer_alias(exact) : exact;
    if (rendered) return rendered;
    if (exact) return exact;

    auto s = std::make_unique<Surface>();
    s->addr = d.addr;
    s->mipAddr = d.mipAddr;
    s->width = std::max(d.width, 1u);
    s->height = std::max(d.height, 1u);
    s->slices = std::max(d.slices, 1u);
    s->pitch = d.pitch;
    s->mips = forRendering ? 1 : std::max(d.mips, 1u);
    s->format = d.format;
    s->dim = d.dim;
    s->tileMode = d.tileMode;
    s->swizzle = d.swizzle;
    s->isDepth = d.isDepth;
    s->fmt = format_info(d.format, d.isDepth);
    if (!create_surface_image(s.get(), forRendering)) {
        LOG("[gfx] cannot create %ux%ux%u image (format %X, vk %d)", s->width, s->height, s->slices, s->format, (int)s->fmt.format);
        return nullptr;
    }
    // debug: WWHD_LOG_SURFACES=1 logs every render target / depth buffer the game creates
    static const bool logSurfaces = getenv("WWHD_LOG_SURFACES") != nullptr;
    if (logSurfaces && forRendering)
        LOG("[surface] %s %08X %ux%u x%u format %X tile %u", s->isDepth ? "depth" : "color", s->addr, s->width, s->height,
            s->slices, s->format, s->tileMode);
    Surface* raw = s.get();
    // another format view of the same texel bits at this address (adopt_newer_alias)
    if (!raw->isDepth)
        for (auto it = range.first; it != range.second; ++it)
            if (!it->second->isDepth && (it->second->format & 0x3F) == (raw->format & 0x3F)) raw->aliased = it->second->aliased = true;
    R.surfaces.emplace(d.addr, std::move(s));
    if (forRendering && !raw->isDepth && raw->tileMode == (uint32_t)Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED) g_linear_targets.push_back(raw);
    surfaces_changed();
    return raw;
}

// One guest surface can be drawn and sampled through views of different formats with the same texel
// bits, such as RGBA8 and RGBA8 sRGB: the Picto Box draws its picture through the sRGB view and then
// through the plain one (issue #22; as upstream's fix for its issue #53). Each format has its own
// image here, so before one is used, it takes over the texels of a more recent compatible one (a raw
// copy, as the memory is shared).
static Surface* adopt_newer_alias(Surface* s) {
    if (!s || !s->aliased || !s->img.image || s->isDepth || s->fmt.compressed || s->fmt.convert != Convert::NONE) return s;
    Surface* newest = nullptr;
    auto range = R.surfaces.equal_range(s->addr);
    for (auto it = range.first; it != range.second; ++it) {
        Surface* o = it->second.get();
        if (o == s || !o->gpuWritten || !o->img.image || o->isDepth || o->fmt.compressed || o->fmt.convert != Convert::NONE) continue;
        if (s->gpuWritten && o->writeSeq <= s->writeSeq) continue;
        if ((o->format & 0x3F) != (s->format & 0x3F) || o->fmt.hostBytesPerBlock != s->fmt.hostBytesPerBlock) continue;
        if (o->img.type != s->img.type || o->img.layers != s->img.layers || o->img.width != s->img.width ||
            o->img.height != s->img.height || o->img.depth != s->img.depth)
            continue;
        if (!newest || o->writeSeq > newest->writeSeq) newest = o;
    }
    if (!newest) return s;
    prepare(newest->img, Use::COPY_SRC);
    prepare(s->img, Use::COPY_DST);
    VkImageCopy regions[16];
    uint32_t n = std::min({s->img.mips, newest->img.mips, 16u});
    for (uint32_t m = 0; m < n; m++) {
        regions[m] = {};
        regions[m].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, m, 0, s->img.layers};
        regions[m].dstSubresource = regions[m].srcSubresource;
        regions[m].extent = {std::max(s->img.width >> m, 1u), std::max(s->img.height >> m, 1u), std::max(s->img.depth >> m, 1u)};
    }
    vkCmdCopyImage(command_buffer(), newest->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, n, regions);
    mark_gpu_written(s);
    static int logged = 0;
    if (logged++ < 8) LOG("[gfx] %08X: format %X view takes the texels of format %X", s->addr, s->format, newest->format);
    return s;
}

// ---------------------------------------------------------------- render targets
// CB_COLORn_BASE holds the full guest address; CB_COLORn_TILE/FRAG hold width/height (our convention).
constexpr uint32_t kDim2D = 1, kDim2DArray = 5;

Surface* color_target(const uint32_t* regs, int i, uint32_t* slice) {
    uint32_t base = regs[mmCB_COLOR0_BASE + i];
    if (!base) return nullptr;
    uint32_t size = regs[mmCB_COLOR0_SIZE + i], info = regs[mmCB_COLOR0_INFO + i];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    // our convention (GX2SetColorBuffer): TILE = width | array slices << 16, FRAG = height
    uint32_t w = regs[mmCB_COLOR0_TILE + i] & 0xFFFF, h = regs[mmCB_COLOR0_FRAG + i];
    uint32_t slices = std::max<uint32_t>(regs[mmCB_COLOR0_TILE + i] >> 16, 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmCB_COLOR0_VIEW + i] & 0x7FF, slices - 1) : 0;
    static const uint32_t numberBits[8] = {0, 0x200, 0, 0, 0x100, 0x300, 0x400, 0x800};
    SurfaceDesc d;
    d.addr = base;
    d.width = w ? w : pitch;
    d.height = h ? h : height;
    d.pitch = pitch;
    d.format = ((info >> 2) & 0x3F) | numberBits[(info >> 12) & 7];
    d.tileMode = (info >> 8) & 0xF;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return adopt_newer_alias(rescaled(find_or_create_surface(d, true)));
}

Surface* depth_target(const uint32_t* regs, uint32_t* slice) {
    uint32_t base = regs[mmDB_DEPTH_BASE];
    if (!base) return nullptr;
    uint32_t slices = std::max<uint32_t>(regs[gx2::kDepthSlicesReg], 1);
    if (slice) *slice = slices > 1 ? std::min<uint32_t>(regs[mmDB_DEPTH_VIEW] & 0x7FF, slices - 1) : 0;
    uint32_t size = regs[mmDB_DEPTH_SIZE], info = regs[mmDB_DEPTH_INFO];
    uint32_t pitch = ((size & 0x3FF) + 1) * 8;
    uint32_t height = (((size >> 10) & 0xFFFFF) + 1) * 64 / pitch;
    uint32_t wh = regs[mmDB_HTILE_DATA_BASE];  // our convention: width << 16 | height
    static const uint32_t fmts[8] = {0, 0x005, 0, 0x011, 0, 0x811, 0x80E, 0x81C};
    SurfaceDesc d;
    d.addr = base;
    d.width = wh ? (wh >> 16) : pitch;
    d.height = wh ? (wh & 0xFFFF) : height;
    d.pitch = pitch;
    d.format = fmts[info & 7];
    d.isDepth = true;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    return rescaled(find_or_create_surface(d, true));
}

Surface* surface_from_color_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* cb = (GX2::GX2ColorBuffer*)mem::ptr(addr);
    SurfaceDesc d;
    auto dim = cb->surface.dim.value();
    uint32_t slices = dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D ? std::max<uint32_t>(cb->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(cb->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(cb->viewNumSlices, 1, slices - std::min<uint32_t>(cb->viewFirstSlice, slices - 1));
    d.addr = gx2::color_buffer_address(cb);
    d.width = std::max<uint32_t>(cb->surface.width >> cb->viewMip, 1);
    d.height = std::max<uint32_t>(cb->surface.height >> cb->viewMip, 1);
    d.pitch = cb->surface.pitch;
    d.format = (uint32_t)cb->surface.format.value();
    d.tileMode = (uint32_t)cb->surface.tileMode.value();
    return find_or_create_surface(d, true);
}

Surface* surface_from_depth_buffer(uint32_t addr, uint32_t* firstSlice, uint32_t* numSlices) {
    auto* db = (GX2::GX2DepthBuffer*)mem::ptr(addr);
    SurfaceDesc d;
    uint32_t slices = db->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(db->surface.depth, 1) : 1;
    d.slices = slices;
    d.dim = slices > 1 ? kDim2DArray : kDim2D;
    if (firstSlice) *firstSlice = std::min<uint32_t>(db->viewFirstSlice, slices - 1);
    if (numSlices) *numSlices = std::clamp<uint32_t>(db->viewNumSlices, 1, slices - std::min<uint32_t>(db->viewFirstSlice, slices - 1));
    d.addr = db->surface.imagePtr;
    d.width = db->surface.width;
    d.height = db->surface.height;
    d.pitch = db->surface.pitch;
    d.format = (uint32_t)db->surface.format.value();
    d.tileMode = (uint32_t)db->surface.tileMode.value();
    d.isDepth = true;
    return find_or_create_surface(d, true);
}

// ---------------------------------------------------------------- sampled textures
static uint64_t sparse_hash(Surface* s);
uint64_t g_stat_full_checks, g_stat_uploads, g_stat_invalidates, g_stat_invalidated_surfaces;

// A rendered picture sampled with mip levels. The game draws level 0, often draws the coarser
// levels itself at their mip addresses (the bloom renders its blurred levels 1-3 there, the
// distance blur its level 1), and samples them: the distance blur (PS 4502D100) reads the scene at
// LOD = distance * k - 1 (the haze on far islands), the bloom passes read levels 1-3. Render targets
// have a single level here, so such a view returned level 0 only. A companion image holds the whole
// chain: each level is the picture the game rendered at that level's address if there is one (as
// on the console, where the levels simply are that memory), else a downscale of the level above.
// Rebuilt when the base or one of the game's levels was drawn to since. WWHD_NO_RT_MIPS=1: level 0
// only, as before.
static Surface* game_level(const SurfaceDesc& d, uint32_t level) {
    uint32_t a;
    if (level == 1) a = d.mipAddr;
    else {
        uint32_t sliceOffset = 0, sliceSize = 0;
        sint32 sub = 0;
        LatteAddrLib::CalculateMipAndSliceAddr(d.addr, d.mipAddr, (Latte::E_GX2SURFFMT)d.format, d.width, d.height, d.slices,
                                               (Latte::E_DIM)d.dim, (Latte::E_HWTILEMODE)d.tileMode, d.swizzle, 0, level, 0,
                                               &sliceOffset, &sliceSize, &sub);
        a = sliceOffset;
    }
    if (!a) return nullptr;
    if (Latte::TM_IsMacroTiled((Latte::E_HWTILEMODE)d.tileMode)) a &= ~0x700u;
    const uint32_t w = std::max(d.width >> level, 1u), h = std::max(d.height >> level, 1u);
    Surface* best = nullptr;
    auto range = R.surfaces.equal_range(a);
    for (auto it = range.first; it != range.second; ++it) {
        Surface* l = it->second.get();
        if (!l->gpuWritten || !l->img.image || l->fmt.depth || l->width != w || l->height != h || (l->format & 0x3F) != (d.format & 0x3F))
            continue;
        if (!best || l->writeSeq > best->writeSeq) best = l;
    }
    return best;
}

static Surface* with_mip_chain(Surface* s, const SurfaceDesc& d, uint32_t maxLevels) {
    static const bool off = getenv("WWHD_NO_RT_MIPS") != nullptr;
    uint32_t mips = d.mips;
    // In place (default): the picture's own image gets the levels the first time it is sampled with mips
    // (its level 0 copied over once), and the chain is built in it: no copy of level 0 into a separate
    // chain image whenever the picture changed (5 copies a frame at Windfall, ~1.1 ms on a Mali-G52).
    // WWHD_RT_MIPS_SEPARATE=1: a separate chain image, as before.
    static const bool separate = getenv("WWHD_RT_MIPS_SEPARATE") != nullptr;
    if (off || mips <= 1 || s->fmt.depth || s->fmt.compressed || s->img.type != VK_IMAGE_TYPE_2D || s->img.layers != 1 ||
        (s->img.mips != 1 && (separate || !s->mipsInPlace)) || !s->img.image)
        return s;
    const uint32_t w = s->img.width, h = s->img.height;
    uint32_t full = 1;
    while ((std::max(w, h) >> full) > 0) full++;
    mips = std::min(mips, full);
    if (mips <= 1) return s;
    // Only the levels the sampler reaches are built (its MAX_LOD, plus one: linear mip filtering at a
    // whole LOD may still fetch the next level at weight 0). Each level is a blit and a barrier, which
    // on Mali drains the GPU: the full chains cost ~3.5 ms a frame at Windfall on a Mali-G52 (0.75x).
    // A sampler without mips reads the picture itself. The image keeps all levels (other samplers).
    static const bool allLevels = getenv("WWHD_RT_MIPS_ALL") != nullptr;  // debug: every level, as before
    const uint32_t need = allLevels ? mips : std::min(mips, std::max(maxLevels, 1u));
    if (need <= 1) return s;
    static std::unordered_map<VkFormat, bool> blittable;
    auto bl = blittable.find(s->img.format);
    if (bl == blittable.end()) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(R.pd, s->img.format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        bl = blittable.emplace(s->img.format, (fp.optimalTilingFeatures & need) == need).first;
    }
    if (!bl->second) return s;
    // the game's own levels, and per level a key of what it is built from: the picture (level 0), the
    // game's own level, or the level below. The game renders its own downsampling chain a level at a
    // time and samples after each: only the levels from the first changed one on are built again
    // (rebuilding the whole chain each time cost copies, blits and barriers that drain Mali GPUs).
    Surface* levels[16] = {};
    uint64_t keys[16] = {};
    keys[0] = s->writeSeq * 0x9E3779B97F4A7C15ull + 1;
    for (uint32_t l = 1; l < mips && l < 16; l++) {
        levels[l] = game_level(d, l);
        keys[l] = levels[l] ? (levels[l]->writeSeq + 1) * 0xC2B2AE3D27D4EB4Full ^ l : (keys[l - 1] ^ l) * 0xFF51AFD7ED558CCDull;
    }
    Surface* c = separate ? s->mipChain : s;
    if (!separate) {
        if (s->img.mips != mips) {
            if (s->img.mips != 1) return s;  // (another level count than it was given: as without a chain)
            Image ni;
            if (!create_image(ni, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, s->img.format, w, h, 1, 1, mips,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                              false, false, false))
                return s;
            prepare(s->img, Use::COPY_SRC);
            prepare(ni, Use::COPY_DST);
            VkImageCopy cp{};
            cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            cp.extent = {w, h, 1};
            vkCmdCopyImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ni.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            retire_image(s->img);  // (views, framebuffers and descriptor sets of the old image are dropped)
            s->img = std::move(ni);
            s->mipsInPlace = true;
            s->mipChainLevels = 0;
        }
    } else if (c && (c->img.mips != mips || c->img.width != w || c->img.height != h || c->img.format != s->img.format)) {
        retire_image(c->img);
        delete c;
        c = s->mipChain = nullptr;
    }
    if (separate && !c) {
        c = new Surface();
        c->width = s->width;
        c->height = s->height;
        c->slices = 1;
        c->mips = mips;
        c->format = s->format;
        c->dim = s->dim;
        c->fmt = s->fmt;
        c->rscale = s->rscale;
        c->ax = s->ax;
        c->ay = s->ay;
        c->gpuWritten = true;
        if (!create_image(c->img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, s->img.format, w, h, 1, 1, mips,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false,
                          false, false)) {
            delete c;
            return s;
        }
        s->mipChain = c;
        s->mipChainLevels = 0;
    }
    static const bool whole = getenv("WWHD_RT_MIPS_WHOLE") != nullptr;  // debug: every chain from level 0, as before
    uint32_t from = 0;  // the first level to build
    while (from < need && from < s->mipChainLevels && s->mipChainKey[from] == keys[from]) from++;
    if (from >= need) return c;
    if (whole) from = 0;
    if (!separate && from == 0) {  // level 0 is the picture itself
        s->mipChainKey[0] = keys[0];
        from = 1;
    }
    for (uint32_t l = from; l < need; l++) s->mipChainKey[l] = keys[l];
    s->mipChainLevels = need;
    if (from == 0) prepare(s->img, Use::COPY_SRC);
    for (uint32_t l = std::max(from, 1u); l < need; l++)
        if (levels[l]) prepare(levels[l]->img, Use::COPY_SRC);
    prepare(c->img, Use::COPY_DST);  // all levels in TRANSFER_DST (the kept ones keep their contents)
    VkCommandBuffer cmd = command_buffer();
    if (from == 0) {
        VkImageCopy cp{};
        cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        cp.extent = {w, h, 1};
        vkCmdCopyImage(cmd, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    }
    auto to_src = [&](uint32_t level) {  // a finished level becomes the next blit's source
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c->img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, level, 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    };
    if (from >= 2) {  // the kept levels below the one the first blit reads: back to transfer sources
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c->img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, from - 1, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    uint32_t fromGame = 0;
    for (uint32_t l = std::max(from, 1u); l < need; l++) {
        to_src(l - 1);
        VkImageBlit b{};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        b.dstOffsets[1] = {(int32_t)std::max(w >> l, 1u), (int32_t)std::max(h >> l, 1u), 1};
        Surface* g = l < 16 ? levels[l] : nullptr;
        if (g) {  // the game's own level (blitted: its image size may differ by rounding)
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            b.srcOffsets[1] = {(int32_t)g->img.width, (int32_t)g->img.height, 1};
            vkCmdBlitImage(cmd, g->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &b, VK_FILTER_LINEAR);
            fromGame++;
        } else {
            b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l - 1, 0, 1};
            b.srcOffsets[1] = {(int32_t)std::max(w >> (l - 1), 1u), (int32_t)std::max(h >> (l - 1), 1u), 1};
            vkCmdBlitImage(cmd, c->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &b, VK_FILTER_LINEAR);
        }
    }
    to_src(need - 1);
    if (need < mips) {  // the levels not built: the same layout as the rest (one layout per image)
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = c->img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, need, mips - need, 0, 1};
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    c->img.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // every level is now a transfer source
    c->img.use = Use::COPY_SRC;
    static int logged = 0;
    if (logged++ < 8)
        LOG("[gfx] mip chain for %08X %ux%u: levels %u-%u of %u built, %u of them the game's own", s->addr, w, h, from, need - 1, mips,
            fromGame);
    return c;
}

// A render target sampled as a 3D texture: the game renders the volume slice by slice (each slice a
// layer of the 2D array render target; the Picto Box's colour grading after a shot), then samples it
// as a 3D texture, which a 2D image can't be viewed as. A companion 3D image gets the layers as its
// slices, copied again whenever the target was drawn to since.
static Surface* with_volume(Surface* s, const SurfaceDesc& d) {
    if (d.dim != (uint32_t)Latte::E_DIM::DIM_3D || !s || s->img.type != VK_IMAGE_TYPE_2D || s->img.layers < 1 || s->fmt.depth ||
        !s->img.image)
        return s;
    const uint32_t w = s->img.width, h = s->img.height, n = s->img.layers;
    Surface* c = s->volume;
    if (c && (c->img.width != w || c->img.height != h || c->img.depth != n || c->img.format != s->img.format)) {
        retire_image(c->img);
        delete c;
        c = s->volume = nullptr;
    }
    if (!c) {
        c = new Surface();
        c->width = s->width;
        c->height = s->height;
        c->slices = n;
        c->mips = 1;
        c->format = s->format;
        c->dim = d.dim;
        c->fmt = s->fmt;
        c->gpuWritten = true;
        if (!create_image(c->img, VK_IMAGE_TYPE_3D, VK_IMAGE_VIEW_TYPE_3D, s->img.format, w, h, n, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false)) {
            delete c;
            return s;
        }
        s->volume = c;
        s->volumeSeq = ~0ull;
        LOG("[gfx] 3D texture %08X %ux%ux%u from its rendered slices", s->addr, w, h, n);
    }
    if (s->volumeSeq == s->writeSeq) return c;
    s->volumeSeq = s->writeSeq;
    prepare(s->img, Use::COPY_SRC);
    prepare(c->img, Use::COPY_DST);
    // layers of a 2D array -> slices of a 3D image (Vulkan 1.1: layer count = extent depth)
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, n};
    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {w, h, n};
    vkCmdCopyImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, c->img.image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    return c;
}

// The surface a set of texture words resolves to only changes when a surface is created or written
// (the choice among surfaces aliasing one address depends on that): remember recent lookups.
static Surface* check_texture(Surface* s);
struct TexLookup {
    uint32_t w[7];
    bool depthSampler;
    SurfaceDesc d;  // what the words describe (the mip chain is applied on every hit)
    uint64_t writeSeq;
    size_t surfaces;
    Surface* s;
    bool used;
};
static TexLookup g_tex_lookups[512];

static const bool g_shadow_color_lookup = getenv("WWHD_SHADOW_COLOR_LOOKUP") != nullptr;  // debug: as before (sampled_texture)
Surface* sampled_texture(const uint32_t* w, bool isDepthSampler, uint32_t maxLevels) {
    uint64_t key = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 7; i++) key = (key ^ w[i]) * 0xFF51AFD7ED558CCDull;
    TexLookup& tl = g_tex_lookups[(key ^ (key >> 29)) & 511];
    if (tl.used && tl.writeSeq == g_write_seq && tl.surfaces == R.surfaces.size() && tl.depthSampler == isDepthSampler &&
        memcmp(tl.w, w, sizeof tl.w) == 0) {
        Surface* s = check_texture(tl.s);
        if (s && s->gpuWritten && tl.d.dim == (uint32_t)Latte::E_DIM::DIM_3D) return with_volume(s, tl.d);
        if (s && s->gpuWritten && tl.d.mips > 1 && !isDepthSampler) return with_mip_chain(s, tl.d, maxLevels);
        return s;
    }
    Latte::LATTE_SQ_TEX_RESOURCE_WORD0_N w0;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N w1;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N w4;
    Latte::LATTE_SQ_TEX_RESOURCE_WORD5_N w5;
    memcpy(&w0, &w[0], 4);
    memcpy(&w1, &w[1], 4);
    memcpy(&w4, &w[4], 4);
    memcpy(&w5, &w[5], 4);
    uint32_t addr = w[2] << 8, mipAddr = w[3] << 8;
    if (!addr) return nullptr;
    auto dim = w0.get_DIM();
    uint32_t pitch = (w0.get_PITCH() + 1) << 3;
    uint32_t width = w0.get_WIDTH() + 1;
    uint32_t height = w1.get_HEIGHT() + 1;
    uint32_t depth = w1.get_DEPTH();
    if (dim == Latte::E_DIM::DIM_2D_ARRAY || dim == Latte::E_DIM::DIM_3D || dim == Latte::E_DIM::DIM_2D_ARRAY_MSAA ||
        dim == Latte::E_DIM::DIM_1D_ARRAY)
        depth += 1;
    else {
        if (dim == Latte::E_DIM::DIM_CUBEMAP) depth = 6 * (depth + 1);
        if (depth == 0) depth = 1;
    }
    if (dim == Latte::E_DIM::DIM_1D || dim == Latte::E_DIM::DIM_1D_ARRAY) height = 1;
    auto tileMode = w0.get_TILE_MODE();
    if (Latte::IsCompressedFormat(w1.get_DATA_FORMAT())) pitch /= 4;
    uint32_t swizzle = 0;
    if (Latte::TM_IsMacroTiled(tileMode)) {
        swizzle = addr & 0x700;
        addr &= ~0x700u;
    }
    SurfaceDesc d;
    d.addr = addr;
    d.mipAddr = mipAddr;
    d.width = width;
    d.height = height;
    d.slices = depth;
    d.pitch = pitch;
    d.mips = w5.get_LAST_LEVEL() + 1;
    d.format = (uint32_t)LatteTexture_ReconstructGX2Format(w1, w4);
    d.dim = (uint32_t)dim;
    d.tileMode = (uint32_t)tileMode;
    d.swizzle = swizzle;
    // A depth-compare (shadow) sampler looks for the depth surfaces at this address, as the original
    // project's renderer does: a GPU-written colour image aliasing the shadow map's memory must not
    // win over the cascade (the shadows alternated between two states).
    const bool colorLookup = g_shadow_color_lookup;
    d.isDepth = isDepthSampler && !colorLookup;
    Surface* s = find_or_create_surface(d, false);
    s = check_texture(s);
    memcpy(tl.w, w, sizeof tl.w);
    tl.depthSampler = isDepthSampler;
    tl.d = d;
    tl.writeSeq = g_write_seq;  // after the check: an upload counts as a write
    tl.surfaces = R.surfaces.size();
    tl.s = s;
    tl.used = true;
    if (s && s->gpuWritten && d.dim == (uint32_t)Latte::E_DIM::DIM_3D) return with_volume(s, d);
    if (s && s->gpuWritten && d.mips > 1 && !d.isDepth) return with_mip_chain(s, d, maxLevels);
    return s;
}

// a sampled CPU texture: upload it if its memory changed (checked once per frame)
// The sparse sample (256 words spread over the texture, nearly all cache misses: ~5% of the render
// thread at 60 fps) is skipped while the game hasn't written the texture's pages since the last
// comparison (mem_writes.h, as vertex buffers); still taken on one frame in 16 (spread by address),
// counting what the tracking missed, and after our own writes (lastCheckedFrame ~0: copies, readbacks).
static const bool g_track_texture_writes = [] { const char* e = getenv("WWHD_TRACK_WRITES"); return !e || atoi(e) != 0; }();
uint64_t g_stat_texture_stale;
static Surface* check_texture(Surface* s) {
    if (s && !s->gpuWritten && s->lastCheckedFrame != R.frame) {
        const bool forced = s->lastCheckedFrame == ~0ull;
        s->lastCheckedFrame = R.frame;
        // full hash only when new, invalidated, every 64 frames, or when a sparse sample changed
        bool full = s->dirty || !s->dataSize || ((R.frame + (s->addr >> 12)) & 63) == 0;
        if (!full) {
            const bool verify = forced || ((R.frame + (s->addr >> 12)) & 15) == 0;
            const bool tracked = g_track_texture_writes && memw::unchanged_since(s->addr, s->dataSize, s->checkGen);
            if (tracked && !verify) return s;
            const uint32_t gen = memw::now();  // before reading
            uint64_t h = sparse_hash(s);
            if (h != s->sparseHash) {
                full = true;
                if (tracked && !forced) g_stat_texture_stale++;
            } else {
                s->checkGen = gen;
            }
        }
        if (full) {
            g_stat_full_checks++;
            const uint32_t gen = memw::now();
            upload_surface(s);
            s->sparseHash = sparse_hash(s);
            s->checkGen = gen;
            s->dirty = false;
        }
    }
    return s;
}

// ---------------------------------------------------------------- BC decoding on the GPU
// Where the GPU can't sample BC textures (most Mali and PowerVR GPUs), a compute shader unpacks
// them to RGBA8 / R8 / RG8 instead of the CPU. Bit-exact with convert_row() in vk_formats.cpp.
// One invocation per 4x4 block; input = the detiled blocks, output = rows of texels as the copy
// to the image expects them. WWHD_BC_DECODE=cpu keeps the CPU decoder (for comparisons).
// runtime/shaders/bc_decode.comp, compiled to SPIR-V at build time
static const uint32_t kBcDecodeSpv[] = {
#include "bc_decode.comp.inc"
};

namespace {
struct BcDecoder {
    bool tried = false;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
} g_bc;
}  // namespace

// the compute pipeline, created on first use; null if unavailable (then the CPU decodes)
static VkPipeline bc_decoder() {
    if (g_bc.tried) return g_bc.pipe;
    g_bc.tried = true;
    const char* env = getenv("WWHD_BC_DECODE");
    if (env && !strcmp(env, "cpu")) {
        LOG("[vk] BC textures: decoded on the CPU (WWHD_BC_DECODE=cpu)");
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof kBcDecodeSpv;
    mi.pCode = kBcDecodeSpv;
    VkShaderModule mod;
    VK_CHECK(vkCreateShaderModule(R.device, &mi, nullptr, &mod));
    VkDescriptorSetLayoutBinding b[2]{};
    for (uint32_t i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 2;
    dl.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dl, nullptr, &g_bc.dsl));
    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g_bc.dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pr;
    VK_CHECK(vkCreatePipelineLayout(R.device, &pl, nullptr, &g_bc.layout));
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = mod;
    ci.stage.pName = "main";
    ci.layout = g_bc.layout;
    VkResult r = vkCreateComputePipelines(R.device, R.pipelineCache, 1, &ci, nullptr, &g_bc.pipe);
    vkDestroyShaderModule(R.device, mod, nullptr);
    if (r != VK_SUCCESS) g_bc.pipe = VK_NULL_HANDLE;
    LOG("[vk] BC textures: %s", g_bc.pipe ? "decoded on the GPU" : "decoded on the CPU (pipeline failed)");
    return g_bc.pipe;
}

static void bc_verify();
// the decoder for upload_surface(); runs the debug comparison on first use
static VkPipeline bc_decoder_checked() {
    static bool verified = false;
    VkPipeline p = bc_decoder();
    if (p && !verified) {
        verified = true;
        if (getenv("WWHD_BC_VERIFY")) bc_verify();
    }
    return p;
}

// records the decode of `blocks` (detiled BC blocks) into a fresh staging range; returns that range
static Upload bc_decode_gpu(const FormatInfo& f, const std::vector<uint8_t>& blocks, uint32_t bw, uint32_t bh, uint32_t slices) {
    const VkDeviceSize align = std::max<VkDeviceSize>(R.props.limits.minStorageBufferOffsetAlignment, 16);
    Upload in = upload(blocks.data(), blocks.size(), align);
    const VkDeviceSize outSize = (VkDeviceSize)bw * 4 * f.hostBytesPerBlock * bh * 4 * slices;
    Upload out = upload_alloc(outSize, align);
    VkDescriptorSet set = alloc_descriptor_set(g_bc.dsl);
    VkDescriptorBufferInfo bi[2] = {{in.buf, in.offset, blocks.size()}, {out.buf, out.offset, outSize}};
    VkWriteDescriptorSet w[2]{};
    for (int i = 0; i < 2; i++) {
        w[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w[i].dstSet = set;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo = &bi[i];
    }
    vkUpdateDescriptorSets(R.device, 2, w, 0, nullptr);
    VkCommandBuffer cmd = command_buffer();
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc.pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g_bc.layout, 0, 1, &set, 0, nullptr);
    const uint32_t n = bw * bh * slices;
    const uint32_t pc[4] = {(uint32_t)f.convert - (uint32_t)Convert::BC1, bw, bh, n};
    vkCmdPushConstants(cmd, g_bc.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    const uint32_t groups = (n + 63) / 64, gx = std::min(groups, 65535u);
    vkCmdDispatch(cmd, gx, (groups + gx - 1) / gx, 1);
    return out;
}

// debug: WWHD_BC_VERIFY=1 decodes random blocks of every BC format on both the GPU and the CPU
// and logs whether they match byte for byte
static void bc_verify() {
    uint32_t rng = 12345;
    auto next = [&] { rng = rng * 1664525u + 1013904223u; return (uint8_t)(rng >> 24); };
    const Convert modes[] = {Convert::BC1, Convert::BC2, Convert::BC3, Convert::BC4U, Convert::BC4S, Convert::BC5U, Convert::BC5S};
    const uint32_t bw = 9, bh = 5, slices = 2;
    int failed = 0;
    for (Convert c : modes) {
        FormatInfo f;
        f.convert = c;
        f.bytesPerBlock = (c == Convert::BC1 || c == Convert::BC4U || c == Convert::BC4S) ? 8 : 16;
        f.hostBytesPerBlock = c <= Convert::BC3 ? 4 : (c <= Convert::BC4S ? 1 : 2);
        std::vector<uint8_t> blocks((size_t)bw * bh * slices * f.bytesPerBlock);
        for (size_t i = 0; i < blocks.size(); i++) blocks[i] = next();
        // edge cases: equal endpoints, and -128 endpoints for the signed formats
        memset(&blocks[0], 0x80, 4);
        memcpy(&blocks[f.bytesPerBlock], &blocks[f.bytesPerBlock + 2], 2);
        const uint32_t rowBytes = bw * 4 * f.hostBytesPerBlock, rows = bh * 4;
        std::vector<uint8_t> cpu((size_t)rowBytes * rows * slices);
        for (uint32_t z = 0; z < slices; z++)
            for (uint32_t y = 0; y < bh; y++)
                convert_row(c, &blocks[((size_t)z * bh + y) * bw * f.bytesPerBlock], &cpu[((size_t)z * rows + y * 4) * rowBytes], bw,
                            rowBytes);
        Upload out = bc_decode_gpu(f, blocks, bw, bh, slices);
        VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(command_buffer(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                             nullptr);
        wait_idle();
        size_t bad = 0, first = 0;
        for (size_t i = 0; i < cpu.size(); i++)
            if (cpu[i] != out.ptr[i] && !bad++) first = i;
        if (bad) {
            failed++;
            LOG("[vk] BC verify: mode %d: %zu of %zu bytes differ (first at %zu: GPU %d, CPU %d)", (int)c - (int)Convert::BC1, bad,
                cpu.size(), first, out.ptr[first], cpu[first]);
        }
    }
    LOG("[vk] BC verify: %s", failed ? "MISMATCH" : "GPU and CPU decoders match for all 7 formats");
}

// ---------------------------------------------------------------- upload (detile + convert)
// Decodes one mip level into `out` in host layout: texels, or 4x4 blocks if the host image is
// block compressed (or if `rawBC`: BC blocks left for the GPU decoder). outW/outH are the level's
// texel size.
static void decode_level(Surface* s, uint32_t level, uint32_t base, std::vector<uint8_t>& out, uint32_t& outW, uint32_t& outH,
                         uint32_t& outSlices, bool rawBC) {
    const FormatInfo& f = s->fmt;
    uint32_t w = std::max(s->width >> level, 1u), h = std::max(s->height >> level, 1u);
    uint32_t slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    outW = w;
    outH = h;
    outSlices = slices;
    const bool decodeBC = is_bc_decode(f.convert) && !rawBC;
    // host row pitch and rows per slice
    const uint32_t hostRowBytes = rawBC ? bw * f.bytesPerBlock : decodeBC ? bw * 4 * f.hostBytesPerBlock : bw * f.hostBytesPerBlock;
    const uint32_t hostRows = decodeBC ? bh * 4 : bh;

    // level geometry from the address library
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s->format, s->width, s->height, s->slices,
                                          (Latte::E_DIM)s->dim, Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s->tileMode),
                                          0, level, &info);
    uint32_t pitch = info.pitch, height = info.height;
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    out.assign((size_t)hostRowBytes * hostRows * slices, 0);
    std::vector<uint8_t> row(bw * f.bytesPerBlock);
    const uint8_t* src = mem::ptr(base);
    const bool norm16 = f.convert == Convert::UNORM16_F16 || f.convert == Convert::SNORM16_F16;
    for (uint32_t z = 0; z < slices; z++) {
        LatteAddrLib::CachedSurfaceAddrInfo ci;
        bool macro = Latte::TM_IsMacroTiled(tm);
        if (macro)
            LatteAddrLib::SetupCachedSurfaceAddrInfo(&ci, z, 0, bpp, pitch, height, slices, 1, tm, false, pipeSwizzle, bankSwizzle);
        for (uint32_t y = 0; y < bh; y++) {
            for (uint32_t x = 0; x < bw; x++) {
                uint32_t off;
                if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, z, 0, bpp, pitch, height, slices);
                else if (!macro)
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, z, bpp, pitch, height, tm, false);
                else
                    off = LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, &ci);
                memcpy(&row[x * f.bytesPerBlock], src + off, f.bytesPerBlock);
            }
            if (decodeBC) {
                uint8_t* dst = &out[((size_t)z * hostRows + y * 4) * hostRowBytes];
                convert_row(f.convert, row.data(), dst, bw, hostRowBytes);
            } else {
                uint8_t* dst = &out[((size_t)z * hostRows + y) * hostRowBytes];
                if (f.convert == Convert::NONE || rawBC) memcpy(dst, row.data(), row.size());
                else convert_row(f.convert, row.data(), dst, norm16 ? bw * f.bytesPerBlock / 2 : bw);
            }
        }
    }
}

// samples 256 words spread over the base level
static uint64_t sparse_hash(Surface* s) {
    if (!s->dataSize) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    uint32_t step = std::max<uint32_t>((s->dataSize / 256) & ~7u, 8);
    for (uint32_t o = 0; o + 8 <= s->dataSize; o += step) {
        uint64_t v;
        memcpy(&v, mem::ptr(s->addr + o), 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    return h;
}

void upload_surface(Surface* s) {
    if (!s->img.image || s->gpuWritten) return;
    const FormatInfo& f = s->fmt;
    // cheap change detection on the base level
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s->format, s->width, s->height, s->slices, (Latte::E_DIM)s->dim,
                                          Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s->tileMode), 0, 0, &info);
    if (s->dataSize != (uint32_t)info.surfSize) {
        s->dataSize = (uint32_t)info.surfSize;
        surfaces_changed();  // invalidate's index (vk_device.cpp)
    }
    uint64_t hash = content_hash(mem::ptr(s->addr), (size_t)info.surfSize);
    if (hash == s->contentHash) return;
    s->contentHash = hash;
    s->writeSeq = next_write_seq();  // fresh CPU data is now the newest version of this memory
    g_stat_uploads++;

    prepare(s->img, Use::COPY_DST);
    std::vector<uint8_t> data;
    std::vector<VkBufferImageCopy> regions;
    const bool is3D = s->img.type == VK_IMAGE_TYPE_3D;
    // BC without device support: decode all levels with one dispatch each, then copy them after one barrier
    const bool gpuBC = is_bc_decode(f.convert) && bc_decoder_checked();
    std::vector<std::pair<Upload, VkBufferImageCopy>> decoded;
    for (uint32_t level = 0; level < s->img.mips; level++) {
        uint32_t base;
        if (level == 0) base = s->addr;
        else if (!s->mipAddr) break;
        else if (level == 1) base = s->mipAddr;
        else {
            // mip offsets relative to the mip chain start
            uint32_t sliceOffset = 0, sliceSize = 0;
            sint32 sub = 0;
            LatteAddrLib::CalculateMipAndSliceAddr(s->addr, s->mipAddr, (Latte::E_GX2SURFFMT)s->format, s->width, s->height,
                                                   s->slices, (Latte::E_DIM)s->dim, (Latte::E_HWTILEMODE)s->tileMode,
                                                   s->swizzle, 0, level, 0, &sliceOffset, &sliceSize, &sub);
            base = sliceOffset;
        }
        uint32_t w, h, slices;
        decode_level(s, level, base, data, w, h, slices, gpuBC);
        Upload u = gpuBC ? bc_decode_gpu(f, data, (w + 3) / 4, (h + 3) / 4, slices) : upload(data.data(), data.size(), 16);
        uint32_t layers = is3D ? 1 : std::min(slices, s->img.layers);
        uint32_t lw = std::max(s->img.width >> level, 1u), lh = std::max(s->img.height >> level, 1u);
        VkBufferImageCopy c{};
        c.bufferOffset = u.offset;
        // rows in the staging data: whole blocks (compressed), texels padded to whole blocks (decoded BC)
        if (f.compressed) {
            c.bufferRowLength = ((w + 3) / 4) * 4;
            c.bufferImageHeight = ((h + 3) / 4) * 4;
        }
        c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers};
        c.imageExtent = {lw, lh, is3D ? slices : 1};
        if (gpuBC) {
            decoded.emplace_back(u, c);
            continue;
        }
        regions.clear();
        regions.push_back(c);
        vkCmdCopyBufferToImage(command_buffer(), u.buf, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions.data());
    }
    if (decoded.empty()) return;
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(command_buffer(), VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, nullptr,
                         0, nullptr);
    for (auto& [u, c] : decoded)
        vkCmdCopyBufferToImage(command_buffer(), u.buf, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

// ---------------------------------------------------------------- write-back to guest memory
// Render results stay on the GPU, except where the game reads them with the CPU: linear-aligned
// render targets and GX2CopySurface destinations with a linear tile mode (as Cemu reads back linear
// surfaces). The Picto Box draws its picture into a linear target and JPEG-encodes it from guest
// memory (issue #22). Written at the guest size; only formats whose host texels are the guest's bytes.
struct LinearCopy {
    Surface* s;
    LatteAddrLib::AddrSurfaceInfo_OUT info;
    Latte::E_HWTILEMODE tm;
    uint32_t base, layer, slice, w, h;
};
static std::vector<LinearCopy> g_linear_copies;  // GPU copies into linear destinations since the last GX2DrawDone

static bool can_write_back(const Surface* s) {
    const FormatInfo& f = s->fmt;
    bool ok = !f.compressed && !f.depth && f.convert == Convert::NONE && f.hostBytesPerBlock == f.bytesPerBlock &&
              s->img.type == VK_IMAGE_TYPE_2D && s->img.image;
    if (!ok) {
        static int logged = 0;
        if (logged++ < 4) LOG("[gfx] linear surface %08X (format %X) is not written back to guest memory", s->addr, s->format);
    }
    return ok;
}

// one layer of s at the guest size w x h, read from the GPU (waits for it)
static bool read_guest_texels(Surface* s, uint32_t layer, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    const uint32_t bytes = s->fmt.bytesPerBlock;
    Image temp;
    Image* src = &s->img;
    uint32_t srcLayer = layer;
    if (s->img.width != w || s->img.height != h) {  // drawn at the internal resolution: filter it down first
        if (!create_image(temp, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, s->img.format, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false))
            return false;
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(R.pd, s->img.format, &fp);
        const bool linear = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) != 0;
        prepare(s->img, Use::COPY_SRC);
        prepare(temp, Use::COPY_DST);
        VkImageBlit b{};
        b.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer, 1};
        b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        b.srcOffsets[1] = {(int32_t)s->img.width, (int32_t)s->img.height, 1};
        b.dstOffsets[1] = {(int32_t)w, (int32_t)h, 1};
        vkCmdBlitImage(command_buffer(), s->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, temp.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &b, linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        src = &temp;
        srcLayer = 0;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = (VkDeviceSize)w * h * bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VkBuffer buf;
    VmaAllocation alloc;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        if (src == &temp) retire_image(temp);
        return false;
    }
    prepare(*src, Use::COPY_SRC);
    VkBufferImageCopy c{};
    c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, srcLayer, 1};
    c.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(command_buffer(), src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c);
    VkBufferMemoryBarrier mb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    mb.srcQueueFamilyIndex = mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.buffer = buf;
    mb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(command_buffer(), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1, &mb, 0, nullptr);
    wait_idle();
    vmaInvalidateAllocation(R.vma, alloc, 0, VK_WHOLE_SIZE);
    const uint8_t* raw = (const uint8_t*)info.pMappedData;
    out.assign(raw, raw + (size_t)w * h * bytes);
    vmaDestroyBuffer(R.vma, buf, alloc);
    if (src == &temp) retire_image(temp);
    return true;
}

// textures made from this memory see the new contents
static void guest_memory_written(uint32_t addr) {
    auto r = R.surfaces.equal_range(addr);
    for (auto it = r.first; it != r.second; ++it) it->second->lastCheckedFrame = ~0ull;
}

void write_back_linear() {
    std::vector<uint8_t> texels;
    for (Surface* s : g_linear_targets) {
        if (!s->gpuWritten || s->writtenBackSeq == s->writeSeq) continue;
        s->writtenBackSeq = s->writeSeq;
        if (s->img.layers != 1 || !can_write_back(s)) continue;
        const uint32_t bytes = s->fmt.bytesPerBlock, pitch = std::max(s->pitch, s->width);
        if (!read_guest_texels(s, 0, s->width, s->height, texels)) continue;
        for (uint32_t y = 0; y < s->height; y++)
            memcpy(mem::ptr(s->addr + y * pitch * bytes), texels.data() + (size_t)y * s->width * bytes, (size_t)s->width * bytes);
        guest_memory_written(s->addr);
        memw::mark(s->addr, s->height * pitch * bytes);  // (kept vertex copies, vk_draw.cpp)
        static int logged = 0;
        if (logged++ < 8) LOG("[gfx] linear target %08X %ux%u (format %X) written to guest memory", s->addr, s->width, s->height, s->format);
    }
    auto copies = std::move(g_linear_copies);
    g_linear_copies.clear();
    for (const LinearCopy& lc : copies) {
        if (!can_write_back(lc.s) || lc.layer >= lc.s->img.layers) continue;
        if (!read_guest_texels(lc.s, lc.layer, lc.w, lc.h, texels)) continue;
        const uint32_t bytes = lc.s->fmt.bytesPerBlock;
        for (uint32_t y = 0; y < lc.h; y++)
            for (uint32_t x = 0; x < lc.w; x++) {
                uint32_t o = LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, lc.slice, 0, bytes * 8, lc.info.pitch, lc.info.height,
                                                                            lc.info.depth);
                memcpy(mem::ptr(lc.base + o), texels.data() + ((size_t)y * lc.w + x) * bytes, bytes);
            }
        guest_memory_written(lc.base);
        memw::mark(lc.base, (uint32_t)std::min<uint64_t>(lc.info.surfSize, 0xFFFFFFFFu));
        static int logged = 0;
        if (logged++ < 8) LOG("[gfx] linear copy %08X %ux%u (format %X) written to guest memory", lc.base, lc.w, lc.h, lc.s->format);
    }
}

// ---------------------------------------------------------------- GX2CopySurface
static uint32_t level_address(GX2Surface* s, uint32_t level) {
    if (level == 0) return s->imagePtr;
    if (level == 1) return s->mipPtr;
    return s->mipPtr + s->mipOffset[level - 1];
}

static uint32_t element_offset(const LatteAddrLib::AddrSurfaceInfo_OUT& info, Latte::E_HWTILEMODE tm, uint32_t x, uint32_t y,
                               uint32_t slice, uint32_t bpp, uint32_t swizzle, LatteAddrLib::CachedSurfaceAddrInfo* ci) {
    if (tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED)
        return LatteAddrLib::ComputeSurfaceAddrFromCoordLinear(x, y, slice, 0, bpp, info.pitch, info.height, info.depth);
    if (!Latte::TM_IsMacroTiled(tm))
        return LatteAddrLib::ComputeSurfaceAddrFromCoordMicroTiled(x, y, slice, bpp, info.pitch, info.height, tm, false);
    return LatteAddrLib::ComputeSurfaceAddrFromCoordMacroTiledCached(x, y, ci);
}

void copy_surface_impl(uint32_t srcAddr, uint32_t srcMip, uint32_t srcSlice, uint32_t dstAddr, uint32_t dstMip, uint32_t dstSlice) {
    auto* s = (GX2Surface*)mem::ptr(srcAddr);
    auto* d = (GX2Surface*)mem::ptr(dstAddr);
    uint32_t sbase = level_address(s, srcMip), dbase = level_address(d, dstMip);
    uint32_t w = std::max<uint32_t>(s->width >> srcMip, 1), h = std::max<uint32_t>(s->height >> srcMip, 1);
    FormatInfo f = format_info((uint32_t)s->format.value(), false);

    // GPU-produced source: copy image to image. Of its format views the one in the source's own format
    // (a newer view in another format is copied into it first), else the most recent.
    Surface* src = nullptr;
    {
        const uint32_t srcFormat = (uint32_t)s->format.value();
        auto range = R.surfaces.equal_range(sbase);
        for (auto it = range.first; it != range.second; ++it) {
            Surface* o = it->second.get();
            if (!o->gpuWritten || o->width != w || o->height != h) continue;
            if (!src || std::make_pair(o->format == srcFormat, o->writeSeq) > std::make_pair(src->format == srcFormat, src->writeSeq))
                src = o;
        }
        src = adopt_newer_alias(src);
    }
    if (src) {
        SurfaceDesc dd;
        dd.addr = dbase;
        dd.width = std::max<uint32_t>(d->width >> dstMip, 1);
        dd.height = std::max<uint32_t>(d->height >> dstMip, 1);
        dd.pitch = d->pitch;
        dd.format = (uint32_t)d->format.value();
        dd.tileMode = (uint32_t)d->tileMode.value();
        // As the original project's renderer: the destination keeps the source's kind (a depth copy
        // goes into a depth surface, e.g. a shadow cascade, instead of a colour one whose format never
        // matched and dropped the copy) and its own array shape, and the copy honours the slices
        // (every cascade landed in slice 0 before).
        static const bool oldCopy = getenv("WWHD_OLD_SURFACE_COPY") != nullptr;  // debug: as before
        if (!oldCopy) {
            dd.swizzle = (uint32_t)d->swizzle;
            dd.isDepth = src->isDepth;
            dd.dim = (uint32_t)d->dim.value();
            dd.slices = (dd.dim == (uint32_t)Latte::E_DIM::DIM_2D || dd.dim == (uint32_t)Latte::E_DIM::DIM_1D)
                            ? 1 : std::max<uint32_t>((uint32_t)d->depth, 1);
        } else {
            srcSlice = dstSlice = 0;
        }
        Surface* dst = find_or_create_surface(dd, true);
        if (!dst || dst->img.format != src->img.format) return;
        if (srcSlice >= src->img.layers || dstSlice >= dst->img.layers) return;
        if (dst == src) return;  // same image: only a slice-to-itself copy reaches here in practice
        prepare(src->img, Use::COPY_SRC);
        prepare(dst->img, Use::COPY_DST);
        if (src->img.width == dst->img.width && src->img.height == dst->img.height) {
            VkImageCopy c{};
            c.srcSubresource = {src->img.aspect, 0, srcSlice, 1};
            c.dstSubresource = {dst->img.aspect, 0, dstSlice, 1};
            c.extent = {src->img.width, src->img.height, 1};
            vkCmdCopyImage(command_buffer(), src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        } else {  // different resolution scales: scale while copying
            // Adreno reports no BLIT_SRC/DST for D32_SFLOAT and D16_UNORM: a blit there is invalid
            // (original project ac81cff/81e57e0 draws such copies instead). The copy is left out; the
            // game clears depth before it renders into it again.
            static std::unordered_map<VkFormat, bool> blittable;
            auto bl = blittable.find(src->img.format);
            if (bl == blittable.end()) {
                VkFormatProperties fp;
                vkGetPhysicalDeviceFormatProperties(R.pd, src->img.format, &fp);
                const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
                bl = blittable.emplace(src->img.format, (fp.optimalTilingFeatures & need) == need).first;
                if (!bl->second) LOG("[vk] format %d cannot be blitted: scaled surface copies of it are skipped", (int)src->img.format);
            }
            if (!bl->second) return;
            VkImageBlit b{};
            b.srcSubresource = {src->img.aspect, 0, srcSlice, 1};
            b.dstSubresource = {dst->img.aspect, 0, dstSlice, 1};
            b.srcOffsets[1] = {(int32_t)src->img.width, (int32_t)src->img.height, 1};
            b.dstOffsets[1] = {(int32_t)dst->img.width, (int32_t)dst->img.height, 1};
            vkCmdBlitImage(command_buffer(), src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, src->isDepth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        mark_gpu_written(dst);
        // a linear destination is read by the CPU (the Picto Box picture): written back at GX2DrawDone
        if (!dst->isDepth) {
            LinearCopy lc{};
            LatteAddrLib::GX2CalculateSurfaceInfo(d->format, d->width, d->height, d->depth, d->dim, d->tileMode, d->aa, dstMip, &lc.info);
            lc.tm = (Latte::E_HWTILEMODE)lc.info.hwTileMode;
            if ((lc.tm == Latte::E_HWTILEMODE::TM_LINEAR_GENERAL || lc.tm == Latte::E_HWTILEMODE::TM_LINEAR_ALIGNED) &&
                dstSlice < std::max<uint32_t>(lc.info.depth, 1)) {
                lc.s = dst;
                lc.base = dbase;
                lc.layer = dstSlice;
                lc.slice = dstSlice;
                lc.w = std::min(dd.width, w);
                lc.h = std::min(dd.height, h);
                g_linear_copies.push_back(lc);
                write_back_linear();  // the game reads it as soon as GX2CopySurface returns
            }
        }
        return;
    }

    // CPU-produced source: re-tile in guest memory; texture uploads pick up the change
    LatteAddrLib::AddrSurfaceInfo_OUT si{}, di{};
    LatteAddrLib::GX2CalculateSurfaceInfo(s->format, s->width, s->height, s->depth, s->dim, s->tileMode, s->aa, srcMip, &si);
    LatteAddrLib::GX2CalculateSurfaceInfo(d->format, d->width, d->height, d->depth, d->dim, d->tileMode, d->aa, dstMip, &di);
    auto stm = (Latte::E_HWTILEMODE)si.hwTileMode, dtm = (Latte::E_HWTILEMODE)di.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    uint32_t sswz = s->swizzle, dswz = d->swizzle;
    LatteAddrLib::CachedSurfaceAddrInfo sci, dci;
    if (Latte::TM_IsMacroTiled(stm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&sci, srcSlice, 0, bpp, si.pitch, si.height, si.depth, 1, stm, false, (sswz >> 8) & 1, (sswz >> 9) & 3);
    if (Latte::TM_IsMacroTiled(dtm))
        LatteAddrLib::SetupCachedSurfaceAddrInfo(&dci, dstSlice, 0, bpp, di.pitch, di.height, di.depth, 1, dtm, false, (dswz >> 8) & 1, (dswz >> 9) & 3);
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint32_t so = element_offset(si, stm, x, y, srcSlice, bpp, sswz, &sci);
            uint32_t dofs = element_offset(di, dtm, x, y, dstSlice, bpp, dswz, &dci);
            memcpy(mem::ptr(dbase + dofs), mem::ptr(sbase + so), f.bytesPerBlock);
        }
    memw::mark(dbase, (uint32_t)std::min<uint64_t>(di.surfSize, 0xFFFFFFFFu));  // (kept vertex copies, vk_draw.cpp)
    // force re-upload of any texture made from the destination
    auto dr = R.surfaces.equal_range(dbase);
    for (auto it = dr.first; it != dr.second; ++it) it->second->lastCheckedFrame = ~0ull;
}

// a save state was loaded: every surface may differ from guest memory now
void ss_reset_surfaces() {
    for (auto& [a, s] : R.surfaces) {
        s->dirty = true;
        s->lastCheckedFrame = ~0ull;
    }
}

}  // namespace gfx
