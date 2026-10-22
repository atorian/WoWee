#version 450
#include "../../assets/shaders/surface_encoding.glsli"
layout(push_constant) uniform Probe { vec4 depths; uvec4 config; } pc;
layout(location=0) out uvec4 keys;
layout(location=1) out uvec4 payload;
void main() {
    ivec2 p=ivec2(gl_FragCoord.xy);
    int s=gl_SampleID;
    bool front=pc.config.y==0u && (pc.config.x>1u?s==0:((p.x&1)==0 && (p.y&1)==0));
    bool reverse=pc.config.x>1u && s==2;
    // Deliberately reverse the winding: RT must face the geometric normal
    // toward the visible side, as the forward consumer does.
    vec3 normal=reverse?vec3(0,0,1):vec3(1,0,0);
    keys=uvec4(packSurfaceNormal(normal),0,0,front?1u:2u);
    payload=uvec4(0,0,floatBitsToUint(front?pc.depths.x:pc.depths.y),0);
    if(pc.config.x>1u && s==3)keys.w=0u;
}
