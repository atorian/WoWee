#include "core/renderer_transport_scene.hpp"

#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee::core {

void RendererTransportScene::setWmoTransform(uint32_t wmoInstanceId, const glm::mat4& transform) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceTransform(wmoInstanceId, transform);
}

void RendererTransportScene::setM2Transform(uint32_t m2InstanceId, const glm::mat4& transform) {
    if (auto* m2 = renderer_.getM2Renderer()) m2->setInstanceTransform(m2InstanceId, transform);
}

void RendererTransportScene::markWmoAsTransport(uint32_t wmoInstanceId) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceIsTransport(wmoInstanceId, true);
}

void RendererTransportScene::setWmoHidden(uint32_t wmoInstanceId, bool hidden) {
    if (auto* wmo = renderer_.getWMORenderer()) wmo->setInstanceHidden(wmoInstanceId, hidden);
}

size_t RendererTransportScene::setWmoDoodadAnimation(uint32_t wmoInstanceId, uint32_t animationId,
                                                     bool loop) {
    auto* wmo = renderer_.getWMORenderer();
    return wmo ? wmo->setInstanceDoodadAnimation(wmoInstanceId, animationId, loop) : 0;
}

bool RendererTransportScene::wmoHasCollision(uint32_t wmoInstanceId) const {
    auto* wmo = renderer_.getWMORenderer();
    return wmo && wmo->instanceHasCollisionGeometry(wmoInstanceId);
}

std::optional<float> RendererTransportScene::wmoFloorHeight(uint32_t wmoInstanceId,
                                                            const glm::vec3& renderPos,
                                                            float* outNormalZ) const {
    auto* wmo = renderer_.getWMORenderer();
    if (!wmo) return std::nullopt;
    return wmo->getInstanceFloorHeight(wmoInstanceId, renderPos.x, renderPos.y, renderPos.z, outNormalZ);
}

bool RendererTransportScene::m2WorldBounds(uint32_t m2InstanceId, glm::vec3& outMin,
                                           glm::vec3& outMax) const {
    auto* m2 = renderer_.getM2Renderer();
    return m2 && m2->getInstanceWorldBounds(m2InstanceId, outMin, outMax);
}

std::optional<float> RendererTransportScene::m2FloorHeight(uint32_t m2InstanceId,
                                                           const glm::vec3& renderPos) const {
    auto* m2 = renderer_.getM2Renderer();
    if (!m2) return std::nullopt;
    return m2->getInstanceFloorHeight(m2InstanceId, renderPos.x, renderPos.y, renderPos.z);
}

} // namespace wowee::core
