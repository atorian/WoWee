#version 450
#include "surface_rt_inputs.glsli"
layout(local_size_x=8,local_size_y=8) in;
// An 8x8 output tile plus the four-pixel filter halo. Decode each receiver
// once for the group instead of reconstructing it at every one of 25 taps.
// Fits the Vulkan minimum shared-memory limit, including MSAA: samples
// reuse this storage and synchronize before the next sample overwrites it.
shared vec4 positions[256];
shared vec3 normals[256];
shared uint materialKeys[256];
shared uvec2 estimates[256];
void main() {
    ivec2 p=ivec2(gl_GlobalInvocationID.xy);
    ivec2 origin=ivec2(gl_WorkGroupID.xy)*8-4;
    uint center=(gl_LocalInvocationID.y+4u)*16u+gl_LocalInvocationID.x+4u;
    bool covered=all(lessThan(p,ivec2(pass.sceneExtent.xy)));
    for(uint s=0u;s<pc.samples;++s) {
        for(uint i=gl_LocalInvocationIndex;i<256u;i+=64u) {
            ivec2 q=origin+ivec2(i%16u,i/16u);
            materialKeys[i]=0u;
            if(any(lessThan(q,ivec2(0))) || any(greaterThanEqual(q,ivec2(pass.sceneExtent.xy))))continue;
            Receiver r=receiverAt(q,s);
            positions[i]=vec4(r.position,r.distance);
            normals[i]=r.normal;
            materialKeys[i]=usable(r)?r.material:0u;
            estimates[i]=rawLight[sampleIndex(q,s)];
        }
        barrier();
        if(covered) {
            uvec2 result=estimates[center];
            if(materialKeys[center]!=0u) {
                vec2 light=vec2(0);vec3 gi=vec3(0);float total=0;
                for(int y=-2;y<=2;++y)for(int x=-2;x<=2;++x) {
                    uint tap=uint(int(center)+y*32+x*2);
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
