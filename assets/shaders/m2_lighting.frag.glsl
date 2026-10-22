#version 450
#define M2_CURRENT_RT
#include "m2_frame.glsli"
#include "surface_rt_data.glsli"
#include "surface_encoding.glsli"
#ifdef SURFACE_MSAA
layout(set=1,binding=0) uniform sampler2DMS surfaceColor;
layout(set=1,binding=1) uniform usampler2DMS surfaceKeys;
layout(set=1,binding=2) uniform usampler2DMS surfacePayload;
#define LOAD(image) texelFetch(image, ivec2(gl_FragCoord.xy), gl_SampleID)
#else
layout(set=1,binding=0) uniform sampler2D surfaceColor;
layout(set=1,binding=1) uniform usampler2D surfaceKeys;
layout(set=1,binding=2) uniform usampler2D surfacePayload;
#define LOAD(image) texelFetch(image, ivec2(gl_FragCoord.xy), 0)
#endif
struct M2SurfaceMaterial {
    M2LightingMaterial lighting;
    mat4 previousFromCurrent;
    uvec4 identity,history;
};
layout(std430,set=1,binding=3) readonly buffer Materials { M2SurfaceMaterial materials[]; };
layout(std430,set=1,binding=4) readonly buffer CurrentLight { uvec2 currentLight[]; };
layout(push_constant) uniform ResolveView { mat4 inverseViewProjection; vec4 extent; } resolveView;
layout(location=0) out vec4 outColor;
void main() {
    currentRtParams=vec4(resolveView.extent.z,resolveView.extent.w*0.8,resolveView.extent.w,0);
    uvec4 keys = LOAD(surfaceKeys);
    if (keys.w == 0u) discard;
    uvec4 payload = LOAD(surfacePayload);
    vec4 texel = LOAD(surfaceColor);
    // Geometry evaluation uses the pixel centre (also under MSAA); depth in
    // the payload is from that same invocation, not a resolved depth sample.
    vec2 pixel = floor(gl_FragCoord.xy) + vec2(0.5);
    vec4 position = resolveView.inverseViewProjection * vec4(
        pixel / resolveView.extent.xy * 2.0 - 1.0, uintBitsToFloat(payload.z), 1.0);
    position /= position.w;
    position.xyz += viewPos.xyz;
    M2SurfaceMaterial m = materials[keys.w - 1u];
    vec2 addXY=unpackHalf2x16(payload.x),addZHeight=unpackHalf2x16(payload.y);
    RtLight rt = RtLight(0.0, 1.0, 1.0, vec3(0));
    if(currentRtParams.x>0.5) {
        uint sampleCount=1u,sampleId=0u;
#ifdef SURFACE_MSAA
        sampleCount=uint(textureSamples(surfaceKeys));sampleId=uint(gl_SampleID);
#endif
        uint index=(uint(gl_FragCoord.y)*uint(resolveView.extent.x)+uint(gl_FragCoord.x))*sampleCount+sampleId;
        vec2 visibility;vec3 gi;
        unpackSurfaceLight(currentLight[index],visibility,gi);
        rt=RtLight(rtRangeWeight(length(viewPos.xyz-position.xyz)),visibility.x,visibility.y,gi);
    }
    outColor=shadeM2Surface(position.xyz,texel,vec3(addXY,addZHeight.x),
        unpackSurfaceNormal(keys.y),unpackSurfaceNormal(keys.z),unpackSurfaceNormal(keys.x),
        addZHeight.y,texel.a,1.0,0,m.lighting,rt);
}
