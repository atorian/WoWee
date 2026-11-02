#version 450

layout(push_constant) uniform Push {
    mat4 lightSpace;        // the map's
    uvec4 pose;             // x its posed request, y its first bone, z its bone count
} push;

// The frame's skeletons as skeleton.comp posed them, and each request's
// model matrix.
layout(set = 1, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};
layout(set = 1, binding = 1) readonly buffer ModelSSBO {
    mat4 models[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

// A bone past the skeleton's own count is the identity.
mat4 boneAt(uint index) {
    return index < push.pose.z ? bones[push.pose.y + index] : mat4(1.0);
}

void main() {
    uvec4 bi = aBoneIndices;
    mat4 skinMat = boneAt(bi.x) * aBoneWeights.x
                 + boneAt(bi.y) * aBoneWeights.y
                 + boneAt(bi.z) * aBoneWeights.z
                 + boneAt(bi.w) * aBoneWeights.w;
    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    TexCoord = aTexCoord;
    gl_Position = push.lightSpace * (models[push.pose.x] * skinnedPos);
}
