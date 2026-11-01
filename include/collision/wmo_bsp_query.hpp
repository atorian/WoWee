#pragma once

#include "pipeline/wmo_bsp.hpp"
#include <glm/glm.hpp>

namespace wowee::collision {
struct WmoBspQueryResult {
    std::vector<uint16_t> tested;
    std::vector<uint16_t> faces;
    bool overflow = false;
};

// Original 12340 CAaBsp_Query_AaBox and BspQuery_Volume, on a validated BSP.
// All inputs are model-local. Flags are World::GetFacets query flags; MOPY
// contains one byte per triangle. Preserve returned order when making facets.
[[nodiscard]] WmoBspQueryResult queryWmoBsp(
    const pipeline::WmoBspData& bsp, std::span<const glm::vec3> vertices,
    std::span<const uint16_t> indices, std::span<const uint8_t> mopy,
    const glm::vec3& boundsMin, const glm::vec3& boundsMax,
    const glm::vec3& queryMin, const glm::vec3& queryMax,
    uint32_t queryFlags, bool digestCacheEnabled = true);
} // namespace wowee::collision
