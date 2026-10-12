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
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
    vec4 volumetricParams;  // x = on, y = near, z = 1 / ln(far / near), w = slices
    mat4 rtViewProj;
    vec4 rtCameraPos;
    vec4 rtParams;
    vec4 fogSkyTop;  // rgb = zenith, w = sky tint amount (0 disables it)
    vec4 fogSkyMiddle;
    vec4 fogSkyBand1;
    vec4 fogSkyBand2;
};

#include "rt_lighting.glsli"
#include "sky_atmosphere.glsli"

layout(set = 1, binding = 0) uniform sampler2D uBaseTexture;
layout(set = 1, binding = 1) uniform sampler2D uLayer1Texture;
layout(set = 1, binding = 2) uniform sampler2D uLayer2Texture;
layout(set = 1, binding = 3) uniform sampler2D uLayer3Texture;
layout(set = 1, binding = 4) uniform sampler2D uLayer1Alpha;
layout(set = 1, binding = 5) uniform sampler2D uLayer2Alpha;
layout(set = 1, binding = 6) uniform sampler2D uLayer3Alpha;

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
};

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;
layout(set = 0, binding = 5) uniform sampler2D uSceneSky;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;

layout(location = 0) out vec4 outColor;

// One texel of the shadow map, handed in by the renderer. The map is 512,
// 1024, 2048 or 4096 a side by the quality setting; this used to be a
// constant for 4096, so at 512 the filter taps all landed inside one texel
// and the bias shrank eightfold. The fallback covers a per-frame block that
// never filled the slot in, such as the character preview's.
float shadowTexel() {
    return shadowParams.z > 0.0 ? shadowParams.z : 1.0 / 4096.0;
}

float sampleShadowPCF(sampler2DShadow smap, vec3 coords) {
    float shadow = 0.0;
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += texture(smap, vec3(coords.xy + vec2(x, y) * shadowTexel(), coords.z));
        }
    }
    return shadow / 9.0;
}

