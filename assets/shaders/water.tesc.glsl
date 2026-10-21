#version 450

// How finely each triangle of the water is divided: each edge into pieces
// about kSegmentPixels long on screen, so the waves near the camera have the
// vertices to rise and fall on and the far sheet stays as it was. An edge's
// level is taken from that edge alone - its two ends and its middle's
// distance - so the two triangles sharing it divide it alike and no crack
// opens between them.

layout(vertices = 3) out;

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
    mat4 rtViewProj;
    vec4 rtCameraPos;
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
};

layout(push_constant) uniform Push {
    mat4 model;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType;
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float meshStep;
    float exposed;
    float pad2;
} push;

#include "water_tess_common.glsli"

layout(location = 0) in vec3 vRest[];
layout(location = 1) in vec2 vTexCoord[];

layout(location = 0) out vec3 tRest[];
layout(location = 1) out vec2 tTexCoord[];

float edgeLevel(vec3 a, vec3 b) {
    // A surface held still has nothing to divide for.
    if (push.exposed < 0.5) return 1.0;
    float pieces = length(a - b) * pixelsPerYardAt(0.5 * (a + b)) / kSegmentPixels;
    return clamp(pieces, 1.0, kMaxTessLevel);
}

void main() {
    tRest[gl_InvocationID] = vRest[gl_InvocationID];
    tTexCoord[gl_InvocationID] = vTexCoord[gl_InvocationID];
    if (gl_InvocationID == 0) {
        // Outer level i is the edge facing vertex i.
        gl_TessLevelOuter[0] = edgeLevel(vRest[1], vRest[2]);
        gl_TessLevelOuter[1] = edgeLevel(vRest[2], vRest[0]);
        gl_TessLevelOuter[2] = edgeLevel(vRest[0], vRest[1]);
        gl_TessLevelInner[0] = max(gl_TessLevelOuter[0], max(gl_TessLevelOuter[1], gl_TessLevelOuter[2]));
    }
}
