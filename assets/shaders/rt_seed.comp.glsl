#version 450
// For each trace texel, where the nearest other surface is: the nearest
// texel, within 16, of a surface behind this one and of a surface in front
// of it. The surface shaders read the result in the next frame
// (rt_lighting.glsli): a receiver the trace did not see there - background a
// moving edge has just uncovered, or an edge that moved over it - takes its
// lighting from that texel when it describes the receiver.
//
// Searched once per trace texel here, so a surface pays one read for it, and
// so does every hidden fragment the forward pass shades and then covers.
//
// xy is the offset to the surface behind, zw to the one in front; (0, 0) when
// there is none.

layout(set = 0, binding = 3) uniform sampler2D uGbufCur;
layout(set = 0, binding = 18, rgba16f) uniform writeonly image2D uSeed;

layout(local_size_x = 8, local_size_y = 8) in;

// Distance to the camera, 0 for sky and for anything off the image.
float distanceAt(ivec2 q, ivec2 size) {
    if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, size))) return 0.0;
    return texelFetch(uGbufCur, q, 0).z;
}

// The surface's change in distance per texel along one axis, from the
// neighbour that continues it best, so an edge beside it does not tilt it.
float slopeAlong(ivec2 px, ivec2 axis, float d, ivec2 size) {
    float before = distanceAt(px - axis, size), after = distanceAt(px + axis, size);
    bool haveBefore = before > 0.0, haveAfter = after > 0.0;
    if (haveBefore && haveAfter) return abs(after - d) < abs(d - before) ? after - d : d - before;
    if (haveAfter) return after - d;
    if (haveBefore) return d - before;
    return 0.0;
}

const ivec2 kDirections[8] = ivec2[8](ivec2(1, 0), ivec2(-1, 0), ivec2(0, 1), ivec2(0, -1),
                                      ivec2(1, 1), ivec2(-1, 1), ivec2(1, -1), ivec2(-1, -1));

void main() {
    ivec2 size = imageSize(uSeed);
    ivec2 px = ivec2(gl_GlobalInvocationID.xy);
    if (any(greaterThanEqual(px, size))) return;

    float d = distanceAt(px, size);
    bool sky = d <= 0.0;
    vec2 slope = sky ? vec2(0.0)
                     : vec2(slopeAlong(px, ivec2(1, 0), d, size), slopeAlong(px, ivec2(0, 1), d, size));

    // Ring by ring, nearest first; in a ring, the next surface out: the
    // nearest of those behind and the farthest of those in front.
    ivec2 behind = ivec2(0), inFront = ivec2(0);
    bool foundBehind = sky, foundInFront = false;
    for (int radius = 1; radius <= 16 && !(foundBehind && foundInFront); radius *= 2) {
        float behindDistance = 3.4e38, inFrontDistance = 0.0;
        for (int i = 0; i < 8; ++i) {
            ivec2 offset = kDirections[i] * radius;
            float other = distanceAt(px + offset, size);
            if (other <= 0.0) continue;
            if (sky) {
                if (!foundInFront && other > inFrontDistance) {
                    inFrontDistance = other;
                    inFront = offset;
                }
                continue;
            }
            // Where this surface's plane would put the texel, and the same
            // tolerance a surface shader matches a texel with.
            float predicted = d + dot(vec2(offset), slope);
            float tolerance = 0.03 * max(predicted, 0.0) + 0.05;
            if (!foundBehind && other > predicted + tolerance && other < behindDistance) {
                behindDistance = other;
                behind = offset;
            }
            if (!foundInFront && other < predicted - tolerance && other > inFrontDistance) {
                inFrontDistance = other;
                inFront = offset;
            }
        }
        foundBehind = foundBehind || behindDistance < 3.4e38;
        foundInFront = foundInFront || inFrontDistance > 0.0;
    }
    imageStore(uSeed, px, vec4(vec2(behind), vec2(inFront)));
}
