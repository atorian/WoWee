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
#include "terrain_distance.glsli"

#define TERRAIN_MATERIAL_SET 1
#include "terrain_layers.glsli"

// The chunk's own normals, every vertex's, as a 17x17 image (ChunkMesh::
// normalGrid): outer vertices on even texels, inner on odd. The ground is lit
// from these rather than from the vertices drawn, so a far chunk drawn with a
// handful of them keeps the light and shade of all 145.
layout(set = 1, binding = 12) uniform sampler2D uChunkNormal;

// LayerUV runs over the alpha maps' texel centres, (offset * 63/8 + 0.5) / 64
// for a grid offset of 0 to 8; the normal image's texel centres for the same
// offset are (offset * 2 + 0.5) / 17.
vec3 chunkNormal(vec2 layerUV) {
    vec2 offset = (layerUV * 64.0 - 0.5) * (8.0 / 63.0);
    return normalize(texture(uChunkNormal, (offset * 2.0 + 0.5) / 17.0).xyz * 2.0 - 1.0);
}

// The far ground: the chunk's tile's colour and relief, its layers blended
// once into 32x32 texels a chunk (TerrainBake), the chunk at its cell.
layout(set = 1, binding = 13) uniform sampler2D uBakedColor;
layout(set = 1, binding = 14) uniform sampler2D uBakedRelief;

// After the vertex stage's model matrix and lights (GPUPushConstants).
layout(push_constant) uniform Surface {
    layout(offset = 80) vec4 relief;  // x = how steeply the relief is drawn, 0 for flat ground
    // x: 0 the layers, 1 the layers giving way to the bake between y and z
    // yards from the camera, 2 the bake alone. w: the chunk's cell in its
    // tile's atlas, x + 16 y.
    vec4 baked;
} surface;

// Where the point is in its tile's bake atlas.
vec2 bakedAtlasUv(vec2 layerUV) {
    float cell = surface.baked.w;
    vec2 origin = vec2(mod(cell, 16.0), floor(cell / 16.0));
    return (origin + terrainChunkLocal(layerUV)) / 16.0;
}

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) flat in uvec4 vLocalLights;

layout(location = 0) out vec4 outColor;

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

// The ground's normal turned by its textures' relief.
//
// The texture coordinates are laid along the world (TerrainMesh): u runs down
// world y and v down world x. Those two axes, laid onto the surface, are the
// directions the map's x and y tilt the normal in. Of a heightfield the turned
// normal is N - h_u T - h_v B whichever way T and B are handed, so only their
// directions need to be right.
vec3 reliefNormal(vec3 n, vec3 mapNormal, float strength) {
    const vec3 uAxis = vec3(0.0, -1.0, 0.0);
    const vec3 vAxis = vec3(-1.0, 0.0, 0.0);
    vec3 t = uAxis - n * dot(n, uAxis);
    if (dot(t, t) < 1e-6) {
        // A cliff face along u: v still lies on it.
        vec3 b = normalize(vAxis - n * dot(n, vAxis));
        t = cross(b, n);
    }
    t = normalize(t);
    vec3 b = cross(n, t);
    if (dot(b, vAxis) < 0.0) b = -b;
    vec3 turned = t * (mapNormal.x * strength) + b * (mapNormal.y * strength) + n * mapNormal.z;
    float len2 = dot(turned, turned);
    return len2 > 1e-8 ? turned * inversesqrt(len2) : n;
}

