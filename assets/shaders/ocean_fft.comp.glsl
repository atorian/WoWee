#version 450

// One line of the sea's modes - a row, or a column when ocean.vertical is
// set - through the inverse FFT (fft_line.glsli). A workgroup takes one line
// of one layer.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

#include "ocean_waves.glsli"

/// Point i of the workgroup's line, in the modes' layers.
ivec3 oceanLineTexel(int i) {
    int across = int(gl_WorkGroupID.y);
    int layer = int(gl_WorkGroupID.z);
    return ocean.vertical == 0 ? ivec3(i, across, layer) : ivec3(across, i, layer);
}

#define FFT_SIZE kOceanSize
#define FFT_LOAD(i) imageLoad(oceanModes, oceanLineTexel(i))
#define FFT_STORE(i, v) imageStore(oceanModes, oceanLineTexel(i), v)
#include "fft_line.glsli"

void main() {
    fftTransformLine();
}
