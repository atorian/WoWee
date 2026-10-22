#version 450
#include "surface_rt_inputs.glsli"
#ifdef SURFACE_WIDE
#define FILTER_SIZE 16
#else
#define FILTER_SIZE 8
#endif
#define INPUT_SIZE (FILTER_SIZE+8)
#define INPUT_COUNT (INPUT_SIZE*INPUT_SIZE)
#define FILTER_THREADS (FILTER_SIZE*FILTER_SIZE)
#define FILTERS_PER_TILE ((16/FILTER_SIZE)*(16/FILTER_SIZE))
layout(local_size_x=FILTER_SIZE,local_size_y=FILTER_SIZE) in;
// Reconstruct on the full-resolution pixel lattice. Stride-two taps keep the
// four pixel parities disconnected and preserve 2x2 blocks after convergence.
// Two separable, receiver-guided 9-tap steps retain the old four-pixel radius
// (sigma=2) without 81 taps per output or a second GPU dispatch/scratch image.
shared vec4 positions[INPUT_COUNT];
shared vec3 normals[INPUT_COUNT];
shared uint materialKeys[INPUT_COUNT];
shared uvec2 estimates[INPUT_COUNT];
shared uvec2 horizontalLight[INPUT_SIZE*FILTER_SIZE];
bool compatible(uint center,uint tap) {
    return materialKeys[tap]==materialKeys[center] &&
        dot(normals[center],normals[tap])>0.99 &&
        abs(dot(positions[center].xyz-positions[tap].xyz,normals[center]))<0.01+0.002*positions[center].w;
}
void main() {
    for(uint item=gl_WorkGroupID.x;item<tileCount*FILTERS_PER_TILE;item+=gl_NumWorkGroups.x) {
        uint quadrant=item%FILTERS_PER_TILE;
        ivec2 outputOrigin=tileOrigin(tiles[item/FILTERS_PER_TILE].x)+ivec2(quadrant&1u,quadrant>>1u)*FILTER_SIZE;
        ivec2 p=outputOrigin+ivec2(gl_LocalInvocationID.xy);
        ivec2 origin=outputOrigin-4;
        uint center=(gl_LocalInvocationID.y+4u)*INPUT_SIZE+gl_LocalInvocationID.x+4u;
        bool covered=all(lessThan(p,ivec2(pass.sceneExtent.xy)));
        for(uint s=0u;s<pc.samples;++s) {
            for(uint i=gl_LocalInvocationIndex;i<INPUT_COUNT;i+=FILTER_THREADS) {
                ivec2 q=origin+ivec2(i%INPUT_SIZE,i/INPUT_SIZE);
                materialKeys[i]=0u;
                if(any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,ivec2(pass.sceneExtent.xy))))continue;
                Receiver r=receiverAt(q,s);
                positions[i]=vec4(r.position,r.distance);
                normals[i]=r.normal;
                materialKeys[i]=usable(r)?r.material:0u;
                // A halo can overlap an inactive tile. Never read its stale light.
                estimates[i]=usable(r)?rawLight[sampleIndex(q,s)]:packSurfaceLight(vec2(1),vec3(0));
            }
            barrier();
            // Include halo rows so vertical reconstruction has the same support
            // at workgroup boundaries. Each horizontal result has one writer.
            for(uint i=gl_LocalInvocationIndex;i<INPUT_SIZE*FILTER_SIZE;i+=FILTER_THREADS) {
                uint mid=(i/FILTER_SIZE)*INPUT_SIZE+(i%FILTER_SIZE)+4u;
                uvec2 result=packSurfaceLight(vec2(1),vec3(0));
                if(materialKeys[mid]!=0u) {
                    vec2 light=vec2(0);vec3 gi=vec3(0);float total=0;
                    for(int x=-4;x<=4;++x) {
                        uint tap=uint(int(mid)+x);
                        if(!compatible(mid,tap))continue;
                        vec2 l;vec3 g;unpackSurfaceLight(estimates[tap],l,g);
                        float w=exp(-float(x*x)*0.125);
                        light+=l*w;gi+=g*w;total+=w;
                    }
                    result=packSurfaceLight(light/total,gi/total);
                }
                horizontalLight[i]=result;
            }
            barrier();
            if(covered) {
                uvec2 result=estimates[center];
                if(materialKeys[center]!=0u) {
                    vec2 light=vec2(0);vec3 gi=vec3(0);float total=0;
                    for(int y=-4;y<=4;++y) {
                        uint tap=uint(int(center)+y*INPUT_SIZE);
                        if(!compatible(center,tap))continue;
                        uint horizontal=uint(int(gl_LocalInvocationID.y)+4+y)*FILTER_SIZE+gl_LocalInvocationID.x;
                        vec2 l;vec3 g;unpackSurfaceLight(horizontalLight[horizontal],l,g);
                        float w=exp(-float(y*y)*0.125);
                        light+=l*w;gi+=g*w;total+=w;
                    }
                    result=packSurfaceLight(light/total,gi/total);
                }
                filteredLight[sampleIndex(p,s)]=result;
            }
            barrier();
        }
    }
}
