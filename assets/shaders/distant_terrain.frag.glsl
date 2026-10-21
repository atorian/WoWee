#version 450
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
    vec4 playerPos;   // xyz = player world position, w = horizontal speed
    vec4 playerWake;  // xyz = trailing player position (springback reference)
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 fogSkyTop;  // rgb = zenith, w = sky tint amount (0 disables it)
    vec4 fogSkyMiddle;
    vec4 fogSkyBand1;
    vec4 fogSkyBand2;
    mat4 contactSpaceMatrix;  // read by contact_shadows.glsli
    vec4 contactParams;
    vec4 windDirLean;
    vec4 windPhases;
    vec4 windTravel;
    vec4 skySmogColor;
    vec4 aerialSun;
    vec4 aerialLight;
    vec4 terrainLodParams;
    vec4 aerialParams;
    mat4 lightningMatrix;
    vec4 lightningPosition;
    vec4 lightningColor;
};
#include "sky_atmosphere.glsli"
#include "aerial_frame.glsli"
#include "terrain_distance.glsli"
layout(set=0,binding=2) uniform sampler3D uFogVolume;
layout(location=0) in vec3 FragPos;
layout(location=1) in vec3 Normal;
layout(location=2) in vec3 Color;
layout(location=3) in vec2 TileUV;
layout(push_constant) uniform Coverage { uvec4 mask[2]; } coverage;
layout(location=0) out vec4 outColor;
void main(){
    if(length(FragPos.xy-viewPos.xy)>=fogSkyBand1.w)discard;
    ivec2 cell=clamp(ivec2(TileUV*16),ivec2(0),ivec2(15));
    int bit=cell.y*16+cell.x;
    uint word=coverage.mask[bit/128][(bit/32)%4];
    if((word&(1u<<uint(bit%32)))!=0u && terrainCoverageThreshold() >= distantTerrainWeight(FragPos)) discard;
    vec3 lit=Color*(ambientColor.rgb+lightColor.rgb*max(dot(normalize(Normal),normalize(-lightDir.xyz)),0));
    float t=aerialVisibilityAt(FragPos,length(FragPos-viewPos.xyz));
    vec3 color=mix(aerialColorAt(FragPos-viewPos.xyz),lit,t);
    if(volumetricParams.x>0.5) {
        vec4 clip=projection*view*vec4(FragPos,1);
        float depth=max(clip.w,1e-4);
        vec2 uv=clip.xy/depth*0.5+0.5;
        float slice=log(max(depth,volumetricParams.y)/volumetricParams.y)*volumetricParams.z;
        vec4 air=textureLod(uFogVolume,vec3(uv,slice-0.5/volumetricParams.w),0);
        color=color*air.a+air.rgb;
    }
    outColor=vec4(color,1);
}
