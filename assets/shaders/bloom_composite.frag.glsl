#version 450
layout(set = 0, binding = 0) uniform sampler2D levels[5];
layout(push_constant) uniform Push { float strength; } push;
layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;
void main() {
    // Keep the fine glints and add progressively wider, fainter light.
    vec3 glow = texture(levels[0], TexCoord).rgb * 0.20
              + texture(levels[1], TexCoord).rgb * 0.23
              + texture(levels[2], TexCoord).rgb * 0.23
              + texture(levels[3], TexCoord).rgb * 0.20
              + texture(levels[4], TexCoord).rgb * 0.14;
    // Screen blend in the existing LDR colour space: bright cores retain
    // their detail instead of clipping when their halo is added.
    outColor = vec4(vec3(1) - exp(-glow * push.strength * 3.0), 0);
}
