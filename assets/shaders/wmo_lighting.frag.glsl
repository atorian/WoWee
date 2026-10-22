#version 450
#include "wmo_frame.glsli"
#include "surface_encoding.glsli"
#ifdef SURFACE_MSAA
layout(set=1,binding=0) uniform sampler2DMS surfaceColor;
layout(set=1,binding=1) uniform usampler2DMS surfaceKeys;
layout(set=1,binding=2) uniform usampler2DMS surfacePayload;
#define LOAD(image) texelFetch(image, ivec2(gl_FragCoord.xy), gl_SampleID)
#else
layout(set=1,binding=0) uniform sampler2D surfaceColor;
layout(set=1,binding=1) uniform usampler2D surfaceKeys;
layout(set=1,binding=2) uniform usampler2D surfacePayload;
#define LOAD(image) texelFetch(image, ivec2(gl_FragCoord.xy), 0)
#endif
struct Material {
    int hasTexture; int alphaTest; int light; int transitionLight;
    float specularIntensity; float heightMapVariance; int isLava; int emissive;
    float ambientR; float ambientG; float ambientB; int hasVertexColors;
    int receivesExteriorShadows; float sidnR; float sidnG; float sidnB;
    uvec4 localLights;
};
layout(std430,set=1,binding=3) readonly buffer Materials { Material materials[]; };
layout(push_constant) uniform ResolveView { mat4 inverseViewProjection; vec4 extent; } resolveView;
layout(location=0) out vec4 outColor;
void main() {
    uvec4 keys = LOAD(surfaceKeys);
    if (keys.w == 0u) discard;
    uvec4 payload = LOAD(surfacePayload);
    vec4 texel = LOAD(surfaceColor);
    // Geometry evaluation uses the pixel centre (also under MSAA); depth in
    // the payload is from that same invocation, not a resolved depth sample.
    vec2 pixel = floor(gl_FragCoord.xy) + vec2(0.5);
    vec4 position = resolveView.inverseViewProjection * vec4(
        pixel / resolveView.extent.xy * 2.0 - 1.0, uintBitsToFloat(payload.z), 1.0);
    position /= position.w;
    position.xyz += viewPos.xyz;
    Material m = materials[keys.w - 1u];
    WmoSurface surface = WmoSurface(position.xyz, texel,
        unpackSurfaceNormal(keys.y), unpackSurfaceNormal(keys.z), unpackSurfaceNormal(keys.x),
        vec4(unpackHalf2x16(payload.x), unpackHalf2x16(payload.y)), m.localLights);
    WmoLightingMaterial material = WmoLightingMaterial(m.light, m.transitionLight,
        m.specularIntensity, vec3(m.ambientR,m.ambientG,m.ambientB),
        vec3(m.sidnR,m.sidnG,m.sidnB), m.hasVertexColors, m.receivesExteriorShadows,
        m.emissive, m.isLava);
    RtLight rt = RtLight(0.0, 1.0, 1.0, vec3(0));
    vec3 lit = shadeWmoSurface(surface, material, rt);
    outColor = vec4(applyFog(lit, surface.position, length(viewPos.xyz-surface.position)), texel.a);
}
