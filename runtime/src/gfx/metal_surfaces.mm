#include <tuple>
// Guest surfaces <-> Metal textures: render targets, depth buffers, sampled textures.
// Tiled layouts are decoded with the vendored LatteAddrLib.
#include "Cafe/HW/Latte/ISA/LatteReg.h"
#include "Cafe/HW/Latte/ISA/RegDefines.h"
#include "Cafe/HW/Latte/LatteAddrLib/LatteAddrLib.h"
#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "metal.h"
#include "runtime.h"

Latte::E_GX2SURFFMT LatteTexture_ReconstructGX2Format(const Latte::LATTE_SQ_TEX_RESOURCE_WORD1_N&, const Latte::LATTE_SQ_TEX_RESOURCE_WORD4_N&);

namespace gfx {

static uint64_t fnv(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    // sample the data sparsely for large surfaces; enough to notice CPU updates
    size_t step = n > (1 << 16) ? 61 : 1;
    for (size_t i = 0; i < n; i += step) h = (h ^ p[i]) * 1099511628211ull;
    return h ^ n;
}

static MTLTextureType texture_type(uint32_t dim, uint32_t slices) {
    switch ((Latte::E_DIM)dim) {
    case Latte::E_DIM::DIM_1D: return MTLTextureType1D;
    case Latte::E_DIM::DIM_3D: return MTLTextureType3D;
    case Latte::E_DIM::DIM_CUBEMAP: return MTLTextureTypeCube;
    case Latte::E_DIM::DIM_1D_ARRAY: return MTLTextureType1DArray;
    case Latte::E_DIM::DIM_2D_ARRAY: case Latte::E_DIM::DIM_2D_ARRAY_MSAA: return MTLTextureType2DArray;
    default: return slices > 1 ? MTLTextureType2DArray : MTLTextureType2D;
    }
}

uint64_t next_write_seq() {
    static uint64_t seq = 0;
    return ++seq;
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
    MTLTextureType type = texture_type(d.dim, s->slices);
    if (forRendering && type != MTLTextureType2DArray) type = MTLTextureType2D;
    MTLTextureDescriptor* td = [MTLTextureDescriptor new];
    td.textureType = type;
    td.pixelFormat = s->fmt.pixel;
    td.width = s->width;
    td.height = (type == MTLTextureType1D || type == MTLTextureType1DArray) ? 1 : s->height;
    td.depth = type == MTLTextureType3D ? s->slices : 1;
    td.arrayLength = (type == MTLTextureType2DArray || type == MTLTextureType1DArray) ? s->slices : 1;
    if (type == MTLTextureTypeCube) td.arrayLength = 1;
    td.mipmapLevelCount = s->mips;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView;
    td.storageMode = MTLStorageModePrivate;
    if (s->fmt.compressed) td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    s->tex = [R.device newTextureWithDescriptor:td];
    if (!s->tex) {
        LOG("[gfx] cannot create %ux%ux%u texture (format %X, pixel %lu)", s->width, s->height, s->slices, s->format,
            (unsigned long)s->fmt.pixel);
        return nullptr;
    }
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
    return find_or_create_surface(d, true);
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
    return find_or_create_surface(d, true);
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

Surface* sampled_texture(const uint32_t* w, bool isDepthSampler) {
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
static void decode_level(Surface* s, uint32_t level, uint32_t base, std::vector<uint8_t>& out, uint32_t& outW,
                         uint32_t& outH, uint32_t& outSlices) {
    const FormatInfo& f = s->fmt;
    uint32_t w = std::max(s->width >> level, 1u), h = std::max(s->height >> level, 1u);
    uint32_t slices = s->dim == (uint32_t)Latte::E_DIM::DIM_3D ? std::max(s->slices >> level, 1u) : s->slices;
    uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
    outW = w;
    outH = h;
    outSlices = slices;

    // level geometry from the address library
    LatteAddrLib::AddrSurfaceInfo_OUT info{};
    LatteAddrLib::GX2CalculateSurfaceInfo((Latte::E_GX2SURFFMT)s->format, s->width, s->height, s->slices,
                                          (Latte::E_DIM)s->dim, Latte::MakeGX2TileMode((Latte::E_HWTILEMODE)s->tileMode),
                                          0, level, &info);
    uint32_t pitch = info.pitch, height = info.height;
    auto tm = (Latte::E_HWTILEMODE)info.hwTileMode;
    uint32_t bpp = f.bytesPerBlock * 8;
    uint32_t pipeSwizzle = (s->swizzle >> 8) & 1, bankSwizzle = (s->swizzle >> 9) & 3;
    // small mips of macro-tiled surfaces drop the swizzle
    out.assign((size_t)bw * bh * slices * f.hostBytesPerBlock, 0);
    std::vector<uint8_t> row(bw * f.bytesPerBlock);
    const uint8_t* src = mem::ptr(base);
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
            uint8_t* dst = &out[((size_t)z * bh + y) * bw * f.hostBytesPerBlock];
            if (f.convert == Convert::NONE) memcpy(dst, row.data(), row.size());
            else convert_row(f.convert, row.data(), dst, bw);
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
    if (!s->tex || s->gpuWritten) return;
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

    std::vector<uint8_t> data;
    id<MTLBuffer> staging = nil;
    end_encoder();
    id<MTLBlitCommandEncoder> blit = [command_buffer() blitCommandEncoder];
    for (uint32_t level = 0; level < s->mips; level++) {
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
        uint32_t bw = f.compressed ? (w + 3) / 4 : w, bh = f.compressed ? (h + 3) / 4 : h;
        staging = [R.device newBufferWithBytes:data.data() length:data.size() options:MTLResourceStorageModeShared];
        bool is3D = s->tex.textureType == MTLTextureType3D;
        uint32_t layers = is3D ? 1 : (s->tex.textureType == MTLTextureTypeCube ? 6 : (uint32_t)s->tex.arrayLength);
        uint32_t perSlice = bw * bh * f.hostBytesPerBlock;
        for (uint32_t z = 0; z < (is3D ? 1 : std::min(slices, layers)); z++) {
            [blit copyFromBuffer:staging
                     sourceOffset:(NSUInteger)z * perSlice
                sourceBytesPerRow:bw * f.hostBytesPerBlock
              sourceBytesPerImage:perSlice
                       sourceSize:MTLSizeMake(f.compressed ? bw * 4 : w, f.compressed ? bh * 4 : h, is3D ? slices : 1)
                        toTexture:s->tex
                 destinationSlice:z
                 destinationLevel:level
                destinationOrigin:MTLOriginMake(0, 0, 0)];
        }
    }
    [blit endEncoding];
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

    // GPU-produced source: copy texture to texture
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
        if (!dst || dst->fmt.pixel != src->fmt.pixel) return;
        end_encoder();
        id<MTLBlitCommandEncoder> b = [command_buffer() blitCommandEncoder];
        [b copyFromTexture:src->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                sourceSize:MTLSizeMake(std::min(w, dst->width), std::min(h, dst->height), 1)
                 toTexture:dst->tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [b endEncoding];
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

}  // namespace gfx
