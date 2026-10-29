#version 450

// Where one image goes in the minimap's composite: its top-left corner, and
// the composite distances its left edge runs down and its top edge across.
// The fragment shader samples (TexCoord.y, TexCoord.x), so the quad's x runs
// down the image and its y across it.
layout(push_constant) uniform Push {
    vec2 corner;
    vec2 down;
    vec2 across;
} push;

layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aUV;

layout(location = 0) out vec2 TexCoord;

void main() {
    TexCoord = aUV;
    vec2 pos = push.corner + aPos.x * push.down + aPos.y * push.across;
    pos = pos * 2.0 - 1.0;
    gl_Position = vec4(pos, 0.0, 1.0);
}
