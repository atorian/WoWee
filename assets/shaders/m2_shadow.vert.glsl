#version 450

// The doodads' side of the shadow maps, drawn instanced: one draw a batch of
// a model, every caster of that model an instance, its matrix and its wind in
// M2ShadowInstance (m2_renderer.hpp). It used to be a draw and a push a
// batch a caster, and recording those was most of the shadow pass's time.
//
// The wind here has to be the wind in m2.vert.glsl, vertex for vertex. A tree
// whose shadow sways by a different amount, or with a different profile up its
// trunk, drops a dappled pattern that drifts against its own canopy as the
// phase advances - which from the ground reads as the shadow flickering under
// a tree that looks perfectly still. Both call foliageWindSway in wind.glsli,
// with the model's numbers from m2_sway.hpp and the wind's from WindField.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // the map's: the sun's or the units'
    ivec4 flags;            // x useTexture, y alphaTest, z bend in the wind; read by shadow.frag
    vec4 windPhases;        // x trunk, y branch, z leaf; w the branch and leaf scale
} push;

struct ShadowInstance {
    mat4 model;
    vec4 sway;              // xy world origin (phase), z reference height,
                            // w amplitude with the distance fade applied
    vec4 wind;              // xy toward in the model's own xy, z lean, w swing
};
layout(set = 1, binding = 0) readonly buffer ShadowInstances {
    ShadowInstance instances[];
};

#include "wind.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

void main() {
    ShadowInstance caster = instances[gl_InstanceIndex];
    vec4 pos = vec4(aPos, 1.0);

    if (push.flags.z != 0) {
        float heightFactor = clamp(pos.z / max(caster.sway.z, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        pos.xy += foliageWindSway(foliageWindFromShadowCaster(caster.wind, push.windPhases),
                                  pos.xyz, aPos, caster.sway.xy, caster.sway.w * heightFactor);
    }

    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * (caster.model * pos);
}
