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
#include "interior_light.glsli"

layout(set = 1, binding = 0) uniform sampler2D uTexture;
layout(set = 1, binding = 3) uniform sampler2D uSecondTexture;

layout(set = 1, binding = 2) uniform M2Material {
    int hasTexture;
    int alphaTest;
    int unlit;
    int blendMode;
    int cullBackFaces;
    float specularIntensity;
    float emissiveBoost;
    float tintR;
    float tintG;
    float tintB;
    int combiner;           // M2Combiner
    int hasSecondTexture;
};

layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;
layout(set = 0, binding = 2) uniform sampler3D uFogVolume;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) flat in vec3 InstanceOrigin;
layout(location = 4) in float ModelHeight;
layout(location = 5) in float vFadeAlpha;
layout(location = 6) flat in int vSkyMode;
layout(location = 7) flat in float vHighlight;
layout(location = 8) in vec2 TexCoord2;
layout(location = 9) flat in vec4 vInteriorAmbient;
layout(location = 10) flat in vec4 vInteriorDirect;
layout(location = 11) flat in uvec4 vLocalLights;

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

// 4x4 Bayer dither matrix (normalized to 0..1)
float bayerDither4x4(ivec2 p) {
    int idx = (p.x & 3) + (p.y & 3) * 4;
    float m[16] = float[16](
         0.0/16.0,  8.0/16.0,  2.0/16.0, 10.0/16.0,
        12.0/16.0,  4.0/16.0, 14.0/16.0,  6.0/16.0,
         3.0/16.0, 11.0/16.0,  1.0/16.0,  9.0/16.0,
        15.0/16.0,  7.0/16.0, 13.0/16.0,  5.0/16.0
    );
    return m[idx];
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
    color = mix(fogColor.rgb, color, fogFactor);
    if (volumetricParams.x > 0.5) {
        vec4 air = fogVolumeAt(worldPos);
        color = color * air.a + air.rgb;
    }
    return color;
}
// The whole depth of the fog volume at this point of the screen, for the sky:
// it lies behind everything, so it takes all the air there is.
vec4 fogVolumeSky(vec2 uv) {
    return textureLod(uFogVolume, vec3(uv, 1.0), 0.0);
}

#include "m2_combiner.glsli"

