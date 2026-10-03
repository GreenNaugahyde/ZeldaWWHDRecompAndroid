// Metal renderer: device, window, presentation, clears and copies.
#import <AppKit/AppKit.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <set>

#include "gx2/gx2.h"
#include "gx2_texture_regs.h"
#include "metal.h"
#include "runtime.h"
#include "input.h"

// gameplay mods (runtime/src/mods): HUD drawn into the TV image
namespace mods { void draw_overlay(id<MTLCommandBuffer> cmd, id<MTLTexture> tex); }

namespace gfx {
Renderer R;
bool log_this_frame();
const char* backend_name() { return "Metal"; }

// ---------------------------------------------------------------- windows
}  // namespace gfx

// GamePad screen: the mouse stands in for the touch panel
@interface WWDrcView : NSView
@end
@implementation WWDrcView
- (BOOL)acceptsFirstMouse:(NSEvent*)e { return YES; }
- (void)touch:(NSEvent*)e down:(bool)down {
    NSPoint p = [self convertPoint:e.locationInWindow fromView:nil];
    NSSize sz = self.bounds.size;
    // the image is letterboxed to 16:9 inside the view
    float a = 854.0f / 480.0f, w = sz.width, h = sz.height, x0 = 0, y0 = 0;
    if (w / h > a) { x0 = (w - h * a) / 2; w = h * a; } else { y0 = (h - w / a) / 2; h = w / a; }
    float x = (p.x - x0) / w, y = 1.0f - (p.y - y0) / h;
    input::set_touch(down, std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
}
- (void)mouseDown:(NSEvent*)e { [self touch:e down:true]; }
- (void)mouseDragged:(NSEvent*)e { [self touch:e down:true]; }
- (void)mouseUp:(NSEvent*)e { [self touch:e down:false]; }
@end

namespace gfx {

void install_menu(NSWindow* tv);  // menu.mm

// GamePad window, shown/hidden from the Input menu (the game keeps rendering its image either way)
static NSWindow* g_drc_window = nil;
bool drc_window_available() { return g_drc_window != nil; }
bool drc_window_shown() { return g_drc_window.visible; }
void show_drc_window(bool on) {
    if (!g_drc_window) return;
    if (on) [g_drc_window orderFront:nil];
    else [g_drc_window orderOut:nil];
}


static NSWindow* make_window(Screen& scr, NSString* title, NSView* view, int w, int h, NSPoint origin) {
    NSWindow* win = [[NSWindow alloc] initWithContentRect:NSMakeRect(origin.x, origin.y, w, h)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                                                          NSWindowStyleMaskResizable | NSWindowStyleMaskMiniaturizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    [win setTitle:title];
    if (view) [win setContentView:view];
    view = [win contentView];
    [view setWantsLayer:YES];
    scr.layer = [CAMetalLayer layer];
    scr.layer.device = R.device;
    scr.layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    scr.layer.framebufferOnly = YES;
    scr.layer.contentsScale = [win backingScaleFactor];
    scr.layer.drawableSize = CGSizeMake(w * scr.layer.contentsScale, h * scr.layer.contentsScale);
    scr.layer.maximumDrawableCount = 3;
    [view setLayer:scr.layer];
    Screen* sp = &scr;
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeOcclusionStateNotification
                                                      object:win queue:nil usingBlock:^(NSNotification*) {
        sp->visible = (win.occlusionState & NSWindowOcclusionStateVisible) != 0;
    }];
    [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidResizeNotification
                                                      object:win queue:nil usingBlock:^(NSNotification*) {
        NSSize sz = view.bounds.size;
        sp->layer.drawableSize = CGSizeMake(sz.width * sp->layer.contentsScale, sz.height * sp->layer.contentsScale);
    }];
    return win;
}

static void create_window() {
    [NSApplication sharedApplication];
    // a game: no App Nap / timer coalescing, even when the window is in the background
    static id activity = [[NSProcessInfo processInfo]
        beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical | NSActivityIdleDisplaySleepDisabled
                          reason:@"game running"];
    (void)activity;
    // scripted test runs (WWHD_NO_HOST_INPUT) run as a background app: no Dock icon, never takes the
    // keyboard focus from the user's game
    [NSApp setActivationPolicy:getenv("WWHD_NO_HOST_INPUT") ? NSApplicationActivationPolicyAccessory
                                                           : NSApplicationActivationPolicyRegular];
    NSWindow* tv = make_window(R.tv, @"The Legend of Zelda: The Wind Waker HD (recompiled)", nil, 1280, 720, NSMakePoint(0, 0));
    install_menu(tv);
    [tv center];
    if (!getenv("WWHD_NO_GAMEPAD")) {
        // GamePad screen to the right of the TV window
        NSRect f = tv.frame;
        NSWindow* drc = make_window(R.drc, @"GamePad", [[WWDrcView alloc] initWithFrame:NSMakeRect(0, 0, 427, 240)], 427, 240,
                                    NSMakePoint(NSMaxX(f) + 8, NSMinY(f)));
        g_drc_window = drc;
        drc.releasedWhenClosed = NO;  // closing only hides it; the Input menu can bring it back
        if (!input::pro_controller()) [drc orderFront:nil];  // Pro Controller: GamePad window starts hidden
    }
    if (getenv("WWHD_NO_HOST_INPUT")) {
        [tv orderBack:nil];  // scripted test run: stay behind, don't take focus
    } else {
        [tv makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
    }
    input::init();
}

// ---------------------------------------------------------------- presentation shader
static const char* kPresentShader = R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float2 uv; };
vertex VOut present_vs(uint vid [[vertex_id]], constant float4& rect [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);          // fullscreen triangle strip corners
    VOut o;
    o.uv = p * 0.5;                                       // 0..1 (flipped below)
    float2 ndc = rect.xy + p * 0.5 * rect.zw;
    o.pos = float4(ndc.x * 2.0 - 1.0, 1.0 - ndc.y * 2.0, 0.0, 1.0);
    return o;
}
// optional edge smoothing (FXAA, the classic console variant), evaluated on the source picture's
// texel grid while scaling it to the window
static float luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }
static float3 fxaa(texture2d<float> t, sampler s, float2 uv) {
    float2 rcp = 1.0 / float2(t.get_width(), t.get_height());
    float3 nw = t.sample(s, uv + float2(-1, -1) * rcp).rgb, ne = t.sample(s, uv + float2(1, -1) * rcp).rgb;
    float3 sw = t.sample(s, uv + float2(-1, 1) * rcp).rgb, se = t.sample(s, uv + float2(1, 1) * rcp).rgb;
    float3 m = t.sample(s, uv).rgb;
    float lnw = luma(nw), lne = luma(ne), lsw = luma(sw), lse = luma(se), lm = luma(m);
    float lmin = min(lm, min(min(lnw, lne), min(lsw, lse))), lmax = max(lm, max(max(lnw, lne), max(lsw, lse)));
    if (lmax - lmin < max(0.0312, lmax * 0.125)) return m;  // no edge here
    float2 dir = float2(-((lnw + lne) - (lsw + lse)), (lnw + lsw) - (lne + lse));
    float reduce = max((lnw + lne + lsw + lse) * (0.25 / 8.0), 1.0 / 128.0);
    dir = clamp(dir / (min(abs(dir.x), abs(dir.y)) + reduce), -8.0, 8.0) * rcp;
    float3 a = 0.5 * (t.sample(s, uv + dir * (1.0 / 3.0 - 0.5)).rgb + t.sample(s, uv + dir * (2.0 / 3.0 - 0.5)).rgb);
    float3 b = a * 0.5 + 0.25 * (t.sample(s, uv - dir * 0.5).rgb + t.sample(s, uv + dir * 0.5).rgb);
    float lb = luma(b);
    return (lb < lmin || lb > lmax) ? a : b;
}
fragment float4 present_fs(VOut in [[stage_in]], texture2d<float> tex [[texture(0)]], sampler s [[sampler(0)]],
                           constant int& aa [[buffer(0)]]) {
    return float4(aa ? fxaa(tex, s, in.uv) : tex.sample(s, in.uv).rgb, 1.0);
}
)";

