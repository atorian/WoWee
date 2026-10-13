#pragma once

#include "game/transport_scene.hpp"

namespace wowee {
namespace rendering { class Renderer; }

namespace core {

/// The transports' hulls as the renderer holds them. Asks the renderer for
/// its WMO and M2 renderers on every call, since a world load rebuilds them;
/// with none (no world loaded) nothing is posed and no floor is found.
class RendererTransportScene final : public game::TransportScene {
public:
    explicit RendererTransportScene(rendering::Renderer& renderer) : renderer_(renderer) {}

    void setWmoTransform(uint32_t wmoInstanceId, const glm::mat4& transform) override;
    void setM2Transform(uint32_t m2InstanceId, const glm::mat4& transform) override;
    void markWmoAsTransport(uint32_t wmoInstanceId) override;
    void setWmoHidden(uint32_t wmoInstanceId, bool hidden) override;
    size_t setWmoDoodadAnimation(uint32_t wmoInstanceId, uint32_t animationId, bool loop) override;

    [[nodiscard]] bool wmoHasCollision(uint32_t wmoInstanceId) const override;
    [[nodiscard]] std::optional<float> wmoFloorHeight(uint32_t wmoInstanceId, const glm::vec3& renderPos,
                                                      float* outNormalZ) const override;
    bool m2WorldBounds(uint32_t m2InstanceId, glm::vec3& outMin, glm::vec3& outMax) const override;
    [[nodiscard]] std::optional<float> m2FloorHeight(uint32_t m2InstanceId,
                                                     const glm::vec3& renderPos) const override;

private:
    rendering::Renderer& renderer_;
};

} // namespace core
} // namespace wowee
