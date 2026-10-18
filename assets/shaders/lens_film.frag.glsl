#version 450

// What covers the whole lens (LensWeather): frost creeping in from the edges
// in snow, and the dusty film a sandstorm leaves, with grit-laden streaks of
// it blowing across in the wind. Drawn over the finished frame ahead of the
// drops and the interface; the frame itself, half size, is what the frost
// blurs.

layout(set = 0, binding = 0) uniform sampler2D uFrame;

layout(push_constant) uniform Push {
    vec4 film;  // x frost, y dust, z width / height, w time (s)
    vec4 wind;  // xy where the wind blows across the screen, screen heights a second
} pc;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

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

float fbm(vec2 p) {
    float sum = 0.0;
    float amp = 0.5;
    for (int i = 0; i < 4; ++i) {
        sum += noise(p) * amp;
        p = p * 2.03 + 7.1;
        amp *= 0.5;
    }
    return sum;
}

void main() {
    float aspect = pc.film.z;
    vec2 centred = (TexCoord - 0.5) * vec2(aspect, 1.0);
    vec3 colour = vec3(0.0);
    float alpha = 0.0;

    float frost = pc.film.x;
    if (frost > 0.001) {
        // In from the corners and edges, ragged, further as it builds.
        float edge = length(centred / vec2(aspect * 0.5, 0.5));
        float ragged = fbm(TexCoord * vec2(aspect, 1.0) * 6.0);
        // Never past the corners and the rim: the middle of the view stays
        // clear however long it snows.
        float reach = mix(1.3, 0.85, frost);
        float mask = smoothstep(reach, reach + 0.3, edge + (ragged - 0.5) * 0.35);
        // Frosted glass: the frame behind it scattered and whitened, and the
        // crystals' own fine grain over it.
        vec2 grain = TexCoord * vec2(aspect, 1.0);
        vec2 scatter = (vec2(fbm(grain * 25.0), fbm(grain * 25.0 + 13.0)) - 0.5) * 0.012;
        vec3 behind = textureLod(uFrame, TexCoord + scatter, 2.0).rgb;
        float crystals = fbm(grain * 18.0);
        vec3 frosted = mix(behind, vec3(0.86, 0.92, 1.0) * (0.6 + 0.4 * crystals), 0.5);
        float a = mask * min(frost * 1.5, 1.0) * 0.75;
        colour = frosted;
        alpha = a;
    }

    float dust = pc.film.y;
    if (dust > 0.001) {
        // A film of fine sand, thicker where streaks of it blow past.
        vec2 windDir = length(pc.wind.xy) > 1e-3 ? normalize(pc.wind.xy) : vec2(1.0, 0.0);
        // Turned so they run with the wind, half again as long as wide -
        // stretched further the noise's cells show as blocks.
        vec2 q = centred * 2.5;
        vec2 along = vec2(dot(q, windDir), dot(q, vec2(-windDir.y, windDir.x)) * 1.5);
        float streaks = fbm(along - vec2(pc.film.w * length(pc.wind.xy) * 1.5, 0.0));
        vec3 behind = texture(uFrame, TexCoord).rgb;
        float lum = dot(behind, vec3(0.299, 0.587, 0.114));
        vec3 sand = vec3(0.80, 0.66, 0.46) * (0.45 + 0.75 * lum);
        float a = dust * (0.2 + 0.4 * streaks);
        colour = mix(colour, sand, a / max(alpha + a, 1e-3));
        alpha = alpha + a * (1.0 - alpha);
    }

    if (alpha < 0.002) discard;
    outColor = vec4(colour, min(alpha, 0.95));
}
