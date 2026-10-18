#version 450

// The sun's disc and glare (sky_sun.hpp): a quad at a fixed distance in the
// view with its translation removed, so it stands at infinity, on the far
// plane like every sky layer.

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};

layout(push_constant) uniform Push {
    mat4 model;
    vec4 colour;   // rgb tint, a = alpha
    vec4 horizon;  // x = centre height, y = scale, z = 1 to cut at the horizon
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aTexCoord;
    vec4 clip = projection * mat4(mat3(view)) * push.model * vec4(aPos, 1.0);
    gl_Position = vec4(clip.xy, clip.w, clip.w);
}
