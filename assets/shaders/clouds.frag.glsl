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
    vec4 cloudColor;      // xyz = DBC-derived base cloud color, w = unused
    vec4 sunDirDensity;   // xyz = sun direction, w = density
    vec4 windAndLight;    // x = windOffset, y = sunIntensity, z = ambient, w = unused
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

    vec3 sunDir = normalize(push.sunDirDensity.xyz);
    float density = push.sunDirDensity.w;
    float windOffset = push.windAndLight.x;
    float sunIntensity = push.windAndLight.y;
    float ambient = push.windAndLight.z;

    CloudShape s = cloudShapeAt(dir, density, windOffset);
    float cloud = s.cloud;
    float cumulus = s.cumulus;
    float shape = s.shape;
    vec2 p = s.p;
    vec2 q = s.q;
    if (cloud < 0.01) discard;

    // --- Lighting ---
    float sunUp = clamp(sunDir.z, 0.0, 1.0);      // day factor
    float sunView = max(dot(dir, sunDir), 0.0);   // view alignment with the sun

    // Self-shadowing: re-sample the shape a step toward the sun; if the cloud
    // is denser upstream, this point sits in its own shadow.
    float towardSun = cloudFbm(p + q * 1.4 + sunDir.xy * 0.35, 5);
    float shadow = clamp(1.0 - (towardSun - shape) * 2.2, 0.35, 1.0);

    // Thick cores read darker - sunlight doesn't penetrate deep cloud
    float coreDarken = mix(1.0, 0.55, cumulus * cumulus);

    vec3 baseColor = push.cloudColor.rgb;
    vec3 shadowColor = baseColor * ambient * 0.75;
    vec3 litColor = baseColor * (ambient + sunIntensity * 0.85 * sunUp);
    vec3 cloudRgb = mix(shadowColor, litColor, shadow) * coreDarken;

    // Forward scattering: thin cloud near the sun glows warm
    float scatter = pow(sunView, 6.0) * sunIntensity * sunUp;
    cloudRgb += vec3(1.0, 0.92, 0.82) * (1.0 - cumulus) * scatter * 0.9;

    // Silver lining on sunlit cloud edges
    float edge = smoothstep(0.0, 0.35, cloud) * (1.0 - smoothstep(0.35, 0.85, cloud));
    cloudRgb += vec3(1.0, 0.95, 0.88) * edge * scatter * 0.6;

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
