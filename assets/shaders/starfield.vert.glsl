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

layout(push_constant) uniform Push {
    float time;
    float intensity;
    float viewportHeight;
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in float aBrightness;
layout(location = 2) in float aTwinklePhase;
layout(location = 3) in float aColorTemp;

layout(location = 0) out float vBrightness;
layout(location = 1) out vec3 vColor;
layout(location = 2) flat out float vFootprint;

// Cool red-orange, through white, to hot blue-white: the spread a real field
// shows. Every star used to be the same (0.9, 0.95, 1.0).
vec3 starColor(float t) {
    const vec3 cool = vec3(1.00, 0.80, 0.62);
    const vec3 mid  = vec3(1.00, 0.97, 0.94);
    const vec3 hot  = vec3(0.78, 0.86, 1.00);
    return t < 0.5 ? mix(cool, mid, t * 2.0)
                   : mix(mid, hot, (t - 0.5) * 2.0);
}

void main() {
    mat4 rotView = mat4(mat3(view));

    // Scintillation is not one slow sine across the whole sky. Two rates that
    // do not divide into each other never repeat, which is what keeps it from
    // reading as a pulse.
    float twinkle = 0.84
        + 0.10 * sin(push.time * 2.3 + aTwinklePhase)
        + 0.06 * sin(push.time * 5.7 + aTwinklePhase * 1.7);
    vBrightness = aBrightness * twinkle * push.intensity;
    vColor = starColor(aColorTemp);

    // gl_PointSize is in pixels, so a fixed one is a different angular size at
    // every resolution - the same star is twice as wide at 1080p as at 4K.
    // projection[1][1] is 1/tan(fovY/2), so this holds the angle instead, and
    // the floor reserves enough fragments to integrate a point over pixels.
    // This is the filter's support, not a filled luminous disc.
    const float kAngularRadius = 0.0019;
    float sizeScale = mix(0.85, 1.30, aBrightness);
    gl_PointSize = clamp(
        kAngularRadius * push.viewportHeight * projection[1][1] * sizeScale,
        3.0, 5.0);
    vFootprint = gl_PointSize;

    vec4 clip = projection * rotView * vec4(aPos, 1.0);
    // On the far plane, so the world drawn before the sky occludes it.
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
