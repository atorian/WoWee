#version 450
// Exercise the production RT surface lookup with a previous-frame silhouette.
#define RT_LOOKUP_PROBE
vec4 rtParams;
#include "../assets/shaders/rt_lighting.glsli"
layout(local_size_x = 1) in;
struct Sample { vec4 point; vec4 settings; vec4 normal; };
layout(set = 0, binding = 0, std430) readonly buffer Inputs { Sample samples[]; };
struct Result { vec4 ambientWeight; vec4 sunAo; };
layout(set = 0, binding = 1, std430) writeonly buffer Outputs { Result results[]; };
void main() {
    uint i = gl_GlobalInvocationID.x;
    Sample s = samples[i];
    rtParams = vec4(s.settings.z, 0.0, 0.0, 0.0);
    RtLight light = rtLightAtProjected(s.point.xy, s.point.z, s.settings.xy, s.normal.xyz);
    results[i].ambientWeight = vec4(rtAmbient(light, vec3(0.3, 0.35, 0.4)), light.weight);
    results[i].sunAo = vec4(light.sun, light.ao, 0.0, 0.0);
}
