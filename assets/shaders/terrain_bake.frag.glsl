#version 450

// The far ground baked: a chunk's layers and their relief blended once, as
// terrain.frag.glsl blends them, into its cell of the tile's atlas
// (TerrainBake). The chunk's material set is bound at set 0 here.
#define TERRAIN_MATERIAL_SET 0
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
