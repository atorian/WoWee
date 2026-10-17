#version 450

// The tessellated water's vertex stage: the still surface as it is, for the
// control stage to divide and the evaluation stage to move (water.tese).

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

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec3 vRest;
layout(location = 1) out vec2 vTexCoord;

void main() {
    vRest = (push.model * vec4(aPos, 1.0)).xyz;
    vTexCoord = aTexCoord;
}