void main() {
    if (cullBackFaces != 0 && !gl_FrontFacing) discard;
    // The stages as the client combines them: armour doubled by its
    // sphere-mapped shine, a flame shaped by its mask, a layer with light
    // of its own added on top of the lit one.
    vec4 t0 = hasTexture != 0 ? texture(uTexture, TexCoord) : vec4(1.0);
    vec4 t1 = hasSecondTexture != 0 ? texture(uSecondTexture, TexCoord2) : vec4(1.0);
    vec3 stageAdd;
    vec4 texColor = combineStages(combiner, t0, t1, stageAdd);
    texColor.a = clamp(texColor.a, 0.0, 1.0);
    // The batch's authored colour. A glow card is painted white and coloured
    // here - Orgrimmar's bonfire carries (1.0, 0.329, 0.0) - so without it
    // every fire in the world burns white. It is part of the vertex colour,
    // which what the stages add on top is not.
    texColor.rgb *= vec3(tintR, tintG, tintB);

    // Original client sky M2s carry their authored colour and alpha, and are
    // taken as they are. They are camera-centered and unlit, and must not be
    // swallowed by world-distance fog.
    //
    // This used to sit below the three discards, which meant the sky was not
    // taken as it is. The alpha test in particular rescales alpha by its own
    // screen-space derivative:
    //
    //     float aGrad = fwidth(texColor.a);
    //     texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) ...
    //
    // fwidth is how fast alpha changes from one pixel to the next, so it
    // changes whenever the view does - and a nebula's alpha ramp is gentle,
    // which makes the divisor tiny and the result a hard edge. Turning the
    // camera moved that edge, so Hellfire's sky flickered while the view moved
    // and stood still when it did not, on its blended layers alone. The rescale
    // is for foliage cutouts, where a hard edge is the point.
    if (vSkyMode != 0) {
        // An opaque layer's alpha channel says nothing - it was never
        // blended - so only the fade is its alpha. That matters while the
        // sky crossfades between zones, when every layer is drawn blended.
        float skyAlpha = (blendMode == 0) ? 1.0 : texColor.a;
        // Behind all the air there is, as the procedural sky is. An
        // additive layer only loses what the air hides of it; the air's own
        // light is already in the layer it is added to.
        vec3 skyColor = texColor.rgb + stageAdd;
        if (volumetricParams.x > 0.5) {
            vec4 clip = projection * view * vec4(FragPos, 1.0);
            vec4 air = fogVolumeSky(clip.xy / max(clip.w, 1e-4) * 0.5 + 0.5);
            skyColor = (blendMode >= 3) ? skyColor * air.a : skyColor * air.a + air.rgb;
        }
        // NoAlphaAdd is drawn ONE, ONE, so it can only fade through its colour.
        if (blendMode == 3) skyColor *= vFadeAlpha;
        outColor = vec4(skyColor, skyAlpha * vFadeAlpha);
        return;
    }


    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;
    bool isFoliage = (alphaTest == 2);

    // Fix DXT fringe: transparent edge texels have garbage (black) RGB, and
    // bilinear drags it into the leaf's edge. The blurred high mip is the
    // colour the edge should have been.
    //
    // Only where the fringe actually is. This used to run on everything below
    // full alpha and trust the texel in proportion to it, so a leaf texel at
    // half alpha - which is leaf, not fringe, and the canopy is full of them -
    // came out sixty percent a blurred sixteen-pixel average of the whole
    // sheet. That is what took the contrast and the colour out of a lit
    // canopy and left it looking milky. A quarter alpha is where the fringe
    // stops and the leaf starts.
    if (alphaTest != 0 && texColor.a > 0.01 && texColor.a < 0.25) {
        vec3 mipColor = textureLod(uTexture, TexCoord, 4.0).rgb;
        float trust = smoothstep(0.0, 0.25, texColor.a);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    float alphaCutoff = 0.5;
    if (alphaTest == 2) {
        alphaCutoff = 0.4;
    } else if (alphaTest == 3) {
        alphaCutoff = 0.25;
    } else if (alphaTest == 4) {
        // The material's own alpha key: GxBlend_AlphaKey's 224/255.
        alphaCutoff = 224.0 / 255.0;
    } else if (alphaTest != 0) {
        alphaCutoff = 0.4;
    }
    // Mip-alpha preservation: alpha mips average downward, thinning distant
    // canopies to skeletons. Boost alpha with mip level so perceived leaf
    // density stays constant with distance.
    if (isFoliage && hasTexture != 0) {
        // Gentler than it was: at 0.18 a mip-4 canopy came back with every
        // leaf texel above 0.23 alpha opaque, which is a solid green mass
        // rather than a thinned one. Coverage carries the rest now.
        float mip = textureQueryLod(uTexture, TexCoord).x;
        texColor.a *= 1.0 + clamp(mip, 0.0, 4.0) * 0.11;
    }
    if (alphaTest != 0) {
        // Screen-space sharpened alpha: rescale so the cutoff maps to the
        // texel boundary. With MSAA + alpha-to-coverage on the cutout
        // pipeline this dithers the edge band across samples, smoothing
        // leaf silhouettes instead of the old hard binary discard.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - alphaCutoff) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    if (blendMode == 1 && texColor.a < 0.004) discard;

    // Per-instance color variation (foliage only).
    //
    // Half what it was. A thirty-point spread in brightness is a third of the
    // way to another time of day, and two canopies of the same tree standing
    // in each other reads as a seam down the middle of the crown rather than
    // as two trees. Enough to break the copy, not enough to be seen as a
    // difference in light.
    if (isFoliage) {
        float hash = fract(sin(dot(InstanceOrigin.xy, vec2(127.1, 311.7))) * 43758.5453);
        float hueShiftR = 1.0 + (hash - 0.5) * 0.10;       // ±5% red
        float hueShiftB = 1.0 + (fract(hash * 7.13) - 0.5) * 0.10; // ±5% blue
        float brightness = 0.93 + hash * 0.14;               // 93-107%
        texColor.rgb *= vec3(hueShiftR, 1.0, hueShiftB) * brightness;
    }

    vec3 norm = normalize(Normal);
    bool foliageTwoSided = (alphaTest == 2);
    if (!foliageTwoSided && !gl_FrontFacing) norm = -norm;

    vec3 vertexNormal = norm;

    // Detail normal perturbation (foliage only) - UV-based only so wind doesn't cause flicker
    if (isFoliage) {
        float nx = sin(TexCoord.x * 12.0 + TexCoord.y * 5.3) * 0.10;
        float ny = sin(TexCoord.y * 14.0 + TexCoord.x * 4.7) * 0.10;
        norm = normalize(norm + vec3(nx, ny, 0.0));
    }

    bool inside = insideInterior(vInteriorAmbient);
    vec3 ldir = normalize(-interiorLightDir(vInteriorAmbient, lightDir.xyz));
        float nDotL = dot(norm, ldir);
        float diff = foliageTwoSided ? abs(nDotL) : max(nDotL, 0.0);

    vec3 result;
    if (unlit != 0) {
        result = texColor.rgb * emissiveBoost;
        if (emissiveBoost > 1.0) {
            // Weighted by the texel's own brightness. Added flat it lit the
            // whole quad, and a glow card is black everywhere but its middle,
            // so the card's rectangle appeared as an orange panel hanging on
            // whatever was behind the fire. Black has nothing to boost.
            float emissiveWeight =
                dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
            result += vec3(0.32, 0.14, 0.025) * (emissiveBoost - 1.0) *
                      emissiveWeight;
        }
    } else {
        vec3 viewDir = normalize(viewPos.xyz - FragPos);
        vec3 sunColor = interiorDirect(vInteriorAmbient, vInteriorDirect, lightColor.rgb);

        float spec = 0.0;
        float shadow = 1.0;
        if (!isFoliage) {
            vec3 halfDir = normalize(ldir + viewDir);
            spec = pow(max(dot(norm, halfDir), 0.0), 32.0) * specularIntensity;
        }

        if (!inside && shadowParams.x > 0.5) {
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

        // Leaf subsurface scattering (foliage only) - uses stable normal, no FragPos dependency
        vec3 sss = vec3(0.0);
        if (isFoliage) {
            float backLit = max(-nDotL, 0.0);
            float viewDotLight = max(dot(viewDir, -ldir), 0.0);
            float sssAmount = backLit * pow(viewDotLight, 4.0) * 0.35 * texColor.a;
            sss = sssAmount * vec3(1.0, 0.9, 0.5) * sunColor;
        }

        // Sky-bounce ambient for foliage: upward-facing leaves catch more
        // ambient than the canopy underside, giving the crown depth instead
        // of a uniformly-lit blob.
        RtLight rt = rtLightAt(FragPos, vertexNormal);
        if (!inside) shadow = rtShadow(rt, shadow);
        vec3 ambientTerm = rtAmbient(rt, interiorAmbient(vInteriorAmbient, ambientColor.rgb));
        if (isFoliage) {
            ambientTerm *= 0.82 + 0.30 * clamp(norm.z, 0.0, 1.0);
        }
        // The units around the player, at full detail, as the terrain and
        // the buildings take them: a model is ground too - a dock, a bridge,
        // a rock. Indoors a model takes only these, kept below the player's
        // head as the room around it does.
        float unitShadow = localShadowAt(FragPos);
        if (inside) unitShadow = mix(unitShadow, 1.0, unitShadowAboveFade(FragPos));
        shadow = min(shadow, unitShadow);
        // The whole lit colour darkened alike, as the client does; the
        // highlight is the sun's own and goes with it.
        result = (ambientTerm + diff * sunColor) * texColor.rgb * shadowLightFactor(shadow)
               + shadow * spec * sunColor
               + sss;
    }

    // Canopy ambient occlusion (foliage only). ModelHeight arrives as the
    // fraction of the plant's own height, so the shading sits in the same
    // place on a forty-yard tree and on a knee-high fern; it used to be raw
    // model-space z against a hardcoded eighteen, which put the darkening on
    // the trunk of anything tall and over the whole of anything short.
    if (isFoliage) {
        float aoFactor = mix(0.55, 1.0, smoothstep(0.0, 0.6, ModelHeight));
        result *= aoFactor;
    }

    if (unlit == 0) result += localLightContribution(FragPos, norm, texColor.rgb, vLocalLights);
    // What the stages add on top of the lit colour - an Add combiner's second
    // stage, a reflection laid over - takes no light.
    result += stageAdd;

    float dist = length(viewPos.xyz - FragPos);
    if (blendMode == 5 || blendMode == 6) {
        // Fade toward the multiplication identity. Black would darken the
        // background, while additive fading would wash out the base texture.
        float visibility = distanceFogVisibility(dist, fogParams.x, fogParams.y) * vFadeAlpha;
        if (volumetricParams.x > 0.5) visibility *= fogVolumeAt(FragPos).a;
        float neutral = blendMode == 6 ? 0.5 : 1.0;
        result = mix(vec3(neutral), result, clamp(visibility, 0.0, 1.0));
    } else if (blendMode >= 3) {
        // Additive. Mixing toward the fog colour would give the card's black
        // corners the fog's colour, and additive then adds that to the scene -
        // the whole quad shows up as a lit rectangle hanging in the air, which
        // is what Orgrimmar's bonfire glow was doing to the wall behind it.
        // Distance can only take an additive contribution away, and so can
        // the air in front of it: its own light is already in the scene
        // behind the card.
        float fogFactor = distanceFogVisibility(dist, fogParams.x, fogParams.y);
        result *= fogFactor;
        if (volumetricParams.x > 0.5) result *= fogVolumeAt(FragPos).a;
    } else {
        result = applyFog(result, FragPos, dist);
    }

    float outAlpha = texColor.a * vFadeAlpha;
    // The distance fade, for a batch drawn with no blending to fade through.
    // Sixteen ordered steps against the fragment's own screen position: a tree
    // at the edge of the draw distance thins out rather than switching off,
    // and it reads the same at every sample count, where leaving the fade in
    // the coverage alone would give two steps at 2x MSAA and none with MSAA
    // off. Multiplying it into the coverage as well would take the leaf edges
    // with it, so the cutout keeps its own alpha.
    if (alphaTest != 0 && blendMode <= 1) {
        if (vFadeAlpha <= bayerDither4x4(ivec2(gl_FragCoord.xy))) discard;
        outAlpha = texColor.a;
    }
    // Pressed on. The real client lifts the whole model while the button is
    // down over it, which is what says "this one, and the click landed": a
    // warm brightening rather than a tint, so a dark door reads as lit and a
    // pale one does not blow out.
    if (vHighlight > 0.0) {
        float lift = clamp(vHighlight, 0.0, 1.0);
        result = result * (1.0 + 0.6 * lift) + vec3(0.22, 0.19, 0.10) * lift;
    }

    // NoAlphaAdd is drawn ONE, ONE: neither the texture's alpha nor the alpha
    // written here scales it, so the distance and instance fade go into the
    // colour.
    if (blendMode == 3) result *= vFadeAlpha;

    outColor = vec4(result, outAlpha);
}
