#version 450

// The water on its own mesh, untessellated: where the device has no
// tessellation, and for every surface's vertices when it has. The mesh moves
// by the waves its own step can carry (water_surface.glsli).

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
    float liquidBasicType; // 0=water, 1=ocean, 2=magma, 3=slime
    // The rest belongs to the fragment stage, and is spelled the same way here
    // so the two declarations of one range agree. This said `float brightness`
    // where the fragment stage has a vec2, which is a field that does not exist
    // sitting at another field's offset - harmless only for as long as nobody
    // reads it.
    vec2 screenSize;
    vec2 depthRange;
    float sceneValid;
    float meshStep;   // yards between the mesh's vertices
    float exposed;    // 1 where the wind reaches the surface, 0 indoors where it lies still
    float pad2;
} push;

const int MAX_WAKE_POINTS = 32;
const int OCEAN_CASCADES = 3;  // kOceanCascadeCount in ocean_waves.hpp
const int OCEAN_KEPT_VEC4S = 408;  // OceanSea::kKeptEntries / 4
layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    vec4 wakeBounds;
    vec4 wakePoints[MAX_WAKE_POINTS];
    vec4 oceanCascades[OCEAN_CASCADES];  // xy = origin, z = 1 / patch, w = patch (yards)
    vec4 oceanSea;                       // x = rms height, y = ride, z = mean square slope
    vec4 oceanKept[OCEAN_KEPT_VEC4S];
    mat4 seaBedMatrix;
    vec4 seaBed;
    vec4 wakeArea;
    vec4 reflectionSun;
    vec4 reflectionSunColor;
};
layout(set = 2, binding = 4) uniform sampler2DArray oceanSurface;
layout(set = 2, binding = 5) uniform sampler2D seaBedMap;
layout(set = 2, binding = 6) uniform sampler2D seaFetchMap;
layout(set = 2, binding = 7) uniform sampler2D wakeSurface;

#include "ocean_surface.glsli"
#include "water_wake_surface.glsli"
#include "water_surface.glsli"

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec3 RestPos;   // where this point lies on the still surface
layout(location = 2) out vec2 TexCoord;
layout(location = 4) out vec2 ScreenUV;

void main() {
    vec3 rest = (push.model * vec4(aPos, 1.0)).xyz;
    vec3 p = displacedWaterPoint(rest, push.meshStep, fogParams.z);

    FragPos = p;
    RestPos = rest;
    TexCoord = aTexCoord;
    vec4 clipPos = projection * view * vec4(p, 1.0);
    gl_Position = clipPos;
    ScreenUV = clipPos.xy / max(clipPos.w, 1e-5) * 0.5 + 0.5;
}
