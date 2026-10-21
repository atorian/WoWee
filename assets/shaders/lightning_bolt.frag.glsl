#version 450
layout(location = 0) in float side;
layout(location = 1) in float brightness;
layout(location = 0) out vec4 outColor;
void main() {
    float halo = exp(-side * side * 6.0) * (1.0 - smoothstep(0.7, 1.0, abs(side)));
    float core = exp(-side * side * 100.0);
    vec3 glow = vec3(0.32, 0.55, 1.0) * halo + vec3(2.5, 2.7, 3.0) * core;
    outColor = vec4(glow, brightness);
}
