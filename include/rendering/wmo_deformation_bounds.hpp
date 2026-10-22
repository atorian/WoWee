#pragma once
#include "rendering/wind_field.hpp"
#include <algorithm>
#include <cmath>

namespace wowee::rendering {

/// Maximum model-space displacement of wmo.vert's cloth, over every wind
/// phase/direction. wind.glsli's projection onto the cloth face only contracts
/// the sway. Expanding the local box before transforming also covers rotated
/// and scaled placements. Keep verified against the actual shader in wind tests.
inline float wmoClothDisplacementBound(float drop) {
    const float along=2.f*WindState::maxLean+(.7f+.25f)*WindState::maxSwing;
    const float across=(.6f+.2f)*WindState::maxSwing;
    const float vertical=.12f*WindState::maxSwing;
    return std::max(drop,0.f)*.05f*std::sqrt(along*along+across*across+vertical*vertical);
}

} // namespace wowee::rendering
