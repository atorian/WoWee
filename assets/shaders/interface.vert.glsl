#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 colour;
layout(push_constant) uniform Push { vec2 scale; vec2 translate; } push;
layout(location=0) out vec2 texCoord;
layout(location=1) out vec4 vertexColour;
void main() {
    texCoord = uv;
    vertexColour = colour;
    gl_Position = vec4(position * push.scale + push.translate, 0, 1);
}
