#version 450
// Built-in: a full-screen triangle over the destination's viewport; uv runs over the source rectangle (normalised)
layout(push_constant) uniform PC { vec4 src; } pc;     // u0 v0 u1 v1
layout(location = 0) out vec2 uv;
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);   // (0,0) (2,0) (0,2)
    uv = mix(pc.src.xy, pc.src.zw, p);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
