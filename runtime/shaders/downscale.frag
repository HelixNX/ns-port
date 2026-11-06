#version 450
// Built-in: the game's frame shrunk to the window (a render scale above the window's size, vk.c present_locked).
// Each window pixel averages its whole footprint in the source: n x n bilinear taps spread evenly over it, at most
// one source texel apart, where a single bilinear tap (a blit) reads 2 x 2 texels and skips the rest.
layout(set = 0, binding = 0) uniform sampler2D tex;
layout(push_constant) uniform PC {
    vec4 src;       // the source rectangle (normalised: u0 v0 u1 v1; the vertex shader's)
    vec2 step;      // one window pixel, in source uv
    int n;          // taps per axis
} pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
void main() {
    vec3 sum = vec3(0.0);
    for (int y = 0; y < pc.n; y++)
        for (int x = 0; x < pc.n; x++)
            sum += textureLod(tex, uv + pc.step * ((vec2(x, y) + 0.5) / float(pc.n) - 0.5), 0.0).rgb;
    color = vec4(sum / float(pc.n * pc.n), 1.0);
}
