#version 450

// The tessellated water's evaluation stage: each new vertex on the still
// surface, moved by the swell and the wind's waves that the mesh here is
// fine enough to carry (water_surface.glsli). It hands the fragment stage
// what water.vert hands it.

layout(triangles, fractional_odd_spacing, cw) in;

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
    vec4 playerWake;
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
    float displaced;
    float pad2;
} push;

const int MAX_WAKE_POINTS = 32;
const int WIND_WAVE_COUNT = 32;  // kWindWaveCount in wind_waves.hpp
layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    vec4 wakeBounds;
    vec4 wakePoints[MAX_WAKE_POINTS];
    vec4 waveOrigin;   // xy = origin of positions, z = how far points ride toward the crests
    vec4 windWaves[WIND_WAVE_COUNT];
};

#include "gerstner_waves.glsli"
#include "water_surface.glsli"
#include "water_tess_common.glsli"

layout(location = 0) in vec3 tRest[];
layout(location = 1) in vec2 tTexCoord[];

layout(location = 0) out vec3 FragPos;
layout(location = 1) out vec2 RestPos;
layout(location = 2) out vec2 TexCoord;
layout(location = 4) out vec2 ScreenUV;

void main() {
    vec3 b = gl_TessCoord;
    vec3 rest = b.x * tRest[0] + b.y * tRest[1] + b.z * tRest[2];
    vec2 texCoord = b.x * tTexCoord[0] + b.y * tTexCoord[1] + b.z * tTexCoord[2];

    vec3 p = displacedWaterPoint(rest, meshStepAt(rest), fogParams.z);

    FragPos = p;
    RestPos = rest.xy;
    TexCoord = texCoord;
    vec4 clipPos = projection * view * vec4(p, 1.0);
    gl_Position = clipPos;
    ScreenUV = clipPos.xy / max(clipPos.w, 1e-5) * 0.5 + 0.5;
}
