#version 450

// The caster side of the shadow map, for terrain and buildings: one combined
// light-space * model matrix a draw, nothing bending. The doodads have their
// own (m2_shadow.vert.glsl), drawn instanced and bent in the wind.
layout(push_constant) uniform Push {
    mat4 lightSpaceModel;   // light-space * model, multiplied on the CPU
    ivec4 flags;            // read by shadow.frag
} push;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aTexCoord;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aTexCoord;
    gl_Position = push.lightSpaceModel * vec4(aPos, 1.0);
}
