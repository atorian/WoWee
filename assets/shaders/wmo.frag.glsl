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

#include "lightning_light.glsli"

#include "rt_lighting.glsli"
#include "contact_shadows.glsli"
#include "sky_atmosphere.glsli"
#include "aerial_frame.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

// Resource-only material. Frame settings are captured in the command stream.
layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    int light;
    int transitionLight;
    float specularIntensity;
    float heightMapVariance;
    int isLava;
    int emissive;
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int hasVertexColors;
    int receivesExteriorShadows;
    float sidnR;
    float sidnG;
    float sidnB;
};
layout(push_constant) uniform SurfaceOptions {
    layout(offset = 96) vec4 geometry; // normal mapping, POM, samples, normal strength
} surfaceOptions;
const float pomScale = 0.012;

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;
layout(location = 6) flat in uvec4 vLocalLights;

layout(location = 0) out vec4 outColor;

// WoW.exe's lighting modes, as WmoLight numbers them.
const int LIGHT_VERTEX_COLOUR = 0;   // the vertex colours are the light
const int LIGHT_SUN = 1;             // the zone's ambient and sun
const int LIGHT_WINDOW = 2;          // the window light of an F_WINDOW material
const int LIGHT_BUILDING_AMBIENT = 3; // the building's MOHD ambient, no sun

// One texel of the shadow map, handed in by the renderer. The map is 512,
// 1024, 2048 or 4096 a side by the quality setting; this used to be a
// constant for 4096, so at 512 the filter taps all landed inside one texel
// and the bias shrank eightfold. The fallback covers a per-frame block that
// never filled the slot in, such as the character preview's.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

#include "local_lights.glsli"
#include "shadow_receiver.glsli"

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

#include "parallax_occlusion.glsli"

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts and torch glow
// included.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    float fogFactor = aerialVisibilityAt(worldPos, dist);
    color = mix(aerialColorAt(worldPos - viewPos.xyz), color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

#include "wmo_surface_lighting.glsli"

void main() {
    const vec3 receiverFace = receiverFaceNormal(FragPos);
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Lava/magma: scroll UVs for flowing effect
    if (isLava != 0) {
        float time = fogParams.z;
        // Scroll both axes - pools get horizontal flow, waterfalls get vertical flow
        // (UV orientation depends on mesh, so animate both)
        finalUV += vec2(time * 0.04, time * 0.06);
    }

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    if (surfaceOptions.geometry.y > 0.5 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(uNormalHeightMap, TexCoord, viewDirTS, lodFactor,
                                       int(surfaceOptions.geometry.z), pomScale);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);
    // A WMO material's alpha key cuts at 224/255, as the original client's
    // does, not at a half. Dalaran's lattice windows (DAL_WINDOWCUTOUT) leave
    // their holes at alpha 0.5-0.6; cut at a half, they drew as solid panes.
    if (alphaTest != 0 && texColor.a < 224.0 / 255.0) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (surfaceOptions.geometry.x > 0.5 && lodFactor < 0.99 && surfaceOptions.geometry.w > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(surfaceOptions.geometry.w, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    WmoLightingMaterial material = WmoLightingMaterial(light, transitionLight,
        specularIntensity, vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB),
        vec3(sidnR, sidnG, sidnB), hasVertexColors, receivesExteriorShadows,
        emissive, isLava);
    WmoSurface surface = WmoSurface(FragPos, texColor, norm, vertexNormal,
        receiverFace, VertColor, vLocalLights);
    RtLight rt = rtLightAt(FragPos, normalize(receiverFace));
    vec3 result = shadeWmoSurface(surface, material, rt);

    float dist = length(viewPos.xyz - FragPos);

    float alpha = texColor.a;

    result = applyFog(result, FragPos, dist);
    outColor = vec4(result, alpha);
}
