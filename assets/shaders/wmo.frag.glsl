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
    mat4 contactSpaceMatrix;
    vec4 contactParams;
};

#include "rt_lighting.glsli"
#include "contact_shadows.glsli"
#include "sky_atmosphere.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(set = 1, binding = 1) uniform WMOMaterial {
    int hasTexture;
    int alphaTest;
    // The client's light the batch is drawn with (rendering/wmo_lighting.hpp):
    // one of the LIGHT_ values below.
    int light;
    // A transition batch's second light, drawn under `light` where the vertex
    // colour's alpha falls to 0; -1 for every other batch.
    int transitionLight;
    float specularIntensity;
    int enableNormalMap;
    int enablePOM;
    float pomScale;
    int pomMaxSamples;
    float heightMapVariance;
    float normalMapStrength;
    int isLava;
    float wmoAmbientR;
    float wmoAmbientG;
    float wmoAmbientB;
    int emissive;
    int hasVertexColors;
    int receivesExteriorShadows;
    // F_SIDN: the colour the material glows in, black without the flag.
    float sidnR;
    float sidnG;
    float sidnB;
};

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;
layout(set = 0, binding = 5) uniform sampler2D uSceneSky;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec4 VertColor;
layout(location = 4) in vec3 Tangent;
layout(location = 5) in vec3 Bitangent;
layout(location = 6) flat in uvec4 vLocalLights;

layout(location = 0) out vec4 outColor;

// WoW.exe's lighting modes, as WmoLight numbers them.
const int LIGHT_VERTEX_COLOUR = 0;   // the vertex colours are the light
const int LIGHT_SUN = 1;             // the zone's ambient and sun
const int LIGHT_WINDOW = 2;          // the window light of an F_WINDOW material
const int LIGHT_BUILDING_AMBIENT = 3; // the building's MOHD ambient, no sun

// One texel of the shadow map, handed in by the renderer. The map is 512,
// 1024, 2048 or 4096 a side by the quality setting; this used to be a
// constant for 4096, so at 512 the filter taps all landed inside one texel
// and the bias shrank eightfold. The fallback covers a per-frame block that
// never filled the slot in, such as the character preview's.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

#include "local_lights.glsli"
#include "shadow_receiver.glsli"

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    // Low density = close/head-on = full detail (0)
    // High density = far/steep = vertex normals only (1)
    return smoothstep(0.0001, 0.005, texelDensity);
}

