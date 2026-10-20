#version 450

layout(location = 0) in float vBrightness;
layout(location = 1) in vec3 vColor;
layout(location = 2) flat in float vFootprint;

layout(location = 0) out vec4 outColor;

// Integral of a unit Gaussian, using an approximation to erf. Integrating
// each pixel preserves a subpixel star's flux as its centre crosses the grid;
// sampling a very narrow peak just once made most point stars disappear.
vec2 gaussianCdf(vec2 x) {
    x *= 0.70710678118;
    vec2 t = 1.0 / (1.0 + 0.3275911 * abs(x));
    vec2 erf = 1.0 - (((((1.061405429 * t - 1.453152027) * t
        + 1.421413741) * t - 0.284496736) * t + 0.254829592) * t)
        * exp(-x * x);
    return 0.5 * (1.0 + sign(x) * erf);
}

float pixelFlux(vec2 centre, float sigma) {
    vec2 integral = gaussianCdf((centre + 0.5) / sigma)
                  - gaussianCdf((centre - 0.5) / sigma);
    return integral.x * integral.y;
}

void main() {
    vec2 centre = (gl_PointCoord - vec2(0.5)) * vFootprint;
    float sigma = clamp(vFootprint * 0.12, 0.45, 0.65);
    float profile = 0.98 * pixelFlux(centre, sigma)
                  + 0.02 * pixelFlux(centre, 1.0);

    // Blending is SRC_ALPHA, ONE, so the destination receives rgb * a and
    // brightness must therefore appear in exactly one of the two. It used to be
    // in both, which squared it: a star at the faint end of the range landed at
    // a tenth of its intended value and disappeared, leaving only the bright
    // ones - each of which was also drawn at the largest point size. Few, fat
    // and soft is the look that reads as bad focus.
    outColor = vec4(vColor * vBrightness * profile, 1.0);
}
