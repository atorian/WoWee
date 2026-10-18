#version 450

// The wake's passes but its FFT (water_wake.glsli), by wake.mode:
//   load     the height and every body's pressure head into the spectrum
//   derive   the spectrum, forward by the inverse of its conjugate, times
//            the vertical derivative
//   step     each texel one step on, held still at the shore and soaked at
//            the edge
//   surface  what the water samples: height, slopes, and the foam its
//            breaking leaves, wake.stepSeconds on from the last
//   shift    the field and its foam moved by whole texels as the camera
//            goes, out into the spectrum's room and back

layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;

#include "water_wake_passes.glsli"

/// Every body's pressure head at the middle of texel `texel`.
float pressureAt(ivec2 texel) {
    vec2 p = (vec2(texel) + vec2(0.5)) * kWakeTexelYards;
    float head = 0.0;
    for (int i = 0; i < moverCount.x; ++i) {
        vec2 centre = mix(moverPlace[i].zw, moverPlace[i].xy, wake.along);
        head += wakePressure(p, centre, moverShape[i].xy, moverShape[i].z, moverShape[i].w, moverHead[i].x);
    }
    return head;
}

/// How hard the bodies churn the water at texel `texel`: each over the
/// strip it crossed this frame, as hard as its speed makes it.
float churnAt(ivec2 texel) {
    vec2 p = (vec2(texel) + vec2(0.5)) * kWakeTexelYards;
    float churn = 0.0;
    for (int i = 0; i < moverCount.x; ++i) {
        churn = max(churn, wakeChurnAt(p, moverPlace[i].zw, moverPlace[i].xy, moverShape[i].xy, moverShape[i].z,
                                       moverShape[i].w, moverHead[i].y));
    }
    return churn;
}

void main() {
    ivec2 texel = ivec2(gl_GlobalInvocationID.xy);
    if (wake.mode == kWakeLoad) {
        vec2 state = imageLoad(wakeState, texel).xy;
        imageStore(wakeSpectrum, texel, vec4(state.x + pressureAt(texel), 0.0, 0.0, 0.0));
    } else if (wake.mode == kWakeDerive) {
        // The inverse transform of a real field's conjugate - itself -
        // conjugated, is its forward transform; scaled once here for both.
        vec2 s = imageLoad(wakeSpectrum, texel).xy;
        vec2 k = wakeWaveVector(texel, kWakeSize, kWakeTexelYards);
        float scale = wakeVerticalDerivative(length(k)) / float(kWakeSize * kWakeSize);
        imageStore(wakeSpectrum, texel, vec4(oceanConjugate(s) * scale, 0.0, 0.0));
    } else if (wake.mode == kWakeStepMode) {
        vec2 state = imageLoad(wakeState, texel).xy;
        float vertical = imageLoad(wakeSpectrum, texel).x;
        float next = wakeStep(state.x, state.y, vertical, wake.stepSeconds) * wakeEdgeKeep(texel, kWakeSize);
        if (wakeIsShore(texel)) next = 0.0;
        // h_tt = -g G (h + P): downward where it is positive.
        float downward = kOceanGravity * vertical;
        imageStore(wakeState, texel, vec4(next, state.x, downward, 0.0));
    } else if (wake.mode == kWakeSurface) {
        ivec2 last = ivec2(kWakeSize - 1);
        vec3 here = imageLoad(wakeState, texel).xyz;
        float east = imageLoad(wakeState, min(texel + ivec2(1, 0), last)).x;
        float west = imageLoad(wakeState, max(texel - ivec2(1, 0), ivec2(0))).x;
        float north = imageLoad(wakeState, min(texel + ivec2(0, 1), last)).x;
        float south = imageLoad(wakeState, max(texel - ivec2(0, 1), ivec2(0))).x;
        vec2 slope = vec2(east - west, north - south) / (2.0 * kWakeTexelYards);
        float foam = wakeFoam(imageLoad(wakeSurface, texel).w, churnAt(texel), here.z, wake.stepSeconds);
        if (wakeIsShore(texel)) foam = 0.0;
        imageStore(wakeSurface, texel, vec4(here.x, slope, foam));
    } else if (wake.mode == kWakeShiftOut) {
        ivec2 from = texel + wake.shift;
        vec3 held = vec3(0.0);
        if (all(greaterThanEqual(from, ivec2(0))) && all(lessThan(from, ivec2(kWakeSize)))) {
            held = vec3(imageLoad(wakeState, from).xy, imageLoad(wakeSurface, from).w);
        }
        imageStore(wakeSpectrum, texel, vec4(held, 0.0));
    } else if (wake.mode == kWakeShiftIn) {
        vec3 held = imageLoad(wakeSpectrum, texel).xyz;
        imageStore(wakeState, texel, vec4(held.xy, 0.0, 0.0));  // no step yet: nothing breaking
        // The height and slopes are made again by the surface pass after.
        imageStore(wakeSurface, texel, vec4(0.0, 0.0, 0.0, held.z));
    }
}
