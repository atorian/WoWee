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

// The layer's shape: domain-warped fbm cumulus over stretched cirrus, faded
// at the horizon. Shared with the sun's glare, which is dimmed by the cloud
// in front of the sun. Octave counts: the shape and its sun-ward re-sample
// keep all five, because the self-shadow is their difference; the warp,
// erosion and cirrus are modulators and stop at three or four.
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
    float shape = s.shape;
    vec2 p = s.p;
    vec2 q = s.q;
    if (cloud < 0.01) discard;

    // --- Lighting, as WoW.exe lights its cloud texture (0x7efae0) ---
    // The base colour everywhere, the fill by how thin the cloud is - a
    // quarter of it at the densest, three quarters at the thinnest - and the
    // sunlit colour where the cloud faces the light, less of it in a storm.
    //
    // Where the client takes N.L off its texture's slope toward the light,
    // this layer has no texture: the slope is the shape re-sampled a step
    // toward the light, lower upstream lit and higher upstream in shadow.
    float towardLight = cloudFbm(p + q * 1.4 + lightDir.xy * 0.35, 5);
    float facing = clamp(1.0 - (towardLight - shape) * 2.2, 0.0, 1.0);
    float thinness = (64.0 + 127.5 * (1.0 - cloud)) / 255.0;
    vec3 cloudRgb = push.baseColor.rgb + thinness * push.fillColor.rgb +
                    push.sunlitColor.rgb * facing * (1.0 - 0.75 * storm);
    cloudRgb = min(cloudRgb, vec3(1.0));

    // --- Edge softness for alpha ---
    float alpha = cloud * smoothstep(0.0, 0.25, cloud);

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
