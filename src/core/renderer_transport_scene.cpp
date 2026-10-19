#include "core/renderer_transport_scene.hpp"

#include "rendering/m2_renderer.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"
#include "rendering/wmo_renderer.hpp"

namespace wowee::core {

// The hulls move, show and animate through the scene's changes, made at the
// start of Present; the floor queries below read them from then on.
void RendererTransportScene::setWmoTransform(uint32_t wmoInstanceId, const glm::mat4& transform) {
    renderer_.scene().worldModels().setInstanceTransform(wmoInstanceId, transform);
}

void RendererTransportScene::setM2Transform(uint32_t m2InstanceId, const glm::mat4& transform) {
    renderer_.scene().models().setInstanceTransform(m2InstanceId, transform);
}

void RendererTransportScene::markWmoAsTransport(uint32_t wmoInstanceId) {
    renderer_.scene().worldModels().setInstanceIsTransport(wmoInstanceId, true);
}

void RendererTransportScene::setWmoHidden(uint32_t wmoInstanceId, bool hidden) {
    renderer_.scene().worldModels().setInstanceHidden(wmoInstanceId, hidden);
}

size_t RendererTransportScene::setWmoDoodadAnimation(uint32_t wmoInstanceId, uint32_t animationId,
                                                     bool loop) {
    return renderer_.scene().worldModels().setInstanceDoodadAnimation(wmoInstanceId, animationId, loop);
}

bool RendererTransportScene::wmoHasCollision(uint32_t wmoInstanceId) const {
    auto* wmo = renderer_.getWMORenderer();
    return wmo && wmo->instanceHasCollisionGeometry(wmoInstanceId);
}

std::optional<float> RendererTransportScene::wmoFloorHeight(uint32_t wmoInstanceId,
                                                            const glm::mat4& placement,
                                                            const glm::vec3& renderPos,
                                                            float* outNormalZ) const {
    auto* wmo = renderer_.getWMORenderer();
    if (!wmo) return std::nullopt;
    return wmo->getInstanceFloorHeight(wmoInstanceId, renderPos.x, renderPos.y, renderPos.z, outNormalZ,
                                       &placement);
}

bool RendererTransportScene::m2WorldBounds(uint32_t m2InstanceId, const glm::mat4& placement,
                                           glm::vec3& outMin, glm::vec3& outMax) const {
    auto* m2 = renderer_.getM2Renderer();
    return m2 && m2->getInstanceWorldBounds(m2InstanceId, outMin, outMax, &placement);
}

std::optional<float> RendererTransportScene::m2FloorHeight(uint32_t m2InstanceId,
                                                           const glm::mat4& placement,
                                                           const glm::vec3& renderPos) const {
    auto* m2 = renderer_.getM2Renderer();
    if (!m2) return std::nullopt;
    return m2->getInstanceFloorHeight(m2InstanceId, renderPos.x, renderPos.y, renderPos.z, &placement);
}

} // namespace wowee::core
