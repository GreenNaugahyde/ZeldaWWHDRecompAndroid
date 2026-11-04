// 60 fps (generated): the game runs at its own 30 frames a second, and after each real frame an
// in-between one is made on the GPU and shown half a frame later (from WindWakerHD-Android-Plus,
// vk/reproj.cpp at 84b0ff2, by Nycolazs).
// The in-between frame is the real one reprojected with the scene's depth to the camera moved on by
// half its last step (eye and turn: the camera orbits Link, so a turn alone would move Link
// sideways). The HUD stays where it is: at the first HUD layout drawn on the TV picture the scene
// (3D and its post-processing) is copied aside, and pixels where the finished picture differs from it
// are HUD. The frame interpolation mode draws every frame twice on the game thread; this costs one GPU
// pass instead, so it holds 60 where the game thread can't (Outset, Forest Haven while moving).
// It runs in the frame generation path (vk_device.cpp present_frame_fg): the window picture is
// composed as usual, record() copies it and makes the in-between one, the present thread shows both.
#include "reproj.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "runtime.h"

namespace gfx::rp {
namespace {

const char* kVertex = R"glsl(#version 450
layout(location=0) out vec2 uv;
void main() {
 vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
 uv = p;
 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// window picture uv -> the generated window picture. p.rect: where the TV picture lies in the window
// (uv); t: tanX, tanY, HUD difference threshold, unused; z: near, far, depth usable, unused;
// r0..r2: rows of H (the generated camera's axes in the real camera's space), .w: its eye there
const char* kWarp = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D comp;      // the window picture (display encoded)
layout(set=0,binding=1) uniform sampler2D depthImg;  // the scene's depth (TV picture space)
layout(set=0,binding=2) uniform sampler2D hudMask;   // where the HUD is, a quarter of the TV picture's size (kHudMask)
layout(set=0,binding=3) uniform sampler2D unused3;
layout(set=0,binding=4) uniform sampler2D flowImg;   // objects' own movement per 16x16 block (kFlow)
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
// the 2x2 depth texels around q, as textureGather would return them, read one by one: a gather on
// the game's depth-stencil image froze an Adreno 650 (Galaxy S20 FE) until it restarted and gave
// stray lines on others (LASRecomp-Android-Plus 08bbaef); plain fetches work on every driver
vec4 gatherDepth(vec2 q) {
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(q * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 return vec4(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r,
             texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r, texelFetch(depthImg, clamp(b, lo, hi), 0).r);
}
// the nearest depth of the 2x2 pixels around q: the game's outlines around characters lie on the
// background's depth and would otherwise move with the background (depth range [-1,1] as GX2)
float viewDepth(vec2 q) {
 vec4 m = gatherDepth(q);
 float d = min(min(m.x, m.y), min(m.z, m.w)), n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
// how much the pixel at q moves with the camera, 0..1: what lies about as far as the point the
// camera looks at (a.x; a.y: on). The camera follows Link (and the title's boat): they and what is
// near them keep their place on the screen while the camera moves, so moving them with the camera's
// movement shook them back and forth every frame; the background, further away, is reprojected.
// (z: the view depth at q, read once per step of the motion)
float follows(float z, vec2 q) {
 if (p.a.y < 0.5 || p.a.x <= 0.0) return 0.0;
 float r = abs(log(max(z, 1e-3) / p.a.x));
 // only around the point the camera looks at (the screen's centre): the world at Link's distance
 // elsewhere (a boat beside him, a wall) moves with the camera like the rest
 vec2 d = (q - 0.5) * vec2(p.t.x / p.t.y, 1.0);
 return (1.0 - smoothstep(0.35, 0.75, r)) * (1.0 - smoothstep(0.12, 0.24, length(d)));
}
// where the real picture's pixel q (TV uv), at view depth z, is in the generated picture
vec2 toGenCam(vec2 q, float z);
vec2 toGen(vec2 q, float z, float fw) { return mix(toGenCam(q, z), q, fw); }
vec2 toGenCam(vec2 q, float z) {
 vec3 P = vec3((q.x * 2.0 - 1.0) * p.t.x, (1.0 - q.y * 2.0) * p.t.y, -1.0) * z - vec3(p.r0.w, p.r1.w, p.r2.w);
 vec3 g = p.r0.xyz * P.x + p.r1.xyz * P.y + p.r2.xyz * P.z;
 if (g.z > -1e-4) return q;
 return vec2(g.x / (-g.z) / p.t.x * 0.5 + 0.5, 0.5 - g.y / (-g.z) / p.t.y * 0.5);
}
// half the movement of the object at q beyond the camera's (TV uv), where q's depth is its block's
// (characters and enemies: with the camera's movement alone they stood still in the in-between frame,
// then jumped: 30 fps motion)
vec2 objectMove(vec2 q, float z, float fw) {
 if (p.z.w < 0.5) return vec2(0.0);
 vec4 f = textureLod(flowImg, q, 0.0);
 if (f.w < 0.5) return vec2(0.0);
 float bz = p.z.x * exp(f.z * log(p.z.y / p.z.x));
 if (abs(z - bz) > 0.15 * bz) return vec2(0.0);
 return f.xy * 0.5 * (1.0 - fw);  // (what follows the camera already keeps its place)
}
// the HUD and a few pixels around it (glows, outlines and their soft edges): the mask's texels cover
// 4x4 TV pixels, its bilinear tap anything within one texel of them
bool hud(vec2 q) { return textureLod(hudMask, q, 0.0).r > 0.01; }
// the window picture's pixel nearest to q, unfiltered: bilinear resampling softened fine detail (water,
// grass, distant edges) in every other frame, which shimmered; the movement is rounded to whole pixels
vec4 window(vec2 q) {
 ivec2 sz = textureSize(comp, 0);
 return texelFetch(comp, clamp(ivec2((p.rect.xy + q * p.rect.zw) * vec2(sz)), ivec2(0), sz - 1), 0);
}
void main() {
 vec2 tv = (uv - p.rect.xy) / p.rect.zw;
 if (p.z.z < 0.5 || any(lessThan(tv, vec2(0.0))) || any(greaterThan(tv, vec2(1.0))) || hud(tv)) {
  result = textureLod(comp, uv, 0.0);  // outside the TV picture, without a depth buffer, or the HUD: unmoved
  return;
 }
 vec2 q = tv;  // the source pixel that lands here: a few fixed-point steps of the motion
 vec2 raw = tv;
 for (int i = 0; i < 3; i++) {
  float z = viewDepth(q), fw = follows(z, q);
  raw = tv - (toGen(q, z, fw) - q) - objectMove(q, z, fw);
  q = clamp(raw, vec2(0.0), vec2(1.0));
 }
 // it would come from outside the real picture (the edge while the camera turns): the edge of the
 // picture along the movement (q is clamped to it). The real picture's own pixel there, as before,
 // showed what lies next to the edge twice (a boat, a sign) in a band as wide as half the camera's
 // step: up to a tenth of the screen in a fast turn
 if (hud(q)) {  // the scene there is hidden under the HUD: the nearest visible scene along the motion
  vec2 px = 1.0 / vec2(textureSize(depthImg, 0));
  vec2 d = q - tv;
  float l = length(d / px);
  d = l > 0.5 ? d / l : vec2(px.x, 0.0);
  for (int k = 1; k <= 8; k++) {
   vec2 a = clamp(q + d * float(k * 12), vec2(0.0), vec2(1.0));
   if (!hud(a)) { q = a; break; }
   a = clamp(q - d * float(k * 12), vec2(0.0), vec2(1.0));
   if (!hud(a)) { q = a; break; }
  }
 }
 result = window(q);
 if (p.t.w > 0.5) result.rgb *= vec3(1.0, 0.6, 0.6);  // (debug: WWHD_FRAMEGEN_TINT=1) generated frames tinted
}
)glsl";


// ---- the in-between picture by a warped mesh (default; WWHD_FRAMEGEN_GRID=0: kWarp / kWarpCov) ----
// kWarp looks, for each pixel of the in-between picture, for the scene's pixel that lands there (a
// few fixed-point steps). Where something near stands in front of something far that search has two
// answers, and with a fast camera it jumped between them: thin parts (the King of Red Lions' fins,
// railings, leaves) came out torn into strips. Here the scene before the HUD is drawn as a mesh, one
// vertex every few TV pixels (a margin beyond the edges, held at their depth), each moved where the
// camera's (and the object's own) movement takes it, with a depth test: what is near always covers
// what is far. Triangles stretched over a gap (the background a near object leaves) are pushed
// behind everything, so they only fill what nothing else covers. kCompose then puts the HUD back
// with its coverage: picture + (moved scene - scene) * (1 - coverage).
// rect: vertices per row and rows, the margin (TV uv); z.w: object movement on
const char* kGridVertex = R"glsl(#version 450
layout(set=0,binding=1) uniform sampler2D depthImg;
layout(set=0,binding=4) uniform sampler2D flowImg;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
layout(location=0) out vec2 src;
layout(location=1) out vec3 farSrc;  // src weighted by depth^4: a gap's triangle takes the far side
layout(location=2) out vec4 dbg;     // WWHD_FRAMEGEN_DEBUG: the movement's kind
// the 2x2 depth texels around q, as textureGather would return them, read one by one: a gather on
// the game's depth-stencil image froze an Adreno 650 (Galaxy S20 FE) until it restarted and gave
// stray lines on others (LASRecomp-Android-Plus 08bbaef); plain fetches work on every driver
vec4 gatherDepth(vec2 q) {
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(q * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 return vec4(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r,
             texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r, texelFetch(depthImg, clamp(b, lo, hi), 0).r);
}
float viewDepth(vec2 q) {
 vec4 m = gatherDepth(q);
 float d = min(min(m.x, m.y), min(m.z, m.w)), n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
float follows(float z, vec2 q) {
 if (p.a.y < 0.5 || p.a.x <= 0.0) return 0.0;
 float r = abs(log(max(z, 1e-3) / p.a.x));
 vec2 d = (q - 0.5) * vec2(p.t.x / p.t.y, 1.0);
 return (1.0 - smoothstep(0.35, 0.75, r)) * (1.0 - smoothstep(0.12, 0.24, length(d)));
}
vec2 toGenCam(vec2 q, float z) {
 vec3 P = vec3((q.x * 2.0 - 1.0) * p.t.x, (1.0 - q.y * 2.0) * p.t.y, -1.0) * z - vec3(p.r0.w, p.r1.w, p.r2.w);
 vec3 g = p.r0.xyz * P.x + p.r1.xyz * P.y + p.r2.xyz * P.z;
 if (g.z > -1e-4) return q;
 return vec2(g.x / (-g.z) / p.t.x * 0.5 + 0.5, 0.5 - g.y / (-g.z) / p.t.y * 0.5);
}
vec2 objectMove(vec2 q, float z, float fw) {
 if (p.z.w < 0.5) return vec2(0.0);
 vec4 f = textureLod(flowImg, q, 0.0);
 if (f.w < 0.5) return vec2(0.0);
 float bz = p.z.x * exp(f.z * log(p.z.y / p.z.x));
 if (abs(z - bz) > 0.15 * bz) return vec2(0.0);
 return f.xy * 0.5 * (1.0 - fw);
}
void main() {
 // (unsigned: a signed division by a variable is what some mobile shader compilers get wrong)
 uint cols = uint(p.rect.x), rows = uint(p.rect.y), vi = uint(gl_VertexIndex);
 vec2 g = vec2(vi % cols, vi / cols) / vec2(cols - 1u, rows - 1u);
 float m = p.rect.z;
 vec2 q = g * (1.0 + 2.0 * m) - m;
 vec2 qc = clamp(q, vec2(0.0), vec2(1.0));
 float z = viewDepth(qc), fw = follows(z, qc);
 vec2 d = mix(toGenCam(q, z), q, fw) + objectMove(qc, z, fw);
 src = qc;
 vec4 fm = textureLod(flowImg, qc, 0.0);
 dbg = vec4(fm.w > 0.75 ? 1.0 : 0.0, fm.w > 0.25 && fm.w < 0.75 ? 1.0 : 0.0, fm.w < 0.25 ? 1.0 : 0.0, length(fm.xy) > 0.002 ? 1.0 : 0.0);
 float w = pow(clamp(z, 1.0, 1e5) / 100.0, 4.0);
 farSrc = vec3(qc * w, w);
 gl_Position = vec4(d * 2.0 - 1.0, clamp(log(max(z, p.z.x) / p.z.x) / log(p.z.y / p.z.x), 0.0, 1.0) * 0.99, 1.0);
}
)glsl";

// the scene's pixel the mesh brings here (display encoded, as the window picture); a.z: sRGB scan buffer
const char* kGridFragment = R"glsl(#version 450
layout(set=0,binding=3) uniform sampler2D sceneImg;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
layout(location=0) in vec2 src;
layout(location=1) in vec3 farSrc;
layout(location=2) in vec4 dbg;
layout(location=0) out vec4 result;
vec3 enc(vec3 c) {
 return p.a.z > 0.5 ? mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)) : c;
}
void main() {
 vec2 ds = vec2(textureSize(sceneImg, 0));
 vec2 dx = dFdx(src) * ds, dy = dFdy(src) * ds;
 // scene texels per pixel: about 1 where the mesh only moved; far less where a triangle spans a gap
 float area = abs(dx.x * dy.y - dx.y * dy.x);
 // a gap (stretched between something near and what is behind it) shows what is behind
 bool gap = area < 0.15;
 gl_FragDepth = gap ? 0.995 : gl_FragCoord.z;
 result = vec4(enc(textureLod(sceneImg, gap ? farSrc.xy / max(farSrc.z, 1e-30) : src, 0.0).rgb), 1.0);
 // debug: red sure, green spread, blue undecided; bright where it moves on its own
 if (p.rect.w > 1.5) result.rgb = vec3(gl_FragCoord.z / 0.99);  // debug 2: the depth (log scale)
 else if (p.rect.w > 0.5) result.rgb = mix(result.rgb, dbg.rgb * (0.4 + 0.6 * dbg.w), 0.6);
}
)glsl";

// the in-between window picture from the warped scene (TV uv: binding 1) and the HUD's coverage
// (binding 2: coverage, or the HUD mask where this frame has none); a.z: sRGB scan buffer
const char* kCompose = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D comp;
layout(set=0,binding=1) uniform sampler2D warpImg;
layout(set=0,binding=2) uniform sampler2D covImg;
layout(set=0,binding=3) uniform sampler2D sceneImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
vec3 enc(vec3 c) {
 return p.a.z > 0.5 ? mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)) : c;
}
void main() {
 vec4 c0 = textureLod(comp, uv, 0.0);
 vec2 tv = (uv - p.rect.xy) / p.rect.zw;
 if (any(lessThan(tv, vec2(0.0))) || any(greaterThan(tv, vec2(1.0)))) { result = c0; return; }
 float c = textureLod(covImg, tv, 0.0).r;
 if (c > 0.996) { result = c0; return; }
 vec3 moved = textureLod(warpImg, tv, 0.0).rgb;
 if (c < 0.004) result = vec4(moved, c0.a);  // no HUD here: the moved scene
 else result = vec4(clamp(c0.rgb + (moved - enc(textureLod(sceneImg, tv, 0.0).rgb)) * (1.0 - c), 0.0, 1.0), c0.a);
 if (p.t.w > 0.5) result.rgb *= vec3(1.0, 0.6, 0.6);
}
)glsl";

