#version 450

// Each wave vector's fields at the sea's time now, out of its own height
// variance and its mirror's and the sea's fixed random draw, packed for the
// inverse FFT (ocean_fft.glsli's oceanEvolve).

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "ocean_waves.glsli"

void main() {
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    int cascade = cell.z;
    ivec2 mirror = (ivec2(kOceanSize) - cell.xy) % kOceanSize;
    float variance = imageLoad(oceanState, cell).r;
    float mirrorVariance = imageLoad(oceanState, ivec3(mirror, cascade)).r;
    vec2 h0 = oceanInitialHeight(oceanGaussianPair(cell.x, cell.y, cascade), variance);
    vec2 h0Mirror = oceanInitialHeight(oceanGaussianPair(mirror.x, mirror.y, cascade), mirrorVariance);

    vec2 k = oceanWaveVector(cell.x, cell.y, cascade);
    float omega = oceanRepeatingAngularSpeed(length(k), kOceanRepeatSeconds);
    OceanModes m = oceanEvolve(k, h0, h0Mirror, omega, ocean.time, ocean.ride);
    imageStore(oceanModes, ivec3(cell.xy, 2 * cascade), m.modesA);
    imageStore(oceanModes, ivec3(cell.xy, 2 * cascade + 1), m.modesB);
}