// Parallax Occlusion Mapping with angle-adaptive sampling
vec2 parallaxOcclusionMap(vec2 uv, vec3 viewDirTS, float lodFactor) {
    float VdotN = abs(viewDirTS.z);  // 1=head-on, 0=grazing

    // Fade out POM at grazing angles to avoid distortion
    if (VdotN < 0.15) return uv;

    float angleFactor = clamp(VdotN, 0.15, 1.0);
    int maxS = pomMaxSamples;
    int minS = max(maxS / 4, 4);
    int numSamples = int(mix(float(minS), float(maxS), angleFactor));
    numSamples = int(mix(float(minS), float(numSamples), 1.0 - lodFactor));

    float layerDepth = 1.0 / float(numSamples);
    float currentLayerDepth = 0.0;

    // Direction to shift UV per layer - clamp denominator to prevent explosion at grazing angles
    vec2 P = viewDirTS.xy / max(VdotN, 0.15) * pomScale;
    // Hard-clamp total UV offset to prevent texture swimming
    float maxOffset = pomScale * 3.0;
    P = clamp(P, vec2(-maxOffset), vec2(maxOffset));
    vec2 deltaUV = P / float(numSamples);

    // The mip level is chosen once, from the undisplaced UV, and used for
    // every sample in the march. Inside the loop the UV differs from one
    // pixel to the next by how many steps each has taken, so the implicit
    // derivatives were noise and the level chosen from them was too - which
    // showed as sparkle on relief at distance and cost a gradient fetch
    // per sample on top.
    float lod = textureQueryLod(uNormalHeightMap, uv).x;
    vec2 currentUV = uv;
    float currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;

    // Ray march through layers
    for (int i = 0; i < 64; i++) {
        if (i >= numSamples || currentLayerDepth >= currentDepthMapValue) break;
        currentUV -= deltaUV;
        currentDepthMapValue = 1.0 - textureLod(uNormalHeightMap, currentUV, lod).a;
        currentLayerDepth += layerDepth;
    }

    // Interpolate between last two layers for smooth result
    vec2 prevUV = currentUV + deltaUV;
    float afterDepth = currentDepthMapValue - currentLayerDepth;
    float beforeDepth = (1.0 - textureLod(uNormalHeightMap, prevUV, lod).a) - currentLayerDepth + layerDepth;
    float weight = afterDepth / (afterDepth - beforeDepth + 0.0001);
    vec2 result = mix(currentUV, prevUV, weight);

    // Fade toward original UV at grazing angles for smooth transition
    float fadeFactor = smoothstep(0.15, 0.35, VdotN);
    return mix(uv, result, fadeFactor);
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
    float fogFactor = distanceFogVisibility(dist, fogParams.x, fogParams.y);
    vec3 haze = fogColor.rgb;
    vec4 air = vec4(0.0, 0.0, 0.0, 1.0);
    if (volumetricParams.x > 0.5) air = fogVolumeAt(worldPos);
    color = color * air.a + air.rgb;
    haze = haze * air.a + air.rgb;
    vec3 ray = (worldPos - viewPos.xyz) / max(dist, 0.001);
    vec3 sky;
    if (fogSkyMiddle.w > 0.5) {
        vec4 clip = projection * view * vec4(worldPos, 1.0);
        vec2 uv = clip.xy / max(clip.w, 0.001) * 0.5 + 0.5;
        // This is already the real sky behind all the air. Do not fog it twice.
        sky = textureLod(uSceneSky, uv, 0.0).rgb;
    } else {
        sky = atmosphereSkyColor(ray, normalize(-lightDir.xyz), fogSkyTop.rgb,
                                 fogSkyMiddle.rgb, fogSkyBand1.rgb, fogSkyBand2.rgb);
        sky = sky * air.a + air.rgb;
    }
    if (fogFactor < 1.0 && fogSkyTop.w > 0.0)
        haze = distanceFogTint(haze, sky, dist, fogParams.x, fogParams.y, fogSkyTop.w);
    color = mix(haze, color, fogFactor);
    // Geometry fades into the actual background before the common hard limit.
    // This also hides the edge of the detailed terrain without bright silhouettes.
    if (fogSkyMiddle.w > 0.5) {
        float visible = worldGeometryVisibility(length(worldPos.xy - viewPos.xy), fogSkyBand1.w);
        color = mix(sky, color, visible);
    }
    return color;
}

// The surface lit by one of the client's lights, before the room's own lamps
// and the fog.
vec3 shadeBy(int mode, vec3 tex, vec3 norm, RtLight rt, float shadow) {
    // F_SIDN: after dark the material's glow colour adds to the light before
    // the texture multiplies it, as WoW.exe's c29 emissive does, and the sum
    // stops at twice the texture (wmo_night_glow.hpp). Every light but the
    // vertex colours alone is drawn with the shader that adds it.
    vec3 glow = mode == LIGHT_VERTEX_COLOUR ? vec3(0.0) : vec3(sidnR, sidnG, sidnB) * lightDir.w;
    const bool glows = max(glow.r, max(glow.g, glow.b)) > 0.0;

    if (mode == LIGHT_VERTEX_COLOUR || mode == LIGHT_BUILDING_AMBIENT) {
        // WotLK MapObj shaders modulate the texture by 2 * vertex color.
        // MOHD is the building's base ambient; MOCV adds authored warm/cool
        // pools to it. Using max() flattened those colors into the ambient,
        // notably erasing Dalaran's blue portal and warm torch lighting.
        vec3 wmoAmbient = vec3(wmoAmbientR, wmoAmbientG, wmoAmbientB);
        vec3 baked = hasVertexColors != 0
            ? max(wmoAmbient + 2.0 * VertColor.rgb, vec3(0.15))
            : vec3(1.0);  // Neutral modulation when no baked colors exist.
        if (glows) baked = min(baked + glow, vec3(2.0));
        // The baked light is multiplied by the shadow, as the client does it.
        return tex * baked * shadowLightFactor(shadow);
    }

    vec3 ambient = ambientColor.rgb;
    vec3 sun = lightColor.rgb;
    if (mode == LIGHT_WINDOW) {
        // The client's window light (light update 0x7ee750): the sun's colour
        // is halfway between the zone's ambient and its sun, and the ambient
        // is that again, sixteen 255ths brighter. A window is lit about as
        // well from inside the room as from the street.
        sun = 0.5 * (ambientColor.rgb + lightColor.rgb);
        ambient = min(sun + 16.0 / 255.0, vec3(1.0));
    }

    vec3 ldir = normalize(-lightDir.xyz);
    float diff = max(dot(norm, ldir), 0.0);

    vec3 viewDir = normalize(viewPos.xyz - FragPos);
    vec3 halfDir = normalize(ldir + viewDir);
    float spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;

    vec3 result = rtAmbient(rt, ambient) * tex + diff * sun * tex;

    // An exterior group's vertex colour is light baked into it - a lamp's
    // pool on a wall - and it adds to the sun, as it does in the client.
    // It was multiplied in as if it were baked shadow, floored at a
    // quarter: Stormwind has two exterior groups with vertex colours at
    // all, both near black, and they drew at a quarter of the light the
    // rest of the city had, shadows on or off - and so did 278 of the 362
    // exterior groups in the game that carry any. A group without them has
    // white filled in by the loader, which must add nothing.
    //
    // At half: a handful of Icecrown exteriors ship them pure white, and
    // added at full those would double in brightness.
    if (hasVertexColors != 0) result += tex * VertColor.rgb * 0.5;

    if (glows) result = min(result + tex * glow, tex * 2.0);

    // All of it darkened alike by a shadow; the highlight is the sun's
    // own and goes with it.
    return result * shadowLightFactor(shadow) + shadow * spec * sun;
}

