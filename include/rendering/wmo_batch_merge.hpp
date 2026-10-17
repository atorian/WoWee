#pragma once

/// Which WMO group batches may be drawn as one.
///
/// A group's batches are merged so a wall made of forty batches on one texture
/// binds its material once. A merged batch has one material: its descriptor
/// set, its UBO, the pipeline it is drawn with and the cloth pivot it pushes
/// all come from the first batch that went into it. So two batches may merge
/// only when everything the draw takes from the material is the same for
/// both - otherwise the second is drawn with the first's state.
///
/// That is what the state below holds. Texture alone was not enough: an opaque
/// and an alpha-blended material on one texture came out both opaque or both
/// transparent, and the two gate banners of Stormwind, one texture, swung
/// about the first banner's pivot.
///
/// Material flags are here only through what they change. F_UNLIT and
/// F_WINDOW are in `lighting`, with the batch's run and its group's flags
/// (wmo_lighting.hpp); F_SIDN is `sidnColor`, the glow it brings. F_UNFOGGED
/// and F_UNCULLED change nothing yet - every WMO pipeline draws both faces and
/// fogs every batch - so keying on them would only split batches that draw the
/// same.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "rendering/wmo_lighting.hpp"

namespace wowee::rendering {

/// Everything a merged batch takes from the material of the batches in it.
struct WmoBatchState {
    const void* texture = nullptr;  ///< the texture bound, fallback included
    uint32_t blendMode = 0;         ///< MOMT blend mode: 1 alpha-tests, 2+ blends
    /// The light the batch is drawn with. A group's transition and interior
    /// batches on one texture are lit apart, so they draw apart.
    WmoBatchLighting lighting;
    /// MOMT sidnColor (BGRA) of an F_SIDN material, 0 for any other.
    uint32_t sidnColor = 0;
    bool isLava = false;
    uint8_t emissiveLevel = 0;
    /// Where cloth hangs: the top edge, how far it drops, and its middle.
    /// All zero when the batch is not cloth.
    float clothTop = 0.0f;
    float clothDrop = 0.0f;
    float clothCentreX = 0.0f;
    float clothCentreY = 0.0f;
    /// The model texture this batch wanted, when its load failed. Every such
    /// batch binds the white fallback, so this keeps each failed texture in a
    /// merged batch of its own, which a retry can then give its texture alone.
    /// UINT32_MAX when the texture loaded.
    uint32_t pendingTexture = UINT32_MAX;

    bool operator==(const WmoBatchState&) const = default;
};

/// The batches that draw as one, as indices into `states`.
///
/// Merged batches come out in the order their first batch appears, and the
/// batches inside one keep their order too, so a group draws the same way on
/// every load.
inline std::vector<std::vector<size_t>> groupWmoBatches(
    const std::vector<WmoBatchState>& states) {
    std::vector<std::vector<size_t>> merged;
    std::vector<size_t> firstOf;  // the first batch of each merged one
    for (size_t i = 0; i < states.size(); ++i) {
        size_t m = 0;
        while (m < firstOf.size() && !(states[firstOf[m]] == states[i])) ++m;
        if (m == firstOf.size()) {
            firstOf.push_back(i);
            merged.emplace_back();
        }
        merged[m].push_back(i);
    }
    return merged;
}

}  // namespace wowee::rendering
