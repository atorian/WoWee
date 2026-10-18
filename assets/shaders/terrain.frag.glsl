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

// Each texture's relief, derived from its colours when the tile loaded
// (TerrainRenderer::deriveNormalMap): the normal in rgb, along the texture's
// u and v and out of it.
layout(set = 1, binding = 8) uniform sampler2D uBaseNormal;
layout(set = 1, binding = 9) uniform sampler2D uLayer1Normal;
layout(set = 1, binding = 10) uniform sampler2D uLayer2Normal;
layout(set = 1, binding = 11) uniform sampler2D uLayer3Normal;

// The chunk's own normals, every vertex's, as a 17x17 image (ChunkMesh::
// normalGrid): outer vertices on even texels, inner on odd. The ground is lit
// from these rather than from the vertices drawn, so a far chunk drawn with a
// handful of them keeps the light and shade of all 145.
layout(set = 1, binding = 12) uniform sampler2D uChunkNormal;

// LayerUV runs over the alpha maps' texel centres, (offset * 63/8 + 0.5) / 64
// for a grid offset of 0 to 8; the normal image's texel centres for the same
// offset are (offset * 2 + 0.5) / 17.
vec3 chunkNormal(vec2 layerUV) {
    vec2 offset = (layerUV * 64.0 - 0.5) * (8.0 / 63.0);
    return normalize(texture(uChunkNormal, (offset * 2.0 + 0.5) / 17.0).xyz * 2.0 - 1.0);
}

// After the vertex stage's model matrix and lights (GPUPushConstants).
layout(push_constant) uniform Surface {
    layout(offset = 80) vec4 relief;  // x = how steeply the relief is drawn, 0 for flat ground
} surface;

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;
layout(set = 0, binding = 5) uniform sampler2D uSceneSky;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;
layout(location = 4) flat in uvec4 vLocalLights;

layout(location = 0) out vec4 outColor;

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

// The ground's normal turned by its textures' relief.
//
// The texture coordinates are laid along the world (TerrainMesh): u runs down
// world y and v down world x. Those two axes, laid onto the surface, are the
// directions the map's x and y tilt the normal in. Of a heightfield the turned
// normal is N - h_u T - h_v B whichever way T and B are handed, so only their
// directions need to be right.
vec3 reliefNormal(vec3 n, vec3 mapNormal, float strength) {
    const vec3 uAxis = vec3(0.0, -1.0, 0.0);
    const vec3 vAxis = vec3(-1.0, 0.0, 0.0);
    vec3 t = uAxis - n * dot(n, uAxis);
    if (dot(t, t) < 1e-6) {
        // A cliff face along u: v still lies on it.
        vec3 b = normalize(vAxis - n * dot(n, vAxis));
        t = cross(b, n);
    }
    t = normalize(t);
    vec3 b = cross(n, t);
    if (dot(b, vAxis) < 0.0) b = -b;
    vec3 turned = t * (mapNormal.x * strength) + b * (mapNormal.y * strength) + n * mapNormal.z;
    float len2 = dot(turned, turned);
    return len2 > 1e-8 ? turned * inversesqrt(len2) : n;
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
    float baseWeight = max(1.0 - total, 0.0);
    vec4 finalColor = texture(uBaseTexture, TexCoord) * baseWeight;
    // These branches depend only on the chunk's uniform layer count, so UV
    // derivatives remain valid for mip selection across alpha-map boundaries.
    if (hasLayer1 != 0) finalColor += texture(uLayer1Texture, TexCoord) * weights.x;
    if (hasLayer2 != 0) finalColor += texture(uLayer2Texture, TexCoord) * weights.y;
    if (hasLayer3 != 0) finalColor += texture(uLayer3Texture, TexCoord) * weights.z;

    // The surface normal from the chunk's normal image, the same whichever
    // level of detail drew this pixel. `Normal`, the drawn vertices' own, is
    // what the image was made of.
    vec3 vertexNormal = chunkNormal(LayerUV);
    vec3 norm = vertexNormal;

    // The relief of each layer, blended as its colour is. It is read from
    // maps made of the textures, not from how the shaded colour changes from
    // one pixel to the next on screen: that answer belonged to the pixel grid,
    // so it changed with every sub-pixel move of the camera and crawled over
    // the ground in 2x2 blocks. The maps' mips flatten it with distance as the
    // colour's mips soften the colour.
    if (surface.relief.x > 0.0) {
        vec3 blended = texture(uBaseNormal, TexCoord).rgb * baseWeight;
        if (hasLayer1 != 0) blended += texture(uLayer1Normal, TexCoord).rgb * weights.x;
        if (hasLayer2 != 0) blended += texture(uLayer2Normal, TexCoord).rgb * weights.y;
        if (hasLayer3 != 0) blended += texture(uLayer3Normal, TexCoord).rgb * weights.z;
        norm = reliefNormal(vertexNormal, blended * 2.0 - 1.0, surface.relief.x);
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
        vec2 receiverSlope = shadowReceiverSlope(proj);
        if (proj.x >= 0.0 && proj.x <= 1.0 && proj.y >= 0.0 && proj.y <= 1.0 && proj.z >= 0.0 && proj.z <= 1.0) {
            shadow = sampleShadowReceiverPlane(uShadowMap, proj, receiverSlope);
            shadow = mix(1.0, shadow, shadowMapEdgeFade(proj.xy * 2.0 - 1.0));
        }
    }

    RtLight rt = rtLightAt(FragPos, vertexNormal);
    shadow = rtShadow(rt, shadow);
    ambient = rtAmbient(rt, ambientColor.rgb) * finalColor.rgb;

    // The units' map and the sun's, the darker of the two, as the client
    // takes the dynamic map and the ground's baked one. Ground has no
    // edge-on fade in the client; it darkens the whole lit colour.
    shadow = min(shadow, localShadowAt(FragPos));
    vec3 result = (ambient + diffuse) * shadowLightFactor(shadow);
    result += localLightContribution(FragPos, norm, finalColor.rgb, vLocalLights);

    result = applyFog(result, FragPos, fragDist);

    outColor = vec4(result, 1.0);
}
