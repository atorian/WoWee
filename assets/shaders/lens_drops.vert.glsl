#version 450

// One quad per thing on the lens (LensDrops): a water drop - drawn out above
// itself while it runs - a snowflake, or a grain of sand along its way.

struct Drop {
    vec4 posRadius;  // xy screen uv, z radius in screen heights, w opacity
    vec4 params;     // x kind (0 water, 1 flake, 2 grit), y how fast it runs, z seed, w heading (radians)
};

layout(std430, set = 0, binding = 1) readonly buffer Drops {
    Drop drops[];
};

layout(push_constant) uniform Push {
    vec4 film;  // x frost, y dust, z width / height, w time (s)
    vec4 wind;
} pc;

layout(location = 0) out vec2 vLocal;
layout(location = 1) flat out vec4 vDrop;    // centre uv, radius, opacity
layout(location = 2) flat out vec4 vParams;
layout(location = 3) out vec2 vUv;

const vec2 kCorners[6] = vec2[](vec2(-1.0, -1.0), vec2(1.0, -1.0), vec2(-1.0, 1.0),
                                vec2(-1.0, 1.0), vec2(1.0, -1.0), vec2(1.0, 1.0));

void main() {
    Drop d = drops[gl_InstanceIndex];
    vec2 corner = kCorners[gl_VertexIndex % 6];
    float aspect = pc.film.z;
    int kind = int(d.params.x + 0.5);
    // Out of focus, a drop or a flake spreads over a wider circle than it
    // covers; a grain is too small to spread.
    float radius = d.posRadius.z * (kind == 2 ? 1.0 : 1.6);

    // In screen heights, x right and y down, about the drop's centre.
    vec2 offset;
    vec2 local = corner;
    if (kind == 0) {
        // A running drop trails a thinning tail above it.
        float tail = clamp(d.params.y * 3.0, 0.0, 1.5);
        float top = 1.0 + tail * 2.0;
        local = vec2(corner.x, corner.y < 0.0 ? -top : 1.0);
        offset = local * radius;
    } else if (kind == 2) {
        // A grain along its way: a short streak.
        vec2 dir = vec2(cos(d.params.w), sin(d.params.w));
        vec2 across = vec2(-dir.y, dir.x);
        offset = dir * corner.x * radius * 4.0 + across * corner.y * radius;
    } else {
        offset = corner * radius;
    }
    vec2 uv = d.posRadius.xy + vec2(offset.x / aspect, offset.y);
    vLocal = local;
    vDrop = d.posRadius;
    vParams = d.params;
    vUv = uv;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
