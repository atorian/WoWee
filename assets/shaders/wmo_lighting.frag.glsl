#version 450
#define WMO_CURRENT_RT
#include "wmo_frame.glsli"
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
layout(std430,set=1,binding=3) readonly buffer Materials { SurfaceMaterial materials[]; };
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
    SurfaceMaterial m = materials[keys.w - 1u];
    WmoSurface surface = WmoSurface(position.xyz, texel,
        unpackSurfaceNormal(keys.y), unpackSurfaceNormal(keys.z), unpackSurfaceNormal(keys.x),
        vec4(unpackHalf2x16(payload.x), unpackHalf2x16(payload.y)), m.localLights);
    WmoLightingMaterial material = WmoLightingMaterial(m.light, m.transitionLight,
        m.specularIntensity, vec3(m.ambientR,m.ambientG,m.ambientB),
        vec3(m.sidnR,m.sidnG,m.sidnB), m.hasVertexColors, m.receivesExteriorShadows,
        m.emissive, m.isLava);
    RtLight rt = RtLight(0.0, 1.0, 1.0, vec3(0));
    if(currentRtParams.x>0.5) {
        uint sampleCount=1u,sampleId=0u;
#ifdef SURFACE_MSAA
        sampleCount=uint(textureSamples(surfaceKeys));sampleId=uint(gl_SampleID);
#endif
        uint index=(uint(gl_FragCoord.y)*uint(resolveView.extent.x)+uint(gl_FragCoord.x))*sampleCount+sampleId;
        vec2 visibility;vec3 gi;
        unpackSurfaceLight(currentLight[index],visibility,gi);
        rt=RtLight(rtRangeWeight(length(viewPos.xyz-surface.position)),visibility.x,visibility.y,gi);
    }
    vec3 lit = shadeWmoSurface(surface, material, rt);
    outColor = vec4(applyFog(lit, surface.position, length(viewPos.xyz-surface.position)), texel.a);
}
