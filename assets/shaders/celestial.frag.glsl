#version 450

layout(push_constant) uniform Push {
    mat4 model;
    vec4 celestialColor; // xyz = color, w unused
    float intensity;
    float moonPhase;
    float animTime;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

float valueNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fract(sin(dot(i, vec2(127.1, 311.7))) * 43758.5453);
    float b = fract(sin(dot(i + vec2(1.0, 0.0), vec2(127.1, 311.7))) * 43758.5453);
    float c = fract(sin(dot(i + vec2(0.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    float d = fract(sin(dot(i + vec2(1.0, 1.0), vec2(127.1, 311.7))) * 43758.5453);
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main() {
    vec2 uv = TexCoord - 0.5;
    float dist = length(uv);

    // Hard circular cutoff - nothing beyond radius 0.35
    if (dist > 0.35) discard;

    // The moons only: the sun is sky_sprite's (sky_sun.hpp).
    vec3 color = push.celestialColor.rgb;
    float alpha;

    {
        // ---------------- Moon ----------------
        const float r = 0.30;              // moon disc radius in UV space
        float disc = smoothstep(r, r - 0.02, dist);
        float d = min(dist / r, 1.0);

        // Sphere normal for the disc point
        float nz = sqrt(max(1.0 - d * d, 0.0));
        vec3 n = vec3(uv / r, nz);

        // Spherical phase terminator: light direction swings around the moon
        // with the phase (0 = new, 0.5 = full).
        float ph = (push.moonPhase - 0.5) * 6.2831853;
        vec3 l = vec3(sin(ph), 0.0, cos(ph));
        float lit = smoothstep(-0.06, 0.18, dot(n, l));
        // Faint earthshine keeps the dark limb barely visible
        lit = max(lit, 0.05);

        // Cratered surface: broad maria patches + finer crater mottling.
        // Seed from the tint so the two moons get different faces.
        float seed = push.celestialColor.r * 43.0;
        vec2 suv = uv / r;
        float maria  = valueNoise(suv * 2.6 + seed);
        float detail = valueNoise(suv * 7.0 + seed * 1.7) * 0.6
                     + valueNoise(suv * 14.0 - seed) * 0.4;
        // Crater pits: darken the floors, brighten a thin high-side rim
        float pits = smoothstep(0.62, 0.85, detail);
        float rims = smoothstep(0.52, 0.62, detail) - pits;
        float surfaceShade = 1.0
            - smoothstep(0.45, 0.80, maria) * 0.30   // dark maria "seas"
            - pits * 0.22                            // crater floors
            + rims * 0.10;                           // sunlit crater rims

        // Limb darkening toward the disc edge
        float limb = mix(1.0, 0.62, d * d);

        color *= surfaceShade * limb * lit;

        // Subtle halo around the moon
        float glow = exp(-dist * dist * 60.0) * 0.18 * lit;

        alpha = max(disc, glow) * push.intensity;
        float edgeFade = 1.0 - smoothstep(r - 0.02, 0.35, dist);
        alpha *= edgeFade;
    }

    if (alpha < 0.01) discard;
    // Pre-multiply for additive blending: RGB is the light contribution
    outColor = vec4(color * alpha, alpha);
}
