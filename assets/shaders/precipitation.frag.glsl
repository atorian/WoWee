#version 450

// The shapes of precipitation.vert.glsl's quads: a streak, a flake, a veil of
// dust, a splash's crown and a ring on water.

layout(location = 0) in vec2 vLocal;
layout(location = 1) in vec4 vColor;
layout(location = 2) flat in float vKind;
layout(location = 3) in float vAge;
layout(location = 4) flat in float vSeed;

layout(location = 0) out vec4 outColor;

const int KIND_RAIN = 0;
const int KIND_SNOW = 1;
const int KIND_GRAINS = 2;
const int KIND_SHEETS = 3;
const int KIND_SPLASH = 4;
const int KIND_RIPPLE = 5;

float hash(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(hash(i), hash(i + vec2(1.0, 0.0)), u.x),
               mix(hash(i + vec2(0.0, 1.0)), hash(i + vec2(1.0, 1.0)), u.x), u.y);
}

void main() {
    int kind = int(vKind + 0.5);
    float a = 0.0;
    if (kind == KIND_RAIN || kind == KIND_GRAINS) {
        // Soft across, brightest toward the head, fading toward the tail.
        // The quad is only a pixel or two wide: most of it is the streak.
        float across = 1.0 - abs(vLocal.x);
        a = smoothstep(0.0, 0.5, across) * smoothstep(0.0, 0.3, vLocal.y) * (0.6 + 0.4 * vLocal.y);
    } else if (kind == KIND_SNOW) {
        float r = length(vLocal);
        a = smoothstep(1.0, 0.3, r);
    } else if (kind == KIND_SHEETS) {
        float r = length(vLocal);
        vec2 q = vLocal * 2.0 + vec2(vSeed * 17.0, vAge * 3.0);
        float n = noise(q) * 0.6 + noise(q * 2.3 + 5.0) * 0.4;
        a = smoothstep(1.0, 0.1, r) * smoothstep(0.25, 0.85, n);
    } else if (kind == KIND_SPLASH) {
        // A crown of a few droplets thrown up and out, falling back.
        float t = vAge;
        for (int i = 0; i < 5; ++i) {
            float side = (float(i) - 2.0) / 2.0 + (vSeed - 0.5) * 0.3;
            float rise = 0.6 + 0.4 * fract(vSeed * 13.0 + float(i) * 0.37);
            vec2 at = vec2(side * t * 0.9, 4.0 * t * (1.0 - t) * rise);
            float d = length((vLocal - at) * vec2(1.0, 1.4));
            a += smoothstep(0.14, 0.04, d);
        }
        // And where it struck, a flat bright mark at the foot.
        a += smoothstep(0.12, 0.0, vLocal.y) * smoothstep(1.0, 0.3, abs(vLocal.x) / max(t, 0.05)) * 0.6;
        a = min(a, 1.0) * (1.0 - t);
    } else if (kind == KIND_RIPPLE) {
        float r = length(vLocal);
        a = smoothstep(0.1, 0.0, abs(r - vAge)) * (1.0 - vAge) * step(r, 1.0);
    }
    a *= vColor.a;
    if (a < 0.003) discard;
    outColor = vec4(vColor.rgb, a);
}
