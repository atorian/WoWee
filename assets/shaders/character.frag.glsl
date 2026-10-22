#version 460
#ifdef RT_HARDWARE
#extension GL_EXT_ray_query : require
#endif

#include "world_lighting_frame.glsli"
#include "interior_light.glsli"

layout(push_constant) uniform Push {
    mat4 model;          // read by character.vert
    vec4 interiorAmbient;
    vec4 interiorDirect;
    uvec4 localLights;   // local_lights.glsli
} push;

layout(set = 1, binding = 0) uniform sampler2D uTexture;

#include "character_material.glsli"
#include "m2_combiner.glsli"

layout(set = 1, binding = 2) uniform sampler2D uNormalHeightMap;
layout(set = 1, binding = 3) uniform sampler2D uSecondTexture;


layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec3 Tangent;
layout(location = 4) in vec3 Bitangent;
layout(location = 5) in vec2 TexCoord2;

layout(location = 0) out vec4 outColor;

const int PREVIEW_SIMPLE_TEXTURE_MODE = -31336;

// LOD factor from screen-space UV derivatives
float computeLodFactor() {
    vec2 dx = dFdx(TexCoord);
    vec2 dy = dFdy(TexCoord);
    float texelDensity = max(dot(dx, dx), dot(dy, dy));
    return smoothstep(0.0001, 0.005, texelDensity);
}

vec3 safeNormalize(vec3 v, vec3 fallback) {
    float len2 = dot(v, v);
    if (len2 > 1e-8) {
        return v * inversesqrt(len2);
    }
    return fallback;
}

vec3 fallbackTangent(vec3 n) {
    vec3 axis = abs(n.z) < 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    return safeNormalize(cross(axis, n), vec3(1.0, 0.0, 0.0));
}

bool finiteVec3(vec3 v) {
    return all(equal(v, v)) && all(lessThan(abs(v), vec3(1e10)));
}

bool isMagentaKeyColor(vec4 color) {
    return color.r >= 0.58 && color.b >= 0.58 && color.g <= 0.48 &&
           color.r >= color.g + 0.22 && color.b >= color.g + 0.22 &&
           abs(color.r - color.b) <= 0.38;
}

ivec2 wrapPreviewTexel(ivec2 texel, ivec2 texSize) {
    return ivec2((texel.x % texSize.x + texSize.x) % texSize.x,
                 (texel.y % texSize.y + texSize.y) % texSize.y);
}

vec4 samplePreviewTexture(sampler2D tex, vec2 uv) {
    ivec2 texSize = textureSize(tex, 0);
    if (texSize.x <= 0 || texSize.y <= 0) {
        return textureLod(tex, uv, 0.0);
    }

    vec2 wrappedUv = uv - floor(uv);
    ivec2 baseTexel = ivec2(floor(wrappedUv * vec2(texSize)));
    baseTexel = wrapPreviewTexel(baseTexel, texSize);

    vec4 color = texelFetch(tex, baseTexel, 0);
    if (!isMagentaKeyColor(color)) {
        return color;
    }

    for (int radius = 1; radius <= 4; ++radius) {
        for (int y = -radius; y <= radius; ++y) {
            for (int x = -radius; x <= radius; ++x) {
                if (abs(x) != radius && abs(y) != radius) {
                    continue;
                }
                vec4 candidate = texelFetch(tex, wrapPreviewTexel(baseTexel + ivec2(x, y), texSize), 0);
                if (!isMagentaKeyColor(candidate)) {
                    return vec4(candidate.rgb, 0.0);
                }
            }
        }
    }

    return vec4(0.0);
}

#include "parallax_occlusion.glsli"