// enhancement, toggled in game (Graphics menu or 8; WWHD_FXAA=1 starts with it on)
static std::atomic<bool> g_fxaa{getenv("WWHD_FXAA") != nullptr};
bool fxaa_enabled() { return g_fxaa.load(std::memory_order_relaxed); }
void set_fxaa(bool v) { g_fxaa = v; LOG("[gfx] edge smoothing (FXAA) %s", v ? "on" : "off"); }

static void create_present_pipeline() {
    NSError* err = nil;
    id<MTLLibrary> lib = [R.device newLibraryWithSource:[NSString stringWithUTF8String:kPresentShader] options:nil error:&err];
    if (!lib) fatal("present shader: %s", err.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"present_vs"];
    d.fragmentFunction = [lib newFunctionWithName:@"present_fs"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
    R.presentPipeline = [R.device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!R.presentPipeline) fatal("present pipeline: %s", err.localizedDescription.UTF8String);
    d.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm_sRGB;
    R.presentPipelineSRGB = [R.device newRenderPipelineStateWithDescriptor:d error:&err];
    if (!R.presentPipelineSRGB) fatal("present pipeline: %s", err.localizedDescription.UTF8String);
    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    R.linearClamp = [R.device newSamplerStateWithDescriptor:sd];
}

// ---------------------------------------------------------------- guest memory
static void map_guest_range(uint32_t base, uint32_t size) {
    id<MTLBuffer> b = [R.device newBufferWithBytesNoCopy:mem::ptr(base)
                                                  length:size
                                                 options:MTLResourceStorageModeShared
                                             deallocator:nil];
    if (!b) fatal("cannot map guest memory %08X+%X for the GPU", base, size);
    R.guest.push_back({base, size, b});
}

id<MTLBuffer> guest_buffer(uint32_t addr, uint32_t* offset) {
    for (auto& g : R.guest) {
        if (addr - g.base < g.size) {
            *offset = addr - g.base;
            return g.buf;
        }
    }
    return nil;
}

// ---------------------------------------------------------------- command buffers
id<MTLCommandBuffer> command_buffer() {
    if (!R.cmd) R.cmd = [R.queue commandBuffer];
    return R.cmd;
}

uint32_t g_draws_since_commit = 0;
std::atomic<uint64_t> g_gpu_ns{0};  // GPU busy time, for the periodic report

static void track_gpu_time(id<MTLCommandBuffer> cb) {
    [cb addCompletedHandler:^(id<MTLCommandBuffer> b) {
        double t = b.GPUEndTime - b.GPUStartTime;
        if (t > 0) g_gpu_ns += (uint64_t)(t * 1e9);
    }];
}

void end_encoder() {
    if (R.enc) {
        [R.enc endEncoding];
        R.enc = nil;
        // submit work in chunks so the GPU starts while the frame is still being built (like the
        // hardware command processor), instead of all at once on swap
        static const bool chunked = getenv("WWHD_NO_CHUNK") == nullptr;
        if (chunked && g_draws_since_commit >= 256 && R.cmd) {
            void pool_retire(id<MTLCommandBuffer> cmd);
            pool_retire(R.cmd);
            track_gpu_time(R.cmd);
            [R.cmd commit];
            R.cmd = nil;
            g_draws_since_commit = 0;
        }
    }
    for (auto& c : R.passColor) c = nullptr;
    R.passDepth = nullptr;
}

void pool_retire(id<MTLCommandBuffer> cmd);

void flush() {
    end_encoder();
    if (R.cmd) {
        pool_retire(R.cmd);
        track_gpu_time(R.cmd);
        [R.cmd commit];
        R.cmd = nil;
    }
}

// Metal binds guest memory directly: the GPU must be done before the game reuses it
void draw_done() { wait_idle(); }

void wait_idle() {
    end_encoder();
    if (R.cmd) {
        id<MTLCommandBuffer> c = R.cmd;
        pool_retire(c);
        [c commit];
        R.cmd = nil;
        [c waitUntilCompleted];
    }
}

// ---------------------------------------------------------------- init
void init() {
    R.device = MTLCreateSystemDefaultDevice();
    if (!R.device) fatal("no Metal device");
    R.queue = [R.device newCommandQueue];
    create_window();
    create_present_pipeline();
    // GPU-visible guest memory: MEM2 (code data + heaps), runtime objects, foreground bucket, MEM1
    map_guest_range(0x10000000, 0x40000000);
    map_guest_range(0x60000000, 0x10000000);
    map_guest_range(0xE0000000, 0x04000000);
    map_guest_range(0xF4000000, 0x02000000);
    LOG("[gfx] Metal device: %s", R.device.name.UTF8String);
}

void run_main_loop() { [NSApp run]; }

// ---------------------------------------------------------------- clears
static void clear_surface(Surface* s, const float* rgba, bool clearDepth, float depth, bool clearStencil, uint32_t stencil,
                          uint32_t firstSlice = 0, uint32_t numSlices = 1) {
    if (!s || !s->tex) return;
    end_encoder();
    for (uint32_t slice = firstSlice; slice < firstSlice + numSlices; slice++) {
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.depthAttachment.slice = rp.stencilAttachment.slice = rp.colorAttachments[0].slice = slice;
    if (s->isDepth) {
        rp.depthAttachment.texture = s->tex;
        rp.depthAttachment.loadAction = clearDepth ? MTLLoadActionClear : MTLLoadActionLoad;
        rp.depthAttachment.clearDepth = depth;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        if (s->fmt.stencil) {
            rp.stencilAttachment.texture = s->tex;
            rp.stencilAttachment.loadAction = clearStencil ? MTLLoadActionClear : MTLLoadActionLoad;
            rp.stencilAttachment.clearStencil = stencil;
            rp.stencilAttachment.storeAction = MTLStoreActionStore;
        }
    } else {
        rp.colorAttachments[0].texture = s->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(rgba[0], rgba[1], rgba[2], rgba[3]);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
    [e endEncoding];
    }
    mark_gpu_written(s);
}

void clear_color(const uint32_t* regs, uint32_t cb, const float rgba[4]) {
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_color_buffer(cb, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] color %08X %ux%u fmt %X -> %.3f %.3f %.3f %.3f", s->addr, s->width, s->height, s->format, rgba[0], rgba[1], rgba[2], rgba[3]);
    clear_surface(s, rgba, false, 0, false, 0, first, num);
}

void clear_depth_stencil(const uint32_t* regs, uint32_t db, float depth, uint32_t stencil, uint32_t flags) {
    // flags: 1 = depth, 2 = stencil
    uint32_t first = 0, num = 1;
    Surface* s = surface_from_depth_buffer(db, &first, &num);
    if (log_this_frame() && s)
        LOG("[clear] depth %08X %ux%ux%u fmt %X pixel %lu -> depth %.4f stencil %u flags %u slices %u+%u", s->addr, s->width, s->height,
            s->slices, s->format, (unsigned long)s->fmt.pixel, depth, stencil, flags, first, num);
    clear_surface(s, nullptr, flags & 1, depth, (flags & 2) != 0, stencil, first, num);
}

// ---------------------------------------------------------------- copies
void copy_surface(uint32_t src, uint32_t srcMip, uint32_t srcSlice, uint32_t dst, uint32_t dstMip, uint32_t dstSlice) {
    void copy_surface_impl(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
    copy_surface_impl(src, srcMip, srcSlice, dst, dstMip, dstSlice);
}

void copy_to_scan(uint32_t cb, uint32_t target) {
    // target: 1 = TV, 4/8 = GamePad
    if (log_this_frame()) LOG("[scan] copy %08X to %s", cb, (target & 1) ? "TV" : "DRC");
    Screen& scr = (target & 1) ? R.tv : R.drc;
    Surface* s = surface_from_color_buffer(cb);
    if (!s || !s->tex) return;
    end_encoder();
    // the image as rendered (internal resolution); the present pass scales it to the window
    NSUInteger w = s->tex.width, h = s->tex.height;
    if (!scr.tex || scr.tex.width != w || scr.tex.height != h || scr.tex.pixelFormat != s->tex.pixelFormat) {
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:s->tex.pixelFormat
                                                                                     width:w
                                                                                    height:h
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;  // render target: mod overlays
        d.storageMode = MTLStorageModePrivate;
        scr.tex = [R.device newTextureWithDescriptor:d];
    }
    id<MTLBlitCommandEncoder> b = [command_buffer() blitCommandEncoder];
    [b copyFromTexture:s->tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
            sourceSize:MTLSizeMake(w, h, 1) toTexture:scr.tex destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    if (target & 1) ::mods::draw_overlay(command_buffer(), scr.tex);  // HUD of gameplay mods (stamina wheel)
}

// debug: WWHD_DUMP_FRAMES=100,300 writes the TV image of those frames to frame_<n>.png
static std::set<uint64_t> g_dump_frames = [] {
    std::set<uint64_t> f;
    if (const char* e = getenv("WWHD_DUMP_FRAMES"))
        for (const char* p = e; *p;) {
            f.insert(strtoull(p, (char**)&p, 10));
            while (*p == ',') p++;
        }
    return f;
}();

// async: write the file when the GPU gets there instead of stalling (keeps frame timing intact)
void set_tv_format(uint32_t gx2Format, bool tv) {
    (tv ? R.tv : R.drc).srgb = (gx2Format & 0x400) != 0;
}

// depth buffers: grey image, contrast-stretched to the range of values present
static void dump_depth(id<MTLTexture> src, const char* name) {
    // all array slices side by side
    uint32_t w = (uint32_t)src.width, h = (uint32_t)src.height, n = (uint32_t)std::max<NSUInteger>(src.arrayLength, 1);
    MTLPixelFormat pf = src.pixelFormat;
    uint32_t bpp = pf == MTLPixelFormatDepth16Unorm ? 2 : 4;
    id<MTLBuffer> buf = [R.device newBufferWithLength:(NSUInteger)w * h * bpp * n options:MTLResourceStorageModeShared];
    end_encoder();
    id<MTLBlitCommandEncoder> b = [command_buffer() blitCommandEncoder];
    for (uint32_t z = 0; z < n; z++)
        [b copyFromTexture:src sourceSlice:z sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
                  toBuffer:buf destinationOffset:(NSUInteger)z * w * h * bpp destinationBytesPerRow:w * bpp destinationBytesPerImage:w * h * bpp
                   options:pf == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
    [b endEncoding];
    wait_idle();
    uint32_t W = w * n;
    std::vector<uint8_t> px((size_t)W * h * 4);
    std::string ranges;
    for (uint32_t z = 0; z < n; z++) {
        std::vector<float> v((size_t)w * h);
        for (size_t i = 0; i < v.size(); i++) {
            size_t k = (size_t)z * w * h + i;
            v[i] = bpp == 2 ? ((const uint16_t*)buf.contents)[k] / 65535.0f : ((const float*)buf.contents)[k];
        }
        float lo = 1, hi = 0;
        for (float x : v) if (x < 1.0f) { lo = std::min(lo, x); hi = std::max(hi, x); }
        char r[64];
        snprintf(r, sizeof r, " [%u] %.4f..%.4f", z, lo, hi);
        ranges += r;
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w; x++) {
                float f = v[(size_t)y * w + x];
                uint8_t g = f >= 1.0f ? 255 : (uint8_t)std::clamp((f - lo) / std::max(hi - lo, 1e-6f) * 230.0f, 0.0f, 230.0f);
                uint8_t* o = &px[((size_t)y * W + z * w + x) * 4];
                o[0] = o[1] = o[2] = g;
                o[3] = 255;
            }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), W, h, 8, W * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL(
        (__bridge CFURLRef)[NSURL fileURLWithPath:[NSString stringWithUTF8String:name]], (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    LOG("[gfx] wrote %s (%ux%u depth x%u, ranges%s)", name, w, h, n, ranges.c_str());
}

void dump_texture(id<MTLTexture> src, const char* name, bool async, bool srgbEncode) {
    if (!src) return;
    uint32_t w = (uint32_t)src.width, h = (uint32_t)src.height;
    MTLPixelFormat pf = src.pixelFormat;
    if (pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8) {
        dump_depth(src, name);
        return;
    }
    bool rgb10 = pf == MTLPixelFormatRGB10A2Unorm;
    bool rgba8 = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB;
    bool f16 = pf == MTLPixelFormatRGBA16Float;
    bool r8 = pf == MTLPixelFormatR8Unorm;
    bool rg11 = pf == MTLPixelFormatRG11B10Float;
    if (!rgb10 && !rgba8 && !f16 && !r8 && !rg11) { LOG("[gfx] dump: unsupported format %lu", (unsigned long)pf); return; }
    uint32_t bpp = f16 ? 8 : r8 ? 1 : 4;
    MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.storageMode = MTLStorageModeShared;
    id<MTLTexture> t = [R.device newTextureWithDescriptor:d];
    end_encoder();
    id<MTLBlitCommandEncoder> b = [command_buffer() blitCommandEncoder];
    [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
             toTexture:t destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [b endEncoding];
    std::string file = name;
    auto write = [=]() {
    std::vector<uint8_t> raw((size_t)w * h * bpp), px((size_t)w * h * 4);
    [t getBytes:raw.data() bytesPerRow:w * bpp fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0];
    for (size_t i = 0; i < (size_t)w * h; i++) {
        uint8_t* o = &px[i * 4];
        if (rgb10) {
            uint32_t v; memcpy(&v, &raw[i * 4], 4);
            o[0] = (v & 0x3FF) >> 2; o[1] = ((v >> 10) & 0x3FF) >> 2; o[2] = ((v >> 20) & 0x3FF) >> 2;
        } else if (rgba8) {
            memcpy(o, &raw[i * 4], 3);
        } else if (r8) {
            o[0] = o[1] = o[2] = raw[i];
        } else if (rg11) {
            uint32_t v; memcpy(&v, &raw[i * 4], 4);
            auto f11 = [](uint32_t m, uint32_t e) { float f = e ? ldexpf(1.0f + m / 64.0f, (int)e - 15) : ldexpf(m / 64.0f, -14); return f; };
            float r = f11(v & 0x3F, (v >> 6) & 0x1F), g = f11((v >> 11) & 0x3F, (v >> 17) & 0x1F), bl = ldexpf(1.0f + ((v >> 22) & 0x1F) / 32.0f, (int)((v >> 27) & 0x1F) - 15);
            o[0] = (uint8_t)std::min(255.0f, r * 255); o[1] = (uint8_t)std::min(255.0f, g * 255); o[2] = (uint8_t)std::min(255.0f, bl * 255);
        } else {
            for (int c = 0; c < 3; c++) {
                __fp16 hv; memcpy(&hv, &raw[i * 8 + c * 2], 2);
                o[c] = (uint8_t)std::clamp((float)hv * 255.0f, 0.0f, 255.0f);
            }
        }
        o[3] = 255;
        if (srgbEncode)
            for (int c = 0; c < 3; c++) {
                float v = o[c] / 255.0f;
                v = v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1 / 2.4f) - 0.055f;
                o[c] = (uint8_t)std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f);
            }
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(px.data(), w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaNoneSkipLast);
    CGImageRef img = CGBitmapContextCreateImage(ctx);
    NSString* path = [NSString stringWithUTF8String:file.c_str()];
    CGImageDestinationRef dst = CGImageDestinationCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:path],
                                                                (__bridge CFStringRef)UTTypePNG.identifier, 1, nullptr);
    CGImageDestinationAddImage(dst, img, nullptr);
    CGImageDestinationFinalize(dst);
    CFRelease(dst);
    CGImageRelease(img);
    CGContextRelease(ctx);
    CGColorSpaceRelease(cs);
    LOG("[gfx] wrote %s (%ux%u)", file.c_str(), w, h);
    };
    if (async) {
        [command_buffer() addCompletedHandler:^(id<MTLCommandBuffer>) { write(); }];
    } else {
        wait_idle();
        write();
    }
}

// save states: thumbnails and test dumps of the TV image a number of frames from now
static std::mutex g_tv_dump_mu;
static std::vector<std::pair<uint64_t, std::string>> g_tv_dumps;
uint64_t frame_count() { return __atomic_load_n(&R.frame, __ATOMIC_RELAXED); }
void request_tv_dump(const std::string& path, int frames_ahead) {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    g_tv_dumps.push_back({frame_count() + frames_ahead, path});
}
static void service_tv_dumps() {
    std::lock_guard<std::mutex> lk(g_tv_dump_mu);
    for (auto it = g_tv_dumps.begin(); it != g_tv_dumps.end();)
        if (it->first <= R.frame && R.tv.tex) {
            dump_texture(R.tv.tex, it->second.c_str(), true, R.tv.srgb);
            it = g_tv_dumps.erase(it);
        } else {
            ++it;
        }
}

static void dump_tv(uint64_t frame) {
    char name[64];
    snprintf(name, sizeof name, "frame_%llu.png", (unsigned long long)frame);
    dump_texture(R.tv.tex, name, true, R.tv.srgb);
    if (R.drc.tex) {
        snprintf(name, sizeof name, "frame_%llu_drc.png", (unsigned long long)frame);
        dump_texture(R.drc.tex, name, true, R.drc.srgb);
    }
}

// draw a screen's image into its window, letterboxed to keep the aspect ratio
static void present(Screen& scr) {
    if (!scr.layer || !scr.tex) return;
    MTLPixelFormat want = scr.srgb ? MTLPixelFormatBGRA8Unorm_sRGB : MTLPixelFormatBGRA8Unorm;
    if (scr.layer.pixelFormat != want) scr.layer.pixelFormat = want;
    id<CAMetalDrawable> drawable = scr.visible ? [scr.layer nextDrawable] : nil;
    if (!drawable) return;
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = drawable.texture;
    rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [command_buffer() renderCommandEncoderWithDescriptor:rp];
    float dw = drawable.texture.width, dh = drawable.texture.height;
    float sa = (float)scr.tex.width / scr.tex.height, da = dw / dh;
    float rect[4] = {0, 0, 1, 1};
    if (da > sa) { rect[2] = sa / da; rect[0] = (1 - rect[2]) / 2; }
    else { rect[3] = da / sa; rect[1] = (1 - rect[3]) / 2; }
    [e setRenderPipelineState:drawable.texture.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? R.presentPipelineSRGB
                                                                                         : R.presentPipeline];
    [e setVertexBytes:rect length:sizeof(rect) atIndex:0];
    [e setFragmentTexture:scr.tex atIndex:0];
    int aa = fxaa_enabled() ? 1 : 0;
    [e setFragmentBytes:&aa length:sizeof aa atIndex:0];
    [e setFragmentSamplerState:R.linearClamp atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
    [command_buffer() presentDrawable:drawable];
}

void with_autorelease_pool(void (*fn)()) {
    @autoreleasepool {
        fn();
    }
}

static std::atomic<uint64_t> g_frames_completed{0};
uint64_t frames_completed() { return g_frames_completed; }

void cache_warm_step();

void swap() {
    cache_warm_step();
    static bool sync_gpu = getenv("WWHD_SYNC_GPU") != nullptr;  // debug: no CPU/GPU overlap
    if (sync_gpu) wait_idle();
    end_encoder();
    if (log_this_frame()) LOG("[frame] end %llu", (unsigned long long)R.frame);
    R.frame++;
    latch_res_scale();
    if (g_dump_frames.count(R.frame)) dump_tv(R.frame);
    service_tv_dumps();
    const char* capture_begin_frame();
    static std::string pendingCapture;  // the TV image is dumped once the captured frame has been drawn
    if (!pendingCapture.empty()) {
        dump_texture(R.tv.tex, (pendingCapture + "/tv.png").c_str(), true, R.tv.srgb);
        if (R.drc.tex) dump_texture(R.drc.tex, (pendingCapture + "/gamepad.png").c_str(), true, R.drc.srgb);
        LOG("[gfx] capture written to %s", pendingCapture.c_str());
        pendingCapture.clear();
    }
    if (const char* dir = capture_begin_frame()) pendingCapture = dir;
    @autoreleasepool {
        present(R.tv);
        present(R.drc);
        [command_buffer() addCompletedHandler:^(id<MTLCommandBuffer>) { g_frames_completed++; }];
        flush();
    }
    if (R.frame % 300 == 1) {
        LOG("[gfx] frame %llu, %llu draws so far, GPU %.1f ms/frame", (unsigned long long)R.frame, (unsigned long long)R.drawCount,
            g_gpu_ns.exchange(0) / 1e6 / 300.0);
        void report_skips();
        report_skips();
    }
}

extern uint64_t g_stat_invalidates, g_stat_invalidated_surfaces;
void invalidate(uint32_t flags, uint32_t addr, uint32_t size) {
    g_stat_invalidates++;
    static int logged = 0;
    if (getenv("WWHD_LOG_INVALIDATE") && (flags & 0x2) && logged++ < 400)
        LOG("[inval] frame %llu flags %X addr %08X size %X", (unsigned long long)R.frame, flags, addr, size);
    // GX2_INVALIDATE_MODE_TEXTURE (0x2): the CPU wrote texture data; force a full check of surfaces in
    // range on next use. Uniform/attribute/shader invalidations need nothing here.
    if (!(flags & 0x2)) return;
    // "invalidate everything" (sent several times per frame) carries no information about CPU writes;
    // changed textures are still caught by the per-frame sparse check and the periodic full check
    if (size >= 0x10000000) return;
    for (auto& [a, s] : R.surfaces)
        // MEM1 holds render targets; CPU-side surfaces there are views of GPU data, not CPU uploads
        if (!s->gpuWritten && !(a >= 0xF4000000 && a < 0xF6000000) && a < addr + size &&
            addr < a + std::max<uint32_t>(s->dataSize, s->pitch * s->height * 4)) {
            s->lastCheckedFrame = ~0ull;
            s->dirty = true;
            g_stat_invalidated_surfaces++;
            static int lg = 0;
            if (getenv("WWHD_LOG_INVALIDATE") && lg++ < 300)
                LOG("[inval]   marks %08X %ux%u fmt %X size %X (range %08X+%X)", a, s->width, s->height, s->format, s->dataSize, addr, size);
        }
}

}  // namespace gfx
