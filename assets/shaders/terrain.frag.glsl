#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif

#include "world_lighting_frame.glsli"
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


layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) flat in uvec4 vLocalLights;

layout(location = 0) out vec4 outColor;

#include "terrain_surface_lighting.glsli"
#ifdef TERRAIN_GBUFFER
#include "surface_encoding.glsli"
layout(location=1) out uvec4 outKeys;
layout(location=2) out uvec4 outPayload;
#endif

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

#ifdef TERRAIN_GBUFFER
    outColor=vec4(finalColor.rgb,1);
    outKeys=uvec4(packSurfaceNormal(normalize(receiverFace)),packSurfaceNormal(norm),packSurfaceNormal(vertexNormal),floatBitsToUint(surface.relief.y)+1u);
    outPayload=uvec4(0,0,floatBitsToUint(gl_FragCoord.z),0);
#else
    outColor=shadeTerrainSurface(FragPos,finalColor,norm,vertexNormal,receiverFace,vLocalLights,rtLightAt(FragPos,normalize(receiverFace)));
#endif
}
