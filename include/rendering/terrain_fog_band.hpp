#pragma once

#include <algorithm>

namespace wowee::rendering {

/// Whether no part of a terrain chunk reaches either distance at which
/// terrain.frag.glsl discards ground: the fog band, past which the sky shows,
/// and the start of the hand-over to the distant terrain, past which pixels
/// are dithered away.
///
/// `centreDistance` is the horizontal distance from the camera to the centre
/// of the chunk's bounding sphere, which holds every vertex. Each pixel lies
/// inside its triangle and so inside the sphere, so none of a chunk that ends
/// short of both is discarded, and it can be drawn by the pipeline built
/// without the discards. A yard is left over for rounding.
[[nodiscard]] inline bool terrainChunkShortOfDiscards(float centreDistance, float sphereRadius,
                                                      float fogBand, float distantStart) {
    return centreDistance + sphereRadius + 1.0f < std::min(fogBand, distantStart);
}

}  // namespace wowee::rendering
