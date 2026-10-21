#version 450
layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
};
layout(location = 0) in vec3 aPos;
layout(location = 1) in float aSide;
layout(location = 2) in float aBrightness;
layout(location = 0) out float side;
layout(location = 1) out float brightness;
void main() {
    side = aSide;
    brightness = aBrightness;
    gl_Position = projection * view * vec4(aPos, 1.0);
}
