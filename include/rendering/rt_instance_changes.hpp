#pragma once

#include <glm/glm.hpp>
#include <cassert>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// Producer-owned bindings from stable scene ids to RT ids. Only mutations
/// queue work; matrices remain in the scene, never in a second mirror here.
/// Model membership makes unload/reload invalidate only its own placements.
/// Move this with its owning instance scene, including temporary scenes.
class RtInstanceChanges {
public:
    static constexpr uint32_t invalid = ~0u;
    struct State {
        uint32_t mesh = invalid; // absent, hidden or otherwise not an RT caster
        glm::mat4 transform{1};
    };

    void add(uint32_t id, uint32_t model) {
        auto& group = models_[model];
        const auto [it, inserted] = instances_.emplace(id, Binding{model, static_cast<uint32_t>(group.size())});
        assert(inserted);
        if (!inserted) return;
        group.push_back(id);
        changed(id);
    }

    void changed(uint32_t id) {
        const auto it = instances_.find(id);
        if (it == instances_.end() || it->second.queueSlot != invalid) return;
        it->second.queueSlot = static_cast<uint32_t>(pending_.size());
        pending_.push_back(id);
    }

    void modelReady(uint32_t model) {
        const auto it = models_.find(model);
        if (it != models_.end()) for (uint32_t id : it->second) changed(id);
    }

    template<class Sink>
    void remove(uint32_t id, Sink* sink) {
        const auto it = instances_.find(id);
        if (it == instances_.end()) return;
        auto& binding = it->second;
        retire(binding, sink);
        if (binding.queueSlot != invalid) {
            const uint32_t moved = pending_.back();
            pending_[binding.queueSlot] = moved;
            instances_.at(moved).queueSlot = binding.queueSlot;
            pending_.pop_back();
        }
        auto group = models_.find(binding.model);
        const uint32_t moved = group->second.back();
        group->second[binding.slot] = moved;
        instances_.at(moved).slot = binding.slot;
        group->second.pop_back();
        if (group->second.empty()) models_.erase(group);
        instances_.erase(it);
        // No tombstones accumulate while RT is disabled and streaming continues.
    }

    template<class Sink>
    void modelRemoved(uint32_t model, Sink* sink) {
        const auto it = models_.find(model);
        if (it == models_.end()) return;
        for (uint32_t id : it->second) {
            retire(instances_.at(id), sink);
            changed(id);
        }
    }

    template<class Sink>
    void clear(Sink* sink) {
        for (auto& [id, binding] : instances_) retire(binding, sink);
        instances_.clear();
        models_.clear();
        pending_.clear();
    }

    /// Called after all producer mutations, before RtScene::extractChanges.
    /// Failures are reported once and retried only after another mutation.
    /// The RT sink retains its own ordered deltas until the consumer acks them.
    template<class Sink, class Read, class Failure>
    void flush(Sink& sink, Read&& read, Failure&& failure) {
        for (uint32_t id : pending_) {
            auto& binding = instances_.at(id);
            binding.queueSlot = invalid;
            const State state = read(id);
            if (state.mesh != binding.mesh) retire(binding, &sink);
            if (state.mesh == invalid) continue;
            if (binding.rt == invalid) {
                binding.rt = sink.addInstance(state.mesh, state.transform);
                if (binding.rt == invalid) { failure(id, state.mesh); continue; }
                binding.mesh = state.mesh;
            } else {
                sink.setInstanceTransform(binding.rt, state.transform);
            }
        }
        pending_.clear();
    }

private:
    struct Binding {
        uint32_t model, slot;
        uint32_t rt = invalid, mesh = invalid;
        uint32_t queueSlot = invalid;
    };
    template<class Sink>
    static void retire(Binding& binding, Sink* sink) {
        if (binding.rt != invalid) {
            assert(sink);
            sink->removeInstance(binding.rt);
        }
        binding.rt = binding.mesh = invalid;
    }
    std::unordered_map<uint32_t, Binding> instances_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> models_;
    std::vector<uint32_t> pending_;
};

} // namespace wowee::rendering
