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

#include "lightning_light.glsli"

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

#include "sky_atmosphere.glsli"
#include "aerial_frame.glsli"
#include "rt_lighting.glsli"
#include "contact_shadows.glsli"

// One texel of the shadow map, as the terrain takes it.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

#include "shadow_receiver.glsli"

// How much of the sun reaches the water here, 1 lit to 0 in shadow: the
// sun's map, the units' map and the traced sun, the way the ground beside it
// takes them, so a tree's shadow runs from the bank onto the water unbroken.
float sunOnWater(vec3 worldPos) {
    float shadow = 1.0;
    const vec3 up = vec3(0.0, 0.0, 1.0);
    vec3 ldir = normalize(-lightDir.xyz);
    float normalOffset = shadowParams.w * 1.5 * (1.0 - abs(dot(up, ldir)));
    vec4 lsPos = lightSpaceMatrix * vec4(worldPos + up * normalOffset, 1.0);
    vec3 proj = lsPos.xyz / lsPos.w;
    proj.xy = proj.xy * 0.5 + 0.5;
    vec2 receiverSlope = shadowReceiverSlope(up);
    if (shadowParams.x > 0.5 && proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 &&
        proj.z >= 0.0 && proj.z <= 1.0) {
        shadow = sampleShadowReceiverPlane(uShadowMap, proj, receiverSlope);
        shadow = mix(1.0, shadow, shadowMapEdgeFade(proj.xy * 2.0 - 1.0));
    }
    RtLight rt = rtLightAt(worldPos, up);
    shadow = rtShadow(rt, shadow);
    return min(shadow, localShadowAt(worldPos, up));
}

layout(push_constant) uniform Push {
    mat4 model;
    float waveAmp;
    float waveFreq;
    float waveSpeed;
    float liquidBasicType;
    vec2 screenSize;  // size of the target being drawn into
    vec2 depthRange;  // the camera's own near and far, for linearising SceneDepth
    float sceneValid; // 1 once the refraction capture holds a frame, 0 before it
    float meshStep;   // yards between the mesh's vertices (the vertex stage's)
    float exposed;    // 1 where the wind reaches the surface, 0 indoors where it lies still
    float pad2;
} push;

layout(set = 1, binding = 0) uniform WaterMaterial {
    vec4 waterColor;
    float waterAlpha;
    float shimmerStrength;
    float alphaScale;
    float bodyFloor;   // least the water's own colour shows, however shallow
};

layout(set = 2, binding = 0) uniform sampler2D SceneColor;
layout(set = 2, binding = 1) uniform sampler2D SceneDepth;
layout(set = 2, binding = 2) uniform sampler2D ReflectionColor;
// Wake points are laid down behind anything wading through the shallows. Each
// is a churn site that spreads and dies, so a running character leaves a trail
// of froth rather than a disc stuck under its feet.
const int MAX_WAKE_POINTS = 32;
// kOceanCascadeCount in ocean_waves.hpp.
const int OCEAN_CASCADES = 3;
// OceanSea::kKeptEntries / 4.
const int OCEAN_KEPT_VEC4S = 408;

layout(set = 2, binding = 3) uniform WaterFrameData {
    mat4 reflViewProj;
    vec4 wakeBounds;                  // xy = centre, z = cull radius, w = count
    vec4 wakePoints[MAX_WAKE_POINTS]; // xy = world pos, z = age 0..1, w = strength
    // The wind's sea (OceanWaves): per cascade xy = the origin its patch is
    // measured from, z = 1 / patch, w = patch, in yards; and the sea as it
    // stands, x = its root mean square height, y = how far its water rides
    // toward the crests, z = its mean square slope.
    vec4 oceanCascades[OCEAN_CASCADES];
    vec4 oceanSea;
    // How much of each cascade's height and slope the water keeps at the
    // table's depths and fetches (OceanSea::keptTable).
    vec4 oceanKept[OCEAN_KEPT_VEC4S];
    // The sea bed under the water (SeaBedMap): world to its clip space, and
    // x = the height of depth 0, y = yards to depth 1, z = 1 once drawn.
    mat4 seaBedMatrix;
    vec4 seaBed;
    // The wake round the camera (WaterWake): xy = its corner, z = 1 / its
    // side in yards, w = 1 once it has run.
    vec4 wakeArea;
    vec4 reflectionSun;
    vec4 reflectionSunColor;
};
layout(set = 2, binding = 4) uniform sampler2DArray oceanSurface;
layout(set = 2, binding = 5) uniform sampler2D seaBedMap;
layout(set = 2, binding = 6) uniform sampler2D seaFetchMap;
layout(set = 2, binding = 7) uniform sampler2D wakeSurface;

#include "ocean_surface.glsli"
#include "water_wake_surface.glsli"

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 RestPos;   // where this point lies on the still surface
layout(location = 2) in vec2 TexCoord;
layout(location = 4) in vec2 ScreenUV;

layout(location = 0) out vec4 outColor;

// ============================================================
// GGX/Cook-Torrance BRDF
// ============================================================
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    return a2 / (3.14159265 * denom * denom + 1e-7);
}

float GeometrySmith(float NdotV, float NdotL, float roughness) {
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    return ggx1 * ggx2;
}

vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ============================================================
// Linearize depth
// ============================================================
float linearizeDepth(float d, float near, float far) {
    return near * far / (far - d * (far - near));
}