#include "m2_surface_lighting.glsli"
#include "character_surface_lighting.glsli"
#ifdef CHARACTER_GBUFFER
#include "surface_encoding.glsli"
layout(location=1) out uvec4 outKeys;
layout(location=2) out uvec4 outPayload;
#endif
void main() {
    const vec3 receiverFace = receiverFaceNormal(FragPos);
    if (fogSkyBand1.w > 0.0 && length(FragPos.xy - viewPos.xy) >= fogSkyBand1.w) discard;

#ifndef CHARACTER_GBUFFER
    if (enablePOM == PREVIEW_SIMPLE_TEXTURE_MODE) {
        vec4 t1 = hasSecondTexture != 0 ? texture(uSecondTexture, TexCoord2) : vec4(1.0);
        vec3 stageAdd;
        vec4 texColor = combineStages(combiner, samplePreviewTexture(uTexture, TexCoord), t1, stageAdd);
        texColor.a = clamp(texColor.a, 0.0, 1.0);
        if (isMagentaKeyColor(texColor)) {
            discard;
        }
        if (alphaTest != 0 && texColor.a < 0.5) {
            discard;
        }
        if (alphaTest != 0 && hairMaterial != 0) {
            texColor.a = 1.0;
        }
        texColor.rgb = texColor.rgb * batchColor.rgb + stageAdd;
        if (modulateNeutral > 0.0)
            texColor.rgb = mix(vec3(modulateNeutral), texColor.rgb, opacity);
        outColor = vec4(texColor.rgb, texColor.a * opacity);
        return;
    }

#endif
    float lodFactor = computeLodFactor();
    // Gradients of the authored UV, taken here where every pixel of the quad
    // still agrees on them. Parallax moves the UV by a different amount per
    // pixel, and a mip level chosen from the moved UV flickers.
    vec2 uvDx = dFdx(TexCoord);
    vec2 uvDy = dFdy(TexCoord);

    // The pipeline culls nothing, so a one-sided material is culled here.
    if (twoSided == 0 && !gl_FrontFacing) discard;

    vec3 vertexNormal = safeNormalize(Normal, vec3(0.0, 0.0, 1.0));
    if (!gl_FrontFacing) vertexNormal = -vertexNormal;

    vec2 finalUV = TexCoord;

    bool usePOM = enablePOM != 0 &&
                  alphaTest == 0 &&
                  heightMapVariance > 0.001 &&
                  lodFactor < 0.99;
    bool useNormalMap = enableNormalMap != 0 &&
                        unlit == 0 &&
                        lodFactor < 0.99 &&
                        normalMapStrength > 0.001;
    mat3 TBN;
    if (usePOM || useNormalMap) {
        vec3 T = safeNormalize(Tangent, fallbackTangent(vertexNormal));
        T = safeNormalize(T - dot(T, vertexNormal) * vertexNormal, fallbackTangent(vertexNormal));
        vec3 B = safeNormalize(Bitangent, safeNormalize(cross(vertexNormal, T), vec3(0.0, 1.0, 0.0)));
        TBN = mat3(T, B, vertexNormal);
    }

    if (usePOM) {
        mat3 TBN_inv = transpose(TBN);
        vec3 viewDirWorld = normalize(viewPos.xyz - FragPos);
        vec3 viewDirTS = TBN_inv * viewDirWorld;
        finalUV = parallaxOcclusionMap(uNormalHeightMap, TexCoord, viewDirTS, lodFactor,
                                       pomMaxSamples, pomScale);
    }

    // The stages as the client combines them, before anything reads colour or
    // alpha: a fire hawk's flame cut to feathers by its mask, armour doubled
    // by its sphere-mapped shine. What a stage adds on top of the lit colour
    // comes back in stageAdd and is added after the lighting.
    vec4 t1 = hasSecondTexture != 0 ? texture(uSecondTexture, TexCoord2) : vec4(1.0);
    vec3 stageAdd;
    vec4 texColor = combineStages(combiner, textureGrad(uTexture, finalUV, uvDx, uvDy), t1, stageAdd);
    texColor.a = clamp(texColor.a, 0.0, 1.0);
    // Only explicit instance/animation opacity may fade an opaque surface.
    if (ignoreTextureAlpha != 0) texColor.a = 1.0;
    // Repair dark DXT fringes on alpha-cut character textures such as hair.
    // Transparent edge texels can carry black/garbage RGB even when alpha is
    // valid; pull color from a coarser mip and trust the source more as alpha
    // approaches opaque. This matches the generic M2 path.
    if (alphaTest != 0 && texColor.a > 0.01 && texColor.a < 1.0) {
        vec3 mipColor = textureLod(uTexture, finalUV, 4.0).rgb;
        float trust = smoothstep(0.0, 0.9, texColor.a);
        texColor.rgb = mix(mipColor, texColor.rgb, trust);
    }

    // Some classic/TBC character textures use bright magenta as a color key.
    // Apply this before any material-specific alpha path because a few preview
    // batches report as opaque/blended even when their texture still carries
    // mask-color texels.
    if (texColor.r > 0.78 && texColor.g < 0.28 && texColor.b > 0.78) {
        discard;
    }

    if (alphaTest != 0 && hairMaterial != 0) {
        if (texColor.a < 0.5) {
            discard;
        }
        texColor.a = 1.0;
    } else if (alphaTest != 0) {
        // Screen-space sharpened alpha for alpha-to-coverage anti-aliasing.
        // Rescales alpha so the 0.5 cutoff maps to exactly the texel boundary,
        // giving smooth edges when MSAA + alpha-to-coverage is active.
        float aGrad = fwidth(texColor.a);
        texColor.a = clamp((texColor.a - 0.5) / max(aGrad, 0.001) * 0.5 + 0.5, 0.0, 1.0);
        if (texColor.a < 1.0 / 255.0) discard;
    }
    // The batch's authored colour, after the alpha test: it judges the texture.
    texColor.rgb *= batchColor.rgb;

    // Compute normal (with normal mapping if enabled)
    vec3 norm = vertexNormal;
    if (useNormalMap) {
        vec3 mapNormal = textureGrad(uNormalHeightMap, finalUV, uvDx, uvDy).rgb * 2.0 - 1.0;
        mapNormal.xy *= normalMapStrength;
        mapNormal = safeNormalize(mapNormal, vec3(0.0, 0.0, 1.0));
        vec3 worldNormal = safeNormalize(TBN * mapNormal, vertexNormal);
        if (!gl_FrontFacing) worldNormal = -worldNormal;
        float blendFactor = max(lodFactor, 1.0 - normalMapStrength);
        norm = safeNormalize(mix(worldNormal, vertexNormal, blendFactor), vertexNormal);
    }

#ifdef CHARACTER_GBUFFER
    outColor=vec4(texColor.rgb,texColor.a*opacity);
    outKeys=uvec4(packSurfaceNormal(normalize(receiverFace)),packSurfaceNormal(norm),packSurfaceNormal(vertexNormal),surfaceMaterialIndex+1u);
    outPayload=uvec4(packHalf2x16(stageAdd.xy),packHalf2x16(vec2(stageAdd.z,0)),floatBitsToUint(gl_FragCoord.z),0);
#else
    M2LightingMaterial lighting=M2LightingMaterial(ivec4(unlit,0,0,0),vec4(specularIntensity,stateTint.rgb),
        push.interiorAmbient,push.interiorDirect,push.localLights);
    outColor=shadeCharacterSurface(FragPos,texColor,stageAdd,norm,vertexNormal,receiverFace,opacity,modulateNeutral,
        lighting,unlit==0?rtLightAt(FragPos,normalize(receiverFace)):RtLight(0.0,1.0,1.0,vec3(0)));
#endif
}
