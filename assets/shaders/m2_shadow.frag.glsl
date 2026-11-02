#version 450
#extension GL_EXT_nonuniform_qualifier : require

// The doodads' side of the shadow maps: a batch's alpha-keyed texture cuts
// its shadow as it cuts the batch. The texture and the test come with the
// draw's record (m2_shadow.vert), the texture by its place in the table,
// since one indirect call draws batches of many textures.
#define TEXTURE_TABLE_SET 0
#include "texture_table.glsli"

layout(location = 0) in vec2 TexCoord;
// x the texture's handle, y bit 0 sample it, bit 1 alpha-test it, z where
// the test cuts in 255ths (0: a half).
layout(location = 1) flat in uvec4 vDraw;

void main() {
    if ((vDraw.y & 1u) != 0u) {
        vec4 texColor = textureLod(tablePlain2D(vDraw.x), TexCoord, 0.0);
        float alphaRef = vDraw.z > 0u ? float(vDraw.z) / 255.0 : 0.5;
        if ((vDraw.y & 2u) != 0u && texColor.a < alphaRef) discard;
    }
}
