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
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 fogSkyTop;  // rgb = zenith, w = sky tint amount (0 disables it)
    vec4 fogSkyMiddle;
    vec4 fogSkyBand1;
    vec4 fogSkyBand2;
    mat4 contactSpaceMatrix;  // read by contact_shadows.glsli
    vec4 contactParams;
    vec4 windDirLean;
    vec4 windPhases;
    vec4 windTravel;
    vec4 skySmogColor;
    vec4 aerialSun;
    vec4 aerialLight;
    vec4 terrainLodParams;
    vec4 aerialParams;
    mat4 lightningMatrix;
    vec4 lightningPosition;
    vec4 lightningColor;
};

layout(set = 1, binding = 0) uniform sampler2D uTexture;

// M2ParticlePush in m2_renderer_internal.h.
layout(push_constant) uniform Push {
    vec2 tileCount;
    float alphaRef;
    int fogMode;  // M2FogMode
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;
layout(location = 3) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + vUV) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    // As the client draws it (Combiners_Mod): the texture times the
    // particle's colour, alpha tested against the blend mode's reference.
    // Nothing is premultiplied, so NoAlphaAdd, Mod and Mod2x, which blend
    // without the source alpha, show no fade. There is no edge falloff; the
    // texture's own alpha shapes the particle. A luminance key used to stand
    // in for AlphaKey's test, and a circular mask cut the corners off every
    // quad, square flipbook cells included.
    float alpha = texColor.a * vColor.a;
    if (alpha < push.alphaRef) discard;
    vec3 rgb = texColor.rgb * vColor.rgb;
    // Fog by blend mode: towards the fog's colour, or towards the colour
    // that leaves what is behind unchanged for the additive and modulating
    // modes.
    if (push.fogMode == 1) rgb = mix(fogColor.rgb, rgb, vFogVisibility);
    else if (push.fogMode == 2) rgb *= vFogVisibility;
    else if (push.fogMode == 3) rgb = mix(vec3(1.0), rgb, vFogVisibility);
    else if (push.fogMode == 4) rgb = mix(vec3(0.5), rgb, vFogVisibility);
    outColor = vec4(rgb, alpha);
}
