#version 450
#extension GL_EXT_nonuniform_qualifier : require

// The far ground baked: a chunk's layers and their relief blended once, as
// terrain.frag.glsl blends them, into its cell of the tile's atlas
// (TerrainBake). The texture table is bound at set 0 here and the chunk
// table at set 1; the chunk's row is pushed.
layout(push_constant) uniform Bake {
    uint chunk;
} bake;

#define TEXTURE_TABLE_SET 0
#define TERRAIN_CHUNK_SET 1
#define TERRAIN_CHUNK bake.chunk
#include "terrain_layers.glsli"

layout(location = 0) in vec2 TexCoord;
layout(location = 1) in vec2 LayerUV;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outRelief;

void main() {
    const vec2 layerDx = dFdx(LayerUV), layerDy = dFdy(LayerUV);
    const vec2 texDx = dFdx(TexCoord), texDy = dFdy(TexCoord);
    TerrainCoverage coverage = terrainCoverage(LayerUV, layerDx, layerDy);
    outColor = terrainColor(coverage, TexCoord, texDx, texDy);
    outRelief = vec4(terrainRelief(coverage, TexCoord, texDx, texDy), 1.0);
}
