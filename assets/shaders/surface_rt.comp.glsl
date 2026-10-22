#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif
#include "surface_rt_inputs.glsli"
#ifdef RT_HARDWARE
#include "rt_trace_hw.glsli"
#else
#include "rt_trace_sw.glsli"
#endif
#include "rt_integrate.glsli"
layout(local_size_x=64) in;
uvec2 traceReceiver(Receiver r,ivec2 pixel,uint sampleId) {
    uint rng=rtSeed(uvec2(pixel),uint(pass.cameraPos.w)^rtHash(sampleId));
    float sun,ao; vec3 gi;
    rtIntegrate(r.position+pass.cameraPos.xyz,r.normal,r.distance,uint(pass.params.x),rng,sun,ao,gi);
    return packSurfaceLight(vec2(sun,ao),gi);
}
uvec2 accumulate(Receiver r,uvec2 raw,uint block) {
    SurfaceMaterial m=materials[r.material-1u];
    vec2 light;vec3 gi;unpackSurfaceLight(raw,light,gi);
    float count=1.0;
    vec4 oldPosition=m.previousFromCurrent*vec4(r.position,1);
    vec4 clip=pass.prevViewProj*oldPosition;
    if(pass.misc.y>0.5 && m.history.x!=0u && clip.w>0.0) {
        vec2 uv=clip.xy/clip.w*0.5+0.5;
        if(all(greaterThanEqual(uv,vec2(0))) && all(lessThan(uv,vec2(1)))) {
            uvec2 q=uvec2(uv*pass.extent.xy);
            SurfaceHistory h=previousHistory[q.y*uint(pass.extent.x)+q.x];
            vec3 oldNormal=normalize(transpose(inverse(mat3(m.previousFromCurrent)))*r.normal);
            float expected=length(oldPosition.xyz);
            vec3 oldPoint=uintBitsToFloat(h.geometry.xyz);
            if(all(equal(h.identity,m.identity)) && h.history.x>0u &&
               abs(length(oldPoint)-expected)<0.02*expected+0.03 &&
               abs(dot(oldPoint-oldPosition.xyz,oldNormal))<0.01+0.0002*expected &&
               dot(unpackSurfaceNormal(h.geometry.w),oldNormal)>0.95) {
                vec2 oldLight;vec3 oldGi;unpackSurfaceLight(h.light,oldLight,oldGi);
                count=min(float(h.history.x)+1.0,24.0);
                light=mix(oldLight,light,1.0/count);gi=mix(oldGi,gi,1.0/count);
            }
        }
    }
    uvec2 result=packSurfaceLight(light,gi);
    currentHistory[block]=SurfaceHistory(m.identity,uvec4(floatBitsToUint(r.position),packSurfaceNormal(r.normal)),result,uvec2(uint(count),0));
    return result;
}
void main() {
#ifdef SURFACE_EXTRA
    uint stride=gl_NumWorkGroups.x*64u;
    for(uint item=gl_GlobalInvocationID.x;item<missingCount;item+=stride) {
        uint index=missing[item], s=index%pc.samples, pixel=index/pc.samples;
        ivec2 p=ivec2(pixel%uint(pass.sceneExtent.x),pixel/uint(pass.sceneExtent.x));
        Receiver r=receiverAt(p,s);
        uvec2 light=traceReceiver(r,p,s);
        // Pixel-centre geometry can give several MSAA samples exactly the same
        // receiver. Trace that receiver once, preserving other covered surfaces.
        for(uint i=s;i<pc.samples;++i) {
            Receiver other=receiverAt(p,i);
            if(other.material==r.material && other.depth==r.depth && other.normalKey==r.normalKey) rawLight[sampleIndex(p,i)]=light;
        }
    }
#else
    for(uint block=gl_GlobalInvocationID.x;block<uint(pass.extent.x)*uint(pass.extent.y);block+=gl_NumWorkGroups.x*64u) {
    ivec2 base=ivec2(block%uint(pass.extent.x),block/uint(pass.extent.x))*2;
    Receiver best;best.material=0u;best.distance=1e30;
    ivec2 bestPixel=base;uint bestSample=0u;
    for(int i=0;i<4;++i) {
        ivec2 p=base+ivec2(i&1,i>>1);
        if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
        for(uint s=0u;s<pc.samples;++s) {
            Receiver r=receiverAt(p,s);
            if(usable(r) && r.distance<best.distance){best=r;bestPixel=p;bestSample=s;}
        }
    }
    uvec2 baseLight=packSurfaceLight(vec2(1),vec3(0));
    if(best.material!=0u)baseLight=accumulate(best,traceReceiver(best,bestPixel,bestSample),block);
    for(int i=0;i<4;++i) {
        ivec2 p=base+ivec2(i&1,i>>1);
        if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
        for(uint s=0u;s<pc.samples;++s) {
            Receiver r=receiverAt(p,s);uint index=sampleIndex(p,s);
            if(!usable(r)){rawLight[index]=packSurfaceLight(vec2(1),vec3(0));continue;}
            if(sameReceiver(r,best)){rawLight[index]=baseLight;continue;}
            bool duplicate=false;
            for(uint prior=0u;prior<s;++prior){Receiver q=receiverAt(p,prior);duplicate=duplicate||(q.material==r.material && q.depth==r.depth && q.normalKey==r.normalKey);}
            if(duplicate)continue;
            // Capacity is the total sample count. Each sample can append at
            // most once; no overflow fallback or stale/background substitution.
            uint at=atomicAdd(missingCount,1u);missing[at]=index;
            atomicMax(groupsX,min((at+64u)/64u,pc.maxDispatch));
        }
    }
    }
#endif
}
