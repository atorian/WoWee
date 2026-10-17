#version 450

// One line of the sea's modes - a row, or a column when ocean.vertical is
// set - through the inverse FFT, in shared memory and back in place. A
// workgroup takes one line of one layer, each of its kOceanSize / 2
// invocations one butterfly of every stage (ocean_fft.glsli).

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

#include "ocean_waves.glsli"

shared vec4 line[2][kOceanSize];

ivec3 texelOf(int i) {
    int across = int(gl_WorkGroupID.y);
    int layer = int(gl_WorkGroupID.z);
    return ocean.vertical == 0 ? ivec3(i, across, layer) : ivec3(across, i, layer);
}

void main() {
    int j = int(gl_LocalInvocationID.x);
    const int halfSize = kOceanSize / 2;
    line[0][j] = imageLoad(oceanModes, texelOf(j));
    line[0][j + halfSize] = imageLoad(oceanModes, texelOf(j + halfSize));
    barrier();

    int from = 0;
    for (int stage = 0; (1 << stage) < kOceanSize; ++stage) {
        OceanButterfly b = oceanStockhamButterfly(j, stage, kOceanSize);
        vec4 s0 = line[from][b.source0];
        vec4 s1 = line[from][b.source1];
        vec2 w0 = oceanComplexMul(b.twiddle, s1.xy);
        vec2 w1 = oceanComplexMul(b.twiddle, s1.zw);
        vec4 w = vec4(w0, w1);
        line[1 - from][b.target0] = s0 + w;
        line[1 - from][b.target1] = s0 - w;
        from = 1 - from;
        barrier();
    }

    imageStore(oceanModes, texelOf(j), line[from][j]);
    imageStore(oceanModes, texelOf(j + halfSize), line[from][j + halfSize]);
}
