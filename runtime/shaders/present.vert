#version 450
layout(push_constant) uniform PC { vec4 rect; vec4 opts; } pc;
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // triangle strip corners
    uv = p * 0.5;
    vec2 ndc = pc.rect.xy + p * 0.5 * pc.rect.zw;                    // 0..1 within the window
    gl_Position = vec4(ndc * 2.0 - 1.0, 0.0, 1.0);
}
