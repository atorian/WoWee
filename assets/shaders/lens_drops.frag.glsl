#version 450

// What is on the lens is far nearer than anything the camera is focused on,
// and so out of focus: a water drop is a soft blot that shows what is behind
// it blurred and a little turned about, brighter in the middle and dimmer at
// its edge, with no hard rim. A flake is soft white crystal, and blurs into a
// blot of water as it melts. A grain of sand is a dark speck.
//
// The frame is read from its copy's lower levels (LensWeather), which is the
// blur.

layout(set = 0, binding = 0) uniform sampler2D uFrame;

layout(push_constant) uniform Push {
    vec4 film;  // x frost, y dust, z width / height, w time (s)
    vec4 wind;
} pc;

layout(location = 0) in vec2 vLocal;
layout(location = 1) flat in vec4 vDrop;
layout(location = 2) flat in vec4 vParams;
layout(location = 3) in vec2 vUv;

layout(location = 0) out vec4 outColor;

void main() {
    int kind = int(vParams.x + 0.5);
    float opacity = vDrop.w;
    float aspect = pc.film.z;
    float seed = vParams.z;

    if (kind == 0) {
        // The head is a disc - not a perfect one; the tail a running drop
        // leaves above it narrows to nothing.
        vec2 p = vLocal;
        float top = 1.0 + clamp(vParams.y * 3.0, 0.0, 1.5) * 2.0;
        vec2 q;
        float trail = 1.0;
        if (p.y >= 0.0 || length(p) <= 1.0) {
            float wobble = 1.0 + 0.08 * sin(atan(p.y, p.x) * 3.0 + seed * 30.0);
            q = p * wobble;
        } else {
            // A thin wet track, thinning and fading toward where it began.
            float t = clamp((-p.y - 0.8) / max(top - 0.8, 1e-3), 0.0, 1.0);
            float w = mix(0.35, 0.08, t);
            q = vec2(p.x / w, 0.0);
            trail = 0.6 * (1.0 - t);
        }
        float d = length(q);
        if (d > 1.0) discard;
        // The bigger the drop, the more of the frame its blur spreads over.
        float blur = clamp(2.2 + vDrop.z * 40.0, 2.0, 4.0);
        // Through it: the frame round its centre, a little turned over.
        vec2 centre = vDrop.xy;
        vec2 through = clamp(centre - vec2(q.x / aspect, q.y) * vDrop.z * 3.5, vec2(0.001), vec2(0.999));
        vec3 behind = textureLod(uFrame, through, blur).rgb;
        // And what is there anyway, as blurred, which the drop's edge melts
        // into.
        vec3 around = textureLod(uFrame, vUv, blur).rgb;
        vec3 colour = mix(around, behind, 0.9 * (1.0 - smoothstep(0.45, 1.0, d)));
        // Brighter where it gathers the light, a soft dimmer ring where its
        // edge bends the light away.
        colour *= 1.14 - 0.34 * smoothstep(0.4, 0.95, d);
        colour += vec3(0.1) * smoothstep(0.6, 0.0, length(q - vec2(-0.25, -0.3)));
        float soft = 1.0 - smoothstep(0.4, 1.0, d);
        outColor = vec4(colour, soft * opacity * 0.95 * trail);
    } else if (kind == 1) {
        // Six soft arms, white over what is behind; melting, the arms go
        // first and what is left is a blot of water, as blurred as a drop.
        float melt = clamp(vParams.y, 0.0, 1.0);
        vec2 p = vLocal;
        float r = length(p);
        if (r > 1.0) discard;
        float angle = atan(p.y, p.x) + seed * 6.2831853;
        float arms = pow(abs(cos(angle * 3.0)), 4.0) * (1.0 - melt) * (1.0 - melt);
        float crystal = smoothstep(1.0, 0.1, r) * (0.3 + 0.7 * arms) + smoothstep(0.45, 0.0, r);
        float blot = 1.0 - smoothstep(0.35, 1.0, r);
        float body = mix(crystal, blot, melt);
        float blur = 1.0 + 2.5 * melt;
        vec3 behind = textureLod(uFrame, vUv, blur).rgb;
        vec3 white = mix(behind, vec3(0.94, 0.96, 1.0), 0.7);
        vec3 colour = mix(white, behind * 1.05, melt);
        outColor = vec4(colour, clamp(body, 0.0, 1.0) * opacity * 0.85);
    } else {
        float d = length(vLocal);
        if (d > 1.0) discard;
        outColor = vec4(vec3(0.42, 0.33, 0.22), (1.0 - d) * opacity * 0.8);
    }
}
