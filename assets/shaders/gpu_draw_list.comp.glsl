#version 450
layout(local_size_x=64) in;
layout(set=0,binding=0,std430) readonly buffer Spheres { vec4 spheres[]; };
struct Group { uint first; uint count; uint visibleCount; };
layout(set=0,binding=1,std430) buffer Groups { Group groups[]; };
layout(set=0,binding=2,std430) writeonly buffer Visible { uint visible[]; };
// VkDrawIndexedIndirectCommand followed by its visibility group ID.
struct Draw { uint indexCount; uint instanceCount; uint firstIndex; int vertexOffset; uint firstInstance; uint group; };
layout(set=0,binding=3,std430) buffer Draws { Draw draws[]; };
layout(push_constant) uniform Push { vec4 planes[6]; uvec4 work; } push;
shared uint prefix[64];
shared uint total;
void main() {
    if(push.work.z==1) {
        uint index=push.work.y+gl_GlobalInvocationID.x;
        if(index<push.work.x)draws[index].instanceCount=groups[draws[index].group].visibleCount;
        return;
    }
    uint group=push.work.y+gl_WorkGroupID.x;
    if(group>=push.work.x)return;
    uint lane=gl_LocalInvocationID.x;
    uvec2 range=uvec2(groups[group].first,groups[group].count);
    if(lane==0)total=0;
    barrier();
    for(uint base=0;base<range.y;base+=64) {
        uint index=base+lane;
        bool keep=index<range.y;
        if(keep && push.work.w==0) {
            vec4 sphere=spheres[range.x+index];
            if(sphere.w>0)for(int p=0;p<6;++p)
                keep=keep && dot(push.planes[p].xyz,sphere.xyz)+push.planes[p].w>=-sphere.w-0.001;
        }
        prefix[lane]=keep?1u:0u;
        barrier();
        // Stable prefix preserves authored order when depths coincide.
        for(uint offset=1;offset<64;offset*=2) {
            uint add=lane>=offset?prefix[lane-offset]:0u;
            barrier();prefix[lane]+=add;barrier();
        }
        if(keep)visible[range.x+total+prefix[lane]-1]=index;
        barrier();
        if(lane==0)total+=prefix[63];
        barrier();
    }
    if(lane==0)groups[group].visibleCount=total;
}
