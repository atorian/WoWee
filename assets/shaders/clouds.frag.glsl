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

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(push_constant) uniform Push {
    vec4 sunlitColor;      // xyz = LightIntBand 10, the side facing the light
    vec4 fillColor;        // xyz = LightIntBand 11, more of it where the cloud is thin
    vec4 baseColor;        // xyz = LightIntBand 12, everywhere
    vec4 lightDirDensity;  // xyz = toward the sun by day, the moon by night; w = density
    vec4 windAndWeather;   // x = windOffset, y = the storm weight
} push;

layout(location = 0) in vec3 vWorldDir;

layout(location = 0) out vec4 outColor;

// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

// Shared classic-style field and final alpha, including celestial glare.
#include "cloud_coverage.glsli"

void main() {
    vec3 dir = normalize(vWorldDir);
    float altitude = dir.z;
    if (altitude < 0.0) discard;

    vec3 lightDir = normalize(push.lightDirDensity.xyz);
    float density = push.lightDirDensity.w;
    float windOffset = push.windAndWeather.x;
    float storm = push.windAndWeather.y;

    CloudShape s = cloudShapeAt(dir, density, windOffset);
    float cloud = s.cloud;
    if (cloud < 0.01) discard;

    vec3 cloudRgb = cloudLighting(s, dir, lightDir, storm,
                                  push.sunlitColor.rgb, push.fillColor.rgb, push.baseColor.rgb);

    float alpha = cloud;

    if (alpha < 0.01) discard;

    // Behind the air, as the sky it is drawn over is: blended by alpha, so
    // the cloud and the dome under it come out with the same air in front.
    if (volumetricParams.x > 0.5) {
        vec4 clip = projection * mat4(mat3(view)) * vec4(vWorldDir, 1.0);
        vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
        cloudRgb = cloudRgb * air.a + air.rgb;
    }
    outColor = vec4(cloudRgb, alpha);
}
