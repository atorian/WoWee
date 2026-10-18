#version 450

layout(set = 0, binding = 0) uniform sampler2D uTexture;

// Every shadow pass keeps its flags here, after its matrix: ShadowPush and
// M2ShadowPush (vk_frame_data.hpp, m2_renderer.hpp).
layout(push_constant) uniform Push {
    layout(offset = 64) ivec4 flags; // x useTexture, y alphaTest, z bent in the wind, w alpha ref in 255ths (0: a half)
} push;

layout(location = 0) in vec2 TexCoord;

void main() {
    if (push.flags.x != 0) {
        vec4 texColor = textureLod(uTexture, TexCoord, 0.0);
        float alphaRef = push.flags.w > 0 ? float(push.flags.w) / 255.0 : 0.5;
        if (push.flags.y != 0 && texColor.a < alphaRef) discard;
    }
}