// ============================================================
// Noise functions for foam
// ============================================================
float hash21(vec2 p) {
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float hash22x(vec2 p) {
    return fract(sin(dot(p, vec2(269.5, 183.3))) * 43758.5453);
}

float noiseValue(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hash21(i);
    float b = hash21(i + vec2(1.0, 0.0));
    float c = hash21(i + vec2(0.0, 1.0));
    float d = hash21(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

float fbmNoise(vec2 p, float time) {
    float v = 0.0;
    v += noiseValue(p * 3.0 + time * 0.3) * 0.5;
    v += noiseValue(p * 6.0 - time * 0.5) * 0.25;
    v += noiseValue(p * 12.0 + time * 0.7) * 0.125;
    return v;
}

// ============================================================
// Gradient (Perlin) noise + rotated fractal - for the magma/slime flow
// ============================================================
// Value noise (noiseValue above) samples a scalar per lattice point and
// interpolates it, so its features sit square on the integer grid and read as
// axis-aligned blocks up close - which is what made the slime motion look like
// tiles. Gradient noise instead interpolates a random gradient that is zero at
// each lattice point, so neighbouring cells blend through zero without the flat
// plateaus, and rotating every octave keeps the separate lattices from ever
// lining up. The result is a fractal flow rather than a grid of squares.
vec2 gradHash(vec2 p) {
    p = vec2(dot(p, vec2(127.1, 311.7)), dot(p, vec2(269.5, 183.3)));
    return -1.0 + 2.0 * fract(sin(p) * 43758.5453123);
}

float gradNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = dot(gradHash(i + vec2(0.0, 0.0)), f - vec2(0.0, 0.0));
    float b = dot(gradHash(i + vec2(1.0, 0.0)), f - vec2(1.0, 0.0));
    float c = dot(gradHash(i + vec2(0.0, 1.0)), f - vec2(0.0, 1.0));
    float d = dot(gradHash(i + vec2(1.0, 1.0)), f - vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y) * 0.5 + 0.5;
}

// Same octave structure and value range as fbmNoise, but on gradient noise and
// with a small rotation between octaves so no two lattices align.
float fbmGrad(vec2 p, float time) {
    const mat2 R = mat2(0.80, -0.60, 0.60, 0.80);   // ~37° per octave
    float v = 0.0;
    v += gradNoise(p * 3.0 + time * 0.3) * 0.5;   p = R * p;
    v += gradNoise(p * 6.0 - time * 0.5) * 0.25;  p = R * p;
    v += gradNoise(p * 12.0 + time * 0.7) * 0.125;
    return v;
}

// Voronoi-like cellular noise for foam particles
// jitter parameter controls how much cell points deviate from grid centers
// (0.0 = regular grid, 1.0 = fully random within cell)
float cellularFoam(vec2 p, float jitter) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float minDist = 1.0;
    for (int y = -1; y <= 1; y++) {
        for (int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 cellId = i + neighbor;
            // Jittered cell point - higher jitter = more irregular placement
            vec2 point = vec2(hash21(cellId), hash22x(cellId)) * jitter
                       + vec2(0.5) * (1.0 - jitter);
            float d = length(neighbor + point - f);
            minDist = min(minDist, d);
        }
    }
    return minDist;
}
float cellularFoam(vec2 p) { return cellularFoam(p, 1.0); }

// How much of a cellular pattern with `cellsPerYard` cells a pixel can show:
// all of it from six pixels a cell, none by two. The foam is drawn in cells a
// few hundredths of a yard across - a speck at the player's feet is a pixel
// a few yards off - and a speck smaller than its pixel lands on it or misses
// it from one frame to the next, so the whole shoreline sparkled and shook as
// the surf and the camera moved.
float cellsShown(float cellsPerYard, float pixelsPerYard) {
    return smoothstep(2.0, 6.0, pixelsPerYard / cellsPerYard);
}

// A dot of `radius` (in cells) in every cell of `cellsPerYard` at `p`. It is
// the dot that has to fit a pixel, not the cell: a dot a tenth of its cell
// across is under a pixel while the cell is still ten. Where the dots are
// too small to show, what is left is what they cover on average, so the
// foam a band of them made stays as a tint rather than going out: a dot
// falling off as smoothstep over its radius covers 0.3 pi r^2.
float foamDots(vec2 p, float radius, float cellsPerYard, float pixelsPerYard) {
    float dotPixels = 2.0 * radius * pixelsPerYard / cellsPerYard;
    float shown = smoothstep(1.5, 4.0, dotPixels);
    float mean = 0.942 * radius * radius;
    if (shown <= 0.001) return mean;
    return mix(mean, 1.0 - smoothstep(0.0, radius, cellularFoam(p)), shown);
}

// The distance to the nearest cell point, or its average - 0.43 for a fully
// jittered grid - where the cells are too small to show.
float cellDistance(vec2 p, float shown) {
    const float kMeanDistance = 0.43;
    if (shown <= 0.001) return kMeanDistance;
    return mix(kMeanDistance, cellularFoam(p), shown);
}

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

// The zone's distance fog, then the air in front of it. The distance fog is
// the far haze the sky is painted to meet, so it goes on first; the volume is
// everything between the camera and that, sunlit shafts and torch glow
// included.
vec3 applyFog(vec3 color, vec3 worldPos, float dist) {
    if (physicalAtmosphere()) {
        vec4 air = worldAtmosphere(worldPos);
        return color*air.a+air.rgb;
    }
    float fogFactor = aerialVisibilityAt(worldPos, dist);
    color = mix(aerialColorAt(worldPos - viewPos.xyz), color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}

// The sky seen along `dir`: the sky as drawn this frame where that way is on
// screen - its clouds, the sun's glow, the colours of the hour - and the
// atmosphere's gradient where it is not. A way below the horizon, which a
// steep wave can reflect, takes the horizon.
vec3 skyAlong(vec3 dir) {
    dir = normalize(vec3(dir.xy, max(dir.z, 0.0)));
    vec3 gradient = atmosphereSkyColor(dir, fogSkyTop.rgb, fogSkyMiddle.rgb,
                                       fogSkyBand1.rgb, fogSkyBand2.rgb,
                                       skySmogColor.rgb, fogColor.rgb);
    gradient = aerialSky(gradient,dir);
    if (physicalAtmosphere()) {
        vec4 air = atmosphereFallback(dir*max(fogSkyBand1.w,1.0));
        gradient = gradient*air.a+air.rgb;
    }
    if (fogSkyMiddle.w < 0.5) return gradient;
    vec4 clip = projection * view * vec4(viewPos.xyz + dir * 1000.0, 1.0);
    if (clip.w <= 0.0) return gradient;
    vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
    float onScreen = smoothstep(0.0, 0.05, uv.x) * smoothstep(1.0, 0.95, uv.x)
                   * smoothstep(0.0, 0.05, uv.y) * smoothstep(1.0, 0.95, uv.y);
    if (onScreen <= 0.0) return gradient;
    vec3 captured=textureLod(uSceneSky,clamp(uv,vec2(0.0),vec2(1.0)),0.0).rgb;
    if(volumetricParams.x>0.5) {
        vec4 air=textureLod(uFogVolume,vec3(uv,1.0),0.0);
        captured=captured*air.a+air.rgb;
    }
    return mix(gradient,captured,onScreen);
}

void main() {
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

    float time = fogParams.z;
    float basicType = push.liquidBasicType;

    // ============================================================
    // Magma / Slime - self-luminous flowing surfaces, skip water path
    // ============================================================
    if (basicType > 1.5) {
        float dist = length(viewPos.xyz - FragPos);
        vec2 flowUV = FragPos.xy;

        bool isMagma = basicType < 2.5;

        // Multi-octave flowing noise for organic lava/slime look. Gradient-noise
        // fractal (fbmGrad/gradNoise) rather than value noise (fbmNoise/
        // noiseValue): same scales, weights and scroll, but the flow reads as a
        // fractal instead of the axis-aligned squares the grid basis produced.
        float n1 = fbmGrad(flowUV * 0.06 + vec2(time * 0.02, time * 0.03), time * 0.4);
        float n2 = fbmGrad(flowUV * 0.10 + vec2(-time * 0.015, time * 0.025), time * 0.3);
        float n3 = gradNoise(flowUV * 0.25 + vec2(time * 0.04, -time * 0.02));
        float flow = n1 * 0.45 + n2 * 0.35 + n3 * 0.20;

        // Dark crust vs bright molten core
        vec3 crustColor, hotColor, coreColor;
        if (isMagma) {
            crustColor = vec3(0.15, 0.04, 0.01);   // dark cooled rock
            hotColor   = vec3(1.0, 0.45, 0.05);     // orange molten
            coreColor  = vec3(1.0, 0.85, 0.3);      // bright yellow-white core
        } else {
            crustColor = vec3(0.05, 0.15, 0.02);
            hotColor   = vec3(0.3, 0.8, 0.15);
            coreColor  = vec3(0.5, 1.0, 0.3);
        }

        // Three-tier color: crust → molten → hot core
        float crustMask = smoothstep(0.25, 0.50, flow);
        float coreMask  = smoothstep(0.60, 0.80, flow);
        vec3 color = mix(crustColor, hotColor, crustMask);
        color = mix(color, coreColor, coreMask);

        // Subtle pulsing emissive glow
        float pulse = 1.0 + 0.15 * sin(time * 1.5 + flow * 6.0);
        color *= pulse;

        // Emissive brightening for hot areas
        color *= 1.0 + coreMask * 0.6;

        color = applyFog(color, FragPos, dist);
        outColor = vec4(color, 0.97);
        return;
    }

    // Derived from the render target, NOT from textureSize(SceneColor): the
    // refraction copy is kept at half resolution, so its size would put every
    // screen-space lookup at twice the intended UV.
    vec2 screenUV = gl_FragCoord.xy / max(push.screenSize, vec2(1.0));

    float dist = length(viewPos.xyz - FragPos);
    float sunReach = sunOnWater(FragPos);

    // --- Normal computation ---
    // How the surface moves from one pixel to the next, along the pixel's
    // longer side, for patterns drawn the same in every direction: those
    // have to fit the worst of it.
    vec2 surfaceDx = dFdx(FragPos.xy);
    vec2 surfaceDy = dFdy(FragPos.xy);
    float pixelsPerYard = 1.0 / max(max(length(surfaceDx), length(surfaceDy)), 1e-4);
    // The wind's sea, taken where this point lay on the still surface: the
    // geometry has carried it along the waves toward their crests, and the
    // surface it is on is the one those waves make of the still one. Its
    // slope is of the waves this pixel is big enough to show - the filtered
    // surface lies down into a smooth sheet as they shrink on screen, which
    // is what a fade by distance stood in for - and the slope of the rest
    // goes into the highlight's width below.
    OceanLight sea = oceanLightAt(RestPos, push.exposed);
    // And on it the wake of what moves through it: its slopes add, as waves'
    // do (WaterWake).
    vec4 wake = waterWakeAt(RestPos.xy);
    vec3 norm = normalize(vec3(-(sea.slope + wake.yz), 1.0));    // The height as a share of the sea's own: one where a crest stands two
    // of its standard deviations up.
    float waveHeight = sea.height / max(2.0 * oceanSea.x, 1e-3);
    // Where riding toward the crests has bunched the surface to under half,
    // the crest is too steep to hold and is breaking.
    float breaking = smoothstep(0.55, 0.25, sea.jacobian) * push.exposed;

    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    // The glitter path follows the disc, not the legacy diffuse-light azimuth.
    vec3 ldir = normalize(reflectionSun.xyz);
    float NdotV = max(dot(norm, viewDir), 0.001);
    float NdotL = max(dot(norm, ldir), 0.0);

    // --- Schlick Fresnel ---
    const vec3 F0 = vec3(0.02);
    float fresnel = F0.x + (1.0 - F0.x) * pow(1.0 - NdotV, 5.0);

    // ============================================================
    // Refraction (screen-space from scene history)
    // ============================================================
    // The camera's own planes, handed in rather than written out again here.
    //
    // This said 0.05 while the camera's near plane is 0.5, so every depth read
    // out of the buffer linearised to about a tenth of its real distance. The
    // shoreline masks are thresholds in yards - foam out to 1.8, the wet band
    // to 0.7 - and against a depth ten times too shallow they matched water far
    // out into the lake instead of a strip along its edge. What was left of the
    // boundary followed whatever the depth texture did at the lake bed's own
    // triangle edges, which is where the hard lines came from.
    float near = push.depthRange.x;
    float far = push.depthRange.y;
    float waterLinDepth = linearizeDepth(gl_FragCoord.z, near, far);

    // How far the bed seen through this pixel moves, in yards on the water:
    // the depth of water the light comes up through, times how far the
    // surface's tilt bends it (1 - 1/1.33 of the slope, for water's index),
    // then put on screen through the camera. It was a fixed share of the
    // screen whatever the depth, so over a hand's depth of water the sand
    // under it slid yards with every ripple and its light streaks danced
    // across the shallows; under deep water the same share was too little.
    // The depth is the one straight down this pixel, before any offset.
    const float kBend = 1.0 - 1.0 / 1.33;
    const float kMaxBend = 1.5;  // yards: further than this the sample is no longer the bed below
    float straightLinDepth = linearizeDepth(texture(SceneDepth, screenUV).r, near, far);
    float straightDepth = max(straightLinDepth - waterLinDepth, 0.0) * max(abs(viewDir.z), 0.05);
    vec2 bend = norm.xy / max(norm.z, 0.2) * straightDepth * kBend;
    float bendLength = length(bend);
    if (bendLength > kMaxBend) bend *= kMaxBend / bendLength;
    vec4 hereClip = projection * view * vec4(FragPos, 1.0);
    vec4 bentClip = projection * view * vec4(FragPos + vec3(bend, 0.0), 1.0);
    vec2 bentOnScreen = (bentClip.xy / max(bentClip.w, 1e-4) - hereClip.xy / max(hereClip.w, 1e-4)) * 0.5;
    vec2 refractUV = clamp(screenUV + bentOnScreen, vec2(0.001), vec2(0.999));
    float sceneDepth = texture(SceneDepth, refractUV).r;
    float sceneLinDepth = linearizeDepth(sceneDepth, near, far);
    // Only what is behind the surface can be seen through it. Next to anything
    // standing in front of the water the offset lands on that thing - a
    // character on the shore - and its colour was copied into the water around
    // its silhouette, moving with the waves. Such a sample is thrown away for
    // the one straight behind this pixel, which the depth test says is behind
    // the water.
    if (sceneLinDepth < waterLinDepth) {
        refractUV = screenUV;
        sceneDepth = texture(SceneDepth, refractUV).r;
        sceneLinDepth = linearizeDepth(sceneDepth, near, far);
    }
    // The capture is taken after the scene is drawn and before the water goes
    // over it, so it holds no water and has had no display post-processing
    // applied yet. That means a single sharp tap is correct: there is no
    // feedback loop to smear out, and no baked-in brightness to divide back off.
    vec3 sceneRefract = texture(SceneColor, refractUV).rgb;
    float depthDiff = max(sceneLinDepth - waterLinDepth, 0.0);

    // Convert screen-space depth difference to approximate vertical water depth.
    // depthDiff is along the view ray; multiply by the vertical component of
    // the view direction so grazing angles don't falsely trigger shoreline foam
    // on occluding geometry (bridges, posts) that isn't at the waterline.
    float verticalFactor = abs(viewDir.z);  // 1.0 looking straight down, ~0 at grazing
    float verticalDepth = depthDiff * max(verticalFactor, 0.05);

    // Shoreline masks use depth sampled straight down the pixel rather than
    // through the refraction offset. The refracted sample wanders with the wave
    // normal, and near the beach it lands on dry ground, so the depth it reports
    // collapses to nothing along wandering lines - which is where the foam and
    // wet sand were picking up hard tile-like edges. The refraction colour still
    // uses the offset sample; only the masks need a depth that stays put.
    float shoreLinDepth = linearizeDepth(texture(SceneDepth, screenUV).r, near, far);
    float shoreDepth = max(shoreLinDepth - waterLinDepth, 0.0) * max(verticalFactor, 0.05);

    // ============================================================
    // Beer-Lambert absorption
    // ============================================================
    vec3 absorptionCoeff = vec3(0.46, 0.09, 0.06);
    if (basicType > 0.5 && basicType < 1.5) {
        absorptionCoeff = vec3(0.35, 0.06, 0.04);
    }
    vec3 absorbed = exp(-absorptionCoeff * verticalDepth);

    // Underwater blue fog - geometry below the waterline fades to a blue haze
    // with depth, masking occlusion edge artifacts and giving a natural look.
    vec3 underwaterFogColor = waterColor.rgb * 0.5 + vec3(0.04, 0.10, 0.20);
    float underwaterFogFade = 1.0 - exp(-verticalDepth * 0.35);
    vec3 foggedScene = mix(sceneRefract, underwaterFogColor, underwaterFogFade);

    vec3 shallowColor = waterColor.rgb * 1.2;
    vec3 deepColor = waterColor.rgb * vec3(0.3, 0.5, 0.7);
    float depthFade = max(1.0 - exp(-verticalDepth * 0.15), bodyFloor);
    vec3 waterBody = mix(shallowColor, deepColor, depthFade);

    // Whether the refraction capture holds a frame is a fact the renderer
    // knows, handed in as one. It used to be inferred per pixel from the
    // captured colour being brighter than 0.003, so every dark pixel in the
    // scene - a cave mouth, the night - flipped that pixel of water onto the
    // no-refraction path, and the two paths speckled against each other.
    bool hasSceneData = push.sceneValid > 0.5;

    // Animated caustic shimmer - only without refraction (refraction already provides movement)
    if (!hasSceneData) {
        float caustic1 = noiseValue(FragPos.xy * 1.8 + time * vec2(0.3, 0.15));
        float caustic2 = noiseValue(FragPos.xy * 3.2 - time * vec2(0.2, 0.35));
        float causticPattern = caustic1 * 0.6 + caustic2 * 0.4;
        vec3 causticTint = vec3(0.08, 0.18, 0.28) * smoothstep(0.35, 0.75, causticPattern);
        waterBody += causticTint;
    }

    vec3 refractedColor;
    if (hasSceneData) {
        refractedColor = mix(foggedScene * absorbed, waterBody, depthFade * 0.7);
        if (verticalDepth < 0.01) {
            float opticalDepth = 1.0 - exp(-dist * 0.004);
            refractedColor = mix(foggedScene, waterBody, opticalDepth * 0.6);
        }
    } else {
        // No refraction data - use lit water body with animated variation
        vec3 litWater = waterBody * (ambientColor.rgb * 0.8 + NdotL * lightColor.rgb * 0.6)
                      * shadowLightFactor(sunReach);
        float normalShift = dot(norm.xy, vec2(0.5, 0.5));
        litWater += vec3(0.02, 0.06, 0.10) * normalShift;
        refractedColor = litWater;
    }

    // In shade the water's own colour darkens as the ground beside it does,
    // by the client's 0.7 + 0.3 of the shadow; what is seen through it was
    // drawn already shadowed.
    vec3 litBase = waterBody * (ambientColor.rgb * 0.7 + NdotL * lightColor.rgb * 0.5)
                 * shadowLightFactor(sunReach);
    refractedColor = mix(refractedColor, litBase, clamp(depthFade * 0.3, 0.0, 0.5));

    // ============================================================
    // Shoreline - wet sand and moving sediment
    // ============================================================
    // The last half metre of depth is where a beach reads as wet rather than
    // submerged: sand darkens and loses a little saturation, and light moving
    // on the surface throws slow banding across it. Both are keyed on depth, so
    // they exist only at the edge and leave open water untouched.
    // The surf front, as a depth contour that runs up the beach and back. Both
    // the wet sand and the foam key off this so they move together; the phase is
    // offset by low-frequency noise along the shore so one stretch breaks before
    // another rather than the whole shoreline pulsing as a ring.
    float swashPhase = sin(time * 0.55 + noiseValue(FragPos.xy * 0.12) * 6.28);
    float swashDepth = 0.30 + 0.18 * swashPhase;

    float wetBand = 1.0 - smoothstep(0.0, 0.70, shoreDepth);
    float shallowBand = 1.0 - smoothstep(0.0, 2.20, shoreDepth);
    // How thoroughly the surf currently covers this point. Sand deeper than the
    // front is under the run-up and soaked; sand shallower has just been
    // uncovered and is draining, so it lightens as the water pulls back. Keying
    // the darkening on depth alone left the wet zone fixed while the water
    // visibly moved over it.
    float coveredness = smoothstep(swashDepth - 0.12, swashDepth + 0.08, shoreDepth);
    if (basicType < 1.5 && shallowBand > 0.001) {
        float sediment = noiseValue(FragPos.xy * 1.6 + time * vec2(0.10, 0.06)) * 0.6
                       + noiseValue(FragPos.xy * 3.4 - time * vec2(0.07, 0.13)) * 0.4;
        float banding = smoothstep(0.42, 0.78, sediment);
        refractedColor *= 1.0 + banding * 0.18 * shallowBand;
        float soak = wetBand * (0.42 + 0.58 * coveredness);
        refractedColor = mix(refractedColor,
                             refractedColor * vec3(0.62, 0.60, 0.56),
                             soak * 0.80);
    }

    // ============================================================
    // Reflection
    // ============================================================
    // What the surface reflects: the scene above it where the planar
    // reflection holds it, and the sky everywhere else. The sky is what makes
    // a sea look like one - at dusk the water is the colour of the sky, and
    // the waves show as its lighter and darker faces. It was left out, and the
    // planar image halved and kept to a third of the Fresnel weight, so the
    // water reflected a tenth of what it does and read as a blue sheet.
    float reflValidity = 0.0;
    vec3 planarReflect = vec3(0.0);

    vec4 reflClip = reflViewProj * vec4(FragPos, 1.0);
    if (reflClip.w > 0.1) {
        // Match the reflection pass's positive-height Vulkan viewport;
        // reflViewProj already includes the projection's Y inversion.
        vec2 reflUV = reflClip.xy / reflClip.w * 0.5 + 0.5;
        reflUV += norm.xy * 0.015;

        // Wide fade so there's no visible boundary - fully gone well inside the edge
        float edgeFade = smoothstep(0.0, 0.15, reflUV.x) * smoothstep(1.0, 0.85, reflUV.x)
                       * smoothstep(0.0, 0.15, reflUV.y) * smoothstep(1.0, 0.85, reflUV.y);

        reflUV = clamp(reflUV, vec2(0.002), vec2(0.998));
        vec3 texReflect = texture(ReflectionColor, reflUV).rgb;

        float reflBrightness = dot(texReflect, vec3(0.299, 0.587, 0.114));
        reflValidity = smoothstep(0.002, 0.05, reflBrightness) * edgeFade;
        planarReflect = texReflect;
    }
    // The planar image holds only what stands above the water, on black
    // (renderReflectionPass); where it holds nothing, the sky.
    vec3 reflected = mix(skyAlong(reflect(-viewDir, norm)), planarReflect, reflValidity);

    // ============================================================
    // GGX Specular
    // ============================================================
    // A pixel covers a patch of surface, not a point, and across the patch
    // the normal turns. At 0.18 the sun's highlight is far smaller than the
    // turn a ripple makes within one pixel a few yards off, so whichever
    // pixel's centre happened to face the sun lit up alone, and the next
    // frame another one did: the water glittered in single pixels that moved
    // with every frame of the ripple. The spread of the normal over the pixel
    // goes into the roughness instead (Kaplanyan and Hoffman 2016, as
    // Tokuyoshi and Kaplanyan 2019 filter it), so the highlight takes in what
    // the pixel covers and a highlight smaller than a pixel spreads to one.
    const float kSurfaceRoughness = 0.18;
    const float kVariance = 0.25;       // the screen-space filter kernel's variance, in pixels
    const float kMaxSpread = 0.18;      // most it may add to alpha squared
    vec3 normalDx = dFdx(norm);
    vec3 normalDy = dFdy(norm);
    float spread = min(2.0 * kVariance * (dot(normalDx, normalDx) + dot(normalDy, normalDy)), kMaxSpread);
    // The sun's path. Only the waves big enough for this pixel tilt its
    // normal; the rest is chop too small to draw, and it is still there - it
    // scatters the sun the way it scattered it in Cox and Munk's photographs.
    // So its slope goes into the highlight's width: a Beckmann surface whose
    // mean square slope, both ways together, is alpha squared, which GGX
    // follows near its peak. Close up the sun glints off the drawn crests;
    // further off fewer are drawn, and the highlight widens into a glitter
    // path running to the horizon; in still air there is none and the sun is
    // a sharp disc.
    float ggxAlpha = kSurfaceRoughness * kSurfaceRoughness;  // DistributionGGX squares roughness
    float alphaSq = clamp(ggxAlpha * ggxAlpha + spread + sea.unseenSlopeSq, 0.0, 1.0);
    float roughness = sqrt(sqrt(alphaSq));
    vec3 halfDir = normalize(ldir + viewDir);
    float D = DistributionGGX(norm, halfDir, roughness);
    float G = GeometrySmith(NdotV, NdotL, roughness);
    vec3 F = fresnelSchlickRoughness(max(dot(halfDir, viewDir), 0.0), F0, roughness);
    // In the units every surface here is lit in. Their diffuse is albedo x
    // light x N.L, with no 1/pi - the renderer's light is what a white face
    // turned to the sun shows - while GGX's D carries the 1/pi of a true
    // BRDF. Taken as it was, the sun's glint was a third as bright as the
    // ground beside it says the sun is, and once the wind spread it into a
    // path it sank into the water's own colour.
    const float kPi = 3.14159265;
    vec3 specular = kPi * (D * G * F) / (4.0 * NdotV * NdotL + 0.001) * reflectionSunColor.rgb * reflectionSun.w * NdotL;

    // Noise-based sparkle. Its features are far smaller than the wave detail:
    // its coarsest octave is twelve to the yard, so it is there only while a
    // pixel can show that. Faded by distance it ran out to 700 yards, a
    // flicker of single pixels over every beach in view.
    float sparkleShown = cellsShown(12.0, pixelsPerYard);
    if (sparkleShown > 0.001) {
        float sparkleNoise = fbmNoise(FragPos.xy * 4.0 + time * 0.5, time * 1.5);
        float sparkle = pow(max(sparkleNoise - 0.55, 0.0) / 0.45, 3.0) * shimmerStrength * 0.10;
        specular += sparkle * reflectionSunColor.rgb * reflectionSun.w * sparkleShown;
    }

    // ============================================================
    // Subsurface scattering
    // ============================================================
    float sssBase = pow(max(dot(viewDir, -ldir), 0.0), 4.0);
    float sss = sssBase * max(0.0, waveHeight * 3.0) * 0.25;
    vec3 sssColor = vec3(0.05, 0.55, 0.35) * sss * reflectionSunColor.rgb * reflectionSun.w;

    // ============================================================
    // Combine: what comes up through the water, and what the surface
    // reflects, by water's Fresnel - a fiftieth looking straight down, all of
    // it at the horizon.
    // ============================================================
    vec3 color = mix(refractedColor, reflected, fresnel);
    // The sun's glint and the light it puts through the crests need the sun:
    // in shadow there is nothing for the surface to reflect.
    color += (specular + sssColor) * sunReach;

    float crest = smoothstep(0.5, 1.0, waveHeight) * 0.04;
    color += vec3(crest);

    // Carried out of the shoreline block so the alpha below can make the foam
    // opaque. Tinting alone left it invisible: at the water's edge alpha sits
    // near its floor, so foam was being blended over the sand at 15%.
    float shorelineFoam = 0.0;

    // ============================================================
    // Shoreline foam - scattered particles, not smooth bands
    // Only on terrain water (waveAmp > 0); WMO water (canals, indoor)
    // has waveAmp == 0 and should not show shoreline interaction.
    // ============================================================
    // The mask below is zero past 0.9 of depth, so nothing here draws beyond
    // it; without that bound the block ran on every water pixel with a bed
    // under it, which is the whole lake, and multiplied its five cellular
    // fields by nothing.
    if (basicType < 1.5 && shoreDepth > 0.001 && shoreDepth < 0.9 && push.waveAmp > 0.0) {
        // How far out from the waterline foam reaches, in yards of depth.
        // Halved from 1.8 once the depth being measured was the real one:
        // against a depth ten times too shallow this had been tuned by eye
        // to something that looked right, and with the scale corrected the
        // same number drew a band twice the width it should be.
        float foamDepthMask = 1.0 - smoothstep(0.0, 0.9, shoreDepth);

        // Foam rides on the water rather than sitting in world space. The surf
        // carries it up the beach and drags it back, so the whole pattern is
        // advected by the swash: it drifts shoreward as the front runs up and
        // reverses as it withdraws. Without this the particles held still while
        // the water visibly moved through them.
        vec2 drift = vec2(noiseValue(FragPos.xy * 0.09) - 0.5,
                          noiseValue(FragPos.xy * 0.09 + vec2(19.0)) - 0.5);
        drift = normalize(drift + vec2(0.001));
        vec2 swashAdvect = drift * swashPhase * 1.35;

        // Two warp octaves at different scales, because a single one leaves the
        // lattice legible underneath it.
        vec2 warpOffset = vec2(
            noiseValue(FragPos.xy * 2.5 + time * 0.08) - 0.5,
            noiseValue(FragPos.xy * 2.5 + vec2(37.0) + time * 0.06) - 0.5
        ) * 1.6;
        warpOffset += vec2(
            noiseValue(FragPos.xy * 6.1 - time * 0.11) - 0.5,
            noiseValue(FragPos.xy * 6.1 + vec2(11.0) + time * 0.09) - 0.5
        ) * 0.55;
        vec2 foamUV = FragPos.xy + warpOffset + swashAdvect;

        // Worley cells thresholded near their centres put a dot in every cell,
        // so the cell lattice itself becomes the pattern - which is the grid.
        // Rotate each octave and use scales that are not simple multiples, so no
        // two lattices line up, and vary the threshold with noise so only some
        // cells produce a speck.
        const mat2 rot1 = mat2( 0.87, -0.50,  0.50,  0.87);   // ~30 degrees
        const mat2 rot2 = mat2( 0.26, -0.97,  0.97,  0.26);   // ~75 degrees
        float thresholdJitter = noiseValue(FragPos.xy * 5.0 + time * 0.2);

        float foam1 = foamDots(rot1 * foamUV * 13.0 + time * vec2(0.15, 0.08),
                               0.075 + 0.075 * thresholdJitter,
                               13.0, pixelsPerYard) * 0.45;
        float foam2 = foamDots(rot2 * foamUV * 27.7 + time * vec2(-0.12, 0.22),
                               0.045 + 0.05 * (1.0 - thresholdJitter),
                               27.7, pixelsPerYard) * 0.30;
        float foam3 = foamDots(foamUV * 51.3 + time * vec2(0.25, -0.1), 0.05,
                               51.3, pixelsPerYard) * 0.18;

        // Noise breakup for clumping
        float noiseMask = noiseValue(FragPos.xy * 3.0 + time * 0.15);
        float foam = (foam1 + foam2 + foam3) * foamDepthMask * smoothstep(0.3, 0.6, noiseMask);

        // The surf line itself, on the contour computed above with the wet sand,
        // so the foam sits exactly where the water currently reaches.
        float swashBand = 1.0 - smoothstep(0.0, 0.085, abs(shoreDepth - swashDepth));
        float swashTexture = 0.55 + 0.45 * cellDistance(rot1 * foamUV * 8.6 + time * vec2(0.05, 0.12),
                                                        cellsShown(8.6, pixelsPerYard));
        foam += swashBand * swashTexture * 0.55 * foamDepthMask;

        // Spray thrown off the front as it runs up: much finer than the foam,
        // sparse, and only while the surf is advancing. It is what sells the
        // edge as breaking water rather than a wet line.
        float sprayMask = 1.0 - smoothstep(0.0, 0.11, abs(shoreDepth - swashDepth + 0.04));
        float spray = foamDots(rot2 * (foamUV + swashAdvect * 0.5) * 96.0 + time * vec2(0.6, -0.45),
                               0.035, 96.0, pixelsPerYard)
                    * sprayMask * max(swashPhase, 0.0) * 0.7;
        foam += spray * foamDepthMask;

        // Only enough of a ramp to keep the water mesh's own boundary from
        // showing as a hard line. The old 0.1 cut removed the foam exactly where
        // the water meets the sand, which is where it belongs.
        foam *= smoothstep(0.0, 0.025, shoreDepth);
        foam = clamp(foam, 0.0, 0.85);
        // Bluer foam tint instead of near-white
        color = mix(color, vec3(0.78, 0.85, 0.92), foam * 0.48);
        shorelineFoam = foam;
    }

    // ============================================================
    // Wake froth - water churned up by something moving through it
    // Not gated on shoreDepth: wading happens wherever the water is shallow
    // enough to stand in, which is not only at the beach.
    // ============================================================
    float wakeFoam = 0.0;
    if (wakeBounds.w > 0.5) {
        // One cheap reject for the whole trail keeps this off every other water
        // pixel on screen - the trail covers a few yards, the sheet covers miles.
        vec2 toWake = FragPos.xy - wakeBounds.xy;
        if (dot(toWake, toWake) < wakeBounds.z * wakeBounds.z) {
            // Clamped, not trusted: an unmapped UBO would leave a garbage count
            // here and run this loop off the end of the array.
            int wakeCount = clamp(int(wakeBounds.w), 0, MAX_WAKE_POINTS);
            for (int i = 0; i < wakeCount; ++i) {
                vec4 wp = wakePoints[i];
                // Churn spreads as it dies, the way a disturbed patch does.
                float radius = mix(0.40, 1.45, wp.z);
                float d = length(FragPos.xy - wp.xy);
                // Falls off from the centre out, with no plateau. A flat middle
                // is what made these read as painted discs rather than froth.
                float disc = 1.0 - smoothstep(0.0, radius, d);
                wakeFoam += disc * disc * wp.w * (1.0 - wp.z);
            }
            wakeFoam = clamp(wakeFoam, 0.0, 1.0);
        }
    }
    // And the white water the wake's breaking leaves: at a body's bow, and
    // trailing back along the arms of its wedge.
    wakeFoam = max(wakeFoam, 0.85 * wake.w);
    {
        {
            if (wakeFoam > 0.002) {
                vec2 churnWarp = vec2(
                    noiseValue(FragPos.xy * 3.4 + time * 0.5) - 0.5,
                    noiseValue(FragPos.xy * 3.4 + vec2(23.0) - time * 0.44) - 0.5
                ) * 0.9;
                vec2 churnUV = FragPos.xy + churnWarp;

                // cellularFoam returns distance to the nearest cell point, so a
                // low threshold marks only the few pixels sitting on a point -
                // specks, which left the patch between them to be filled solid.
                // Thresholding across the middle of the range inverts that: most
                // of the patch is foam and the gaps between cells are the
                // texture, which is what aerated water actually looks like.
                // Each of the two, where its cells are too small to show, is
                // what it covers on average (17% and 6% of a cell).
                float coarseShown = cellsShown(12.0, pixelsPerYard);
                float cover = 0.175;
                if (coarseShown > 0.001) {
                    float cells = cellularFoam(churnUV * 12.0 + time * vec2(0.25, -0.18));
                    cover = mix(cover, 1.0 - smoothstep(0.08, 0.38, cells), coarseShown);
                }
                float fineShown = cellsShown(29.0, pixelsPerYard);
                float fineCover = 0.062;
                if (fineShown > 0.001) {
                    float fine = cellularFoam(churnUV * 29.0 - time * vec2(0.14, 0.30));
                    fineCover = mix(fineCover, 1.0 - smoothstep(0.05, 0.22, fine), fineShown);
                }
                cover = max(cover * 0.85, fineCover * 0.5);

                // Clumping, so the trail thins and thickens along its length.
                cover *= 0.55 + 0.45 * noiseValue(churnUV * 2.2 + time * 0.35);

                wakeFoam *= cover;
                // Aerated water is lighter than the surface around it but it is
                // still water. Near-opaque white read as paint lying on top.
                wakeFoam = min(wakeFoam, 0.60);
                color = mix(color, vec3(0.86, 0.91, 0.95), wakeFoam);
            }
        }
    }

    // ============================================================
    // Whitecaps - particle-based
    // ============================================================
    // Only where a crest is breaking (`breaking`, from how far riding
    // toward it has bunched the surface): a wind of a few yards a second
    // breaks none, a gale many. The mask is zero elsewhere, and the cellular
    // field it would multiply is the expensive part.
    if (basicType < 1.5 && breaking > 0.001) {
        float crestMask = breaking;
        vec2 crestWarp = vec2(
            noiseValue(FragPos.xy * 1.8 + time * 0.1) - 0.5,
            noiseValue(FragPos.xy * 1.8 + vec2(53.0) + time * 0.07) - 0.5
        ) * 2.0;
        float crestFoam = foamDots((FragPos.xy + crestWarp) * 6.0 + time * vec2(0.12, 0.08), 0.18,
                                   6.0, pixelsPerYard) * crestMask;
        float crestNoise = noiseValue(FragPos.xy * 3.0 - time * 0.3);
        crestFoam *= smoothstep(0.3, 0.6, crestNoise);
        color = mix(color, vec3(0.68, 0.78, 0.88), crestFoam * 0.30);
    }

    // ============================================================
    // Alpha and fog
    // ============================================================
    float baseAlpha = mix(waterAlpha, min(1.0, waterAlpha * 1.5), depthFade);
    float alpha = mix(baseAlpha, min(1.0, baseAlpha * 1.3), fresnel) * alphaScale;
    alpha = clamp(alpha, 0.15, 0.92);
    // What the surface reflects is not seen through: by the horizon the
    // water is all reflection, and alpha below one let the bed under it back
    // through the sky's colour.
    alpha = mix(alpha, 1.0, fresnel);
    // Wet sand is a band on the beach, not a film of water, so it needs enough
    // presence to darken what is under it; foam has to be close to opaque or it
    // does not read at all.
    alpha = max(alpha, wetBand * 0.42 * smoothstep(0.0, 0.05, shoreDepth));
    // Foam opacity, kept in step with the colour mix above. Both come down
    // together - a fifth, then a fifth again - because lowering one alone
    // leaves the foam as opaque as it was and merely paler.
    alpha = max(alpha, shorelineFoam * 0.58);
    alpha = max(alpha, wakeFoam * 0.55);
    // Dissolve the sheet before the water geometry runs out, so the ocean fades
    // into the horizon haze instead of ending on a hard line. This has to come
    // after the clamp - clamping afterwards restored the 0.15 floor and put the
    // edge straight back.
    float waterRange = max(fogSkyBand1.w, 1.0);
    float horizontalDist = length(FragPos.xy - viewPos.xy);
    alpha *= 1.0 - smoothstep(waterRange * 0.25, waterRange, horizontalDist);
    // And into the beach: the sheet thins out over its last fifth of a yard of
    // depth, so there is no line where it meets the sand. Only a few
    // hundredths of a yard did this before, and with the 0.15 floor above the
    // water ended on its own edge, which every rise and fall of the surface
    // then moved across the sand. Not where there is no capture of what is
    // under the water to measure the depth against.
    if (hasSceneData) {
        alpha *= smoothstep(0.0, 0.2, shoreDepth);
    }

    color += lightningDiffuse(FragPos, norm, vec3(0.22, 0.28, 0.34));
    color = applyFog(color, FragPos, dist);

    outColor = vec4(color, alpha);
}
