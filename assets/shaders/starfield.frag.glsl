#version 450

layout(location = 0) in float vBrightness;
layout(location = 1) in vec3 vColor;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 point = (gl_PointCoord - vec2(0.5)) * 2.0;
    // Filter the narrow core over the pixel footprint. Sampling its centre
    // alone loses most of the light when a star falls between pixel centres.
    vec2 footprint = fwidth(point);
    vec2 width = vec2(1.0 / 26.0) + footprint * footprint / 6.0;
    float core = (1.0 / 26.0) / sqrt(width.x * width.y)
               * exp(-dot(point * point, 1.0 / width));
    float r = length(point);
    if (r > 1.0) discard;
    float halo = 0.035 * exp(-r * r * 3.0);
    float profile = core + halo;

    // Blending is SRC_ALPHA, ONE, so the destination receives rgb * a and
    // brightness must therefore appear in exactly one of the two. It used to be
    // in both, which squared it: a star at the faint end of the range landed at
    // a tenth of its intended value and disappeared, leaving only the bright
    // ones - each of which was also drawn at the largest point size. Few, fat
    // and soft is the look that reads as bad focus.
    // Keep the compact core visible after MSAA and sky composition.
    const float radianceScale = 12.0;
    outColor = vec4(vColor * vBrightness * profile * radianceScale, 1.0);
}
