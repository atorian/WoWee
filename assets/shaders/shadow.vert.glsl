#version 450

// The caster side of the shadow map, for terrain, buildings and doodads.
//
// The wind here has to be the wind in m2.vert.glsl, vertex for vertex. A tree
// whose shadow sways by a different amount, or with a different profile up its
// trunk, drops a dappled pattern that drifts against its own canopy as the
// phase advances - which from the ground reads as the shadow flickering under
// a tree that looks perfectly still. This shader used to keep its own copy of
// the sway and say it matched; both now call foliageWindSway in wind.glsli,
// with the model's numbers from m2_sway.hpp and the wind's from WindField.
layout(push_constant) uniform Push {
    mat4 lightSpaceModel;   // light-space * model, multiplied on the CPU
    vec4 sway;              // xy world origin (phase), z reference height,
                            // w amplitude with the distance fade applied
    ivec4 flags;            // x useTexture, y alphaTest, z foliageSway
    vec4 wind;              // xy toward in the model's own xy, z lean, w swing
    vec4 windPhases;        // x trunk, y branch, z leaf, w detail
} push;

#include "wind.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aBoneWeights;
layout(location = 3) in vec4 aBoneIndicesF;

layout(location = 0) out vec2 TexCoord;

void main() {
    vec4 pos = vec4(aPos, 1.0);

    if (push.flags.z != 0) {
        float heightFactor = clamp(pos.z / max(push.sway.z, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        pos.xy += foliageWindSway(foliageWindFromShadowPush(push.wind, push.windPhases),
                                  pos.xyz, aPos, push.sway.xy, push.sway.w * heightFactor);
    }

    TexCoord = aTexCoord;
    gl_Position = push.lightSpaceModel * pos;
}
