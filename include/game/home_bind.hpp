#pragma once

#include <glm/glm.hpp>

#include <cstdint>

namespace wowee::game {

/// Where the hearthstone takes the character: SMSG_BINDPOINTUPDATE.
struct HomeBind {
    uint32_t mapId = 0;
    glm::vec3 position{0.0f};  ///< canonical
};

} // namespace wowee::game
