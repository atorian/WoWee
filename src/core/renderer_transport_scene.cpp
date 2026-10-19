#include "core/renderer_transport_scene.hpp"

#include "collision/collision_world.hpp"
#include "core/logger.hpp"
#include "rendering/renderer.hpp"
#include "rendering/scene_commands.hpp"

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

// The hulls' geometry is the collision world's: a hull is in it from the
// apply point that made the instance, with its model's shape.
bool RendererTransportScene::wmoHasCollision(uint32_t wmoInstanceId) const {
    return renderer_.collision().hasWmo(wmoInstanceId);
}

std::optional<float> RendererTransportScene::wmoFloorHeight(uint32_t wmoInstanceId,
                                                            const glm::mat4& placement,
                                                            const glm::vec3& renderPos,
                                                            float* outNormalZ) const {
    return renderer_.collision().wmoInstanceFloor(wmoInstanceId, placement, renderPos.x, renderPos.y,
                                                  renderPos.z, outNormalZ);
}

bool RendererTransportScene::m2WorldBounds(uint32_t m2InstanceId, const glm::mat4& placement,
                                           glm::vec3& outMin, glm::vec3& outMax) const {
    if (renderer_.collision().m2InstanceBounds(m2InstanceId, placement, outMin, outMax)) return true;
    // A car drawn without a collision mesh has no deck to be over, and the
    // game boards it by distance alone; said once a car.
    if (renderer_.scene().models().hasInstance(m2InstanceId) &&
        hullsWithoutCollision_.insert(m2InstanceId).second) {
        LOG_WARNING("Transport M2 instance ", m2InstanceId, " has no collision mesh - boarding it is "
                    "decided by distance alone, with no deck under the feet to check");
    }
    return false;
}

std::optional<float> RendererTransportScene::m2FloorHeight(uint32_t m2InstanceId,
                                                           const glm::mat4& placement,
                                                           const glm::vec3& renderPos) const {
    return renderer_.collision().m2InstanceFloor(m2InstanceId, placement, renderPos.x, renderPos.y,
                                                 renderPos.z);
}

} // namespace wowee::core
