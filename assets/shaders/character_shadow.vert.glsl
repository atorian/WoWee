#version 450
#extension GL_ARB_shader_draw_parameters : require

// A character's side of the shadow maps: the draws the cull wrote for a map
// (char_draws.comp), a page's casters by one indirect call. A draw's record
// names its instance, whose pose this frame says where its bones are.
layout(push_constant) uniform Push {
    mat4 lightSpace;        // the map's
    uvec4 call;             // x the call's first caster place
} push;

// The frame's skeletons as skeleton.comp posed them, and each request's
// model matrix.
layout(set = 1, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};
layout(set = 1, binding = 1) readonly buffer ModelSSBO {
    mat4 models[];
};
// By caster place: x the texture's handle, y bit 0 sample it, bit 1
// alpha-test it, w the instance's slot.
layout(std430, set = 2, binding = 0) readonly buffer CasterRecords {
    uvec4 casterRecords[];
};
// Each instance's pose this frame (CharInstanceGPU's first word).
struct CharInstance {
    uvec4 pose;
    vec4 rest[7];
};
layout(std430, set = 2, binding = 1) readonly buffer CharInstances {
    CharInstance charInstances[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;
layout(location = 1) flat out uvec4 vDraw;

uvec4 pose;

// A bone past the skeleton's own count is the identity.
mat4 boneAt(uint index) {
    return index < pose.z ? bones[pose.y + index] : mat4(1.0);
}

void main() {
    const uvec4 record = casterRecords[push.call.x + uint(gl_DrawIDARB)];
    vDraw = record;
    pose = charInstances[record.w].pose;
    uvec4 bi = aBoneIndices;
    mat4 skinMat = boneAt(bi.x) * aBoneWeights.x
                 + boneAt(bi.y) * aBoneWeights.y
                 + boneAt(bi.z) * aBoneWeights.z
                 + boneAt(bi.w) * aBoneWeights.w;
    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * (models[pose.x] * skinnedPos);
}
