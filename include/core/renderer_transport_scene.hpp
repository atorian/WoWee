#pragma once

#include "game/transport_scene.hpp"

#include <unordered_set>

namespace wowee {
namespace rendering { class Renderer; }

namespace core {

/// The transports' hulls as the client holds them: posed through the scene's
/// changes, stood on through the collision world.
class RendererTransportScene final : public game::TransportScene {
public:
    explicit RendererTransportScene(rendering::Renderer& renderer) : renderer_(renderer) {}

    void setWmoTransform(uint32_t wmoInstanceId, const glm::mat4& transform) override;
    void setM2Transform(uint32_t m2InstanceId, const glm::mat4& transform) override;
    void markWmoAsTransport(uint32_t wmoInstanceId) override;
    void setWmoHidden(uint32_t wmoInstanceId, bool hidden) override;
    size_t setWmoDoodadAnimation(uint32_t wmoInstanceId, uint32_t animationId, bool loop) override;

    [[nodiscard]] bool wmoHasCollision(uint32_t wmoInstanceId) const override;
    [[nodiscard]] std::optional<float> wmoFloorHeight(uint32_t wmoInstanceId, const glm::mat4& placement,
                                                      const glm::vec3& renderPos,
                                                      float* outNormalZ) const override;
    bool m2WorldBounds(uint32_t m2InstanceId, const glm::mat4& placement, glm::vec3& outMin,
                       glm::vec3& outMax) const override;
    [[nodiscard]] std::optional<float> m2FloorHeight(uint32_t m2InstanceId, const glm::mat4& placement,
                                                     const glm::vec3& renderPos) const override;

private:
    rendering::Renderer& renderer_;
    /// M2 hulls already reported as having no collision mesh.
    mutable std::unordered_set<uint32_t> hullsWithoutCollision_;
};

} // namespace core
} // namespace wowee
