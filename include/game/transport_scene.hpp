#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <glm/glm.hpp>

namespace wowee::game {

/// The drawn hulls of the transports, as the TransportManager needs them: it
/// poses and hides them, runs a ship's deck doodads, and asks a hull's own
/// geometry where its floor is - which is what decides whether a player is
/// aboard.
///
/// Instance ids are the ones the hulls were spawned with, a WMO's or an M2's;
/// positions are in render space. The client implements this over its
/// renderer (core::RendererTransportScene). Without one, transports still
/// move but are neither drawn nor stood on.
class TransportScene {
public:
    virtual ~TransportScene() = default;

    virtual void setWmoTransform(uint32_t wmoInstanceId, const glm::mat4& transform) = 0;
    virtual void setM2Transform(uint32_t m2InstanceId, const glm::mat4& transform) = 0;
    /// A moving deck only counts as floor while it is underfoot.
    virtual void markWmoAsTransport(uint32_t wmoInstanceId) = 0;
    virtual void setWmoHidden(uint32_t wmoInstanceId, bool hidden) = 0;
    /// Plays the animation on the hull's doodads; how many took it.
    virtual size_t setWmoDoodadAnimation(uint32_t wmoInstanceId, uint32_t animationId, bool loop) = 0;

    /// Whether the hull's collision has been built yet.
    [[nodiscard]] virtual bool wmoHasCollision(uint32_t wmoInstanceId) const = 0;
    [[nodiscard]] virtual std::optional<float> wmoFloorHeight(uint32_t wmoInstanceId,
                                                              const glm::vec3& renderPos,
                                                              float* outNormalZ) const = 0;
    virtual bool m2WorldBounds(uint32_t m2InstanceId, glm::vec3& outMin, glm::vec3& outMax) const = 0;
    [[nodiscard]] virtual std::optional<float> m2FloorHeight(uint32_t m2InstanceId,
                                                             const glm::vec3& renderPos) const = 0;
};

} // namespace wowee::game
