#version 450

// The shelter map's word on the camera (ShelterMap::probe): how open to the
// sky it is and how much of the sky its surroundings leave it, two numbers
// for the CPU to read back once the frame is done (shelter_probe.glsli).

layout(local_size_x = 1) in;

layout(set = 0, binding = 0) uniform sampler2D uShelter;
layout(std430, set = 0, binding = 1) writeonly buffer Result {
    vec4 result;  // x exposure, y sky view, z 1 once written
};

layout(push_constant) uniform Push {
    vec4 eye;    // xyz the camera, w a texel in yards
    vec4 frame;  // xy the map's centre, z its half extent, w its ceiling
    vec4 range;  // x yards from the ceiling to depth 1, y texels a side
} pc;

float shelterTop(vec2 xy);

#include "shelter_probe.glsli"

float shelterTop(vec2 xy) {
    vec2 uv = (xy - pc.frame.xy) / pc.frame.z * 0.5 + 0.5;
    if (any(lessThan(uv, vec2(0.0))) || any(greaterThanEqual(uv, vec2(1.0)))) return kNoShelter;
    float depth = texelFetch(uShelter, ivec2(uv * pc.range.y), 0).r;
    if (depth >= 1.0) return kNoShelter;
    return pc.frame.w - depth * pc.range.x;
}

void main() {
    result = vec4(shelterExposure(pc.eye.xyz, pc.eye.w), shelterSkyView(pc.eye.xyz), 1.0, 0.0);
}
