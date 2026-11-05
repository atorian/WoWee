#version 450
#extension GL_ARB_shader_draw_parameters : require

// The buildings' side of the shadow maps: the draws the cull wrote for a
// map (wmo_cull.comp), a page's casters by one indirect call. A draw's record
// names its group, whose instance says where it stands, and carries what
// m2_shadow.frag cuts an alpha-keyed batch by.
layout(push_constant) uniform Push {
    mat4 lightSpace;    // the map's
    uvec4 call;         // x the call's first caster place
} push;

// By caster place: x the texture's handle, y bit 0 sample it, bit 1
// alpha-test it, z where the test cuts in 255ths, w the group row.
layout(std430, set = 1, binding = 0) readonly buffer CasterRecords {
    uvec4 casterRecords[];
};
// The placed groups (WmoGroupGPU): refs.x is its instance's slot.
struct WmoGroup {
    vec4 worldMin;
    vec4 worldMax;
    vec4 localCentre;
    uvec4 refs;
    uvec4 slots0;
    uvec4 slots1;
};
layout(std430, set = 1, binding = 1) readonly buffer WmoGroups {
    WmoGroup wmoGroups[];
};
struct WmoInstance {
    mat4 model;
    uvec4 state;
};
layout(std430, set = 1, binding = 2) readonly buffer WmoInstances {
    WmoInstance wmoInstances[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;
layout(location = 1) flat out uvec4 vDraw;

void main() {
    const uvec4 record = casterRecords[push.call.x + uint(gl_DrawIDARB)];
    vDraw = uvec4(record.xyz, 0u);
    const mat4 model = wmoInstances[wmoGroups[record.w].refs.x].model;
    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * model * vec4(aPos, 1.0);
}
