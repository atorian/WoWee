#version 450

// The sea bed's map the water reads (SeaBedMap), out of its three views:
// the ground from above, and the buildings from above and from below - a
// building is the bed only where it stands on the ground (sea_bed.glsli).

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

layout(set = 0, binding = 0) uniform sampler2D groundFromAbove;
layout(set = 0, binding = 1) uniform sampler2D buildingsFromAbove;
layout(set = 0, binding = 2) uniform sampler2D buildingsFromBelow;
layout(set = 0, binding = 3, r32f) uniform writeonly image2D bed;

layout(push_constant) uniform Push {
    float range;  // yards from the water to the views' foot
} push;

#include "sea_bed.glsli"

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    float depth = seaBedDepth(texelFetch(groundFromAbove, texel, 0).r,
                              texelFetch(buildingsFromAbove, texel, 0).r,
                              texelFetch(buildingsFromBelow, texel, 0).r, push.range);
    imageStore(bed, texel, vec4(depth));
}
