#version 450

// Each wave vector's height variance, one step on toward what this frame's
// wind asks of it (ocean_spectrum.glsli): the chop follows a gust in
// seconds, the long waves over minutes.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "ocean_waves.glsli"

void main() {
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    int cascade = cell.z;
    vec2 k = oceanWaveVector(cell.x, cell.y, cascade);
    float target = oceanModeHeightVariance(k, cascade, ocean.toward, ocean.speed, ocean.slopeScale);
    float variance = target;
    if (ocean.restart == 0) {
        float current = imageLoad(oceanState, cell).r;
        variance = oceanRelaxedVariance(current, target, length(k), ocean.deltaTime);
    }
    imageStore(oceanState, cell, vec4(variance));
}
