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
            vec3 oldPoint=uintBitsToFloat(uvec3(h.positionXY,h.positionZNormalCount.x));
            if(all(equal(h.placement,m.identity.xy)) && all(equal(h.publication,m.identity.zw)) && (h.positionZNormalCount.y>>24u)>0u &&
               abs(length(oldPoint)-expected)<0.02*expected+0.03 &&
               abs(dot(oldPoint-oldPosition.xyz,oldNormal))<0.01+0.0002*expected &&
               dot(historyNormal(h.positionZNormalCount.y),oldNormal)>0.95) {
                vec2 oldLight;vec3 oldGi;unpackSurfaceLight(h.light,oldLight,oldGi);
                count=min(float(h.positionZNormalCount.y>>24u)+1.0,24.0);
                light=mix(oldLight,light,1.0/count);gi=mix(oldGi,gi,1.0/count);
            }
        }
    }
    uvec2 result=packSurfaceLight(light,gi);
    currentHistory[block]=SurfaceHistory(m.identity.xy,m.identity.zw,floatBitsToUint(r.position.xy),uvec2(floatBitsToUint(r.position.z),historyNormalCount(r.normal,uint(count))),result);
    return result;
}
#ifndef SURFACE_EXTRA
shared uint unresolved;
shared uint occupied;
bool traceBlock(ivec2 base) {
    if(any(greaterThanEqual(base,ivec2(pass.sceneExtent.xy))))return false;
    uint block=uint(base.y/2)*uint(pass.extent.x)+uint(base.x/2);
    Receiver best;best.material=0u;best.distance=1e30;
    ivec2 bestPixel=base;uint bestSample=0u;
    bool covered=false;
    for(int i=0;i<4;++i) {
        ivec2 p=base+ivec2(i&1,i>>1);
        if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
        for(uint s=0u;s<pc.samples;++s) {
            Receiver r=receiverAt(p,s);
            covered=covered || r.material!=0u;
            if(usable(r) && r.distance<best.distance){best=r;bestPixel=p;bestSample=s;}
        }
    }
    if(!covered)return false;
    atomicOr(occupied,1u);
    uvec2 baseLight=packSurfaceLight(vec2(1),vec3(0));
    if(best.material!=0u)baseLight=accumulate(best,traceReceiver(best,bestPixel,bestSample),block);
    bool missing=false;
    for(int i=0;i<4;++i) {
        ivec2 p=base+ivec2(i&1,i>>1);
        if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
        for(uint s=0u;s<pc.samples;++s) {
            Receiver r=receiverAt(p,s);uint index=sampleIndex(p,s);
            if(!usable(r)){rawLight[index]=packSurfaceLight(vec2(1),vec3(0));continue;}
            if(sameReceiver(r,best)){rawLight[index]=baseLight;continue;}
            rawLight[index]=pendingSurfaceLight;
            missing=true;
        }
    }
    if(missing)atomicOr(unresolved,1u);
    return true;
}
#endif
void main() {
#ifdef SURFACE_EXTRA
    for(uint item=gl_WorkGroupID.x;item<extraTileCount;item+=gl_NumWorkGroups.x) {
        ivec2 origin=tileOrigin(tiles[item].y);
        // One invocation owns all samples of a pixel: duplicate MSAA receivers
        // can be completed together without cross-invocation reads or writes.
        for(uint i=gl_LocalInvocationIndex;i<256u;i+=64u) {
            ivec2 p=origin+ivec2(i%16u,i/16u);
            if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
            for(uint s=0u;s<pc.samples;++s) {
                if(!pendingLight(rawLight[sampleIndex(p,s)]))continue;
                Receiver r=receiverAt(p,s);
                uvec2 light=traceReceiver(r,p,s);
                for(uint next=s;next<pc.samples;++next) {
                    Receiver other=receiverAt(p,next);
                    if(other.material==r.material && other.depth==r.depth && other.normalKey==r.normalKey)
                        rawLight[sampleIndex(p,next)]=light;
                }
            }
        }
    }
#else
    if(gl_LocalInvocationIndex==0u){unresolved=0u;occupied=0u;}
    barrier();
    uint tile=gl_WorkGroupID.y*tileWidth()+gl_WorkGroupID.x;
    ivec2 base=tileOrigin(tile)+ivec2(gl_LocalInvocationIndex%8u,gl_LocalInvocationIndex/8u)*2;
    bool covered=traceBlock(base);
    barrier();
    // Extra tracing scans all pixels of an occupied tile. Initialize its empty
    // blocks too, so it never tests a stale/uninitialized pending marker.
    // Entirely empty tiles still need no raw/output writes.
    if(occupied!=0u && !covered) {
        for(int i=0;i<4;++i) {
            ivec2 p=base+ivec2(i&1,i>>1);
            if(any(greaterThanEqual(p,ivec2(pass.sceneExtent.xy))))continue;
            for(uint sampleId=0u;sampleId<pc.samples;++sampleId)
                rawLight[sampleIndex(p,sampleId)]=packSurfaceLight(vec2(1),vec3(0));
        }
    }
    if(gl_LocalInvocationIndex==0u) {
        if(occupied!=0u) {
            uint index=atomicAdd(tileCount,1u);
            tiles[index].x=tile;
            atomicMax(filterGroups.x,min((index+1u)*pc.filtersPerTile,pc.maxDispatch));
        }
        if(unresolved!=0u) {
            uint index=atomicAdd(extraTileCount,1u);
            tiles[index].y=tile;
            atomicMax(extraGroups.x,min(index+1u,pc.maxDispatch));
        }
    }
#endif
}
