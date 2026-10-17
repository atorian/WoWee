#version 450

// The sun's disc and glare (sky_sun.hpp), as WoW.exe 3.3.5a draws them: the
// texture modulated by the sun's colour, colour and alpha alike (0x9ac660,
// 0x9ac400). The disc is cut at the horizon and fades in over the 0.4 units
// above it (0x7edee0): the client splits its quad there and fades per vertex,
// alpha = clamp(2.5 h, 0, 1); taken per pixel, the same line exactly.

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    mat4 model;
    vec4 colour;   // rgb tint, a = alpha
    vec4 horizon;  // x = centre height, y = scale, z = 1 to cut at the horizon
} push;

layout(location = 0) in vec2 TexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 texel = texture(uTexture, TexCoord);
    float alpha = push.colour.a * texel.a;
    if (push.horizon.z > 0.5) {
        // v runs from 0 on the top edge to 1 on the bottom one.
        float h = push.horizon.x + (0.5 - TexCoord.y) * push.horizon.y;
        if (h <= 0.0) discard;
        alpha *= clamp(2.5 * h, 0.0, 1.0);
    }
    outColor = vec4(texel.rgb * push.colour.rgb, alpha);
}
