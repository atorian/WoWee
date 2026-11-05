#version 450
#extension GL_EXT_nonuniform_qualifier : require

// A character's side of the shadow maps: an alpha-keyed batch casts the
// shape of its texture, cut at a half; everything else casts solid. The
// texture comes with the draw's record (character_shadow.vert) by its place
// in the table, since one indirect call draws batches of many textures.
#define TEXTURE_TABLE_SET 0
#include "texture_table.glsli"

layout(location = 0) in vec2 TexCoord;
// x the texture's handle, y bit 0 sample it, bit 1 alpha-test it.
layout(location = 1) flat in uvec4 vDraw;

void main() {
    if ((vDraw.y & 1u) != 0u) {
        vec4 texColor = texture(tablePlain2D(vDraw.x), TexCoord);
        if ((vDraw.y & 2u) != 0u && texColor.a < 0.5) discard;
    }
}
