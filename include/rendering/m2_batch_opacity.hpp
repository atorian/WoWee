#pragma once

// How see-through an M2 batch is: its colour's alpha track times its texture
// weight track, the two the client multiplies into every layer it draws.
//
// Most batches say one thing for the whole life of the model, and that value
// is read once at load. The rest animate - a spell effect's emitter mesh that
// is there for a frame and gone, a portal swirl that breathes, a doodad that
// fades in - and have to be read on the instance's own clock each time they
// are drawn: no single value taken at load is right for all of their life.

#include "rendering/m2_track_sampler.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// Whether a fixed16 track says one value at every moment of every sequence,
/// and that value (1 when the track has no keys at all, as the client reads an
/// empty track).
struct M2ConstantTrack {
    bool constant = true;
    float value = 1.0f;
};

inline M2ConstantTrack m2ConstantTrack(const pipeline::M2AnimationTrack& track) {
    M2ConstantTrack out;
    bool seen = false;
    const auto take = [&](float v) {
        const float clamped = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
        if (!seen) {
            out.value = clamped;
            seen = true;
            return true;
        }
        return std::abs(clamped - out.value) <= 1.0f / 512.0f;
    };
    // A sequence with no keys reads as the default, 1 - it is not left out.
    // Dalaran's fountain gives its basin alpha 0 in Opened and no keys in
    // Stand and Closed, its statue 0 in Stand and Closed and none in Opened:
    // read past the empty ones, both were taken for "always 0" and neither
    // was ever drawn.
    const bool anyKeys = std::any_of(track.sequences.begin(), track.sequences.end(),
                                     [](const auto& keys) { return !keys.floatValues.empty(); });
    if (!anyKeys) return out;
    for (const auto& keys : track.sequences) {
        if (keys.floatValues.empty()) {
            if (!take(1.0f)) return {.constant = false, .value = 1.0f};
            continue;
        }
        for (const float v : keys.floatValues) {
            if (!take(v)) return {.constant = false, .value = 1.0f};
        }
    }
    return out;
}

/// A batch's transparency as the model describes it: one value for its whole
/// life, or animated and to be sampled per instance.
struct M2BatchOpacity {
    bool animated = false;
    float atRest = 1.0f;  ///< the value when not animated; 1 when animated
};

inline M2BatchOpacity m2BatchOpacity(const std::vector<pipeline::M2AnimationTrack>& colors,
                                     uint16_t colorIndex,
                                     const std::vector<pipeline::M2AnimationTrack>& weights,
                                     uint16_t weightIndex) {
    M2BatchOpacity out;
    const auto fold = [&](const std::vector<pipeline::M2AnimationTrack>& tracks, uint16_t index) {
        if (index >= tracks.size()) return;
        const M2ConstantTrack c = m2ConstantTrack(tracks[index]);
        if (!c.constant) out.animated = true;
        else out.atRest *= c.value;
    };
    fold(colors, colorIndex);
    fold(weights, weightIndex);
    if (out.animated) out.atRest = 1.0f;
    return out;
}

/// The batch's transparency at this moment of this sequence.
inline float m2SampleBatchOpacity(const std::vector<pipeline::M2AnimationTrack>& colors,
                                  uint16_t colorIndex,
                                  const std::vector<pipeline::M2AnimationTrack>& weights,
                                  uint16_t weightIndex,
                                  int sequenceIndex, float timeMs, float globalTimeMs,
                                  const std::vector<uint32_t>& globalDurations) {
    const auto sample = [&](const std::vector<pipeline::M2AnimationTrack>& tracks, uint16_t index) {
        if (index >= tracks.size()) return 1.0f;
        const float value = m2_track::sampleFloat(tracks[index], sequenceIndex, timeMs,
                                                  globalTimeMs, globalDurations, 1.0f);
        return std::isfinite(value) ? std::clamp(value, 0.0f, 1.0f) : 1.0f;
    };
    return sample(colors, colorIndex) * sample(weights, weightIndex);
}

/// At or below this a batch is not drawn at all: the client skips a layer
/// whose alpha has gone to nothing rather than drawing it invisible
/// (CM2Scene::Animate, WoW.exe 3.3.5a 0x821ea3).
inline constexpr float kM2BatchHiddenOpacity = 0.0001f;

/// Below this a batch is drawn blended, whatever its material's blend mode:
/// an opaque or alpha-key layer part way through a fade becomes alpha blended
/// and goes to the sorted pass (0xa45528, blend table 0xa453b0).
inline constexpr float kM2BatchOpaqueOpacity = 0.99999f;

} // namespace wowee::rendering