// The in-between picture with the HUD's coverage (vk_draw.cpp, hud coverage): covImg holds how much
// of the scene the HUD covers at each TV pixel, sceneImg the scene before the HUD. Where nothing
// covers it the scene is reprojected as in kWarp; where the HUD covers it fully the pixel stays;
// in between (translucent HUD parts, soft edges) the scene's change shows through by the share the
// HUD leaves of it: picture + (scene moved - scene) * (1 - coverage). The scene under an opaque
// HUD part is known (sceneImg): no search for a visible pixel. a.z: the scan buffer is sRGB (the
// window picture is encoded, the scene not).
const char* kWarpCov = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D comp;
layout(set=0,binding=1) uniform sampler2D depthImg;
layout(set=0,binding=2) uniform sampler2D covImg;
layout(set=0,binding=3) uniform sampler2D sceneImg;
layout(set=0,binding=4) uniform sampler2D flowImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
// the 2x2 depth texels around q, as textureGather would return them, read one by one: a gather on
// the game's depth-stencil image froze an Adreno 650 (Galaxy S20 FE) until it restarted and gave
// stray lines on others (LASRecomp-Android-Plus 08bbaef); plain fetches work on every driver
vec4 gatherDepth(vec2 q) {
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(q * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 return vec4(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r,
             texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r, texelFetch(depthImg, clamp(b, lo, hi), 0).r);
}
float viewDepth(vec2 q) {
 vec4 m = gatherDepth(q);
 float d = min(min(m.x, m.y), min(m.z, m.w)), n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
float follows(float z, vec2 q) {
 if (p.a.y < 0.5 || p.a.x <= 0.0) return 0.0;
 float r = abs(log(max(z, 1e-3) / p.a.x));
 // only around the point the camera looks at (the screen's centre): the world at Link's distance
 // elsewhere (a boat beside him, a wall) moves with the camera like the rest
 vec2 d = (q - 0.5) * vec2(p.t.x / p.t.y, 1.0);
 return (1.0 - smoothstep(0.35, 0.75, r)) * (1.0 - smoothstep(0.12, 0.24, length(d)));
}
vec2 toGenCam(vec2 q, float z) {
 vec3 P = vec3((q.x * 2.0 - 1.0) * p.t.x, (1.0 - q.y * 2.0) * p.t.y, -1.0) * z - vec3(p.r0.w, p.r1.w, p.r2.w);
 vec3 g = p.r0.xyz * P.x + p.r1.xyz * P.y + p.r2.xyz * P.z;
 if (g.z > -1e-4) return q;
 return vec2(g.x / (-g.z) / p.t.x * 0.5 + 0.5, 0.5 - g.y / (-g.z) / p.t.y * 0.5);
}
vec2 toGen(vec2 q, float z, float fw) { return mix(toGenCam(q, z), q, fw); }
vec2 objectMove(vec2 q, float z, float fw) {
 if (p.z.w < 0.5) return vec2(0.0);
 vec4 f = textureLod(flowImg, q, 0.0);
 if (f.w < 0.5) return vec2(0.0);
 float bz = p.z.x * exp(f.z * log(p.z.y / p.z.x));
 if (abs(z - bz) > 0.15 * bz) return vec2(0.0);
 return f.xy * 0.5 * (1.0 - fw);
}
vec4 window(vec2 q) {
 ivec2 sz = textureSize(comp, 0);
 return texelFetch(comp, clamp(ivec2((p.rect.xy + q * p.rect.zw) * vec2(sz)), ivec2(0), sz - 1), 0);
}
vec3 enc(vec3 c) {
 return p.a.z > 0.5 ? mix(c * 12.92, 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c)) : c;
}
vec3 scene(vec2 q) {
 ivec2 sz = textureSize(sceneImg, 0);
 return enc(texelFetch(sceneImg, clamp(ivec2(q * vec2(sz)), ivec2(0), sz - 1), 0).rgb);
}
float cover(vec2 q) { return textureLod(covImg, q, 0.0).r; }
void main() {
 vec4 c0 = textureLod(comp, uv, 0.0);
 vec2 tv = (uv - p.rect.xy) / p.rect.zw;
 if (p.z.z < 0.5 || any(lessThan(tv, vec2(0.0))) || any(greaterThan(tv, vec2(1.0)))) { result = c0; return; }
 float c = cover(tv);
 if (c > 0.996) { result = c0; return; }  // under opaque HUD: as drawn
 vec2 q = tv, raw = tv;
 for (int i = 0; i < 3; i++) {
  float z = viewDepth(q), fw = follows(z, q);
  raw = tv - (toGen(q, z, fw) - q) - objectMove(q, z, fw);
  q = clamp(raw, vec2(0.0), vec2(1.0));
 }
 if (c < 0.004)  // no HUD here: the picture moved (the scene itself where the HUD covers the source)
  result = cover(q) < 0.004 ? window(q) : vec4(scene(q), 1.0);
 else
  result = vec4(clamp(c0.rgb + (scene(q) - scene(tv)) * (1.0 - c), 0.0, 1.0), c0.a);
 if (p.t.w > 0.5) result.rgb *= vec3(1.0, 0.6, 0.6);
}
)glsl";

