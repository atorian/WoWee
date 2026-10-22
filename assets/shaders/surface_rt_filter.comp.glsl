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
// Cache the output tile plus the four-pixel halo. The 16x16 variant reduces
// overlapping receiver loads from four per output to 2.25. The owner selects
// it only when device shared-memory capacity covers these arrays.
shared vec4 positions[INPUT_COUNT];
shared vec3 normals[INPUT_COUNT];
shared uint materialKeys[INPUT_COUNT];
shared uvec2 estimates[INPUT_COUNT];
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
                // Inactive tiles have no raw storage initialized in this run.
                // They can occur in the halo; never fetch their stale estimates.
                estimates[i]=usable(r)?rawLight[sampleIndex(q,s)]:packSurfaceLight(vec2(1),vec3(0));
            }
            barrier();
            if(covered) {
                uvec2 result=estimates[center];
                if(materialKeys[center]!=0u) {
                    vec2 light=vec2(0);vec3 gi=vec3(0);float total=0;
                    for(int y=-2;y<=2;++y) for(int x=-2;x<=2;++x) {
                        uint tap=uint(int(center)+y*2*INPUT_SIZE+x*2);
                        if(materialKeys[tap]!=materialKeys[center] ||
                           dot(normals[center],normals[tap])<=0.99 ||
                           abs(dot(positions[center].xyz-positions[tap].xyz,normals[center]))>=0.01+0.002*positions[center].w)continue;
                        vec2 l;vec3 g;unpackSurfaceLight(estimates[tap],l,g);
                        float w=exp(-float(x*x+y*y)*0.5);
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
