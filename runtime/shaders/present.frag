#version 450
layout(push_constant) uniform PC { vec4 rect; vec4 opts; } pc;
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
// Sharp upscaling (opts.y = strength of the extra sharpening; 0 = bilinear, the classic view):
// Catmull-Rom bicubic in 5 bilinear taps, sharpened against the bilinear picture and kept within
// the 2x2 source texels around the pixel, so edges get crisper without halos. Only where the
// picture is enlarged.
vec3 upscale(vec2 uv, float sharp) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 pos = uv * size;
    vec2 tc = floor(pos - 0.5) + 0.5;
    vec2 f = pos - tc;
    vec2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    vec2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    vec2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    vec2 w3 = f * f * (-0.5 + 0.5 * f);
    vec2 w12 = w1 + w2;
    vec2 t0 = (tc - 1.0) / size, t3 = (tc + 2.0) / size, t12 = (tc + w2 / w12) / size;
    float a = w12.x * w0.y, b = w0.x * w12.y, m = w12.x * w12.y, d = w3.x * w12.y, e = w12.x * w3.y;
    vec3 c = textureLod(tex, vec2(t12.x, t0.y), 0.0).rgb * a + textureLod(tex, vec2(t0.x, t12.y), 0.0).rgb * b +
             textureLod(tex, t12, 0.0).rgb * m + textureLod(tex, vec2(t3.x, t12.y), 0.0).rgb * d +
             textureLod(tex, vec2(t12.x, t3.y), 0.0).rgb * e;
    c /= a + b + m + d + e;
    c += sharp * (c - textureLod(tex, uv, 0.0).rgb);
    ivec2 i = clamp(ivec2(tc - 0.5), ivec2(0), ivec2(size) - 2);
    vec3 p0 = texelFetch(tex, i, 0).rgb, p1 = texelFetch(tex, i + ivec2(1, 0), 0).rgb;
    vec3 p2 = texelFetch(tex, i + ivec2(0, 1), 0).rgb, p3 = texelFetch(tex, i + ivec2(1, 1), 0).rgb;
    return clamp(c, min(min(p0, p1), min(p2, p3)), max(max(p0, p1), max(p2, p3)));
}
// Pixel perfect (opts.z = 1): each source texel a hard-edged block of the same size; only the screen
// pixel on a block's edge is blended (one bilinear tap pulled onto the texel centre), so blocks stay
// even and don't shimmer at scales that aren't whole numbers. d = source texels per screen pixel.
vec3 pixels(vec2 uv, vec2 d) {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 pos = uv * size - 0.5;
    vec2 t = clamp((fract(pos) - 0.5) / d + 0.5, 0.0, 1.0);
    return textureLod(tex, (floor(pos) + 0.5 + t) / size, 0.0).rgb;
}
void main() {
    vec2 size = vec2(textureSize(tex, 0));
    vec2 d = max(vec2(abs(dFdx(uv.x)), abs(dFdy(uv.y))) * size, vec2(1e-4));  // source texels per screen pixel
    bool enlarged = max(d.x, d.y) < 0.95;
    vec3 c = enlarged && pc.opts.z > 0.5 ? pixels(uv, d)
           : enlarged && pc.opts.y > 0.0 ? upscale(uv, pc.opts.y) : texture(tex, uv).rgb;
    c = clamp(c, 0.0, 1.0);
    // opts.x: the scan buffer is sRGB, i.e. the display encodes (the window surface is UNORM)
    if (pc.opts.x > 0.5) c = mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
    color = vec4(c, 1.0);
}