void main() {
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    vec3 vertexNormal = normalize(Normal);
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    // Compute final UV (with POM if enabled)
    vec2 finalUV = TexCoord;

    // Lava/magma: scroll UVs for flowing effect
    if (isLava != 0) {
        float time = fogParams.z;
        // Scroll both axes - pools get horizontal flow, waterfalls get vertical flow
        // (UV orientation depends on mesh, so animate both)
        finalUV += vec2(time * 0.04, time * 0.06);
    }

    // Build TBN matrix
    vec3 T = normalize(Tangent);
    vec3 B = normalize(Bitangent);
    vec3 N = vertexNormal;
    mat3 TBN = mat3(T, B, N);

    if (enablePOM != 0 && heightMapVariance > 0.001 && lodFactor < 0.99) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(TexCoord, viewDirTS, lodFactor);
    }

    vec4 texColor = hasTexture != 0 ? textureGrad(uTexture, finalUV, uvDx, uvDy) : vec4(1.0);
    // A WMO material's alpha key cuts at 224/255, as the original client's
    // does, not at a half. Dalaran's lattice windows (DAL_WINDOWCUTOUT) leave
    // their holes at alpha 0.5-0.6; cut at a half, they drew as solid panes.
    if (alphaTest != 0 && texColor.a < 224.0 / 255.0) discard;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (enableNormalMap != 0 && lodFactor < 0.99 && normalMapStrength > 0.001) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal = normalize(mapNormal);
        vec3 worldNormal = normalize(TBN * mapNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        // Linear blend: strength controls how much normal map detail shows,
        // LOD fades out at distance. Both multiply for smooth falloff.
        float blend = clamp(normalMapStrength, 0.0, 1.0) * (1.0 - lodFactor);
        norm = normalize(mix(vertexNormal, worldNormal, blend));
    }

    vec3 result;

    // Interior MOCV already contains the building's baked light and shade.
    // Its roof blocks the outdoor sun; multiplying the baked term by that
    // shadow darkened the entire room a second time. A room still takes the
    // shadows of the units in it. Which batches the sun's map falls on is
    // the renderer's call (wmoReceivesExteriorShadows).
    const bool baked = light == LIGHT_VERTEX_COLOUR || light == LIGHT_BUILDING_AMBIENT;
    const bool sunReaches = receivesExteriorShadows != 0;
    float shadow = 1.0;
    if (sunReaches && shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        // Texture relief changes lighting, not the caster geometry. Offsetting
        // along its perturbed normal can push the lookup into the surface.
        vec3 shadowNormal = dot(vertexNormal, ldir) >= 0.0 ? vertexNormal : -vertexNormal;
        float normalOffset = shadowParams.w * 1.5 * (1.0 - abs(dot(shadowNormal, ldir)));
        vec3 biasedPos = FragPos + shadowNormal * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        vec2 receiverSlope = shadowReceiverSlope(proj);
        if (proj.x >= 0.0 && proj.x <= 1.0 &&
            proj.y >= 0.0 && proj.y <= 1.0 &&
            proj.z >= 0.0 && proj.z <= 1.0) {
            shadow = sampleShadowReceiverPlane(uShadowMap, proj, receiverSlope);
            shadow = mix(1.0, shadow, shadowMapEdgeFade(proj.xy * 2.0 - 1.0));
        }
    }
    RtLight rt = rtLightAt(FragPos, vertexNormal);
    shadow = rtShadow(rt, shadow);
    float unitShadow = localShadowAt(FragPos);
    if (baked) unitShadow = mix(unitShadow, 1.0, unitShadowAboveFade(FragPos));
    shadow = min(shadow, unitShadow);

    if (emissive == 1) {
        // Authored luminous glass must remain bright in direct sun and shadow.
        // A small warm bias keeps low-valued texels from reading as dark glass.
        vec3 glass = texColor.rgb * 2.0 + vec3(0.16, 0.07, 0.015);

        // Gentle guttering, weaker than the clock's open fire - these are steady
        // lamps, not flames in the wind.
        //
        // Every lamp in a building shares one batch, so a uniform phase would
        // pulse a whole street in lockstep. The phase is hashed from the lamp's
        // world position instead, quantised into cells a few units across: large
        // enough that one lamp's glass falls in a single cell, small enough that
        // neighbouring lamps land in different ones.
        vec3 cell = floor(FragPos * 0.2);
        float h = fract(sin(dot(cell, vec3(12.9898, 78.233, 37.719))) * 43758.5453);
        float phase = h * 6.2831853;
        float t = fogParams.z;
        float flicker = 0.93
                      + 0.05 * sin(t * 1.3 + phase)
                      + 0.02 * sin(t * 2.9 + phase * 1.7);
        result = glass * flicker;
    } else if (emissive == 2) {
        // Firelit from behind (Darkshire's clock face): the surface is still lit
        // by the sun so it belongs to the building by day, with a warm glow
        // seeping through it as if a fire burned in the tower.
        vec3 ldir = normalize(-lightDir.xyz);
        float diff = max(dot(norm, ldir), 0.0);
        vec3 lit = texColor.rgb * (ambientColor.rgb + lightColor.rgb * diff) * shadowLightFactor(shadow);

        // Three detuned sines: a slow breathing sway, a quicker wobble, and a
        // faint fast jitter. Their periods share no common multiple over any
        // watchable span, so the flame never visibly loops.
        float t = fogParams.z;
        float flicker = 0.84
                      + 0.10 * sin(t * 1.3)
                      + 0.05 * sin(t * 2.9 + 1.7)
                      + 0.03 * sin(t * 6.7 + 0.6);

        // Firelight only competes with daylight once the sun is down, so fade the
        // glow up as the scene darkens. A small floor keeps it faintly visible in
        // daytime shade rather than switching on at dusk.
        float daylight = clamp(dot(ambientColor.rgb + lightColor.rgb,
                                   vec3(0.299, 0.587, 0.114)), 0.0, 1.0);
        float night = mix(1.0, 0.22, daylight);

        const vec3 kFireColor = vec3(1.0, 0.58, 0.22);
        result = lit + kFireColor * (0.30 * flicker * night);

        // Glass covering the dial: a tight sun highlight plus a Fresnel sheen
        // that picks up sky colour at grazing angles, which is what sells a pane
        // in front of the face rather than paint on stone. Both are additive and
        // unaffected by the fire, since they live on the outer surface.
        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 halfDir = normalize(ldir + viewDir);
        float gloss  = pow(max(dot(norm, halfDir), 0.0), 96.0);
        float fresnel = pow(1.0 - clamp(dot(norm, viewDir), 0.0, 1.0), 4.0);
        result += lightColor.rgb * (gloss * 0.55 * shadow)
                + ambientColor.rgb * (fresnel * 0.35);
    } else if (isLava != 0) {
        // Lava is self-luminous - bright emissive, no shadows
        result = texColor.rgb * 1.5;
    } else {
        result = shadeBy(light, texColor.rgb, norm, rt, shadow);
        // A transition batch is where a room meets the outside: the client
        // draws it lit, then unlit over it, the two weighed by the vertex
        // colour's alpha - lit where it is 1, the room's own light where it
        // is 0.
        if (transitionLight >= 0)
            result = mix(shadeBy(transitionLight, texColor.rgb, norm, rt, shadow), result,
                         VertColor.a);
    }

    if (isLava == 0)
        result +=localLightContribution(FragPos, norm, texColor.rgb, vLocalLights);

    float dist = length(viewPos.xyz - FragPos);

    float alpha = texColor.a;

    result = applyFog(result, FragPos, dist);
    outColor = vec4(result, alpha);
}