// Objects' own movement, one value per 16x16 block of the TV picture: the block of this frame's scene
// (quarter size, no HUD) is looked for in the previous one around where the camera's movement alone
// puts it (depth), and around the same place on the screen (what the camera follows). Output
// (RGBA16F): the movement beyond the camera's, previous -> this frame (TV uv), the block's nearest
// depth (log scale), valid. Blocks with little detail or no clearly better match: 0.
// r0..r2: rows of D (this camera's axes in the previous camera's space), .w: this eye there
const char* kFlow = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D curImg;
layout(set=0,binding=1) uniform sampler2D prevImg;
layout(set=0,binding=2) uniform sampler2D depthImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; } p;
// the 2x2 depth texels around q, as textureGather would return them, read one by one: a gather on
// the game's depth-stencil image froze an Adreno 650 (Galaxy S20 FE) until it restarted and gave
// stray lines on others (LASRecomp-Android-Plus 08bbaef); plain fetches work on every driver
vec4 gatherDepth(vec2 q) {
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(q * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 return vec4(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r,
             texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r, texelFetch(depthImg, clamp(b, lo, hi), 0).r);
}
float lin(float d) {
 float n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
// 4x4 quarter-size pixels as four gathers of the green channel (brightness is enough to match blocks)
void block(sampler2D img, vec2 at, vec2 ts, out vec4 b[4]) {
 b[0] = textureGather(img, at + vec2(-1.0, -1.0) * ts, 1); b[1] = textureGather(img, at + vec2(1.0, -1.0) * ts, 1);
 b[2] = textureGather(img, at + vec2(-1.0, 1.0) * ts, 1);  b[3] = textureGather(img, at + vec2(1.0, 1.0) * ts, 1);
}
float sad(vec4 a[4], vec4 b[4]) {
 vec4 d = abs(a[0] - b[0]) + abs(a[1] - b[1]) + abs(a[2] - b[2]) + abs(a[3] - b[3]);
 return d.x + d.y + d.z + d.w;
}
void main() {
 result = vec4(0.0);
 vec2 ts = 1.0 / vec2(textureSize(curImg, 0));
 vec4 cur[4]; block(curImg, uv, ts, cur);
 vec4 m4 = (cur[0] + cur[1] + cur[2] + cur[3]) * 0.25; float mean = (m4.x + m4.y + m4.z + m4.w) * 0.25;
 vec4 dv = abs(cur[0] - mean) + abs(cur[1] - mean) + abs(cur[2] - mean) + abs(cur[3] - mean);
 vec2 dt = 1.0 / vec2(textureSize(depthImg, 0));
 vec4 g = min(min(gatherDepth(uv + vec2(-4.0, -4.0) * dt), gatherDepth(uv + vec2(4.0, -4.0) * dt)),
              min(gatherDepth(uv + vec2(-4.0, 4.0) * dt), gatherDepth(uv + vec2(4.0, 4.0) * dt)));
 float z = lin(min(min(g.x, g.y), min(g.z, g.w)));
 float zl = clamp(log(z / p.z.x) / log(p.z.y / p.z.x), 0.0, 1.0);
 result.z = zl;
 if (dv.x + dv.y + dv.z + dv.w < 0.12) return;  // little detail: no reliable match
 // where the camera's movement alone puts this block in the previous picture (the world held still)
 vec3 P = vec3((uv.x * 2.0 - 1.0) * p.t.x, (1.0 - uv.y * 2.0) * p.t.y, -1.0) * z;
 vec3 q = vec3(dot(p.r0.xyz, P), dot(p.r1.xyz, P), dot(p.r2.xyz, P)) + vec3(p.r0.w, p.r1.w, p.r2.w);
 bool camOk = q.z < -1e-4;
 vec2 pp = camOk ? vec2(q.x / (-q.z) / p.t.x * 0.5 + 0.5, 0.5 - q.y / (-q.z) / p.t.y * 0.5) : uv;
 camOk = camOk && all(greaterThan(pp, vec2(0.01))) && all(lessThan(pp, vec2(0.99)));
 // two places to look: around pp, and around the same place on the screen (something the camera
 // follows: Link, the boat he sails, the title's boat), each +-6 quarter-size pixels, then refined
 float best = 1e9, atCam = 1e9; vec2 bp = uv; vec4 b[4];
 for (int h = 0; h < 2; h++) {
  vec2 c = h == 0 ? pp : uv;
  if (h == 0 && !camOk) continue;
  for (int oy = -6; oy <= 6; oy += 2) for (int ox = -6; ox <= 6; ox += 2) {
   vec2 at = c + vec2(ox, oy) * ts;
   if (any(lessThan(at, vec2(0.01))) || any(greaterThan(at, vec2(0.99)))) continue;
   block(prevImg, at, ts, b); float sd = sad(cur, b);
   if (h == 0 && ox == 0 && oy == 0) atCam = sd;
   if (sd < best) { best = sd; bp = at; }
  }
 }
 vec2 c0 = bp;
 for (int oy = -1; oy <= 1; oy++) for (int ox = -1; ox <= 1; ox++) {
  if (ox == 0 && oy == 0) continue;
  vec2 at = c0 + vec2(ox, oy) * ts;
  block(prevImg, at, ts, b); float sd = sad(cur, b);
  if (sd < best) { best = sd; bp = at; }
 }
 if (best > 1.2 || atCam <= best * 1.5 + 0.05) return;  // no clear match, or the camera's movement explains it
 // the movement beyond the camera's, previous -> this frame (uv)
 result = vec4(pp - bp, zl, 1.0);
}
)glsl";
// a block keeps its movement only when at least 2 of its 8 neighbours moved the same way, and takes
// their average; lone blocks (water, flicker) are dropped. t.x: the agreement distance in the encoding
const char* kFilter = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D flowIn;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; } p;
void main() {
 vec4 c = textureLod(flowIn, uv, 0.0);
 result = vec4(0.0, 0.0, c.z, 0.0);
 if (c.w < 0.5) return;
 vec2 ts = 1.0 / vec2(textureSize(flowIn, 0));
 vec2 sum = c.xy; float n = 0.0;
 for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
  if (i == 0 && j == 0) continue;
  vec4 o = textureLod(flowIn, uv + vec2(i, j) * ts, 0.0);
  if (o.w > 0.5 && all(lessThan(abs(o.xy - c.xy), vec2(p.t.x)))) { sum += o.xy; n += 1.0; }
 }
 if (n >= 2.0) result = vec4(sum / (n + 1.0), c.z, 1.0);
}
)glsl";
// where the HUD is (WWHD: the finished TV picture differs from the scene there), one texel per 4x4 TV
// pixels: 1 if any of them differs by more than t.z
const char* kHudMask = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D scanImg;
layout(set=0,binding=1) uniform sampler2D sceneImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
void main() {
 ivec2 sz = textureSize(scanImg, 0), b = ivec2(uv * vec2(sz) / 4.0) * 4;
 float m = 0.0;
 for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
  ivec2 c = min(b + ivec2(x, y), sz - 1);
  vec3 d = abs(texelFetch(scanImg, c, 0).rgb - texelFetch(sceneImg, c, 0).rgb);
  m = max(m, max(max(d.r, d.g), d.b));
 }
 result = vec4(m > p.t.z ? 1.0 : 0.0, 0.0, 0.0, 1.0);
}
)glsl";


