#version 450
#define WMO_CURRENT_RT
#include "wmo_frame.glsli"
#include "interior_light.glsli"
#include "m2_surface_lighting.glsli"
#include "terrain_surface_lighting.glsli"
#include "character_surface_lighting.glsli"
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
    vec3 face=unpackSurfaceNormal(keys.x), normal=unpackSurfaceNormal(keys.y), vertex=unpackSurfaceNormal(keys.z);
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
    if(m.history.y==0u) {
        WmoSurface surface=WmoSurface(position.xyz,texel,normal,vertex,face,
            vec4(unpackHalf2x16(payload.x),unpackHalf2x16(payload.y)),m.payload[4]);
        WmoLightingMaterial material=WmoLightingMaterial(int(m.payload[0].z),int(m.payload[0].w),
            uintBitsToFloat(m.payload[1].x),uintBitsToFloat(m.payload[2].xyz),uintBitsToFloat(m.payload[3].yzw),
            int(m.payload[2].w),int(m.payload[3].x),int(m.payload[1].w),int(m.payload[1].z));
        vec3 lit=shadeWmoSurface(surface,material,rt);
        outColor=vec4(applyFog(lit,position.xyz,length(viewPos.xyz-position.xyz)),texel.a);
    } else if(m.history.y==1u) {
        M2LightingMaterial material=M2LightingMaterial(ivec4(m.payload[0]),uintBitsToFloat(m.payload[1]),
            uintBitsToFloat(m.payload[2]),uintBitsToFloat(m.payload[3]),m.payload[4]);
        vec2 addXY=unpackHalf2x16(payload.x),addZHeight=unpackHalf2x16(payload.y);
        outColor=shadeM2Surface(position.xyz,texel,vec3(addXY,addZHeight.x),normal,vertex,face,
            addZHeight.y,texel.a,1.0,0,material,rt);
    } else if(m.history.y==3u) {
        M2LightingMaterial material=M2LightingMaterial(ivec4(m.payload[0]),uintBitsToFloat(m.payload[1]),
            uintBitsToFloat(m.payload[2]),uintBitsToFloat(m.payload[3]),m.payload[4]);
        vec2 addXY=unpackHalf2x16(payload.x),addZ=unpackHalf2x16(payload.y);
        outColor=shadeCharacterSurface(position.xyz,texel,vec3(addXY,addZ.x),normal,vertex,face,1.0,0.0,material,rt);
    } else {
        outColor=shadeTerrainSurface(position.xyz,texel,normal,vertex,face,m.payload[4],rt);
    }
}
