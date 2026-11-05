#include "rendering/character_emitters.hpp"
#include <stdexcept>
#include "rendering/character_renderer.hpp"
#include "rendering/cpu_stage_times.hpp"
#include "rendering/m2_renderer.hpp"
#include "foundation/logger.hpp"

namespace wowee {
namespace rendering {

CharacterEmitters::CharacterEmitters(CharacterRenderer& characters, M2Renderer& m2, CpuStageTimes* stages)
    : characters_(characters), m2_(m2), stages_(stages) {}

CharacterEmitters::~CharacterEmitters() {
    clearInstances();
}

void CharacterEmitters::clearInstances() {
    for (auto& [instanceId, binding] : scene_->bindings_) release(binding);
    *scene_ = InstanceState{};
}

size_t CharacterEmitters::loading() const {
    size_t waiting = 0;
    for (const auto& [serial, model] : models_) waiting += model.waiting ? 1 : 0;
    return waiting;
}

void CharacterEmitters::update() {
    CpuStageLaps laps(stages_, "rendererUpdate/character emitters");
    ++frame_;
    characters_.forEachEmitterSource([&](const CharacterRenderer::EmitterSource& source) {
        Binding& binding = scene_->bindings_[source.instanceId];
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
                // Its textures are made on a worker first, as a frame cannot
                // wait for the disk; until then it has no emitters.
                model.waiting = !m2_.posedEmitterTexturesReady(*source.model);
                if (model.waiting) return;
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
        pose.skeletonOwner = source.instanceId;
        pose.sequenceIndex = source.sequenceIndex;
        pose.animationTimeMs = source.animationTimeMs;
        pose.globalSequenceTimeMs = source.globalSequenceTimeMs;
        pose.posed = source.posed;
        m2_.setPosedEmittersPose(binding.emitters, pose);
    });
    laps.lap("sources");

    // Instances that are gone, or whose model no longer has emitters.
    for (auto it = scene_->bindings_.begin(); it != scene_->bindings_.end();) {
        if (it->second.seenFrame != frame_) {
            release(it->second);
            it = scene_->bindings_.erase(it);
        } else {
            ++it;
        }
    }

    laps.lap("gone");
    m2_.updatePosedEmitters();
    laps.lap("posed sets");
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

namespace wowee::rendering {
CharacterEmitters::InstanceSession::InstanceSession(CharacterEmitters& renderer)
    : renderer_(renderer) {
    if (renderer_.scene_ != &renderer_.defaultInstances_)
        throw std::logic_error("CharacterEmitters already has a scene owner");
    state_ = std::make_unique<InstanceState>(std::move(renderer_.defaultInstances_));
    renderer_.defaultInstances_ = InstanceState{};
    renderer_.scene_ = state_.get();
}

CharacterEmitters::InstanceSession::~InstanceSession() {
    renderer_.clearInstances();
    renderer_.scene_ = &renderer_.defaultInstances_;
}
}
