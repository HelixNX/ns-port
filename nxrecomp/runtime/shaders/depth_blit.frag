#version 450
// Built-in: copies depth through the sampler (nearest), for depth blits the device cannot do (AMD: no depth format
// is a blit destination)
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) in vec2 uv;
void main() { gl_FragDepth = textureLod(src, uv, 0.0).r; }
