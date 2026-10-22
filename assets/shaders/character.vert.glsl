#version 450

#define MAX_BONES 240u  // must match CharacterRenderer::MAX_BONES

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    vec4 rtTraceParams;
    vec4 rtParams;
    vec4 fogSkyTop;  // rgb = zenith, w = sky tint amount (0 disables it)
    vec4 fogSkyMiddle;
    vec4 fogSkyBand1;
    vec4 fogSkyBand2;
    mat4 contactSpaceMatrix;  // read by contact_shadows.glsli
    vec4 contactParams;
    vec4 windDirLean;
    vec4 windPhases;
    vec4 windTravel;
    vec4 skySmogColor;
    vec4 aerialSun;
    vec4 aerialLight;
    vec4 terrainLodParams;
    vec4 aerialParams;
    mat4 lightningMatrix;
    vec4 lightningPosition;
    vec4 lightningColor;
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 interiorAmbient;  // read by character.frag
    vec4 interiorDirect;   // read by character.frag
    uvec4 localLights;     // read by character.frag
} push;

#include "character_material.glsli"

layout(set = 2, binding = 0) readonly buffer BoneSSBO {
    mat4 bones[];
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aBoneWeights;
layout(location = 2) in uvec4 aBoneIndices;
layout(location = 3) in vec3 aNormal;
layout(location = 4) in vec2 aTexCoord;
layout(location = 5) in vec4 aTangent;
layout(location = 6) in vec2 aTexCoord2;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 Normal;
layout(location = 2) out vec2 TexCoord;
layout(location = 3) out vec3 Tangent;
layout(location = 4) out vec3 Bitangent;
layout(location = 5) out vec2 TexCoord2;
// A faded body is drawn into depth by one pipeline and coloured where that
// depth is by another (LESS_OR_EQUAL); both must compute the same depth.
invariant gl_Position;

#include "sphere_map.glsli"

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

void main() {
    // Bone slots past the model's own count stay identity, so clamping keeps a
    // stray index harmless instead of reading past the buffer.
    uvec4 bi = min(aBoneIndices, uvec4(MAX_BONES - 1u));
    mat4 skinMat = bones[bi.x] * aBoneWeights.x
                 + bones[bi.y] * aBoneWeights.y
                 + bones[bi.z] * aBoneWeights.z
                 + bones[bi.w] * aBoneWeights.w;

    vec4 skinnedPos = skinMat * vec4(aPos, 1.0);
    vec3 skinnedNorm = mat3(skinMat) * aNormal;
    vec3 skinnedTan = mat3(skinMat) * aTangent.xyz;

    vec4 worldPos = push.model * skinnedPos;
    mat3 modelMat3 = mat3(push.model);
    FragPos = worldPos.xyz;
    Normal = modelMat3 * skinnedNorm;
    // Env-mapped armour and metal sample eye-space sphere coordinates, not
    // the atlas UV, as doodads do in m2.vert. A sphere-mapped stage takes no
    // UV animation (render spec 6.1).
    vec3 eyePosition = mat3(view) * (worldPos.xyz - viewPos.xyz);
    vec3 eyeNormal = safeNormalize(mat3(view) * Normal, vec3(0.0, 0.0, 1.0));
    if (texCoordSet == -1) {
        TexCoord = sphereMapUV(eyePosition, eyeNormal);
    } else {
        vec2 uv = texCoordSet == 1 ? aTexCoord2 : aTexCoord;
        TexCoord = vec2(dot(uvRow0.xy, uv) + uvRow0.z, dot(uvRow1.xy, uv) + uvRow1.z);
    }
    // The second stage, from its own source through its own animation - or,
    // behind a sphere-mapped first stage, UV0 as it is (Diffuse_Env_T2).
    if (texCoordSet2 == -1) {
        TexCoord2 = sphereMapUV(eyePosition, eyeNormal);
    } else if (texCoordSet2 == 2) {
        TexCoord2 = aTexCoord;
    } else {
        vec2 uv2 = texCoordSet2 == 1 ? aTexCoord2 : aTexCoord;
        TexCoord2 = vec2(dot(uv2Row0.xy, uv2) + uv2Row0.z, dot(uv2Row1.xy, uv2) + uv2Row1.z);
    }

    // Gram-Schmidt re-orthogonalize tangent w.r.t. normal
    vec3 N = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    vec3 T = safeNormalize(modelMat3 * skinnedTan, fallbackTangent(N));
    T = safeNormalize(T - dot(T, N) * N, fallbackTangent(N));
    vec3 B = safeNormalize(cross(N, T) * aTangent.w, safeNormalize(cross(N, fallbackTangent(N)), vec3(0.0, 1.0, 0.0)));

    Tangent = T;
    Bitangent = B;

    gl_Position = projection * view * worldPos;
}
