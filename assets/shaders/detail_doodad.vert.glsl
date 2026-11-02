#version 450

// Ground clutter, as the 3.3.5a client draws it (WoW.exe 12340, 0x7b15d0):
// meshes already standing where they grow, lit by the ground's normal rather
// than their own, fading out over the last 15% of groundEffectDist by how far
// ahead of the camera they are.

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
    mat4 lightningMatrix;
    vec4 lightningPosition;
    vec4 lightningColor;
};

// DetailChunkMesh::Vertex.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

// x = groundEffectDist; y = the depth the fade starts at.
layout(push_constant) uniform Fade {
    vec4 fade;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vTexCoord;
layout(location = 3) out float vFade;

void main() {
    vec4 eye = view * vec4(aPosition, 1.0);
    // Depth ahead of the camera, as the client measures the distance.
    float depth = -eye.z;
    vFade = clamp((pc.fade.x - depth) / max(pc.fade.x - pc.fade.y, 1e-3), 0.0, 1.0);
    vWorldPos = aPosition;
    vNormal = aNormal;
    vTexCoord = aTexCoord;
    gl_Position = projection * eye;
}
