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
#include "runtime.h"
#include "vk.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);

namespace gfx {

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    // sample the data sparsely for large surfaces; enough to notice CPU updates
    size_t step = n > (1 << 16) ? 61 : 1;
    for (size_t i = 0; i < n; i += step) h = (h ^ p[i]) * 1099511628211ull;
    return h ^ n;
}

static uint64_t g_write_seq = 0;
uint64_t next_write_seq() { return ++g_write_seq; }

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

float resolution_scale() { return g_res; }

void set_resolution_scale(float v) { g_res_requested = clamp_scale(v); }

void latch_resolution_scale() {
    float v = g_res_requested.exchange(0);
    if (v <= 0 || v == g_res) return;
    g_res = v;
    LOG("[gfx] resolution scale %.2fx (3D view %.0fx%.0f)", v, 1280 * v, 720 * v);
}

static bool screen_shaped(const Surface* s) {
    if (s->slices != 1 || s->width < 16 || s->height < 9) return false;
    float aspect = (float)s->width / (float)s->height;
    return aspect > 1.70f && aspect < 1.84f;
}

Surface* rescaled(Surface* s) {
    if (!s || !s->img.image || s->img.type != VK_IMAGE_TYPE_2D || s->mips > 1 || !screen_shaped(s)) return s;
    float want = resolution_scale();
    if (std::fabs(s->rscale - want) < 1e-3f) return s;
    Image old = s->img;
    float oldScale = s->rscale;
    s->img = Image{};
    if (!create_surface_image(s, true)) {
        s->img = old;
        s->rscale = oldScale;
        return s;
    }
    // keep the contents: a filtered copy where the format allows, else a nearest one; none if the
    // GPU can't blit the format (depth on some GPUs: the game clears depth every frame anyway)
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(R.pd, s->img.format, &fp);
    VkFormatFeatureFlags f = fp.optimalTilingFeatures;
    if ((f & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (f & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
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
    static int logged = 0;
    if (getenv("WWHD_LOG_RESCALE") || logged++ < 3)
        LOG("[gfx] rescaled %08X %ux%u to %ux%u", s->addr, s->width, s->height, s->img.width, s->img.height);
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
    if (forRendering && type == VK_IMAGE_TYPE_2D && resolution_scale() != 1.0f && screen_shaped(s)) {
        s->rscale = resolution_scale();
        w = std::max<uint32_t>(1, (uint32_t)lroundf(s->width * s->rscale));
        h = std::max<uint32_t>(1, (uint32_t)lroundf(s->height * s->rscale));
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
        if (s->width == d.width && s->height == d.height && s->format == d.format && s->slices == d.slices &&
            (forRendering || s->mips >= d.mips || s->gpuWritten)) {
            if (forRendering) return s;
            if (!exact || s->writeSeq > exact->writeSeq) exact = s;
            continue;
        }
        // render target being sampled with a compatible format but different view parameters
        if (!forRendering && s->gpuWritten && (s->format & 0x3F) == (d.format & 0x3F)) consider(s);
    }
    if (exact && (exact->gpuWritten || !rendered || exact->writeSeq > rendered->writeSeq)) return exact;
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
    R.surfaces.emplace(d.addr, std::move(s));
    return raw;
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
    return rescaled(find_or_create_surface(d, true));
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
    uint32_t slices = cb->surface.dim.value() == Latte::E_DIM::DIM_2D_ARRAY ? std::max<uint32_t>(cb->surface.depth, 1) : 1;
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

// The surface a set of texture words resolves to only changes when a surface is created or written
// (the choice among surfaces aliasing one address depends on that): remember recent lookups.
static Surface* check_texture(Surface* s);
struct TexLookup {
    uint32_t w[7];
    uint64_t writeSeq;
    size_t surfaces;
    Surface* s;
    bool used;
};
static TexLookup g_tex_lookups[512];

Surface* sampled_texture(const uint32_t* w, bool isDepthSampler) {
    uint64_t key = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 7; i++) key = (key ^ w[i]) * 0xFF51AFD7ED558CCDull;
    TexLookup& tl = g_tex_lookups[(key ^ (key >> 29)) & 511];
    if (tl.used && tl.writeSeq == g_write_seq && tl.surfaces == R.surfaces.size() && memcmp(tl.w, w, sizeof tl.w) == 0)
        return check_texture(tl.s);
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
    d.isDepth = false;
    Surface* s = find_or_create_surface(d, false);
    s = check_texture(s);
    memcpy(tl.w, w, sizeof tl.w);
    tl.writeSeq = g_write_seq;  // after the check: an upload counts as a write
    tl.surfaces = R.surfaces.size();
    tl.s = s;
    tl.used = true;
    return s;
}

// a sampled CPU texture: upload it if its memory changed (checked once per frame)
static Surface* check_texture(Surface* s) {
    if (s && !s->gpuWritten && s->lastCheckedFrame != R.frame) {
        s->lastCheckedFrame = R.frame;
        // full hash only when new, invalidated, every 64 frames, or when a sparse sample changed
        bool full = s->dirty || !s->dataSize || ((R.frame + (s->addr >> 12)) & 63) == 0;
        if (!full) {
            uint64_t h = sparse_hash(s);
            if (h != s->sparseHash) full = true;
        }
        if (full) {
            g_stat_full_checks++;
            upload_surface(s);
            s->sparseHash = sparse_hash(s);
            s->dirty = false;
        }
    }
    return s;
}

// ---------------------------------------------------------------- upload (detile + convert)
// Decodes one mip level into `out` in host layout: texels, or 4x4 blocks if the host image is
// block compressed. outW/outH are the level's texel size.
static void decode_level(Surface* s, uint32_t level, uint32_t base, std::vector<uint8_t>& out, uint32_t& outW, uint32_t& outH,
                         uint32_t& outSlices) {
    const FormatInfo& f = s->fmt;
    uint32_t w = std::max(s->width >> level, 1u), h = std::max(s->height >> level, 1u);
    uint32_t slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    outW = w;
    outH = h;
    outSlices = slices;
    const bool decodeBC = is_bc_decode(f.convert);
    // host row pitch and rows per slice
    const uint32_t hostRowBytes = decodeBC ? bw * 4 * f.hostBytesPerBlock : bw * f.hostBytesPerBlock;
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
                if (f.convert == Convert::NONE) memcpy(dst, row.data(), row.size());
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
    s->dataSize = (uint32_t)info.surfSize;
    uint64_t hash = fnv(mem::ptr(s->addr), (size_t)info.surfSize);
    if (hash == s->contentHash) return;
    s->contentHash = hash;
    s->writeSeq = next_write_seq();  // fresh CPU data is now the newest version of this memory
    g_stat_uploads++;

    prepare(s->img, Use::COPY_DST);
    std::vector<uint8_t> data;
    std::vector<VkBufferImageCopy> regions;
    const bool is3D = s->img.type == VK_IMAGE_TYPE_3D;
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
        decode_level(s, level, base, data, w, h, slices);
        Upload u = upload(data.data(), data.size(), 16);
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
        regions.clear();
        regions.push_back(c);
        vkCmdCopyBufferToImage(command_buffer(), u.buf, s->img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, regions.data());
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

    // GPU-produced source: copy image to image
    auto range = R.surfaces.equal_range(sbase);
    for (auto it = range.first; it != range.second; ++it) {
        Surface* src = it->second.get();
        if (!src->gpuWritten || src->width != w || src->height != h) continue;
        SurfaceDesc dd;
        dd.addr = dbase;
        dd.width = std::max<uint32_t>(d->width >> dstMip, 1);
        dd.height = std::max<uint32_t>(d->height >> dstMip, 1);
        dd.pitch = d->pitch;
        dd.format = (uint32_t)d->format.value();
        dd.tileMode = (uint32_t)d->tileMode.value();
        Surface* dst = find_or_create_surface(dd, true);
        if (!dst || dst->img.format != src->img.format || dst == src) return;
        prepare(src->img, Use::COPY_SRC);
        prepare(dst->img, Use::COPY_DST);
        if (src->img.width == dst->img.width && src->img.height == dst->img.height) {
            VkImageCopy c{};
            c.srcSubresource = {src->img.aspect, 0, 0, 1};
            c.dstSubresource = {dst->img.aspect, 0, 0, 1};
            c.extent = {src->img.width, src->img.height, 1};
            vkCmdCopyImage(command_buffer(), src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
        } else {  // different resolution scales: scale while copying
            VkImageBlit b{};
            b.srcSubresource = {src->img.aspect, 0, 0, 1};
            b.dstSubresource = {dst->img.aspect, 0, 0, 1};
            b.srcOffsets[1] = {(int32_t)src->img.width, (int32_t)src->img.height, 1};
            b.dstOffsets[1] = {(int32_t)dst->img.width, (int32_t)dst->img.height, 1};
            vkCmdBlitImage(command_buffer(), src->img.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->img.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, src->isDepth ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
        }
        mark_gpu_written(dst);
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