// The movement of each quarter-size pixel (4x4 TV pixels), chosen among three explanations of where
// it was in the previous picture: the world held still (the camera's movement alone), held on the
// screen (something the camera follows: Link, his boat, the title's boat), or its block's own
// movement (kFlow). The block movement alone tore objects apart: a block's textured part went one
// way and the rest of the same object the other (the title's dragon came out twice, trembling).
// Each is tried with the 3x3 neighbourhood in colour; the one that fits clearly better wins. Output
// (RGBA16F): the movement beyond the camera's, previous -> this frame (TV uv), log depth, 1 where
// sure, 0 where nothing tells them apart (flat colour: kSpread fills it in).
const char* kSelect = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D curImg;
layout(set=0,binding=1) uniform sampler2D prevImg;
layout(set=0,binding=2) uniform sampler2D depthImg;
layout(set=0,binding=3) uniform sampler2D flowImg;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
// the 2x2 depth texels around q, as textureGather would return them, read one by one: a gather on
// the game's depth-stencil image froze an Adreno 650 (Galaxy S20 FE) until it restarted and gave
// stray lines on others (LASRecomp-Android-Plus 08bbaef); plain fetches work on every driver
vec4 gatherDepth(vec2 q) {
 ivec2 sz = textureSize(depthImg, 0), b = ivec2(floor(q * vec2(sz) - 0.5));
 ivec2 lo = ivec2(0), hi = sz - 1;
 return vec4(texelFetch(depthImg, clamp(b + ivec2(0, 1), lo, hi), 0).r, texelFetch(depthImg, clamp(b + ivec2(1, 1), lo, hi), 0).r,
             texelFetch(depthImg, clamp(b + ivec2(1, 0), lo, hi), 0).r, texelFetch(depthImg, clamp(b, lo, hi), 0).r);
}
float lin(float d) {
 float n = p.z.x, f = p.z.y;
 return 2.0 * n * f / max((f + n) - (d * 2.0 - 1.0) * (f - n), 1e-6);
}
float err(vec2 at, vec2 from, vec2 ts) {
 float e = 0.0;
 for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
  vec2 o = vec2(i, j) * ts;
  vec3 d = abs(textureLod(curImg, at + o, 0.0).rgb - textureLod(prevImg, from + o, 0.0).rgb);
  e += d.r + d.g + d.b;
 }
 return e;
}
void main() {
 vec2 ts = 1.0 / vec2(textureSize(curImg, 0));
 vec4 g = gatherDepth(uv);
 float z = lin(min(min(g.x, g.y), min(g.z, g.w)));
 float zl = clamp(log(z / p.z.x) / log(p.z.y / p.z.x), 0.0, 1.0);
 result = vec4(0.0, 0.0, zl, 0.0);
 vec3 P = vec3((uv.x * 2.0 - 1.0) * p.t.x, (1.0 - uv.y * 2.0) * p.t.y, -1.0) * z;
 vec3 q = vec3(dot(p.r0.xyz, P), dot(p.r1.xyz, P), dot(p.r2.xyz, P)) + vec3(p.r0.w, p.r1.w, p.r2.w);
 if (q.z > -1e-4) return;
 vec2 pp = vec2(q.x / (-q.z) / p.t.x * 0.5 + 0.5, 0.5 - q.y / (-q.z) / p.t.y * 0.5);
 bool inside = all(greaterThan(pp, ts)) && all(lessThan(pp, 1.0 - ts));
 // detail: flat colour can't tell the explanations apart
 vec3 m = vec3(0.0);
 for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) m += textureLod(curImg, uv + vec2(i, j) * ts, 0.0).rgb;
 m /= 9.0;
 float detail = 0.0;
 for (int j = -1; j <= 1; j++) for (int i = -1; i <= 1; i++) {
  vec3 d = abs(textureLod(curImg, uv + vec2(i, j) * ts, 0.0).rgb - m);
  detail += d.r + d.g + d.b;
 }
 float eCam = inside ? err(uv, pp, ts) : 1e9;
 float eHold = err(uv, uv, ts);
 vec4 f = textureLod(flowImg, uv, 0.0);
 vec2 fromFlow = pp - f.xy;
 float eFlow = f.w > 0.5 && all(greaterThan(fromFlow, ts)) && all(lessThan(fromFlow, 1.0 - ts)) ? err(uv, fromFlow, ts) : 1e9;
 // the camera's explanation is the default: the others must fit clearly better
 float best = eCam; vec2 res = vec2(0.0); float second = 1e9;
 if (eHold < best * 0.8 - 0.02) { second = best; best = eHold; res = pp - uv; } else second = min(second, eHold);
 if (eFlow < best * 0.8 - 0.02) { second = min(second, best); best = eFlow; res = f.xy; } else second = min(second, eFlow);
 bool sure = detail > 0.15 && (second > best * 1.25 + 0.03);
 result = vec4(sure ? res : vec2(0.0), zl, sure ? 1.0 : 0.0);
}
)glsl";

// pixels without a sure movement take the one of sure neighbours at the same depth (5x5, every
// t.x texels; t.y: the depth tolerance in the log encoding); found: 0.5
const char* kSpread = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D selIn;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
layout(push_constant) uniform P { vec4 r0; vec4 r1; vec4 r2; vec4 t; vec4 z; vec4 rect; vec4 a; } p;
void main() {
 vec4 c = textureLod(selIn, uv, 0.0);
 result = c;
 if (c.w > 0.25) return;
 vec2 ts = p.t.x / vec2(textureSize(selIn, 0));
 vec2 sum = vec2(0.0); float n = 0.0;
 for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) {
  vec4 o = textureLod(selIn, uv + vec2(i, j) * ts, 0.0);
  if (o.w > 0.25 && abs(o.z - c.z) < p.t.y) { sum += o.xy; n += 1.0; }
 }
 if (n > 0.0) result = vec4(sum / n, c.z, 0.5);
}
)glsl";

// the depth of the frame's two 3D passes as one (the nearer of both)
const char* kDepthMerge = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D curDepth;
layout(set=0,binding=1) uniform sampler2D keptDepth;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
void main() {
 ivec2 c = ivec2(gl_FragCoord.xy);
 result = vec4(min(texelFetch(curDepth, c, 0).r, texelFetch(keptDepth, c, 0).r), 0.0, 0.0, 1.0);
}
)glsl";

// the scene at quarter size (a 4x4 average from 4 bilinear taps)
const char* kQuarter = R"glsl(#version 450
layout(set=0,binding=0) uniform sampler2D src;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 result;
void main() {
 vec2 t = 1.0 / vec2(textureSize(src, 0));
 result = 0.25 * (textureLod(src, uv + vec2(-t.x, -t.y), 0.0) + textureLod(src, uv + vec2(t.x, -t.y), 0.0) +
                  textureLod(src, uv + vec2(-t.x, t.y), 0.0) + textureLod(src, uv + vec2(t.x, t.y), 0.0));
}
)glsl";

// a small render target of the generation passes (RGBA8, GENERAL once drawn)
struct Target {
    Image img;
    bool f16 = false;  // RGBA16F (S.pass16): the objects' movement
    bool r32 = false;  // R32F (S.pass32): the merged depth
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
};

struct Push { float r0[4], r1[4], r2[4], t[4], z[4], rect[4], a[4]; };

struct Cam {
    float m[9];  // camera-to-world: columns right, up, back (the camera looks along -z)
    float eye[3];
    float target = 0;  // distance from the eye to the point the camera looks at (Link, the title's boat)
    float fovy = 40, znear = 1, zfar = 100000;
    uint64_t frame = 0;
    bool valid = false;
};

struct State {
    bool enabled = false;
    std::mutex mu;
    Cam cur, prev;
    // the window-sized pictures handed to the present thread: real copies and generated ones (3 sets:
    // record()'s images stay unchanged until it was called twice more)
    Image real[3], gen[3];
    VkImageView realView[3]{}, genView[3]{};
    VkFramebuffer genFb[3]{};
    int next = 0;
    uint32_t w = 0, h = 0;
    // the TV picture before the HUD, and the scene's depth buffer, of this frame
    Image scene;
    uint64_t sceneFrame = ~0ull;
    uint32_t depthAddr = 0, depthW = 0, depthH = 0;
    uint64_t depthFrame = ~0ull;
    // the depth of an earlier 3D pass of the frame (keep_depth), and both merged (R32F, kDepthMerge)
    Image depthKeep;
    uint64_t keepFrame = ~0ull;
    uint32_t keepAddr = 0;
    Target depthMerged;
    VkRenderPass pass32 = VK_NULL_HANDLE;
    std::vector<std::pair<const char*, VkPipeline>> pipelines32;
    VkRenderPass pass = VK_NULL_HANDLE, pass16 = VK_NULL_HANDLE;  // RGBA8 and RGBA16F targets
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;  // the warp
    std::vector<std::pair<const char*, VkPipeline>> pipelines, pipelines16;  // the other passes, by shader
    VkSampler nearest = VK_NULL_HANDLE;
    // objects' movement: the scene at quarter size (this frame's and the one before), per-block movement
    Target small[2], flowRaw, flow, hudMask, sel[2];  // sel: the movement per quarter-size pixel (kSelect)
    uint64_t smallFrame[2] = {~0ull, ~0ull};
    int smallCur = 0;
    uint64_t generated = 0, skipped = 0, withCoverage = 0;
    // the warped mesh: its pass (colour + depth), layout, pipeline, index buffer and targets
    VkRenderPass gridPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout gridDsl = VK_NULL_HANDLE;
    VkPipelineLayout gridLayout = VK_NULL_HANDLE;
    VkPipeline gridPipe = VK_NULL_HANDLE;
    bool gridFailed = false;
    VkBuffer gridIndex = VK_NULL_HANDLE;
    VmaAllocation gridIndexAlloc = nullptr;
    uint32_t gridCols = 0, gridRows = 0, gridIndices = 0;
    Image gridColor, gridDepth;
    VkImageView gridColorView = VK_NULL_HANDLE, gridDepthView = VK_NULL_HANDLE;
    VkFramebuffer gridFb = VK_NULL_HANDLE;
} S;

