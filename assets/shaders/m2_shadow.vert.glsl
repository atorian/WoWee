#version 450

// The doodads' side of the shadow maps, drawn instanced: one draw a batch of
// a model, every caster of that model an instance, its slot and its bend in
// M2ShadowInstance (m2_renderer.hpp). Where it stands is its slot's row in the
// instance table, the row the view draws it from. It used to be a draw and a
// push a batch a caster, and recording those was most of the shadow pass's
// time.
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
    vec4 windDirLean;       // xy toward (world), z lean, w swing: m2.vert's PerFrame one
} push;

struct ShadowInstance {
    uint slot;              // its row in InstanceStatics
    float refHeight;        // the height the bend is normalised against
    float amplitude;        // the bend, with the distance fade applied; zero for none
    float pad;
};
layout(set = 1, binding = 0) readonly buffer ShadowInstances {
    ShadowInstance instances[];
};
// m2.vert's instance table (M2Renderer::InstanceStaticGPU), by slot.
struct InstanceStatic {
    mat4 model;
    vec4 interiorAmbient;
    vec4 interiorDirect;
};
layout(set = 1, binding = 1) readonly buffer InstanceStatics {
    InstanceStatic instanceStatics[];
};

#include "wind.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

void main() {
    ShadowInstance caster = instances[gl_InstanceIndex];
    mat4 model = instanceStatics[caster.slot].model;
    vec4 pos = vec4(aPos, 1.0);

    if (push.flags.z != 0) {
        float heightFactor = clamp(pos.z / max(caster.refHeight, 0.01), 0.0, 1.0);
        heightFactor *= heightFactor;   // quadratic - the base stays planted
        // Turned into the model's own space as m2.vert turns it, since the
        // sway is applied before the model matrix; the origin gives the phase.
        vec4 wind = vec4(windDirectionInModel(model, push.windDirLean.xy), push.windDirLean.z, push.windDirLean.w);
        pos.xy += foliageWindSway(foliageWindFromShadowCaster(wind, push.windPhases),
                                  pos.xyz, aPos, model[3].xy, caster.amplitude * heightFactor);
    }

    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * (model * pos);
}
