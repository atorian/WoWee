#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
};

layout(push_constant) uniform Push {
    mat4 model;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    // The rest belongs to the fragment stage, and is spelled the same way here
    // so the two declarations of one range agree. This said `float brightness`
    // where the fragment stage has a vec2, which is a field that does not exist
    // sitting at another field's offset - harmless only for as long as nobody
    // reads it.
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float meshStep;   // yards between the mesh's vertices
    float pad1, pad2;
} push;

#include "gerstner_waves.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec2 RestPos;   // where this point lies on the still surface
layout(location = 2) out vec2 TexCoord;
layout(location = 4) out vec2 ScreenUV;

/// How much of a wave the mesh can carry: all of it from three vertices a
/// wavelength, none at two. At two the vertices land on crest and trough
/// alike and the wave stops being a wave; below that each one samples it at a
/// phase that drifts slowly from vertex to vertex, and the sea showed a large
/// slow plaid that was nothing but the mesh's spacing beating against the
/// short waves. The fragment stage lights the surface by those waves instead.
float meshCarries(SwellWave w) {
    float verticesPerWave = 6.28318 / (w.freq * max(push.meshStep, 1e-3));
    return smoothstep(2.0, 3.0, verticesPerWave);
}

void main() {
    float time = fogParams.z;
    vec4 worldPos = push.model * vec4(aPos, 1.0);
    vec2 rest = worldPos.xy;

    vec3 displacement = vec3(0.0);
    if (push.liquidBasicType >= 1.5) {
        // Magma and slime: a slow undulation, far longer than the mesh's step.
        float amp = push.waveAmp, freq = push.waveFreq, spd = push.waveSpeed;
        float wave = sin(rest.x * freq * 0.5 + time * spd * 0.3) * 0.4
                   + sin(rest.y * freq * 0.3 + time * spd * 0.5) * 0.3;
        displacement.z = wave * amp * 0.5;
    } else {
        Swell swell = noSwell();
        for (int i = 0; i < kSwellWaves; ++i) {
            SwellWave w = swellWave(i);
            addSwellWave(swell, w, meshCarries(w), rest, time);
        }
        displacement = swell.displacement;
    }
    worldPos.xyz += displacement;

    // Player interaction ripples - concentric waves emanating from player position
    vec2 rippleOrigin = playerPos.xy;
    float rippleStrength = fogParams.w;
    float d = length(worldPos.xy - rippleOrigin);
    float ripple = rippleStrength * 0.12 * exp(-d * 0.12) * sin(d * 2.5 - time * 6.0);
    worldPos.z += ripple;

    FragPos = worldPos.xyz;
    RestPos = rest;
    TexCoord = aTexCoord;
    vec4 clipPos = projection * view * worldPos;
    gl_Position = clipPos;
    vec2 ndc = clipPos.xy / max(clipPos.w, 1e-5);
    ScreenUV = ndc * 0.5 + 0.5;
}
