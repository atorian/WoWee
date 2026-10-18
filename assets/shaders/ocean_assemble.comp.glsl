#version 450

// The transformed modes of each grid point unpacked into the surface the
// water samples (ocean_waves.glsli): where the point has gone, the
// derivatives that shape its light, and their squares, which the filtered
// mips turn into the slope a pixel is too coarse to show.

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "ocean_waves.glsli"

void main() {
    ivec3 cell = ivec3(gl_GlobalInvocationID);
    int cascade = cell.z;
    vec4 a = imageLoad(oceanModes, ivec3(cell.xy, 2 * cascade));
    vec4 b = imageLoad(oceanModes, ivec3(cell.xy, 2 * cascade + 1));
    OceanSurface s = oceanSurfaceAt(a, b, oceanGridSign(cell.x, cell.y));
    vec2 slope = s.derivatives.xy;
    imageStore(oceanSurface, ivec3(cell.xy, kOceanDisplacementLayer + cascade), vec4(s.displacement, s.stretchXY));
    imageStore(oceanSurface, ivec3(cell.xy, kOceanDerivativeLayer + cascade), s.derivatives);
    imageStore(oceanSurface, ivec3(cell.xy, kOceanMomentLayer + cascade),
               vec4(slope.x * slope.x, slope.y * slope.y, slope.x * slope.y, 0.0));
}
