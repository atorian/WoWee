#pragma once

#include <cstdint>

namespace wowee::rendering {

/// Which of a pass's WMO batches one recording draws. The scene pass draws
/// the opaque ones, the sky, then the blended ones: the sky depth-tests
/// against the walls and skips every pixel they cover, and a blended window
/// still finds the sky behind it.
enum class WmoDrawLayer : uint8_t { All, Opaque, Blended };

/// Whether a batch belongs to `layer`. Every batch is in All and in exactly
/// one of Opaque and Blended, so the two halves together draw what All does.
[[nodiscard]] constexpr bool wmoBatchInLayer(bool isTransparent, WmoDrawLayer layer) {
    return layer == WmoDrawLayer::All || (isTransparent == (layer == WmoDrawLayer::Blended));
}

}  // namespace wowee::rendering
