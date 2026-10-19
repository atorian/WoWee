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
    vec4 playerPos;
    vec4 playerWake;
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
};

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

#include "sky_atmosphere.glsli"

layout(push_constant) uniform Push {
    vec4 zenithColor;
    vec4 midColor;
    vec4 horizonColor;
    vec4 band2Color;
    vec4 smogColor;
    vec4 fogColorPush;
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

void main() {
    // Reconstruct world-space ray direction from screen position.
    float ndcX =  TexCoord.x * 2.0 - 1.0;
    float ndcY = -(TexCoord.y * 2.0 - 1.0);

    vec3 viewDir = vec3(ndcX / projection[0][0],
                        ndcY / abs(projection[1][1]),
                        -1.0);

    mat3 invViewRot = transpose(mat3(view));
    vec3 worldDir = normalize(invViewRot * viewDir);

    vec3 sky = atmosphereSkyColor(worldDir, push.zenithColor.rgb,
                                  push.midColor.rgb, push.horizonColor.rgb,
                                  push.band2Color.rgb, push.smogColor.rgb,
                                  push.fogColorPush.rgb);

    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeSky(TexCoord);
        sky = sky * air.a + air.rgb;
    }

    outColor = vec4(sky, 1.0);
}
