#include "rendering/character_emitters.hpp"
#include "rendering/character_renderer.hpp"
#include "rendering/m2_renderer.hpp"
#include "core/logger.hpp"

namespace wowee {
namespace rendering {

CharacterEmitters::CharacterEmitters(CharacterRenderer& characters, M2Renderer& m2)
    : characters_(characters), m2_(m2) {}

CharacterEmitters::~CharacterEmitters() {
    for (auto& [instanceId, binding] : bindings_) release(binding);
}

void CharacterEmitters::update(float deltaTime) {
    ++frame_;
    characters_.forEachEmitterSource([&](const CharacterRenderer::EmitterSource& source) {
        Binding& binding = bindings_[source.instanceId];
        binding.seenFrame = frame_;

        // The model was loaded again under the same id, or M2Renderer was
        // cleared (a map change) and took every posed set with it.
        if (binding.emitters != 0 &&
            (binding.modelSerial != source.modelSerial || !m2_.hasPosedEmitters(binding.emitters))) {
            release(binding);
        }

        if (binding.emitters == 0) {
            Model& model = models_[source.modelSerial];
            if (model.handle != 0 && !m2_.hasPosedEmitterModel(model.handle)) model.handle = 0;
            if (model.handle == 0) {
                if (model.failed) return;
                model.handle = m2_.loadPosedEmitterModel(*source.model);
                if (model.handle == 0) {
                    LOG_WARNING("Emitters of '", source.model->name, "' (",
                                source.model->particleEmitters.size(), " particle, ",
                                source.model->ribbonEmitters.size(),
                                " ribbon) did not load - drawn without them");
                    model.failed = true;
                    return;
                }
            }
            binding.emitters = m2_.createPosedEmitters(model.handle);
            binding.modelSerial = source.modelSerial;
            ++model.users;
        }

        M2Renderer::EmitterPose pose;
        pose.modelMatrix = source.modelMatrix;
        pose.scale = source.scale;
        pose.bones = source.bones;
        pose.sequenceIndex = source.sequenceIndex;
        pose.animationTimeMs = source.animationTimeMs;
        pose.globalSequenceTimeMs = source.globalSequenceTimeMs;
        pose.posed = source.posed;
        m2_.setPosedEmittersPose(binding.emitters, pose);
    });

    // Instances that are gone, or whose model no longer has emitters.
    for (auto it = bindings_.begin(); it != bindings_.end();) {
        if (it->second.seenFrame != frame_) {
            release(it->second);
            it = bindings_.erase(it);
        } else {
            ++it;
        }
    }

    m2_.updatePosedEmitters(deltaTime);
}

void CharacterEmitters::release(Binding& binding) {
    if (binding.emitters == 0) return;
    m2_.removePosedEmitters(binding.emitters);
    binding.emitters = 0;
    auto it = models_.find(binding.modelSerial);
    if (it != models_.end() && --it->second.users == 0) {
        m2_.unloadPosedEmitterModel(it->second.handle);
        models_.erase(it);
    }
    binding.modelSerial = 0;
}

} // namespace rendering
} // namespace wowee
