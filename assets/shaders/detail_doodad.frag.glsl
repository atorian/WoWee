#version 450

// Ground clutter: its texture, alpha-tested at half as the client does
// (detailDoodadAlpha, 128), against the fade toward groundEffectDist. Lit
// with the normal of the ground it grows from, so a tuft is as bright as the
// earth around it, and darkened in shadow by the client's 0.7 + 0.3 * shadow.

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

#include "lightning_light.glsli"
#include "sky_atmosphere.glsli"
#include "aerial_frame.glsli"

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vTexCoord;
layout(location = 3) in float vFade;

layout(location = 0) out vec4 outColor;

float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

#include "shadow_receiver.glsli"
#include "shadow_shading.glsli"

// The air between the camera and this point, out of the fog volume. See
// VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// Fogged as the ground under it is.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    if (physicalAtmosphere()) {
        vec4 air = worldAtmosphere(worldPos);
        return color*air.a+air.rgb;
    }
    float fogFactor = aerialVisibilityAt(worldPos, dist);
    color = mix(aerialColorAt(worldPos - viewPos.xyz), color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

void main() {
    // Before any discard: the quad's derivatives are undefined after one.
    const vec3 face = receiverFaceNormal(vWorldPos);
    vec4 tex = texture(uTexture, vTexCoord);
    if (tex.a * vFade < 0.5) discard;

    vec3 n = normalize(vNormal);
    vec3 ldir = normalize(-lightDir.xyz);
    float diffuse = max(dot(n, ldir), 0.0);

    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec4 ls = lightSpaceMatrix * vec4(vWorldPos, 1.0);
        vec3 proj = ls.xyz / ls.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (all(greaterThanEqual(proj, vec3(0.0))) && all(lessThanEqual(proj, vec3(1.0)))) {
            shadow = sampleShadowReceiverPlane(uShadowMap, proj, shadowReceiverSlope(face));
            shadow = mix(1.0, shadow, shadowMapEdgeFade(proj.xy * 2.0 - 1.0));
        }
    }

    vec3 lit = tex.rgb * (ambientColor.rgb + lightColor.rgb * diffuse) * shadowLightFactor(shadow);
    lit += lightningDiffuse(vWorldPos, n, tex.rgb);
    lit = applyFog(lit, vWorldPos, length(viewPos.xyz - vWorldPos));
    outColor = vec4(lit, 1.0);
}
