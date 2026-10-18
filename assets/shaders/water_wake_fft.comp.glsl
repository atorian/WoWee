#version 450

// One line of the wake's spectrum - a row, or a column when wake.vertical
// is set - through the inverse FFT (fft_line.glsli). A workgroup takes one
// line.

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

#include "water_wake_passes.glsli"

/// Point i of the workgroup's line.
ivec2 wakeLineTexel(int i) {
    int across = int(gl_WorkGroupID.y);
    return wake.vertical == 0 ? ivec2(i, across) : ivec2(across, i);
}

#define FFT_SIZE kWakeSize
#define FFT_LOAD(i) imageLoad(wakeSpectrum, wakeLineTexel(i))
#define FFT_STORE(i, v) imageStore(wakeSpectrum, wakeLineTexel(i), v)
#include "fft_line.glsli"

void main() {
    fftTransformLine();
}
