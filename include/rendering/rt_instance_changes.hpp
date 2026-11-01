#pragma once

#include "rendering/instance_slots.hpp"

#include <glm/glm.hpp>
#include <cassert>
#include <cstdint>
#include <vector>

namespace wowee::rendering {

/// The ray tracer's copy of a scene's instances, kept as the scene's
/// InstanceSlots mirror RayTracing: for each slot, the RT instance placed for
/// it and the mesh it was placed with. Only the slots changed since the last
/// flush are read; matrices remain in the scene, never in a second copy here.
/// Move this with its owning instance scene, including temporary scenes.
class RtInstanceChanges {
public:
    static constexpr uint32_t invalid = ~0u;
    struct State {
        uint32_t mesh = invalid; // absent, hidden or otherwise not an RT caster
        glm::mat4 transform{1};
    };

    /// Lets go of the RT instance placed for a slot. An instance being
    /// removed does this before its slot is freed for another.
    template<class Sink>
    void release(uint32_t slot, Sink* sink) {
        if (slot < bindings_.size()) retire(bindings_[slot], sink);
    }

    /// The model's mesh is going: its placements leave RT before it does,
    /// and are read again once something says the model is back.
    template<class Sink>
    void releaseModel(InstanceSlots& slots, uint32_t model, Sink* sink) {
        slots.forEachOfModel(model, [&](uint32_t slot) { release(slot, sink); });
        slots.modelChanged(model);
    }

    template<class Sink>
    void clear(Sink* sink) {
        for (auto& binding : bindings_) retire(binding, sink);
        bindings_.clear();
        // No tombstones accumulate while RT is disabled and streaming continues.
    }

    /// Called after all producer mutations, before RtScene::extractChanges.
    /// Failures are reported once and retried only after another mutation.
    /// The RT sink retains its own ordered deltas until the consumer acks them.
    template<class Sink, class Read, class Failure>
    void flush(InstanceSlots& slots, Sink& sink, Read&& read, Failure&& failure) {
        if (bindings_.size() < slots.slotCount()) bindings_.resize(slots.slotCount());
        slots.catchUp(InstanceSlots::Mirror::RayTracing, [&](uint32_t slot, uint32_t id) {
            auto& binding = bindings_[slot];
            const State state = read(id);
            if (state.mesh != binding.mesh) retire(binding, &sink);
            if (state.mesh == invalid) return;
            if (binding.rt == invalid) {
                binding.rt = sink.addInstance(state.mesh, state.transform);
                if (binding.rt == invalid) { failure(id, state.mesh); return; }
                binding.mesh = state.mesh;
            } else {
                sink.setInstanceTransform(binding.rt, state.transform);
            }
        });
    }

private:
    struct Binding {
        uint32_t rt = invalid, mesh = invalid;
    };
    template<class Sink>
    static void retire(Binding& binding, Sink* sink) {
        if (binding.rt != invalid) {
            assert(sink);
            sink->removeInstance(binding.rt);
        }
        binding.rt = binding.mesh = invalid;
    }
    std::vector<Binding> bindings_;
};

} // namespace wowee::rendering