void main() {
    if (terrainCoverageThreshold() < distantTerrainWeight(FragPos)) discard;
    const vec3 receiverFace = receiverFaceNormal(FragPos);
    // The gradients every texture read below takes, here where the whole quad
    // is still there: past the discard, and inside the per-pixel branches
    // further down, implicit ones would be undefined.
    const vec2 texDx = dFdx(TexCoord), texDy = dFdy(TexCoord);
    const vec2 layerDx = dFdx(LayerUV), layerDy = dFdy(LayerUV);
    const vec2 atlasUv = bakedAtlasUv(LayerUV);
    const vec2 atlasDx = dFdx(atlasUv), atlasDy = dFdy(atlasUv);
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

    float fragDist = length(viewPos.xyz - FragPos);

    // The surface normal from the chunk's normal image, the same whichever
    // level of detail drew this pixel. `Normal`, the drawn vertices' own, is
    // what the image was made of.
    vec3 vertexNormal = chunkNormal(LayerUV);

    // The ground's colour and relief: from its layers near, from its bake
    // far, and both across the band between - pixel by pixel, so a pixel past
    // the band reads only the bake and one short of it only its layers. The
    // layers are laid from above, so a slope reads them as finely as flat
    // ground nearer by its normal's up-component, and takes the bake as much
    // later (terrainSurfaceSourceFor). The renderer's choice for the chunk
    // settles the chunks wholly on one side without the test.
    const bool relief = surface.relief.x > 0.0;
    const float share = surface.baked.x > 1.5 ? 1.0
        : surface.baked.x > 0.5 ? smoothstep(surface.baked.y, surface.baked.z, fragDist * abs(vertexNormal.z))
        : 0.0;
    vec4 finalColor = vec4(0.0);
    vec3 reliefMap = vec3(0.5, 0.5, 1.0);
    if (share < 1.0) {
        TerrainCoverage coverage = terrainCoverage(LayerUV, layerDx, layerDy);
        finalColor = terrainColor(coverage, TexCoord, texDx, texDy);
        if (relief) reliefMap = terrainRelief(coverage, TexCoord, texDx, texDy);
    }
    if (share > 0.0) {
        finalColor = mix(finalColor, textureGrad(uBakedColor, atlasUv, atlasDx, atlasDy), share);
        if (relief) {
            reliefMap = mix(reliefMap, textureGrad(uBakedRelief, atlasUv, atlasDx, atlasDy).rgb, share);
        }
    }

    vec3 norm = relief ? reliefNormal(vertexNormal, reliefMap * 2.0 - 1.0, surface.relief.x)
                       : vertexNormal;

    vec3 lightDir2 = normalize(-lightDir.xyz);
    vec3 ambient = ambientColor.rgb * finalColor.rgb;
    // Lambert, as every other surface has it. This took abs() of the angle
    // and floored it at 0.2, so a slope turned from the sun was lit as one
    // turned toward it, and hills had no shape at low sun. The ambient term
    // is what keeps the shaded side from black, as it does for the models
    // standing on it.
    float diff = max(dot(norm, lightDir2), 0.0);
    vec3 diffuse = diff * lightColor.rgb * finalColor.rgb;

    RtLight rt = rtLightAt(FragPos, normalize(receiverFace));
    float shadow = 1.0;
    if (shadowParams.x > 0.5 && rtSunVisibility(rt) > 0.0) {
        vec3 ldir = normalize(-lightDir.xyz);
        // Texture relief changes lighting, not the caster geometry. Offsetting
        // along its perturbed normal can push the lookup into the surface.
        vec3 shadowNormal = dot(vertexNormal, ldir) >= 0.0 ? vertexNormal : -vertexNormal;
        float normalOffset = shadowParams.w * 1.5 * (1.0 - abs(dot(shadowNormal, ldir)));
        vec3 biasedPos = FragPos + shadowNormal * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        vec2 receiverSlope = shadowReceiverSlope(receiverFace);
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            shadow = sampleShadowReceiverPlane(uShadowMap, proj, receiverSlope);
            shadow = mix(1.0, shadow, shadowMapEdgeFade(proj.xy * 2.0 - 1.0));
        }
    }

    shadow = rtShadow(rt, shadow);
    ambient = rtAmbient(rt, ambientColor.rgb) * finalColor.rgb;

    // The units' map and the sun's, the darker of the two, as the client
    // takes the dynamic map and the ground's baked one. Ground has no
    // edge-on fade in the client; it darkens the whole lit colour.
    if (shadow > 0.0) shadow = min(shadow, localShadowAt(FragPos, receiverFace));
    vec3 result = (ambient + diffuse) * shadowLightFactor(shadow);
    result += lightningDiffuse(FragPos, norm, finalColor.rgb);
    result += localLightContribution(FragPos, norm, finalColor.rgb, vLocalLights);

    result = applyFog(result, FragPos, fragDist);

    outColor = vec4(result, 1.0);
}
