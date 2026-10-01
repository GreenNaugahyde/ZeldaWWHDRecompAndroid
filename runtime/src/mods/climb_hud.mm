// Stamina wheel for the "climb any wall" mod (climb.cpp): a ring drawn by the renderer into the TV
// image right after the game's frame is copied to the scan buffer (so frame dumps show it too).
// It sits to the upper right of the screen centre, where the follow camera keeps Link, and is only
// drawn while visible (climbing a plain wall, refilling, or fading out).
#import <Metal/Metal.h>

#include <unordered_map>

#include "mods/climb.h"

namespace {
const char* kShader = R"(
#include <metal_stdlib>
using namespace metal;
struct U { float4 rect; float stamina; float alpha; float exhausted; float pad; };
struct V { float4 pos [[position]]; float2 p; };
vertex V hud_vs(uint vid [[vertex_id]], constant U& u [[buffer(0)]]) {
    float2 q = float2(vid & 1, vid >> 1);
    V o;
    o.pos = float4(mix(u.rect.xy, u.rect.zw, q), 0, 1);
    o.p = q * 2.0 - 1.0;  // -1..1, +y up
    return o;
}
fragment float4 hud_fs(V in [[stage_in]], constant U& u [[buffer(0)]]) {
    float r = length(in.p);
    float aa = max(fwidth(r), 1e-4);
    // ring 0.58..0.92 (fill), outline 0.50..1.0 (dark)
    float ring = smoothstep(0.58 - aa, 0.58 + aa, r) * (1.0 - smoothstep(0.92 - aa, 0.92 + aa, r));
    float outline = smoothstep(0.50 - aa, 0.50 + aa, r) * (1.0 - smoothstep(1.0 - 2.0 * aa, 1.0, r));
    float ang = atan2(in.p.x, in.p.y);  // 0 at the top, clockwise
    float frac = (ang < 0.0 ? ang + 2.0 * M_PI_F : ang) / (2.0 * M_PI_F);
    float filled = 1.0 - smoothstep(u.stamina - 0.004, u.stamina + 0.004, frac);
    float3 green = float3(0.30, 0.90, 0.35), yellow = float3(1.0, 0.80, 0.15), red = float3(0.95, 0.20, 0.15);
    float3 full = u.exhausted > 0.5 ? red : (u.stamina < 0.3 ? mix(red, yellow, u.stamina / 0.3) : green);
    // the empty part: dark grey, dark red while exhausted (refilling, no climbing until full)
    float3 empty = u.exhausted > 0.5 ? float3(0.55, 0.08, 0.06) : float3(0.10, 0.10, 0.10);
    float3 col = mix(empty, full, filled);
    float fa = ring * mix(0.45, 0.95, filled);
    float oa = outline * 0.45;
    // fill over outline, premultiplied
    float a = fa + oa * (1.0 - fa);
    float3 c = col * fa;
    return float4(c, a) * u.alpha;
}
)";

struct Hud {
    id<MTLDevice> device = nil;
    id<MTLLibrary> lib = nil;
    std::unordered_map<NSUInteger, id<MTLRenderPipelineState>> pipelines;  // per pixel format
};
Hud& hud() {
    static Hud h;
    return h;
}

id<MTLRenderPipelineState> pipeline(id<MTLDevice> dev, MTLPixelFormat fmt) {
    Hud& h = hud();
    if (h.device != dev) {
        h = Hud{};
        h.device = dev;
        NSError* err = nil;
        h.lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:kShader] options:nil error:&err];
        if (!h.lib) {
            NSLog(@"[climb] HUD shader: %@", err.localizedDescription);
            return nil;
        }
    }
    if (!h.lib) return nil;
    auto it = h.pipelines.find(fmt);
    if (it != h.pipelines.end()) return it->second;
    MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [h.lib newFunctionWithName:@"hud_vs"];
    d.fragmentFunction = [h.lib newFunctionWithName:@"hud_fs"];
    d.colorAttachments[0].pixelFormat = fmt;
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne;
    d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorZero;
    d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOne;  // keep the image's alpha
    NSError* err = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) NSLog(@"[climb] HUD pipeline: %@", err.localizedDescription);
    h.pipelines[fmt] = p;
    return p;
}
}  // namespace

namespace mods {
// called by the renderer after the TV image was copied to the scan texture
void draw_overlay(id<MTLCommandBuffer> cmd, id<MTLTexture> tex) {
    if (!climb_enabled() || !tex || !(tex.usage & MTLTextureUsageRenderTarget)) return;
    ClimbHud s = climb_hud();
    if (s.alpha <= 0.0f) return;
    id<MTLRenderPipelineState> p = pipeline(cmd.device, tex.pixelFormat);
    if (!p) return;
    float w = tex.width, h = tex.height;
    float cx = 0.60f * w, cy = 0.38f * h, rad = 0.05f * h;  // pixels, from the top left
    struct { float rect[4]; float stamina, alpha, exhausted, pad; } u = {
        {(cx - rad) / w * 2 - 1, 1 - (cy + rad) / h * 2, (cx + rad) / w * 2 - 1, 1 - (cy - rad) / h * 2},
        s.stamina, s.alpha, s.exhausted ? 1.0f : 0.0f, 0};
    MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = tex;
    rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> e = [cmd renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setVertexBytes:&u length:sizeof u atIndex:0];
    [e setFragmentBytes:&u length:sizeof u atIndex:0];
    [e drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
    [e endEncoding];
}
}  // namespace mods