void basis(const float eye[3], const float center[3], const float upv[3], float m[9]) {
    float f[3] = {center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    float fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    if (fl < 1e-6f) fl = 1;
    for (float& v : f) v /= fl;
    float r[3] = {f[1] * upv[2] - f[2] * upv[1], f[2] * upv[0] - f[0] * upv[2], f[0] * upv[1] - f[1] * upv[0]};
    float rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    if (rl < 1e-6f) rl = 1;
    for (float& v : r) v /= rl;
    float u[3] = {r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
    for (int i = 0; i < 3; i++) {
        m[i * 3 + 0] = r[i];
        m[i * 3 + 1] = u[i];
        m[i * 3 + 2] = -f[i];
    }
}

VkShaderModule module(const char* src, bool vertex) {
    std::vector<uint32_t> spirv;
    std::string log;
    if (!compile_glsl(src, vertex, spirv, log)) {
        LOG("[framegen] shader failed: %s", log.c_str());
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spirv.size() * 4;
    ci.pCode = spirv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    VK_CHECK(vkCreateShaderModule(R.device, &ci, nullptr, &m));
    return m;
}

VkPipeline create_pipeline(const char* src, VkRenderPass pass = VK_NULL_HANDLE);
VkPipeline pipeline_for(const char* src);
bool make_pipeline() {
    if (S.pipeline) return true;
    VkAttachmentDescription a{};
    a.format = VK_FORMAT_R8G8B8A8_UNORM;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // read by the present thread
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    VkSubpassDependency deps[2] = {
        // the present thread's earlier read of the image, before it is written again
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0},
        // the sampled images were written by earlier passes and copies
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &S.pass));
    a.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &S.pass16));
    a.format = VK_FORMAT_R32_SFLOAT;
    VK_CHECK(vkCreateRenderPass(R.device, &rp, nullptr, &S.pass32));

    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++) b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 5;
    dci.pBindings = b;
    VK_CHECK(vkCreateDescriptorSetLayout(R.device, &dci, nullptr, &S.dsl));
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &S.dsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    VK_CHECK(vkCreatePipelineLayout(R.device, &lci, nullptr, &S.layout));

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0;
    VK_CHECK(vkCreateSampler(R.device, &si, nullptr, &S.nearest));

    S.pipeline = create_pipeline(kWarp);
    return S.pipeline != VK_NULL_HANDLE;
}

// a full-screen pass's pipeline (the shared render pass and layout) for fragment shader `src`
VkPipeline create_pipeline(const char* src, VkRenderPass pass) {
    VkShaderModule vs = module(kVertex, true), fs = module(src, false);
    if (!vs || !fs) return VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = i ? fs : vs;
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &ds;
    ci.layout = S.layout;
    ci.renderPass = pass ? pass : S.pass;
    VkPipeline pipe = VK_NULL_HANDLE;
    VK_CHECK(create_graphics_pipeline(&ci, &pipe));
    vkDestroyShaderModule(R.device, vs, nullptr);
    vkDestroyShaderModule(R.device, fs, nullptr);
    return pipe;
}

VkPipeline pipeline_for(const char* src) {
    for (auto& [k, v] : S.pipelines)
        if (k == src) return v;
    VkPipeline pl = create_pipeline(src);
    if (pl) S.pipelines.push_back({src, pl});
    return pl;
}

// the same for a pass drawn into an R32F target
VkPipeline pipeline32_for(const char* src) {
    for (auto& [k, v] : S.pipelines32)
        if (k == src) return v;
    VkPipeline pl = create_pipeline(src, S.pass32);
    if (pl) S.pipelines32.push_back({src, pl});
    return pl;
}

// the same for a pass drawn into an RGBA16F target
VkPipeline pipeline16_for(const char* src) {
    for (auto& [k, v] : S.pipelines16)
        if (k == src) return v;
    VkPipeline pl = create_pipeline(src, S.pass16);
    if (pl) S.pipelines16.push_back({src, pl});
    return pl;
}

void release_images() {
    for (int i = 0; i < 3; i++) {
        if (S.genFb[i]) vkDestroyFramebuffer(R.device, S.genFb[i], nullptr);
        if (S.genView[i]) vkDestroyImageView(R.device, S.genView[i], nullptr);
        if (S.realView[i]) vkDestroyImageView(R.device, S.realView[i], nullptr);
        if (S.gen[i].image) vmaDestroyImage(R.vma, S.gen[i].image, S.gen[i].alloc);
        if (S.real[i].image) vmaDestroyImage(R.vma, S.real[i].image, S.real[i].alloc);
        S.genFb[i] = VK_NULL_HANDLE;
        S.genView[i] = S.realView[i] = VK_NULL_HANDLE;
        S.gen[i] = S.real[i] = Image{};
    }
    S.w = S.h = 0;
}

void image_barrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags srcStage,
                   VkAccessFlags srcAccess, VkPipelineStageFlags dstStage, VkAccessFlags dstAccess) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

void release_target(Target& t) {
    if (t.fb) vkDestroyFramebuffer(R.device, t.fb, nullptr);
    if (t.view) vkDestroyImageView(R.device, t.view, nullptr);
    if (t.img.image) vmaDestroyImage(R.vma, t.img.image, t.img.alloc);
    t = Target{};
}

// a small RGBA8 target of w x h (recreated when the size changes; the GPU may still read the old one:
// it is released once the frame is done)
bool ensure_target(Target& t, uint32_t w, uint32_t h, bool f16 = false, bool r32 = false) {
    if (t.img.image && t.img.width == w && t.img.height == h && t.f16 == f16 && t.r32 == r32) return true;
    if (t.img.image) {
        Target old = t;
        on_complete([old]() mutable { release_target(old); });
        t = Target{};
    }
    if (!create_image(t.img, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D,
                      r32 ? VK_FORMAT_R32_SFLOAT : f16 ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                      w, h, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, false, false))
        return false;
    t.f16 = f16;
    t.r32 = r32;
    t.view = make_view(t.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = r32 ? S.pass32 : f16 ? S.pass16 : S.pass;
    fi.attachmentCount = 1;
    fi.pAttachments = &t.view;
    fi.width = w;
    fi.height = h;
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &t.fb));
    return true;
}

struct Input {
    VkImageView view;
    VkSampler sampler;
    VkImageLayout layout;
};
// one full-screen pass of `pipe` into framebuffer `fb` (w x h); up to 5 sampled inputs
void run_pass(VkCommandBuffer cmd, VkPipeline pipe, VkFramebuffer fb, uint32_t w, uint32_t h, const Input* in, int n, const Push& pc,
              VkRenderPass pass = VK_NULL_HANDLE) {
    VkDescriptorSet set = alloc_descriptor_set(S.dsl);
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5]{};
    for (int k = 0; k < 5; k++) {
        const Input& x = in[k < n ? k : 0];  // unused bindings get the first input
        ii[k] = {x.sampler, x.view, x.layout};
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstSet = set;
        wr[k].dstBinding = (uint32_t)k;
        wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(R.device, 5, wr, 0, nullptr);
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = pass ? pass : S.pass;
    bi.framebuffer = fb;
    bi.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.layout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, S.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}