vec3 localLightContribution(vec3 pos, vec3 normal, vec3 albedo) {
    vec3 sum = vec3(0.0);
    for (int i = 0; i < min(localLightMeta.x, 64); ++i) {
        vec3 toLight = localLightPosRadius[i].xyz - pos;
        float radius = localLightPosRadius[i].w;
        // Rejected on the squared distance, before the square root and the
        // divide: most of the sixty-four are out of range of any one pixel,
        // and this is what each of them costs.
        float distSq = dot(toLight, toLight);
        if (radius <= 0.0 || distSq >= radius * radius) continue;
        float dist = sqrt(distSq);
        float attenuation = 1.0 - dist / radius;
        attenuation *= attenuation;
        float wrappedDiffuse = 0.22 + 0.78 * max(dot(normal, toLight / max(dist, 0.001)), 0.0);
        sum += albedo * localLightColorIntensity[i].rgb *
               (localLightColorIntensity[i].w * attenuation * wrappedDiffuse);
    }
    return sum;
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
void main() {
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

    float fragDist = length(viewPos.xyz - FragPos);
    // Maps uploaded by the mesh generator contain absolute coverage. Big
    // alpha must not be blended as successive opacities: that exposes the
    // base layer and changes the blend when adjacent chunks reorder textures.
    vec3 weights = vec3(0.0);
    if (hasLayer1 != 0) weights.x = texture(uLayer1Alpha, LayerUV).r;
    if (hasLayer2 != 0) weights.y = texture(uLayer2Alpha, LayerUV).r;
    if (hasLayer3 != 0) weights.z = texture(uLayer3Alpha, LayerUV).r;
    float total = weights.x + weights.y + weights.z;
    weights /= max(total, 1.0);
    vec4 finalColor = texture(uBaseTexture, TexCoord) * max(1.0 - total, 0.0);
    // These branches depend only on the chunk's uniform layer count, so UV
    // derivatives remain valid for mip selection across alpha-map boundaries.
    if (hasLayer1 != 0) finalColor += texture(uLayer1Texture, TexCoord) * weights.x;
    if (hasLayer2 != 0) finalColor += texture(uLayer2Texture, TexCoord) * weights.y;
    if (hasLayer3 != 0) finalColor += texture(uLayer3Texture, TexCoord) * weights.z;

    vec3 vertexNormal = normalize(Normal);
    vec3 norm = vertexNormal;

    // Derivative-based normal mapping: perturb vertex normal using texture detail.
    // Fade out with distance and near chunk edges (dFdx/dFdy are invalid across
    // chunk draw-call boundaries, producing visible seams if not faded).
    float bumpFade = 1.0 - smoothstep(50.0, 125.0, fragDist);
    float edgeDist = min(min(LayerUV.x, 1.0 - LayerUV.x), min(LayerUV.y, 1.0 - LayerUV.y));
    bumpFade *= smoothstep(0.0, 0.06, edgeDist);
    if (bumpFade > 0.001) {
        float lum = dot(finalColor.rgb, vec3(0.299, 0.587, 0.114));
        float dLdx = dFdx(lum);
        float dLdy = dFdy(lum);
        vec3 dpdx = dFdx(FragPos);
        vec3 dpdy = dFdy(FragPos);
        // Unit tangents, not the raw position derivatives.
        //
        // cross(norm, dpdy) is as long as a pixel is wide in world space, and
        // multiplying a luminance step by it made the perturbation grow with
        // the size of the pixel's footprint on the ground. Standing in an inn
        // a pixel covers a centimetre and this is invisible; looking out
        // across a Dragonblight snowfield it covers tens of yards, so a
        // luminance step of 0.02 became a perturbation many times longer than
        // the unit normal it was subtracted from. The normal then pointed
        // wherever the derivative did - constant across each triangle, because
        // dFdx of a planar surface is - and the ground broke into hard-edged
        // facets and streaks. Snow shows it worst: it has no real detail to
        // enhance, so all that is amplified is sampling noise.
        //
        // Normalised, the strength means what it says: how far the normal
        // tilts per unit of luminance change from one pixel to the next,
        // whatever the pixel happens to cover.
        vec3 tangentU = cross(norm, dpdy);
        vec3 tangentV = cross(dpdx, norm);
        float lenU = length(tangentU);
        float lenV = length(tangentV);
        if (lenU > 1e-6 && lenV > 1e-6) {
            vec3 perturbation = (dLdx * (tangentU / lenU) + dLdy * (tangentV / lenV))
                              * (9.0 * bumpFade);
            // A texture edge is a step, not a slope. Left unbounded, one
            // crossing a sharp boundary still turns the normal further than
            // any real ground ever slopes.
            float amount = length(perturbation);
            if (amount > 0.7) perturbation *= 0.7 / amount;
            vec3 candidate = norm - perturbation;
            float len2 = dot(candidate, candidate);
            norm = (len2 > 1e-8) ? candidate * inversesqrt(len2) : norm;
        }
    }

    vec3 lightDir2 = normalize(-lightDir.xyz);
    vec3 ambient = ambientColor.rgb * finalColor.rgb;
    // Lambert, as every other surface has it. This took abs() of the angle
    // and floored it at 0.2, so a slope turned from the sun was lit as one
    // turned toward it, and hills had no shape at low sun. The ambient term
    // is what keeps the shaded side from black, as it does for the models
    // standing on it.
    float diff = max(dot(norm, lightDir2), 0.0);
    vec3 diffuse = diff * lightColor.rgb * finalColor.rgb;

    float shadow = 1.0;
    if (shadowParams.x > 0.5) {
        vec3 ldir = normalize(-lightDir.xyz);
        // Texture relief changes lighting, not the caster geometry. Offsetting
        // along its perturbed normal can push the lookup into the surface.
        vec3 shadowNormal = dot(vertexNormal, ldir) >= 0.0 ? vertexNormal : -vertexNormal;
        float normalOffset = shadowParams.w * 1.5 * (1.0 - abs(dot(shadowNormal, ldir)));
        vec3 biasedPos = FragPos + shadowNormal * normalOffset;
        vec4 lsPos = lightSpaceMatrix * vec4(biasedPos, 1.0);
        vec3 proj = lsPos.xyz / lsPos.w;
        proj.xy = proj.xy * 0.5 + 0.5;
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            float bias = max(0.0005 * (1.0 - abs(dot(shadowNormal, ldir))), 0.00005);
            shadow = sampleShadowPCF(uShadowMap, vec3(proj.xy, proj.z - bias));
            shadow = mix(1.0, shadow, shadowParams.y);
        }
    }

    RtLight rt = rtLightAt(FragPos, vertexNormal);
    shadow = rtShadow(rt, shadow);
    ambient = rtAmbient(rt, ambientColor.rgb) * finalColor.rgb;

    vec3 result = ambient + shadow * diffuse;
    result += localLightContribution(FragPos, norm, finalColor.rgb);

    result = applyFog(result, FragPos, fragDist);

    outColor = vec4(result, 1.0);
}
