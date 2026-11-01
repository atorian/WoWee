#pragma once

#include <array>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wowee::rendering {

/// The instances of one scene as the copies of it kept elsewhere address
/// them: a slot each, which stays its own however the scene's list is
/// reordered, the slots of each model, and for each such copy - a mirror -
/// the slots changed since it last caught up.
///
/// A scene's instance list is swap-removed, so an instance's index moves
/// whenever another goes; a copy addressed by index would have to move its
/// rows with it. A slot is given when the instance is added and taken back
/// when it is removed, and the next instance added may have it.
///
/// Only a change queues work. A scene that stands still costs a mirror
/// nothing to keep up with, and an instance changed many times between two
/// catch-ups is read once. Move this with the scene that owns it.
class InstanceSlots {
public:
    static constexpr uint32_t invalid = ~0u;
    /// The copies kept of a scene. Each catches up at its own time: the ray
    /// tracer's when the renderer syncs it.
    enum class Mirror : uint8_t { RayTracing, Count };

    /// A new instance: the slot it takes, changed for every mirror.
    uint32_t add(uint32_t id, uint32_t model) {
        uint32_t slot;
        if (!free_.empty()) {
            slot = free_.back();
            free_.pop_back();
        } else {
            slot = static_cast<uint32_t>(slots_.size());
            slots_.emplace_back();
        }
        auto& group = slotsByModel_[model];
        slots_[slot] = Slot{.id = id, .model = model, .inModel = static_cast<uint32_t>(group.size())};
        group.push_back(slot);
        slotById_[id] = slot;
        markChanged(slot);
        return slot;
    }

    /// Its state changed: every mirror reads it again when it next catches up.
    void changed(uint32_t id) {
        const auto it = slotById_.find(id);
        if (it != slotById_.end()) markChanged(it->second);
    }

    /// Every placement of the model changed: its meshes came, went or were
    /// replaced.
    void modelChanged(uint32_t model) {
        const auto it = slotsByModel_.find(model);
        if (it == slotsByModel_.end()) return;
        for (const uint32_t slot : it->second) markChanged(slot);
    }

    /// Gone. Its slot is free for the next instance added; a mirror lets go
    /// of what it kept for the slot before this. The slot it had, or
    /// `invalid` for an id this does not know.
    uint32_t remove(uint32_t id) {
        const auto it = slotById_.find(id);
        if (it == slotById_.end()) return invalid;
        const uint32_t slot = it->second;
        slotById_.erase(it);
        Slot& s = slots_[slot];
        for (size_t m = 0; m < queues_.size(); ++m) {
            if (s.queuedAt[m] == invalid) continue;
            auto& queue = queues_[m];
            const uint32_t moved = queue.back();
            queue[s.queuedAt[m]] = moved;
            slots_[moved].queuedAt[m] = s.queuedAt[m];
            queue.pop_back();
            s.queuedAt[m] = invalid;
        }
        auto group = slotsByModel_.find(s.model);
        const uint32_t moved = group->second.back();
        group->second[s.inModel] = moved;
        slots_[moved].inModel = s.inModel;
        group->second.pop_back();
        if (group->second.empty()) slotsByModel_.erase(group);
        s = Slot{};
        free_.push_back(slot);
        return slot;
    }

    /// No instances, no slots, nothing to catch up on.
    void clear() {
        slotById_.clear();
        slots_.clear();
        free_.clear();
        slotsByModel_.clear();
        for (auto& queue : queues_) queue.clear();
    }

    [[nodiscard]] uint32_t slotOf(uint32_t id) const {
        const auto it = slotById_.find(id);
        return it == slotById_.end() ? invalid : it->second;
    }
    /// The instance in a slot, `invalid` for a free one.
    [[nodiscard]] uint32_t idAt(uint32_t slot) const {
        return slot < slots_.size() ? slots_[slot].id : invalid;
    }
    /// Slots handed out so far, free ones included: the rows a copy needs.
    [[nodiscard]] uint32_t slotCount() const { return static_cast<uint32_t>(slots_.size()); }
    [[nodiscard]] size_t instanceCount() const { return slotById_.size(); }

    template<class Visit>
    void forEachOfModel(uint32_t model, Visit&& visit) const {
        const auto it = slotsByModel_.find(model);
        if (it == slotsByModel_.end()) return;
        for (const uint32_t slot : it->second) visit(slot);
    }
    template<class Visit>
    void forEachInstance(Visit&& visit) const {
        for (uint32_t slot = 0; slot < slots_.size(); ++slot)
            if (slots_[slot].id != invalid) visit(slot);
    }

    /// The slots changed since the mirror last caught up, each once, as
    /// visit(slot, id); after this there are none.
    template<class Visit>
    void catchUp(Mirror mirror, Visit&& visit) {
        const auto m = static_cast<size_t>(mirror);
        auto& queue = queues_[m];
        for (const uint32_t slot : queue) {
            slots_[slot].queuedAt[m] = invalid;
            visit(slot, slots_[slot].id);
        }
        queue.clear();
    }

private:
    static constexpr size_t kMirrors = static_cast<size_t>(Mirror::Count);
    struct Slot {
        uint32_t id = invalid, model = 0;
        /// Its place in its model's list.
        uint32_t inModel = 0;
        /// Its place in each mirror's queue, `invalid` when not queued.
        std::array<uint32_t, kMirrors> queuedAt = filled(invalid);
    };
    static constexpr std::array<uint32_t, kMirrors> filled(uint32_t value) {
        std::array<uint32_t, kMirrors> out{};
        for (auto& v : out) v = value;
        return out;
    }
    void markChanged(uint32_t slot) {
        Slot& s = slots_[slot];
        for (size_t m = 0; m < kMirrors; ++m) {
            if (s.queuedAt[m] != invalid) continue;
            s.queuedAt[m] = static_cast<uint32_t>(queues_[m].size());
            queues_[m].push_back(slot);
        }
    }

    std::unordered_map<uint32_t, uint32_t> slotById_;
    std::vector<Slot> slots_;
    std::vector<uint32_t> free_;
    std::unordered_map<uint32_t, std::vector<uint32_t>> slotsByModel_;
    std::array<std::vector<uint32_t>, kMirrors> queues_;
};

} // namespace wowee::rendering