// ---- the warped mesh (kGridVertex / kGridFragment) ----
bool make_grid_pipeline() {
    if (S.gridPipe) return true;
    if (S.gridFailed) return false;
    S.gridFailed = true;  // until it is made
    VkAttachmentDescription a[2]{};
    a[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    a[0].samples = VK_SAMPLE_COUNT_1_BIT;
    a[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    a[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    a[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    a[1] = a[0];
    a[1].format = VK_FORMAT_D16_UNORM;
    a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}, dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sp{};
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cref;
    sp.pDepthStencilAttachment = &dref;
    VkSubpassDependency deps[2] = {
        {VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
             VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT,
         VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, 0},
        {0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0}};
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 2;
    rp.pAttachments = a;
    rp.subpassCount = 1;
    rp.pSubpasses = &sp;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (vkCreateRenderPass(R.device, &rp, nullptr, &S.gridPass) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[5];
    for (uint32_t i = 0; i < 5; i++)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dci.bindingCount = 5;
    dci.pBindings = b;
    if (vkCreateDescriptorSetLayout(R.device, &dci, nullptr, &S.gridDsl) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &S.gridDsl;
    lci.pushConstantRangeCount = 1;
    lci.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(R.device, &lci, nullptr, &S.gridLayout) != VK_SUCCESS) return false;
    VkShaderModule vs = module(kGridVertex, true), fs = module(kGridFragment, false);
    if (!vs || !fs) return false;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (int i = 0; i < 2; i++) {
        stages[i].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[i].stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[i].module = i ? fs : vs;
        stages[i].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    ia.primitiveRestartEnable = VK_TRUE;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 15;
    VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    bs.attachmentCount = 1;
    bs.pAttachments = &blend;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &dss;
    ci.pColorBlendState = &bs;
    ci.pDynamicState = &ds;
    ci.layout = S.gridLayout;
    ci.renderPass = S.gridPass;
    VkResult r = create_graphics_pipeline(&ci, &S.gridPipe);
    vkDestroyShaderModule(R.device, vs, nullptr);
    vkDestroyShaderModule(R.device, fs, nullptr);
    if (r != VK_SUCCESS) {
        S.gridPipe = VK_NULL_HANDLE;
        LOG("[framegen] warped mesh pipeline failed (%d): the in-between frames are made per pixel", (int)r);
        return false;
    }
    S.gridFailed = false;
    return true;
}

void release_grid_targets() {
    if (S.gridFb) vkDestroyFramebuffer(R.device, S.gridFb, nullptr);
    if (S.gridColorView) vkDestroyImageView(R.device, S.gridColorView, nullptr);
    if (S.gridDepthView) vkDestroyImageView(R.device, S.gridDepthView, nullptr);
    if (S.gridColor.image) vmaDestroyImage(R.vma, S.gridColor.image, S.gridColor.alloc);
    if (S.gridDepth.image) vmaDestroyImage(R.vma, S.gridDepth.image, S.gridDepth.alloc);
    S.gridFb = VK_NULL_HANDLE;
    S.gridColorView = S.gridDepthView = VK_NULL_HANDLE;
    S.gridColor = S.gridDepth = Image{};
}

// the warped scene's targets at the TV picture's size, and the mesh's index buffer (strips, one per
// row of cells, joined by primitive restarts) for `cols` x `rows` vertices
bool ensure_grid(uint32_t w, uint32_t h, uint32_t cols, uint32_t rows) {
    if (!S.gridColor.image || S.gridColor.width != w || S.gridColor.height != h) {
        if (S.gridColor.image) {
            VkFramebuffer fb = S.gridFb;
            VkImageView cv = S.gridColorView, dv = S.gridDepthView;
            Image c = S.gridColor, d = S.gridDepth;
            on_complete([fb, cv, dv, c, d] {
                vkDestroyFramebuffer(R.device, fb, nullptr);
                vkDestroyImageView(R.device, cv, nullptr);
                vkDestroyImageView(R.device, dv, nullptr);
                vmaDestroyImage(R.vma, c.image, c.alloc);
                vmaDestroyImage(R.vma, d.image, d.alloc);
            });
            S.gridFb = VK_NULL_HANDLE;
            S.gridColorView = S.gridDepthView = VK_NULL_HANDLE;
            S.gridColor = S.gridDepth = Image{};
        }
        if (!create_image(S.gridColor, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, false, false) ||
            !create_image(S.gridDepth, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_D16_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, false, true, false)) {
            release_grid_targets();
            return false;
        }
        S.gridColorView = make_view(S.gridColor, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
        S.gridDepthView = make_view(S.gridDepth, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkImageView views[2] = {S.gridColorView, S.gridDepthView};
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = S.gridPass;
        fi.attachmentCount = 2;
        fi.pAttachments = views;
        fi.width = w;
        fi.height = h;
        fi.layers = 1;
        if (vkCreateFramebuffer(R.device, &fi, nullptr, &S.gridFb) != VK_SUCCESS) {
            S.gridFb = VK_NULL_HANDLE;
            return false;
        }
    }
    if (S.gridIndex && S.gridCols == cols && S.gridRows == rows) return true;
    if (S.gridIndex) {
        VkBuffer b = S.gridIndex;
        VmaAllocation al = S.gridIndexAlloc;
        on_complete([b, al] { vmaDestroyBuffer(R.vma, b, al); });
        S.gridIndex = VK_NULL_HANDLE;
    }
    std::vector<uint32_t> idx;
    idx.reserve((size_t)2 * cols * (rows - 1) + rows);
    for (uint32_t y = 0; y + 1 < rows; y++) {
        for (uint32_t x = 0; x < cols; x++) {
            idx.push_back(y * cols + x);
            idx.push_back((y + 1) * cols + x);
        }
        idx.push_back(0xFFFFFFFFu);
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = idx.size() * 4;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    ai.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(R.vma, &bi, &ai, &S.gridIndex, &S.gridIndexAlloc, &info) != VK_SUCCESS || !info.pMappedData) {
        S.gridIndex = VK_NULL_HANDLE;
        return false;
    }
    std::memcpy(info.pMappedData, idx.data(), idx.size() * 4);
    vmaFlushAllocation(R.vma, S.gridIndexAlloc, 0, VK_WHOLE_SIZE);
    S.gridCols = cols;
    S.gridRows = rows;
    S.gridIndices = (uint32_t)idx.size();
    return true;
}

// draws the warped mesh into the grid targets
void run_grid(VkCommandBuffer cmd, const Input* in, const Push& pc) {
    VkDescriptorSet set = alloc_descriptor_set(S.gridDsl);
    VkDescriptorImageInfo ii[5];
    VkWriteDescriptorSet wr[5]{};
    for (int k = 0; k < 5; k++) {
        ii[k] = {in[k].sampler, in[k].view, in[k].layout};
        wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        wr[k].dstSet = set;
        wr[k].dstBinding = (uint32_t)k;
        wr[k].descriptorCount = 1;
        wr[k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr[k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(R.device, 5, wr, 0, nullptr);
    const uint32_t w = S.gridColor.width, h = S.gridColor.height;
    VkClearValue clears[2];
    clears[0].color = {{0, 0, 0, 0}};
    clears[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    bi.renderPass = S.gridPass;
    bi.framebuffer = S.gridFb;
    bi.renderArea = {{0, 0}, {w, h}};
    bi.clearValueCount = 2;
    bi.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp{0, 0, (float)w, (float)h, 0, 1};
    VkRect2D sc{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.gridPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, S.gridLayout, 0, 1, &set, 0, nullptr);
    vkCmdPushConstants(cmd, S.gridLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
    vkCmdBindIndexBuffer(cmd, S.gridIndex, 0, VK_INDEX_TYPE_UINT32);
    vkCmdDrawIndexed(cmd, S.gridIndices, 1, 0, 0, 0);
    vkCmdEndRenderPass(cmd);
}

void keep_depth_impl(Surface* s) {
    if (!S.enabled || !s || !s->isDepth || !s->img.image || S.sceneFrame == R.frame) return;
    Image& src = s->img;
    if (S.depthKeep.image && (S.depthKeep.width != src.width || S.depthKeep.height != src.height || S.depthKeep.format != src.format)) {
        retire_image(S.depthKeep);
        S.depthKeep = Image{};
    }
    if (!S.depthKeep.image &&
        !create_image(S.depthKeep, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, src.format, src.width, src.height, 1, 1, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, true, false)) {
        S.depthKeep = Image{};
        return;
    }
    prepare(src, Use::COPY_SRC);
    prepare(S.depthKeep, Use::COPY_DST);
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    r.extent = {src.width, src.height, 1};
    vkCmdCopyImage(command_buffer(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.depthKeep.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &r);
    S.keepFrame = R.frame;
    S.keepAddr = s->addr;
    static int logged = 0;
    if (logged++ < 3) LOG("[framegen] a second 3D pass in the frame: the first one's depth kept (%08X %ux%u)", s->addr, src.width, src.height);
}

// the scene's depth buffer of this frame (the TV picture's size), or null
Surface* scene_depth() {
    if (S.depthFrame != S.sceneFrame || !S.depthAddr) return nullptr;
    auto range = R.surfaces.equal_range(S.depthAddr);
    for (auto it = range.first; it != range.second; ++it) {
        Surface* s = it->second.get();
        if (s->isDepth && s->img.image && s->img.width == S.depthW && s->img.height == S.depthH) return s;
    }
    return nullptr;
}
}  // namespace

void keep_depth(Surface* s) { keep_depth_impl(s); }

bool enabled() { return S.enabled; }
void set_enabled(bool on) {
    S.enabled = on;
    LOG("[framegen] built-in 60 fps generation %s", on ? "on" : "off");
}

// render thread (OP_FRAMEGEN_CAMERA): the 3D camera of the frame being drawn
void note_camera(const float eye[3], const float center[3], const float up[3], float fovy, float znear, float zfar) {
    if (!S.enabled) return;
    std::lock_guard<std::mutex> lk(S.mu);
    if (getenv("WWHD_CAM_LOG"))
        LOG("[framegen] cam frame %llu eye %.1f %.1f %.1f center %.1f %.1f %.1f fovy %.2f near %.2f far %.0f", (unsigned long long)R.frame,
            eye[0], eye[1], eye[2], center[0], center[1], center[2], fovy, znear, zfar);
    if (S.cur.frame != R.frame) S.prev = S.cur;  // the frame's last camera counts (others: 2D, menus)
    basis(eye, center, up, S.cur.m);
    std::memcpy(S.cur.eye, eye, sizeof S.cur.eye);
    S.cur.target = std::sqrt((center[0] - eye[0]) * (center[0] - eye[0]) + (center[1] - eye[1]) * (center[1] - eye[1]) +
                             (center[2] - eye[2]) * (center[2] - eye[2]));
    S.cur.fovy = fovy;
    S.cur.znear = znear;
    S.cur.zfar = zfar;
    S.cur.frame = R.frame;
    S.cur.valid = true;
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOG("[framegen] camera: fovy %.1f near %.2f far %.1f", fovy, znear, zfar);
    }
}

// render thread (OP_LAYOUT_ROOT): a layout is about to be drawn into the target bound now. At the first
// one on the TV picture this frame, the scene is complete: copy it aside, and note its depth buffer.
void layout_root(const uint32_t* regs, bool) {
    if (!S.enabled || S.sceneFrame == R.frame) return;
    Surface* color = color_target(regs, 0);
    if (!color || !color->img.image || color->img.type != VK_IMAGE_TYPE_2D) return;
    // the TV picture: 16:9 at the console's size (the GamePad's 854x480 family is not it)
    if (color->height < 360 || color->height == 480 || std::abs((float)color->width * 9.0f / ((float)color->height * 16.0f) - 1.0f) > 0.03f)
        return;
    S.sceneFrame = R.frame;
    Image& src = color->img;
    if (S.scene.image && (S.scene.width != src.width || S.scene.height != src.height || S.scene.format != src.format)) {
        retire_image(S.scene);
        S.scene = Image{};
    }
    if (!S.scene.image &&
        !create_image(S.scene, VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, src.format, src.width, src.height, 1, 1, 1,
                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false))
        return;
    prepare(src, Use::COPY_SRC);
    prepare(S.scene, Use::COPY_DST);
    VkImageCopy r{};
    r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.extent = {src.width, src.height, 1};
    vkCmdCopyImage(command_buffer(), src.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.scene.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &r);
    hud_coverage_begin(color);  // the HUD draws from here on also write how much they cover
    // the scene's depth: the TV-sized depth buffer of its last pass (the HUD draws none)
    S.depthFrame = ~0ull;
    if (Surface* d = depth_target(regs))
        if (d->isDepth && d->img.image && d->img.width == src.width && d->img.height == src.height) {
            S.depthAddr = d->addr;
            S.depthW = d->img.width;
            S.depthH = d->img.height;
            S.depthFrame = R.frame;
        }
    if (S.depthFrame != R.frame && R.mainDepthAddr) {
        S.depthAddr = R.mainDepthAddr;
        S.depthW = src.width;
        S.depthH = src.height;
        S.depthFrame = R.frame;
    }
    static int logged = 0;
    if (logged < 2) {
        logged++;
        LOG("[framegen] scene %08X %ux%u format %d, depth %08X", color->addr, src.width, src.height, (int)src.format, S.depthAddr);
    }
}

bool resize(uint32_t w, uint32_t h) {
    release_images();
    for (int i = 0; i < 3; i++) {
        if (!create_image(S.real[i], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, false, false, false) ||
            !create_image(S.gen[i], VK_IMAGE_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, w, h, 1, 1, 1,
                          VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, false, false, false))
            return false;
        S.realView[i] = make_view(S.real[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
        S.genView[i] = make_view(S.gen[i], VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    }
    S.w = w;
    S.h = h;
    return true;
}

VkImageView view_of(const Image* img) {
    for (int i = 0; i < 3; i++) {
        if (img == &S.real[i]) return S.realView[i];
        if (img == &S.gen[i]) return S.genView[i];
    }
    return VK_NULL_HANDLE;
}

std::vector<const Image*> record(VkCommandBuffer cmd, const Image& comp, VkImageView compView, const float tvRect[4], bool generate) {
    std::vector<const Image*> out;
    if (!S.w) return out;
    if ((S.generated + S.skipped) % 600 == 599)
        LOG("[framegen] %llu in-between frames generated (%llu with the HUD's coverage), %llu shown as the real one (no 3D scene or camera)",
            (unsigned long long)S.generated, (unsigned long long)S.withCoverage, (unsigned long long)S.skipped);
    const int i = S.next;
    S.next = (S.next + 1) % 3;
    // the real picture: a copy of the window picture (the composite is drawn again next frame)
    image_barrier(cmd, S.real[i].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    image_barrier(cmd, comp.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                  VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT);
    VkImageCopy c{};
    c.srcSubresource = c.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    c.extent = {S.w, S.h, 1};
    vkCmdCopyImage(cmd, comp.image, VK_IMAGE_LAYOUT_GENERAL, S.real[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    image_barrier(cmd, S.real[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    out.push_back(&S.real[i]);
    if (!generate || !make_pipeline()) return out;

    // the generated camera: half the last rotation (D = Cprev^T Ccur, D^(1/2)) and half the last
    // movement, in the real camera's space; none across a cut or without a camera of the last two frames
    float half[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, move[3] = {0, 0, 0};
    float full[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}, fullMove[3] = {0, 0, 0};
    bool fullValid = false;
    float fovy, znear, zfar, target;
    // WWHD_FRAMEGEN_FOLLOW=0: everything moves with the camera's movement (as before)
    static const bool followOn = !getenv("WWHD_FRAMEGEN_FOLLOW") || atoi(getenv("WWHD_FRAMEGEN_FOLLOW")) != 0;
    {
        std::lock_guard<std::mutex> lk(S.mu);
        target = S.cur.target;
        fovy = S.cur.fovy;
        znear = S.cur.znear;
        zfar = S.cur.zfar;
        // the camera the scene was drawn with and the one of the frame before (R.frame has moved on by
        // the time the frame is presented)
        const bool both = S.cur.valid && S.prev.valid && S.cur.frame == S.sceneFrame && S.prev.frame + 1 == S.cur.frame;
        float step[3] = {0, 0, 0}, dist = 0;
        if (both)
            for (int k = 0; k < 3; k++) {
                step[k] = S.cur.eye[k] - S.prev.eye[k];
                dist += step[k] * step[k];
            }
        if (both && std::sqrt(dist) < 400.0f) {
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += S.cur.m[k * 3 + j] * step[k];
                move[j] = 0.5f * v;
            }
            float d[9];
            for (int a = 0; a < 3; a++)
                for (int b = 0; b < 3; b++) {
                    float s = 0;
                    for (int k = 0; k < 3; k++) s += S.prev.m[k * 3 + a] * S.cur.m[k * 3 + b];
                    d[a * 3 + b] = s;
                }
            // the flow pass: a point in this camera's space, in the previous camera's = D p + Cprev^T step
            std::memcpy(full, d, sizeof full);
            for (int j = 0; j < 3; j++) {
                float v = 0;
                for (int k = 0; k < 3; k++) v += S.prev.m[k * 3 + j] * step[k];
                fullMove[j] = v;
            }
            fullValid = true;
            const float tr = d[0] + d[4] + d[8];
            const float ang = std::acos(std::clamp((tr - 1) * 0.5f, -1.0f, 1.0f));
            if (ang > 1e-5f && ang < 0.5f) {
                float ax[3] = {d[7] - d[5], d[2] - d[6], d[3] - d[1]};
                const float l = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
                if (l > 1e-8f) {
                    for (float& v : ax) v /= l;
                    const float a = ang * 0.5f, co = std::cos(a), si = std::sin(a), t = 1 - co;
                    const float x = ax[0], y = ax[1], z = ax[2];
                    const float rm[9] = {t * x * x + co,     t * x * y - si * z, t * x * z + si * y, t * x * y + si * z, t * y * y + co,
                                         t * y * z - si * x, t * x * z - si * y, t * y * z + si * x, t * z * z + co};
                    std::memcpy(half, rm, sizeof half);
                }
            } else if (ang >= 0.5f) {
                move[0] = move[1] = move[2] = 0;  // a cut
                fullValid = false;
            }
        }
    }
    Surface* depth = scene_depth();
    const bool sceneOk = S.sceneFrame != ~0ull && S.sceneFrame + 2 >= R.frame && S.scene.image && R.tv.img.image;
    const bool depthOk = depth && sceneOk && znear > 0 && zfar > znear;
    if (!depthOk) {  // no 3D scene this frame (menus, title, loading): the generated picture is the real one
        S.skipped++;
        if (S.skipped % 600 == 1)
            LOG("[framegen] not generated: scene frame %lld (now %llu), scene image %d, depth %p (frame %lld addr %08X), camera frame %llu, near %.2f far %.1f",
                (long long)S.sceneFrame, (unsigned long long)R.frame, S.scene.image != VK_NULL_HANDLE, (void*)depth, (long long)S.depthFrame,
                S.depthAddr, (unsigned long long)S.cur.frame, znear, zfar);
        out.push_back(&S.real[i]);
        return out;
    }
    // the TV picture's size relative to its own height: its projection's aspect (the game widens it)
    const float tanY = std::tan(fovy * 0.5f * 3.14159265f / 180.0f), tanX = tanY * (float)R.tv.img.width / (float)R.tv.img.height;
    static const float hudThreshold = getenv("WWHD_FRAMEGEN_HUD") ? (float)atof(getenv("WWHD_FRAMEGEN_HUD")) : 3.0f / 255.0f;
    Push pc{{half[0], half[1], half[2], move[0]}, {half[3], half[4], half[5], move[1]}, {half[6], half[7], half[8], move[2]},
            {tanX, tanY, hudThreshold, getenv("WWHD_FRAMEGEN_TINT") ? 1.0f : 0.0f}, {znear, zfar, 1, 0}, {tvRect[0], tvRect[1], tvRect[2], tvRect[3]},
            {target, followOn ? 1.0f : 0.0f, 0, 0}};

    // sampled images: layouts the renderer tracks (recorded into this command buffer before the pass)
    prepare(depth->img, Use::SAMPLED);
    prepare(R.tv.img, Use::SAMPLED);
    prepare(S.scene, Use::SAMPLED);
    // views made per frame (the depth surface may be recreated); released once the frame is done
    VkImageView depthView = make_view(depth->img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageView scanView = make_view(R.tv.img, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    VkImageView sceneView = make_view(S.scene, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
    on_complete([depthView, scanView, sceneView] {
        vkDestroyImageView(R.device, depthView, nullptr);
        vkDestroyImageView(R.device, scanView, nullptr);
        vkDestroyImageView(R.device, sceneView, nullptr);
    });
    // a second 3D pass cleared the depth (the title's boat over Outset): both passes' depth merged
    VkImageLayout depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (S.keepFrame == S.sceneFrame && S.keepAddr == depth->addr && S.depthKeep.image && S.depthKeep.width == depth->img.width &&
        S.depthKeep.height == depth->img.height) {
        VkPipeline mergePipe = pipeline32_for(kDepthMerge);
        if (mergePipe && ensure_target(S.depthMerged, depth->img.width, depth->img.height, false, true)) {
            prepare(S.depthKeep, Use::SAMPLED);
            VkImageView keepView = make_view(S.depthKeep, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_DEPTH_BIT);
            on_complete([keepView] { vkDestroyImageView(R.device, keepView, nullptr); });
            Push none{};
            Input min_[2] = {{depthView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                             {keepView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_pass(cmd, mergePipe, S.depthMerged.fb, depth->img.width, depth->img.height, min_, 2, none, S.pass32);
            depthView = S.depthMerged.view;
            depthLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
    }
    // objects' own movement (WWHD_FRAMEGEN_OBJECTS=0: off): this frame's scene at quarter size, searched
    // in the previous frame's per 16x16 block, then filtered
    static const bool objects = !getenv("WWHD_FRAMEGEN_OBJECTS") || atoi(getenv("WWHD_FRAMEGEN_OBJECTS")) != 0;
    bool flow = false;
    VkImageView motionView = VK_NULL_HANDLE;  // the movement the generation passes read (binding 4)
    VkPipeline quarterPipe = objects ? pipeline_for(kQuarter) : VK_NULL_HANDLE;
    VkPipeline flowPipe = objects ? pipeline16_for(kFlow) : VK_NULL_HANDLE;
    VkPipeline filterPipe = objects ? pipeline16_for(kFilter) : VK_NULL_HANDLE;
    if (quarterPipe && flowPipe && filterPipe) {
        const uint32_t tw = S.scene.width, th = S.scene.height;
        const uint32_t sw = std::max(1u, tw / 4), sh = std::max(1u, th / 4);
        S.smallCur ^= 1;
        Target& cur = S.small[S.smallCur];
        Target& prev = S.small[S.smallCur ^ 1];
        if (ensure_target(cur, sw, sh) && ensure_target(S.flowRaw, (tw + 15) / 16, (th + 15) / 16, true) &&
            ensure_target(S.flow, (tw + 15) / 16, (th + 15) / 16, true)) {
            Push none{};
            Input qin[1] = {{sceneView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_pass(cmd, quarterPipe, cur.fb, sw, sh, qin, 1, none);
            S.smallFrame[S.smallCur] = S.sceneFrame;
            if (fullValid && prev.img.image && prev.img.width == sw && prev.img.height == sh &&
                S.smallFrame[S.smallCur ^ 1] + 1 == S.sceneFrame) {
                Push fp{{full[0], full[1], full[2], fullMove[0]}, {full[3], full[4], full[5], fullMove[1]}, {full[6], full[7], full[8], fullMove[2]},
                        {tanX, tanY, 0, 0}, {znear, zfar, 1, 0}, {}};
                Input fin[3] = {{cur.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                                {prev.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                                {depthView, S.nearest, depthLayout}};
                run_pass(cmd, flowPipe, S.flowRaw.fb, S.flowRaw.img.width, S.flowRaw.img.height, fin, 3, fp, S.pass16);
                Push ff{{}, {}, {}, {1.2f / (float)sw, 0, 0, 0}, {}, {}};  // ~1 quarter-size pixel
                Input gin[1] = {{S.flowRaw.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL}};
                run_pass(cmd, filterPipe, S.flow.fb, S.flow.img.width, S.flow.img.height, gin, 1, ff, S.pass16);
                flow = true;
                // the movement per quarter-size pixel (WWHD_FRAMEGEN_SELECT=0: the blocks' as they are)
                static const bool selectOn = !getenv("WWHD_FRAMEGEN_SELECT") || atoi(getenv("WWHD_FRAMEGEN_SELECT")) != 0;
                VkPipeline selPipe = selectOn ? pipeline16_for(kSelect) : VK_NULL_HANDLE;
                VkPipeline spreadPipe = selectOn ? pipeline16_for(kSpread) : VK_NULL_HANDLE;
                if (selPipe && spreadPipe && ensure_target(S.sel[0], sw, sh, true) && ensure_target(S.sel[1], sw, sh, true)) {
                    Input sin_[4] = {{cur.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                                     {prev.view, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                                     {depthView, S.nearest, depthLayout},
                                     {S.flow.view, S.nearest, VK_IMAGE_LAYOUT_GENERAL}};
                    run_pass(cmd, selPipe, S.sel[0].fb, sw, sh, sin_, 4, fp, S.pass16);
                    // spread twice (every 2, then every 4 quarter-size pixels); depth within ~8%
                    const float tol = (float)(0.08 / std::log(zfar / znear));
                    for (int k = 0; k < 2; k++) {
                        Push sp{{}, {}, {}, {k ? 4.0f : 2.0f, tol, 0, 0}, {}, {}, {}};
                        Input spin[1] = {{S.sel[k].view, S.nearest, VK_IMAGE_LAYOUT_GENERAL}};
                        run_pass(cmd, spreadPipe, S.sel[k ^ 1].fb, sw, sh, spin, 1, sp, S.pass16);
                    }
                    motionView = S.sel[0].view;
                }
            }
        }
    }
    pc.z[3] = flow ? 1.0f : 0.0f;
    if (flow && !motionView) motionView = S.flow.view;
    if (!S.genFb[i]) {
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = S.pass;
        fi.attachmentCount = 1;
        fi.pAttachments = &S.genView[i];
        fi.width = S.w;
        fi.height = S.h;
        fi.layers = 1;
        VK_CHECK(vkCreateFramebuffer(R.device, &fi, nullptr, &S.genFb[i]));
    }
    // the warped mesh (WWHD_FRAMEGEN_GRID=0: the per-pixel warp below), with the HUD's coverage of
    // this frame where every HUD draw wrote it, else its mask
    static const bool gridOn = !getenv("WWHD_FRAMEGEN_GRID") || atoi(getenv("WWHD_FRAMEGEN_GRID")) != 0;
    if (gridOn && make_grid_pipeline()) {
        VkPipeline composePipe = pipeline_for(kCompose);
        Image* cov = hud_coverage(S.sceneFrame);
        VkPipeline maskPipe2 = cov ? VK_NULL_HANDLE : pipeline_for(kHudMask);
        const uint32_t tw = S.scene.width, th = S.scene.height;
        // a vertex every 2 TV pixels at 720 lines, every 3 at 1080 (about the same on the screen)
        const uint32_t cell = std::max(2u, (th + 359) / 360);
        const float margin = 0.08f;
        const uint32_t cols = (uint32_t)((tw * (1 + 2 * margin)) / cell) + 2, rows = (uint32_t)((th * (1 + 2 * margin)) / cell) + 2;
        const uint32_t mw = std::max(1u, (R.tv.img.width + 3) / 4), mh = std::max(1u, (R.tv.img.height + 3) / 4);
        if (composePipe && ensure_grid(tw, th, cols, rows) && (cov || (maskPipe2 && ensure_target(S.hudMask, mw, mh)))) {
            VkImageView covView = VK_NULL_HANDLE;
            if (cov) {
                prepare(*cov, Use::SAMPLED);
                covView = make_view(*cov, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
                on_complete([covView] { vkDestroyImageView(R.device, covView, nullptr); });
            } else {
                Input min_[2] = {{scanView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                 {sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
                run_pass(cmd, maskPipe2, S.hudMask.fb, mw, mh, min_, 2, pc);
            }
            Push gp = pc;
            gp.rect[0] = (float)cols;
            gp.rect[1] = (float)rows;
            gp.rect[2] = margin;
            static const bool dbgOn = getenv("WWHD_FRAMEGEN_DEBUG") != nullptr;
            gp.rect[3] = dbgOn ? (float)atof(getenv("WWHD_FRAMEGEN_DEBUG")) : 0.0f;
            gp.a[2] = R.tv.srgb ? 1.0f : 0.0f;
            Input gin[5] = {{sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {depthView, S.nearest, depthLayout},
                            {sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {sceneView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {flow ? motionView : sceneView, S.nearest,
                             flow ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_grid(cmd, gin, gp);
            Push cp = pc;
            cp.a[2] = gp.a[2];
            Input cin[4] = {{compView, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                            {S.gridColorView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {cov ? covView : S.hudMask.view, R.linearClamp, cov ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL},
                            {sceneView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_pass(cmd, composePipe, S.genFb[i], S.w, S.h, cin, 4, cp);
            S.generated++;
            if (cov) S.withCoverage++;
            out.push_back(&S.gen[i]);
            return out;
        }
    }
    // the HUD's coverage of this frame (every HUD draw wrote it): the scene moves behind translucent
    // HUD parts too, and no mask pass is needed
    if (Image* cov = hud_coverage(S.sceneFrame)) {
        VkPipeline covPipe = pipeline_for(kWarpCov);
        if (covPipe) {
            prepare(*cov, Use::SAMPLED);
            VkImageView covView = make_view(*cov, VK_IMAGE_VIEW_TYPE_2D, 0, 1, {}, VK_IMAGE_ASPECT_COLOR_BIT);
            on_complete([covView] { vkDestroyImageView(R.device, covView, nullptr); });
            pc.a[2] = R.tv.srgb ? 1.0f : 0.0f;
            Input cin[5] = {{compView, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                            {depthView, S.nearest, depthLayout},
                            {covView, R.linearClamp, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                            {flow ? motionView : sceneView, S.nearest,
                             flow ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            run_pass(cmd, covPipe, S.genFb[i], S.w, S.h, cin, 5, pc);
            S.generated++;
            S.withCoverage++;
            out.push_back(&S.gen[i]);
            return out;
        }
    }
    // the HUD mask (one pass at a quarter of the TV picture's size; the warp used to compare the two
    // pictures at five places twice for every window pixel: about 7 ms a frame at 1.5x on an Adreno 650)
    VkPipeline maskPipe = pipeline_for(kHudMask);
    const uint32_t mw = std::max(1u, (R.tv.img.width + 3) / 4), mh = std::max(1u, (R.tv.img.height + 3) / 4);
    if (!maskPipe || !ensure_target(S.hudMask, mw, mh)) {
        out.push_back(&S.real[i]);
        return out;
    }
    {
        Input min_[2] = {{scanView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                         {sceneView, S.nearest, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
        run_pass(cmd, maskPipe, S.hudMask.fb, mw, mh, min_, 2, pc);
    }
    Input win[5] = {{compView, S.nearest, VK_IMAGE_LAYOUT_GENERAL},
                    {depthView, S.nearest, depthLayout},
                    {S.hudMask.view, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                    {S.hudMask.view, R.linearClamp, VK_IMAGE_LAYOUT_GENERAL},
                    {flow ? motionView : sceneView, S.nearest,
                     flow ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    run_pass(cmd, S.pipeline, S.genFb[i], S.w, S.h, win, 5, pc);
    S.generated++;
    out.push_back(&S.gen[i]);
    return out;
}

void destroy() {
    release_images();
    release_grid_targets();
    if (S.gridIndex) vmaDestroyBuffer(R.vma, S.gridIndex, S.gridIndexAlloc);
    S.gridIndex = VK_NULL_HANDLE;
    S.gridCols = S.gridRows = 0;
    for (Target* t : {&S.small[0], &S.small[1], &S.flowRaw, &S.flow, &S.hudMask, &S.sel[0], &S.sel[1], &S.depthMerged}) release_target(*t);
    if (S.depthKeep.image) {
        retire_image(S.depthKeep);
        S.depthKeep = Image{};
    }
    S.smallFrame[0] = S.smallFrame[1] = ~0ull;
    if (S.scene.image) {
        retire_image(S.scene);
        S.scene = Image{};
    }
}

}  // namespace gfx::rp
