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
    vec4 rtTraceParams;
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

#include "interior_light.glsli"

layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

// One quad per instance (M2ParticleQuad), four corners drawn as a strip.
layout(location = 0) in vec3 aPos;     // the particle (its head)
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aSize;    // half its width and height, in world units
layout(location = 3) in float aTile;
layout(location = 4) in vec3 aAxisA;   // tail vector, -velocity, or the plane's x axis
layout(location = 5) in vec3 aAxisB;   // the plane's y axis
layout(location = 6) in float aSpin;   // radians, counter-clockwise, for a billboard
layout(location = 7) in uint aShape;   // M2ParticleShape
layout(location = 8) in uint aLit;
layout(location = 9) in vec4 aInteriorAmbient;
layout(location = 10) in vec4 aInteriorDirect;
layout(location = 11) in uvec4 aLocalLights;  // local_lights.glsli

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;
layout(location = 2) out float vFogVisibility;
layout(location = 3) out vec2 vUV;

const uint kShapeBillboard = 0u;
const uint kShapeTail = 1u;
const uint kShapeAlongVelocity = 2u;
const uint kShapePlane = 3u;

// The air between the camera and this point, out of the fog volume: rgb is
// the light it scatters toward the camera, a how much of the point shows
// through it. See VolumetricFog.
vec4 fogVolumeAt(vec3 worldPos) {
    vec4 clip = projection * view * vec4(worldPos, 1.0);
    float depth = max(clip.w, 1e-4);
    vec2 uv = clip.xy / depth * 0.5 + 0.5;
    float slice = log(max(depth, volumetricParams.y) / volumetricParams.y) * volumetricParams.z;
    // Each slice holds the air up to its far edge, so a point is read half a
    // slice back from where it stands.
    return textureLod(uFogVolume, vec3(uv, slice - 0.5 / volumetricParams.w), 0.0);
}

#include "local_lights.glsli"

// The light on a lit particle (CDiffuse_T1): every vertex has the normal
// world +Z, so it is lit like a surface facing up, whichever way it faces.
// The model's light - the sun, or the interior's where the model stands -
// plus the ambient and the local lights, saturated.
vec3 particleLight(vec3 worldPos) {
    const vec3 n = vec3(0.0, 0.0, 1.0);
    const vec3 toSun = normalize(-interiorLightDir(aInteriorAmbient, lightDir.xyz));
    const vec3 sun = min(interiorDirect(aInteriorAmbient, aInteriorDirect, lightColor.rgb), vec3(1.0));
    vec3 light = clamp(dot(n, toSun), 0.0, 1.0) * sun +
                 interiorAmbient(aInteriorAmbient, ambientColor.rgb) +
                 localLightDiffuse(worldPos, n, aLocalLights);
    return clamp(light, 0.0, 1.0);
}

void main() {
    // The corner: x across the quad, y along it (+1 at the head); u from
    // left to right, v from top to bottom.
    const vec2 corner = vec2((gl_VertexIndex & 1) != 0 ? 1.0 : -1.0,
                             gl_VertexIndex >= 2 ? 1.0 : -1.0);
    const vec2 cornerUV = vec2(corner.x * 0.5 + 0.5, 0.5 - corner.y * 0.5);
    const vec3 head = (view * vec4(aPos, 1.0)).xyz;

    // As the client lays them out (section 4.5 of the render spec).
    vec3 p = head;
    vUV = cornerUV;
    bool billboard = aShape == kShapeBillboard;
    if (aShape == kShapeTail) {
        // From the head back along its flight, in view space, as wide as
        // the particle across it; u from the head to the end, v across. One
        // too short to see on screen is a plain billboard.
        const vec3 t = mat3(view) * aAxisA;
        const float tLen2 = dot(t.xy, t.xy);
        if (tLen2 > 1.0 / 1296.0) {
            const vec2 perp = vec2(-t.y * aSize.y, t.x * aSize.x) * inversesqrt(tLen2);
            const vec3 end = corner.y > 0.0 ? head : head + t;
            p = end + vec3(perp * corner.x, 0.0);
            vUV = vec2(corner.y > 0.0 ? 0.0 : 1.0, corner.x > 0.0 ? 0.0 : 1.0);
        } else {
            billboard = true;
        }
    } else if (aShape == kShapeAlongVelocity) {
        // Facing the camera, its x along the projected flight and shortened
        // as the flight turns toward the camera; u from the leading edge to
        // the trailing one. Flying straight at the camera it has no width.
        const vec3 d3 = mat3(view) * aAxisA;
        const float len2 = dot(d3.xy, d3.xy);
        if (len2 > 2.4e-7) {
            const float len = sqrt(len2);
            const vec2 along = d3.xy / len;
            const float a = corner.x * aSize.x * len / length(d3);
            const float b = corner.y * aSize.y;
            p = head + vec3(a * along.x - b * along.y, a * along.y + b * along.x, 0.0);
        }
    } else if (aShape == kShapePlane) {
        // Flat in its plane, both axes already turned by the spin.
        const vec3 world = aPos + corner.x * aSize.x * aAxisA + corner.y * aSize.y * aAxisB;
        p = (view * vec4(world, 1.0)).xyz;
    }
    if (billboard) {
        // Corners at +-1 times the half size, turned by the spin.
        const float c = cos(aSpin);
        const float s = sin(aSpin);
        const vec2 offset = corner * aSize;
        p = head + vec3(c * offset.x - s * offset.y, s * offset.x + c * offset.y, 0.0);
    }

    // Its colour times the light, if it is lit; nothing is added to it.
    vColor = aColor;
    if (aLit != 0u) vColor.rgb = clamp(aColor.rgb * particleLight(aPos), 0.0, 1.0);
    vTile = aTile;
    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
    // And thinned by the air in front of it, the way distance thins it:
    // a spark deep in a bank of mist is mostly the mist.
    if (volumetricParams.x > 0.5) vFogVisibility *= fogVolumeAt(aPos).a;
    gl_Position = projection * vec4(p, 1.0);
}
