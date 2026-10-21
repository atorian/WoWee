#version 450
layout(local_size_x = 8, local_size_y = 8) in;
layout(set = 0, binding = 0) uniform sampler2D source;
layout(set = 0, binding = 1, rgba16f) uniform writeonly image2D target;
layout(push_constant) uniform Push { int firstLevel; } push;

vec3 extractLight(vec3 c) {
    // LDR soft knee. Extract each source tap BEFORE averaging: a narrow
    // bright glint must not disappear into dark water during downsampling.
    float peak = max(c.r, max(c.g, c.b));
    const float threshold = 0.78;
    const float knee = 0.16;
    float soft = clamp(peak - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contribution = max(peak - threshold, soft);
    return c * (contribution / max(peak, 0.0001));
}

vec3 sampleLight(vec2 uv) { return textureLod(source, uv, 0.0).rgb; }

void main() {
    ivec2 pixel = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);
    if (any(greaterThanEqual(pixel, size))) return;
    if (push.firstLevel != 0) {
        ivec2 last = textureSize(source, 0) - 1;
        vec3 bright = vec3(0);
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                bright += extractLight(texelFetch(source, min(pixel * 2 + ivec2(x,y), last), 0).rgb);
        imageStore(target, pixel, vec4(bright * 0.25, 1));
        return;
    }
    vec2 uv = (vec2(pixel) + 0.5) / vec2(size);
    vec2 d = 1.0 / vec2(textureSize(source, 0));
    // A normalized 13-tap filter: stable small highlights, without the
    // box-filter grid or a full-resolution Gaussian blur.
    vec3 c = sampleLight(uv) * 0.125;
    c += (sampleLight(uv + d * vec2(-2,-2)) + sampleLight(uv + d * vec2(2,-2))
        + sampleLight(uv + d * vec2(-2,2)) + sampleLight(uv + d * vec2(2,2))) * 0.03125;
    c += (sampleLight(uv + d * vec2(-2,0)) + sampleLight(uv + d * vec2(2,0))
        + sampleLight(uv + d * vec2(0,-2)) + sampleLight(uv + d * vec2(0,2))) * 0.0625;
    c += (sampleLight(uv + d * vec2(-1,-1)) + sampleLight(uv + d * vec2(1,-1))
        + sampleLight(uv + d * vec2(-1,1)) + sampleLight(uv + d * vec2(1,1))) * 0.125;
    imageStore(target, pixel, vec4(c, 1));
}
