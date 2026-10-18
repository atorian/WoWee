#version 450

// A terrain chunk laid flat over its cell of the tile's bake atlas
// (TerrainBake): the viewport is the cell, and each vertex lands where it is
// across the chunk. The texture coordinates go through as the ground has
// them, so over 32 texels a chunk the layers are read at the mip a pixel that
// size would read them at.
layout(location = 2) in vec2 aTexCoord;
layout(location = 3) in vec2 aLayerUV;

layout(location = 0) out vec2 TexCoord;
layout(location = 1) out vec2 LayerUV;

void main() {
    // terrainChunkLocal (terrain_layers.glsli), which this stage does not
    // include for the material's samplers it declares.
    vec2 local = (aLayerUV * 64.0 - 0.5) * (8.0 / 63.0) / 8.0;
    TexCoord = aTexCoord;
    LayerUV = aLayerUV;
    gl_Position = vec4(local * 2.0 - 1.0, 0.0, 1.0);
}
